#include "EnginePCH.h"
#include "Engine/Navigation/NavigationWorld.h"

#include "Engine/Asset/NavMeshData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Random.h"
#include "Engine/Navigation/NavigationRuntime.h"

#include <DetourAlloc.h>
#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <utility>

// The run-time navigation mesh and its queries (NavigationWorld.h; Docs/Decisions/0045-m30-navigation.md decisions 10 and
// 11).

namespace Engine {

	struct NavigationWorld::Agent
	{
		std::string Name{};
		glm::vec3 BoundsMin{ 0.0f };
		glm::vec3 BoundsMax{ 0.0f };
		dtQueryFilter Filter{};
		// Null for an agent type without polygons.
		dtNavMesh* Mesh = nullptr;
		dtNavMeshQuery* Query = nullptr;
		dtPolyRef Base = 0; // the reference of polygon 0; polygon i is Base | i (one tile)
		int PolygonCount = 0;
		// Each polygon's area in XZ (its triangle fan, never below zero), and the running sum in polygon order.
		std::vector<double> Areas{};
		std::vector<double> CumulativeAreas{};
		// FindPath's corridor (scratch, MaxNavigationPathPolygons).
		std::vector<dtPolyRef> Corridor{};

		Agent() = default;
		~Agent()
		{
			// The query refers to the mesh: freed first. Both accept null.
			dtFreeNavMeshQuery(Query);
			dtFreeNavMesh(Mesh);
		}

		Agent(const Agent&) = delete;
		Agent& operator=(const Agent&) = delete;
		Agent(Agent&&) = delete;
		Agent& operator=(Agent&&) = delete;
	};

	namespace Utils {

		// A position on the mesh: its polygon and the point.
		struct NavigationSurfacePoint
		{
			dtPolyRef Ref = 0;
			glm::vec3 Point{ 0.0f };
		};

		[[nodiscard]] static bool IsNavigationQueryPosition(const glm::vec3& position)
		{
			return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)
				&& !glm::any(glm::greaterThan(glm::abs(position), glm::vec3(MaxNavigationCoordinate)));
		}

		[[nodiscard]] static glm::vec3 ToVector(const float* values)
		{
			return glm::vec3(values[0], values[1], values[2]);
		}

		// Collects the polygon whose closest point (Detour's closestPointOnPoly, on the detail surface) is nearest to a
		// position in 3D, the first one visited winning a tie.
		class NearestPolygonQuery final : public dtPolyQuery
		{
		public:
			NearestPolygonQuery(const dtNavMeshQuery& query, const glm::vec3& position)
				: m_Query(query), m_Position(position)
			{
			}

			void process(const dtMeshTile* /*tile*/, dtPoly** /*polys*/, dtPolyRef* refs, int count) override
			{
				for (int index = 0; index < count; ++index)
				{
					float closest[3] = {};
					if (dtStatusFailed(m_Query.closestPointOnPoly(refs[index], &m_Position.x, closest, nullptr)))
						continue;
					const float distance = dtVdistSqr(&m_Position.x, closest);
					if (distance < m_DistanceSquared)
					{
						m_DistanceSquared = distance;
						m_Nearest = { .Ref = refs[index], .Point = ToVector(closest) };
					}
				}
			}

			[[nodiscard]] float GetDistanceSquared() const { return m_DistanceSquared; }
			[[nodiscard]] const NavigationSurfacePoint& GetNearest() const { return m_Nearest; }
		private:
			const dtNavMeshQuery& m_Query;
			glm::vec3 m_Position{ 0.0f };
			float m_DistanceSquared = FLT_MAX;
			NavigationSurfacePoint m_Nearest{};
		};

		// Whether `filter` lets a walk enter `polygon`: dtQueryFilter::passFilter's rule, which Detour defines inline in its own
		// translation unit (DetourNavMeshQuery.cpp), so other files cannot call it.
		[[nodiscard]] static bool PassesFilter(const dtQueryFilter& filter, const dtPoly& polygon)
		{
			return (polygon.flags & filter.getIncludeFlags()) != 0 && (polygon.flags & filter.getExcludeFlags()) == 0;
		}

		// The polygon's area in XZ: its triangle fan (dtTriArea2D, positive for Recast's winding), never below zero, so a
		// polygon of repeated or collinear vertices weighs nothing.
		[[nodiscard]] static double GetPolygonArea(const dtMeshTile& tile, const dtPoly& polygon)
		{
			double area = 0.0;
			const float* first = &tile.verts[polygon.verts[0] * 3];
			for (int corner = 2; corner < static_cast<int>(polygon.vertCount); ++corner)
				area += static_cast<double>(dtTriArea2D(first, &tile.verts[polygon.verts[corner - 1] * 3], &tile.verts[polygon.verts[corner] * 3]));
			return std::max(area, 0.0);
		}

	}

	namespace {

		// Builds `agent`'s Detour mesh and query from `source` (validated, with polygons).
		[[nodiscard]] Status BuildDetourMesh(const NavMeshAgent& source, NavigationWorld::Agent& agent)
		{
			const size_t polygonCount = source.GetPolygonCount();
			const std::vector<unsigned short> flags(polygonCount, 1);
			dtNavMeshCreateParams parameters{};
			parameters.verts = source.Vertices.data();
			parameters.vertCount = static_cast<int>(source.GetVertexCount());
			parameters.polys = source.Polygons.data();
			parameters.polyFlags = flags.data();
			parameters.polyAreas = source.Areas.data();
			parameters.polyCount = static_cast<int>(polygonCount);
			parameters.nvp = static_cast<int>(source.VerticesPerPolygon);
			parameters.detailMeshes = source.DetailMeshes.data();
			parameters.detailVerts = source.DetailVertices.data();
			parameters.detailVertsCount = static_cast<int>(source.DetailVertices.size() / 3);
			parameters.detailTris = source.DetailTriangles.data();
			parameters.detailTriCount = static_cast<int>(source.DetailTriangles.size() / 4);
			parameters.walkableHeight = source.Height;
			parameters.walkableRadius = source.Radius;
			parameters.walkableClimb = source.MaxStep;
			for (int axis = 0; axis < 3; ++axis)
			{
				parameters.bmin[axis] = source.BoundsMin[axis];
				parameters.bmax[axis] = source.BoundsMax[axis];
			}
			parameters.cs = source.CellSize;
			parameters.ch = source.CellHeight;
			parameters.buildBvTree = true;

			// After ValidateNavMeshData, dtCreateNavMeshData fails only to allocate.
			unsigned char* data = nullptr;
			int size = 0;
			if (!dtCreateNavMeshData(&parameters, &data, &size))
				return MakeError(ErrorCode::OutOfMemory, "Detour could not allocate the navigation mesh of agent type '{}'", source.Name);
			agent.Mesh = dtAllocNavMesh();
			if (agent.Mesh == nullptr)
			{
				dtFree(data);
				return MakeError(ErrorCode::OutOfMemory, "Detour could not allocate the navigation mesh of agent type '{}'", source.Name);
			}
			// With DT_TILE_FREE_DATA the mesh owns `data` once its tile is added; a failed init leaves it with the caller.
			const dtStatus initialized = agent.Mesh->init(data, size, DT_TILE_FREE_DATA);
			if (dtStatusFailed(initialized))
			{
				dtFree(data);
				if (dtStatusDetail(initialized, DT_OUT_OF_MEMORY))
					return MakeError(ErrorCode::OutOfMemory, "Detour could not allocate the navigation mesh of agent type '{}'", source.Name);
				return MakeError(ErrorCode::Validation, "Detour refused the navigation mesh of agent type '{}' (status {:#x})", source.Name, initialized);
			}
			agent.Query = dtAllocNavMeshQuery();
			if (agent.Query == nullptr || dtStatusFailed(agent.Query->init(agent.Mesh, static_cast<int>(MaxNavigationQueryNodes))))
				return MakeError(ErrorCode::OutOfMemory, "Detour could not allocate the navigation query of agent type '{}'", source.Name);

			const dtNavMesh& mesh = *agent.Mesh;
			const dtMeshTile* tile = mesh.getTile(0);
			ENGINE_CORE_VERIFY(tile != nullptr && tile->header != nullptr && tile->header->polyCount == static_cast<int>(polygonCount),
				"Detour's single tile holds every polygon of the payload");
			agent.Base = mesh.getPolyRefBase(tile);
			agent.PolygonCount = tile->header->polyCount;
			agent.Areas.reserve(polygonCount);
			agent.CumulativeAreas.reserve(polygonCount);
			double total = 0.0;
			for (int polygon = 0; polygon < agent.PolygonCount; ++polygon)
			{
				const double area = Utils::GetPolygonArea(*tile, tile->polys[polygon]);
				total += area;
				agent.Areas.push_back(area);
				agent.CumulativeAreas.push_back(total);
			}
			agent.Corridor.resize(MaxNavigationPathPolygons);
			return {};
		}

		// The point of `agent`'s surface nearest to `position` within `maxDistance` (NavigationWorld::FindClosestPoint), with
		// its polygon. The search box is clipped to the mesh's bounds before Detour sees it.
		[[nodiscard]] std::optional<Utils::NavigationSurfacePoint> FindNearest(const NavigationWorld::Agent& agent, const glm::vec3& position, float maxDistance)
		{
			if (agent.Query == nullptr)
				return std::nullopt;
			const glm::vec3 low = glm::max(position - glm::vec3(maxDistance), agent.BoundsMin);
			const glm::vec3 high = glm::min(position + glm::vec3(maxDistance), agent.BoundsMax);
			if (glm::any(glm::greaterThan(low, high)))
				return std::nullopt;
			const glm::vec3 center = (low + high) * 0.5f;
			const glm::vec3 halfExtents = (high - low) * 0.5f;
			Utils::NearestPolygonQuery nearest(*agent.Query, position);
			if (dtStatusFailed(agent.Query->queryPolygons(&center.x, &halfExtents.x, &agent.Filter, &nearest)))
				return std::nullopt;
			if (nearest.GetNearest().Ref == 0 || nearest.GetDistanceSquared() > maxDistance * maxDistance)
				return std::nullopt;
			return nearest.GetNearest();
		}

		// `polygon`'s point closest to `position`, on the detail surface (Detour's closestPointOnPoly); `position` itself
		// when Detour refuses the reference (never for the mesh's own polygons).
		[[nodiscard]] glm::vec3 ClosestPointOnPolygon(const NavigationWorld::Agent& agent, dtPolyRef polygon, const glm::vec3& position)
		{
			float closest[3] = {};
			if (dtStatusFailed(agent.Query->closestPointOnPoly(polygon, &position.x, closest, nullptr)))
				return position;
			return Utils::ToVector(closest);
		}

		// A uniform point of `polygon` (two draws: s, then t), lifted onto its detail surface.
		[[nodiscard]] glm::vec3 DrawPointInPolygon(const NavigationWorld::Agent& agent, dtPolyRef polygon, Random& random)
		{
			const dtMeshTile* tile = nullptr;
			const dtPoly* poly = nullptr;
			agent.Mesh->getTileAndPolyByRefUnsafe(polygon, &tile, &poly);
			float vertices[DT_VERTS_PER_POLYGON * 3] = {};
			float areas[DT_VERTS_PER_POLYGON] = {};
			const int count = poly->vertCount;
			for (int corner = 0; corner < count; ++corner)
				dtVcopy(&vertices[corner * 3], &tile->verts[poly->verts[corner] * 3]);
			const float s = random.NextFloat();
			const float t = random.NextFloat();
			float point[3] = {};
			dtRandomPointInConvexPoly(vertices, count, areas, s, t, point);
			return ClosestPointOnPolygon(agent, polygon, Utils::ToVector(point));
		}

		// The index of the entry of `cumulative` (non-decreasing, its last entry `total` > 0) that a draw of `random` picks:
		// the first whose running sum exceeds NextDouble() * total, so an entry that adds nothing is never picked.
		[[nodiscard]] size_t PickByArea(const std::vector<double>& cumulative, Random& random)
		{
			const double total = cumulative.back();
			const double target = random.NextDouble() * total;
			size_t index = static_cast<size_t>(std::upper_bound(cumulative.begin(), cumulative.end(), target) - cumulative.begin());
			if (index < cumulative.size())
				return index;
			// The product rounded up to the total: the last entry that adds area.
			index = cumulative.size() - 1;
			while (index > 0 && cumulative[index - 1] == cumulative[index])
				--index;
			return index;
		}

	}

	NavigationWorld::NavigationWorld(ConstructionKey /*key*/)
	{
	}

	NavigationWorld::~NavigationWorld() = default;

	Result<Scope<NavigationWorld>> NavigationWorld::Create(const NavMeshData& mesh)
	{
		ENGINE_CORE_ASSERT(IsNavigationRuntimeInitialized(), "NavigationWorld::Create needs the process context's Navigation step");
		ENGINE_TRY(WithContext(ValidateNavMeshData(mesh), "while building the navigation world"));
		Scope<NavigationWorld> world = CreateScope<NavigationWorld>(ConstructionKey{});
		world->m_Agents.reserve(mesh.Agents.size());
		for (const NavMeshAgent& source : mesh.Agents)
		{
			Scope<Agent> agent = CreateScope<Agent>();
			agent->Name = source.Name;
			agent->BoundsMin = source.BoundsMin;
			agent->BoundsMax = source.BoundsMax;
			for (size_t cost = 0; cost < mesh.AreaCosts.size(); ++cost)
				agent->Filter.setAreaCost(static_cast<int>(1 + cost), mesh.AreaCosts[cost]);
			if (source.GetPolygonCount() > 0)
				ENGINE_TRY(WithContext(BuildDetourMesh(source, *agent), "while building the navigation world"));
			world->m_Agents.push_back(std::move(agent));
		}
		return world;
	}

	size_t NavigationWorld::GetAgentCount() const
	{
		return m_Agents.size();
	}

	std::string_view NavigationWorld::GetAgentName(size_t agent) const
	{
		ENGINE_CORE_ASSERT(agent < m_Agents.size(), "Navigation agent index {} of {}", agent, m_Agents.size());
		return m_Agents[agent]->Name;
	}

	std::optional<size_t> NavigationWorld::FindAgent(std::string_view name) const
	{
		for (size_t index = 0; index < m_Agents.size(); ++index)
			if (m_Agents[index]->Name == name)
				return index;
		return std::nullopt;
	}

	std::optional<NavigationPath> NavigationWorld::FindPath(size_t agentIndex, const glm::vec3& from, const glm::vec3& to, size_t maxCorners)
	{
		ENGINE_CORE_ASSERT(agentIndex < m_Agents.size(), "Navigation agent index {} of {}", agentIndex, m_Agents.size());
		ENGINE_CORE_ASSERT(Utils::IsNavigationQueryPosition(from) && Utils::IsNavigationQueryPosition(to), "FindPath's ends are finite and in range");
		ENGINE_CORE_ASSERT(maxCorners >= 1, "FindPath returns at least one corner");
		Agent& agent = *m_Agents[agentIndex];
		const std::optional<Utils::NavigationSurfacePoint> start = FindNearest(agent, from, NavigationEndpointSearchDistance);
		if (!start.has_value())
			return std::nullopt;
		const std::optional<Utils::NavigationSurfacePoint> goal = FindNearest(agent, to, NavigationEndpointSearchDistance);
		if (!goal.has_value())
			return std::nullopt;

		int corridorSize = 0;
		const dtStatus searched = agent.Query->findPath(start->Ref, goal->Ref, &start->Point.x, &goal->Point.x, &agent.Filter, agent.Corridor.data(), &corridorSize,
			static_cast<int>(agent.Corridor.size()));
		if (dtStatusFailed(searched) || corridorSize <= 0)
			return std::nullopt;
		bool complete = agent.Corridor[static_cast<size_t>(corridorSize) - 1] == goal->Ref && !dtStatusDetail(searched, DT_PARTIAL_RESULT);
		// An unreachable goal (or a search cut by a cap): the corridor's last polygon is the closest to it reached.
		const glm::vec3 end = complete ? goal->Point : ClosestPointOnPolygon(agent, agent.Corridor[static_cast<size_t>(corridorSize) - 1], goal->Point);

		std::vector<float> corners(maxCorners * 3);
		int cornerCount = 0;
		const dtStatus straightened = agent.Query->findStraightPath(&start->Point.x, &end.x, agent.Corridor.data(), corridorSize, corners.data(), nullptr, nullptr,
			&cornerCount, static_cast<int>(maxCorners), 0);
		if (dtStatusFailed(straightened) || cornerCount <= 0)
			return std::nullopt;

		NavigationPath path;
		path.Corners.reserve(static_cast<size_t>(cornerCount));
		for (int corner = 0; corner < cornerCount; ++corner)
			path.Corners.push_back(Utils::ToVector(&corners[static_cast<size_t>(corner) * 3]));
		// Detour reports a full corner buffer even when the corner that filled it is the end (the review of W30.2), so the
		// path reaches its goal when its last corner is the goal's point, which Detour appends unchanged.
		path.Complete = complete && path.Corners.back() == end;
		return path;
	}

	std::optional<glm::vec3> NavigationWorld::FindClosestPoint(size_t agentIndex, const glm::vec3& position, float maxDistance)
	{
		ENGINE_CORE_ASSERT(agentIndex < m_Agents.size(), "Navigation agent index {} of {}", agentIndex, m_Agents.size());
		ENGINE_CORE_ASSERT(Utils::IsNavigationQueryPosition(position), "FindClosestPoint's position is finite and in range");
		ENGINE_CORE_ASSERT(maxDistance > 0.0f && maxDistance <= MaxNavigationSearchDistance, "FindClosestPoint's distance is in range");
		const std::optional<Utils::NavigationSurfacePoint> nearest = FindNearest(*m_Agents[agentIndex], position, maxDistance);
		if (!nearest.has_value())
			return std::nullopt;
		return nearest->Point;
	}

	std::optional<NavigationRaycast> NavigationWorld::Raycast(size_t agentIndex, const glm::vec3& from, const glm::vec3& to)
	{
		ENGINE_CORE_ASSERT(agentIndex < m_Agents.size(), "Navigation agent index {} of {}", agentIndex, m_Agents.size());
		ENGINE_CORE_ASSERT(Utils::IsNavigationQueryPosition(from) && Utils::IsNavigationQueryPosition(to), "Raycast's ends are finite and in range");
		Agent& agent = *m_Agents[agentIndex];
		const std::optional<Utils::NavigationSurfacePoint> start = FindNearest(agent, from, NavigationEndpointSearchDistance);
		if (!start.has_value())
			return std::nullopt;

		// Detour's raycast walk (dtNavMeshQuery::raycast without costs), bounded: a line crosses a convex polygon once, so a
		// walk that would visit more polygons than the mesh holds is a cycle in crafted data, ended as a blocked walk.
		const glm::vec3 origin = start->Point;
		dtPolyRef current = start->Ref;
		float farthest = 0.0f;
		NavigationRaycast result;
		for (int visited = 1;; ++visited)
		{
			const dtMeshTile* tile = nullptr;
			const dtPoly* poly = nullptr;
			agent.Mesh->getTileAndPolyByRefUnsafe(current, &tile, &poly);
			float vertices[DT_VERTS_PER_POLYGON * 3] = {};
			const int count = poly->vertCount;
			for (int corner = 0; corner < count; ++corner)
				dtVcopy(&vertices[corner * 3], &tile->verts[poly->verts[corner] * 3]);
			float entering = 0.0f;
			float leaving = 0.0f;
			int enteringEdge = -1;
			int leavingEdge = -1;
			if (!dtIntersectSegmentPoly2D(&origin.x, &to.x, vertices, count, entering, leaving, enteringEdge, leavingEdge))
				break; // the walk cannot enter the polygon: it stops where it is
			farthest = std::max(farthest, leaving);
			if (leavingEdge == -1)
			{
				// The target lies inside the polygon.
				result.Clear = true;
				result.Fraction = 1.0f;
				result.Point = ClosestPointOnPolygon(agent, current, to);
				return result;
			}
			dtPolyRef next = 0;
			for (unsigned int link = poly->firstLink; link != DT_NULL_LINK; link = tile->links[link].next)
			{
				const dtLink& candidate = tile->links[link];
				if (static_cast<int>(candidate.edge) != leavingEdge)
					continue;
				const dtMeshTile* nextTile = nullptr;
				const dtPoly* nextPoly = nullptr;
				agent.Mesh->getTileAndPolyByRefUnsafe(candidate.ref, &nextTile, &nextPoly);
				if (Utils::PassesFilter(agent.Filter, *nextPoly))
				{
					next = candidate.ref;
					break;
				}
			}
			if (next == 0)
			{
				// A wall: the leaving edge's horizontal normal, facing the walk.
				const float* a = &vertices[leavingEdge * 3];
				const float* b = &vertices[((leavingEdge + 1) % count) * 3];
				const float dx = b[0] - a[0];
				const float dz = b[2] - a[2];
				const float length = std::sqrt(dx * dx + dz * dz);
				if (length > 0.0f)
					result.Normal = glm::vec3(dz / length, 0.0f, -dx / length);
				break;
			}
			if (visited >= agent.PolygonCount)
				break;
			current = next;
		}
		result.Fraction = std::clamp(farthest, 0.0f, 1.0f);
		result.Point = ClosestPointOnPolygon(agent, current, origin + (to - origin) * result.Fraction);
		return result;
	}

	std::optional<glm::vec3> NavigationWorld::FindRandomPoint(size_t agentIndex, Random& random)
	{
		ENGINE_CORE_ASSERT(agentIndex < m_Agents.size(), "Navigation agent index {} of {}", agentIndex, m_Agents.size());
		const Agent& agent = *m_Agents[agentIndex];
		if (agent.Mesh == nullptr || agent.CumulativeAreas.back() <= 0.0)
			return std::nullopt;
		const size_t polygon = PickByArea(agent.CumulativeAreas, random);
		return DrawPointInPolygon(agent, agent.Base | static_cast<dtPolyRef>(polygon), random);
	}

	std::optional<glm::vec3> NavigationWorld::FindRandomPointAround(size_t agentIndex, const glm::vec3& center, float radius, Random& random)
	{
		ENGINE_CORE_ASSERT(agentIndex < m_Agents.size(), "Navigation agent index {} of {}", agentIndex, m_Agents.size());
		ENGINE_CORE_ASSERT(Utils::IsNavigationQueryPosition(center), "FindRandomPointAround's center is finite and in range");
		ENGINE_CORE_ASSERT(radius > 0.0f && radius <= MaxNavigationSearchDistance, "FindRandomPointAround's radius is in range");
		const Agent& agent = *m_Agents[agentIndex];
		const std::optional<Utils::NavigationSurfacePoint> start = FindNearest(agent, center, radius);
		if (!start.has_value())
			return std::nullopt;

		// The polygons reached from the start's across an edge within the radius, breadth first in Detour's link order.
		const float radiusSquared = radius * radius;
		std::vector<uint8_t> reachedFlags(static_cast<size_t>(agent.PolygonCount), 0);
		std::vector<dtPolyRef> reached{ start->Ref };
		reachedFlags[agent.Mesh->decodePolyIdPoly(start->Ref)] = 1;
		for (size_t next = 0; next < reached.size(); ++next)
		{
			const dtMeshTile* tile = nullptr;
			const dtPoly* poly = nullptr;
			agent.Mesh->getTileAndPolyByRefUnsafe(reached[next], &tile, &poly);
			for (unsigned int link = poly->firstLink; link != DT_NULL_LINK; link = tile->links[link].next)
			{
				const dtLink& candidate = tile->links[link];
				const unsigned int neighbour = agent.Mesh->decodePolyIdPoly(candidate.ref);
				if (candidate.ref == 0 || neighbour >= reachedFlags.size() || reachedFlags[neighbour] != 0)
					continue;
				const float* a = &tile->verts[poly->verts[candidate.edge] * 3];
				const float* b = &tile->verts[poly->verts[(candidate.edge + 1) % poly->vertCount] * 3];
				float along = 0.0f;
				if (dtDistancePtSegSqr2D(&center.x, a, b, along) > radiusSquared)
					continue;
				const dtMeshTile* neighbourTile = nullptr;
				const dtPoly* neighbourPoly = nullptr;
				agent.Mesh->getTileAndPolyByRefUnsafe(candidate.ref, &neighbourTile, &neighbourPoly);
				if (!Utils::PassesFilter(agent.Filter, *neighbourPoly))
					continue;
				reachedFlags[neighbour] = 1;
				reached.push_back(candidate.ref);
			}
		}

		std::vector<double> cumulative;
		cumulative.reserve(reached.size());
		double total = 0.0;
		for (const dtPolyRef polygon : reached)
		{
			total += agent.Areas[agent.Mesh->decodePolyIdPoly(polygon)];
			cumulative.push_back(total);
		}
		if (total <= 0.0)
			return std::nullopt;
		dtPolyRef drawn = 0;
		for (uint32_t attempt = 0; attempt < MaxNavigationRandomPointAttempts; ++attempt)
		{
			drawn = reached[PickByArea(cumulative, random)];
			const glm::vec3 point = DrawPointInPolygon(agent, drawn, random);
			const float dx = point.x - center.x;
			const float dz = point.z - center.z;
			if (dx * dx + dz * dz <= radiusSquared)
				return point;
		}
		// Every drawn point fell outside the circle: the last polygon came within the radius, so its point closest to the
		// center does too.
		return ClosestPointOnPolygon(agent, drawn, center);
	}

}
