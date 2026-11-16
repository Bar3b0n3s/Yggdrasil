#include "EditorPCH.h"
#include "EditorCore/Navigation/NavMeshBuilder.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Scene/NavigationBakeInput.h"

#include <Recast.h>
#include <RecastAlloc.h>
#include <RecastAssert.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The navigation bake's builder (NavMeshBuilder.h; Docs/Decisions/0045-m30-navigation.md).

namespace Engine {

	// Process-level state (Architecture §3 rule 5): installed and removed by the ProcessContext's Navigation step on the main
	// thread; the counter is changed by every thread that allocates through Recast.
	static std::atomic<bool> s_AreNavMeshBuildHooksInstalled{ false };
	static std::atomic<size_t> s_LiveNavMeshBuildAllocations{ 0 };

	namespace Utils {

		// rcAllocFunc: the C runtime's heap, as Recast's default, counted.
		static void* AllocateForRecast(size_t size, rcAllocHint /*hint*/)
		{
			void* memory = std::malloc(size);
			if (memory != nullptr)
				s_LiveNavMeshBuildAllocations.fetch_add(1, std::memory_order_relaxed);
			return memory;
		}

		// rcFreeFunc: frees what AllocateForRecast returned (Recast never frees null, but a null pointer is ignored).
		static void FreeForRecast(void* memory)
		{
			if (memory == nullptr)
				return;
			s_LiveNavMeshBuildAllocations.fetch_sub(1, std::memory_order_relaxed);
			std::free(memory);
		}

		// A Recast context that keeps the error lines Recast logs (its progress lines and timers are not needed), so a failed
		// step's error quotes Recast's reason.
		class NavMeshBuildContext final : public rcContext
		{
		public:
			NavMeshBuildContext()
				: rcContext(true)
			{
				enableTimer(false);
			}

			[[nodiscard]] const std::string& GetErrors() const { return m_Errors; }
		protected:
			void doResetLog() override { m_Errors.clear(); }
			void doLog(const rcLogCategory category, const char* message, const int length) override
			{
				if (category != RC_LOG_ERROR || message == nullptr || length <= 0)
					return;
				if (!m_Errors.empty())
					m_Errors += "; ";
				m_Errors.append(message, static_cast<size_t>(length));
			}
		private:
			std::string m_Errors;
		};

		// Recast's working objects, each freed by its own function on every path.
		struct HeightfieldDeleter
		{
			void operator()(rcHeightfield* heightfield) const { rcFreeHeightField(heightfield); }
		};
		struct CompactHeightfieldDeleter
		{
			void operator()(rcCompactHeightfield* heightfield) const { rcFreeCompactHeightfield(heightfield); }
		};
		struct ContourSetDeleter
		{
			void operator()(rcContourSet* contours) const { rcFreeContourSet(contours); }
		};
		struct PolyMeshDeleter
		{
			void operator()(rcPolyMesh* mesh) const { rcFreePolyMesh(mesh); }
		};
		struct PolyMeshDetailDeleter
		{
			void operator()(rcPolyMeshDetail* mesh) const { rcFreePolyMeshDetail(mesh); }
		};

		// The input's geometry as Recast reads it, and the area ids of its Cost modifiers.
		struct BuildGeometry
		{
			std::vector<float> Vertices{}; // x, y, z per vertex
			std::vector<int> Triangles{};  // 3 per triangle
			std::vector<glm::vec3> Normals{};
			float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
			float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
			std::map<float, uint8_t> CostAreas{}; // a cost's area id (1 + its index among the sorted distinct costs)
		};

		[[nodiscard]] static Error MakeStepError(std::string_view step, std::string_view agent, const NavMeshBuildContext& context)
		{
			const std::string reason = context.GetErrors().empty() ? std::string("Recast gave no reason") : context.GetErrors();
			return Error(ErrorCode::Validation, std::format("baking the agent type '{}': Recast's {} failed: {}", agent, step, reason));
		}

		[[nodiscard]] static Error MakeAllocationError(std::string_view what, std::string_view agent)
		{
			return Error(ErrorCode::OutOfMemory, std::format("baking the agent type '{}': Recast could not allocate its {}", agent, what));
		}

		// The marked box of `modifier` as rcMarkConvexPolyArea reads it: its four corners at its bottom.
		static void MarkModifier(rcContext& context, const NavBakeModifier& modifier, unsigned char area, rcCompactHeightfield& heightfield)
		{
			float corners[12] = {};
			for (size_t corner = 0; corner < modifier.Corners.size(); ++corner)
			{
				corners[corner * 3 + 0] = modifier.Corners[corner].x;
				corners[corner * 3 + 1] = modifier.MinY;
				corners[corner * 3 + 2] = modifier.Corners[corner].y;
			}
			rcMarkConvexPolyArea(&context, corners, 4, modifier.MinY, modifier.MaxY, area, heightfield);
		}

		// One agent type's mesh (the file comment's steps 1 to 4).
		[[nodiscard]] static Result<NavMeshAgent> BuildAgent(const NavigationBakeInput& input, const BuildGeometry& geometry, const NavAgentType& type)
		{
			const NavigationComponent& settings = input.Settings;
			NavMeshAgent agent;
			agent.Name = type.Name;
			agent.Radius = type.Radius;
			agent.Height = type.Height;
			agent.MaxSlope = type.MaxSlope;
			agent.MaxStep = type.MaxStep;
			agent.CellSize = settings.CellSize;
			agent.CellHeight = settings.CellHeight;
			agent.VerticesPerPolygon = NavMeshMaxVerticesPerPolygon;
			if (geometry.Triangles.empty())
				return agent;

			const float cellSize = settings.CellSize;
			const float cellHeight = settings.CellHeight;
			const int walkableHeight = static_cast<int>(std::ceil(type.Height / cellHeight));
			const int walkableClimb = static_cast<int>(std::floor(type.MaxStep / cellHeight));
			const int walkableRadius = static_cast<int>(std::ceil(type.Radius / cellSize));
			int width = 0;
			int height = 0;
			rcCalcGridSize(geometry.BoundsMin, geometry.BoundsMax, cellSize, &width, &height);

			// 1. The walkable triangles: cos(MaxSlope) through DetMath, never Recast's cosf.
			const float walkableThreshold = DetMath::Cos(type.MaxSlope / 180.0f * glm::pi<float>());
			const size_t triangleCount = geometry.Triangles.size() / 3;
			std::vector<unsigned char> areas(triangleCount, RC_NULL_AREA);
			for (size_t triangle = 0; triangle < triangleCount; ++triangle)
			{
				if (geometry.Normals[triangle].y > walkableThreshold)
					areas[triangle] = RC_WALKABLE_AREA;
			}

			// 2. The voxels.
			NavMeshBuildContext context;
			const std::unique_ptr<rcHeightfield, HeightfieldDeleter> solid(rcAllocHeightfield());
			if (solid == nullptr)
				return std::unexpected(MakeAllocationError("heightfield", type.Name));
			if (!rcCreateHeightfield(&context, *solid, width, height, geometry.BoundsMin, geometry.BoundsMax, cellSize, cellHeight))
				return std::unexpected(MakeStepError("rcCreateHeightfield", type.Name, context));
			if (!rcRasterizeTriangles(&context, geometry.Vertices.data(), static_cast<int>(geometry.Vertices.size() / 3), geometry.Triangles.data(),
					areas.data(), static_cast<int>(triangleCount), *solid, walkableClimb))
			{
				return std::unexpected(MakeStepError("rcRasterizeTriangles", type.Name, context));
			}
			rcFilterLowHangingWalkableObstacles(&context, walkableClimb, *solid);
			rcFilterLedgeSpans(&context, walkableHeight, walkableClimb, *solid);
			rcFilterWalkableLowHeightSpans(&context, walkableHeight, *solid);
			const std::unique_ptr<rcCompactHeightfield, CompactHeightfieldDeleter> compact(rcAllocCompactHeightfield());
			if (compact == nullptr)
				return std::unexpected(MakeAllocationError("compact heightfield", type.Name));
			if (!rcBuildCompactHeightfield(&context, walkableHeight, walkableClimb, *solid, *compact))
				return std::unexpected(MakeStepError("rcBuildCompactHeightfield", type.Name, context));

			// 3. The modifiers: holes before the erosion, costs after it, the highest cost last.
			for (const NavBakeModifier& modifier : input.Modifiers)
			{
				if (modifier.Kind == NavModifierKind::NotWalkable)
					MarkModifier(context, modifier, RC_NULL_AREA, *compact);
			}
			if (!rcErodeWalkableArea(&context, walkableRadius, *compact))
				return std::unexpected(MakeStepError("rcErodeWalkableArea", type.Name, context));
			std::vector<const NavBakeModifier*> costly;
			for (const NavBakeModifier& modifier : input.Modifiers)
			{
				if (modifier.Kind == NavModifierKind::Cost)
					costly.push_back(&modifier);
			}
			std::stable_sort(costly.begin(), costly.end(), [](const NavBakeModifier* a, const NavBakeModifier* b)
			{
				return a->Cost < b->Cost;
			});
			for (const NavBakeModifier* modifier : costly)
				MarkModifier(context, *modifier, geometry.CostAreas.at(modifier->Cost), *compact);

			// 4. The regions, contours, polygons and detail mesh.
			if (!rcBuildDistanceField(&context, *compact))
				return std::unexpected(MakeStepError("rcBuildDistanceField", type.Name, context));
			const int minRegionCells = static_cast<int>(settings.MinRegionArea / (cellSize * cellSize));
			if (!rcBuildRegions(&context, *compact, 0, minRegionCells, NavBakeMergeRegionCells))
				return std::unexpected(MakeStepError("rcBuildRegions", type.Name, context));
			const std::unique_ptr<rcContourSet, ContourSetDeleter> contours(rcAllocContourSet());
			if (contours == nullptr)
				return std::unexpected(MakeAllocationError("contour set", type.Name));
			if (!rcBuildContours(&context, *compact, NavBakeMaxEdgeError, static_cast<int>(NavBakeMaxEdgeLength / cellSize), *contours))
				return std::unexpected(MakeStepError("rcBuildContours", type.Name, context));
			const std::unique_ptr<rcPolyMesh, PolyMeshDeleter> polygons(rcAllocPolyMesh());
			if (polygons == nullptr)
				return std::unexpected(MakeAllocationError("polygon mesh", type.Name));
			if (!rcBuildPolyMesh(&context, *contours, static_cast<int>(NavMeshMaxVerticesPerPolygon), *polygons))
				return std::unexpected(MakeStepError("rcBuildPolyMesh", type.Name, context));
			const std::unique_ptr<rcPolyMeshDetail, PolyMeshDetailDeleter> detail(rcAllocPolyMeshDetail());
			if (detail == nullptr)
				return std::unexpected(MakeAllocationError("detail mesh", type.Name));
			if (!rcBuildPolyMeshDetail(&context, *polygons, *compact, cellSize * NavBakeDetailSampleCells, cellHeight * NavBakeDetailMaxErrorCells, *detail))
				return std::unexpected(MakeStepError("rcBuildPolyMeshDetail", type.Name, context));

			const rcPolyMesh& mesh = *polygons;
			agent.CellSize = mesh.cs;
			agent.CellHeight = mesh.ch;
			agent.BoundsMin = glm::vec3(mesh.bmin[0], mesh.bmin[1], mesh.bmin[2]);
			agent.BoundsMax = glm::vec3(mesh.bmax[0], mesh.bmax[1], mesh.bmax[2]);
			agent.VerticesPerPolygon = static_cast<uint32_t>(mesh.nvp);
			agent.Vertices.assign(mesh.verts, mesh.verts + static_cast<size_t>(mesh.nverts) * 3);
			agent.Polygons.assign(mesh.polys, mesh.polys + static_cast<size_t>(mesh.npolys) * 2 * static_cast<size_t>(mesh.nvp));
			agent.Areas.assign(mesh.areas, mesh.areas + mesh.npolys);
			const rcPolyMeshDetail& details = *detail;
			agent.DetailMeshes.assign(details.meshes, details.meshes + static_cast<size_t>(details.nmeshes) * 4);
			agent.DetailVertices.assign(details.verts, details.verts + static_cast<size_t>(details.nverts) * 3);
			agent.DetailTriangles.assign(details.tris, details.tris + static_cast<size_t>(details.ntris) * 4);
			return agent;
		}

		// rcAssertFailFunc: a failed Recast assert is a programmer error, reported to the engine's assert handler (which ends
		// the process) with Recast's file and line.
		static void OnRecastAssertFailed(const char* expression, const char* file, int line)
		{
			Detail::ReportAssertFailure(AssertKind::Assert, false, expression != nullptr ? expression : "",
				std::format("Recast: assertion failed ({}:{})", file != nullptr ? file : "(unknown file)", line), std::source_location::current());
		}

	}

	Result<NavMeshData> BuildNavMesh(const NavigationBakeInput& input)
	{
		const size_t triangleCount = input.Indices.size() / 3;
		if (triangleCount > MaxNavBakeTriangles || input.Vertices.size() > MaxNavBakeTriangles * 3)
		{
			return MakeError(ErrorCode::Validation, "the scene's static colliders make {} triangles of {} vertices; a bake reads at most {} triangles", triangleCount,
				input.Vertices.size(), MaxNavBakeTriangles);
		}

		Utils::BuildGeometry geometry;
		geometry.Vertices.reserve(input.Vertices.size() * 3);
		for (const glm::vec3& vertex : input.Vertices)
			geometry.Vertices.insert(geometry.Vertices.end(), { vertex.x, vertex.y, vertex.z });
		geometry.Triangles.reserve(triangleCount * 3);
		geometry.Normals.reserve(triangleCount);
		for (size_t triangle = 0; triangle < triangleCount; ++triangle)
		{
			const uint32_t a = input.Indices[triangle * 3];
			const uint32_t b = input.Indices[triangle * 3 + 1];
			const uint32_t c = input.Indices[triangle * 3 + 2];
			ENGINE_CORE_ASSERT(a < input.Vertices.size() && b < input.Vertices.size() && c < input.Vertices.size(),
				"The bake input's triangle {} names a vertex past its {}", triangle, input.Vertices.size());
			geometry.Triangles.insert(geometry.Triangles.end(), { static_cast<int>(a), static_cast<int>(b), static_cast<int>(c) });
			// Recast's calcTriNormal: cross(b - a, c - a), normalized (a degenerate triangle's normal is zero, never walkable).
			const glm::vec3 normal = glm::cross(input.Vertices[b] - input.Vertices[a], input.Vertices[c] - input.Vertices[a]);
			const float length = std::sqrt(glm::dot(normal, normal));
			geometry.Normals.push_back(length > 0.0f ? normal / length : glm::vec3(0.0f));
		}
		if (!input.Vertices.empty())
			rcCalcBounds(geometry.Vertices.data(), static_cast<int>(input.Vertices.size()), geometry.BoundsMin, geometry.BoundsMax);

		NavMeshData mesh;
		mesh.InputHash = HashNavigationBakeInput(input);
		for (const NavBakeModifier& modifier : input.Modifiers)
		{
			if (modifier.Kind == NavModifierKind::Cost)
				geometry.CostAreas.emplace(modifier.Cost, uint8_t{ 0 });
		}
		if (geometry.CostAreas.size() > MaxNavMeshAreaCosts)
		{
			return MakeError(ErrorCode::Validation, "the scene's NavModifiers have {} distinct costs; a navigation mesh holds at most {}", geometry.CostAreas.size(),
				MaxNavMeshAreaCosts);
		}
		for (auto& [cost, area] : geometry.CostAreas)
		{
			mesh.AreaCosts.push_back(cost);
			area = static_cast<uint8_t>(mesh.AreaCosts.size());
		}

		if (!geometry.Triangles.empty())
		{
			int width = 0;
			int height = 0;
			rcCalcGridSize(geometry.BoundsMin, geometry.BoundsMax, input.Settings.CellSize, &width, &height);
			if (width > MaxNavBakeGridCells || height > MaxNavBakeGridCells)
			{
				return std::unexpected(Error(ErrorCode::Validation,
					std::format("the scene's static colliders span {} x {} cells of {} metres; a bake voxelizes at most {} along X and Z", width, height,
						input.Settings.CellSize, MaxNavBakeGridCells))
						.WithHint("raise the Navigation component's CellSize, or leave far geometry out of its Layers"));
			}
		}

		for (const NavAgentType& type : input.Settings.AgentTypes)
		{
			ENGINE_TRY_ASSIGN(NavMeshAgent agent, Utils::BuildAgent(input, geometry, type));
			mesh.Agents.push_back(std::move(agent));
		}
		const Status valid = ValidateNavMeshData(mesh);
		if (!valid.has_value())
		{
			return std::unexpected(Error(valid.error())
					.WithContext("the baked navigation mesh is outside the NavMesh format's limits")
					.WithHint("raise the Navigation component's CellSize, or split the scene's walkable area"));
		}
		return mesh;
	}

	void InstallNavMeshBuildHooks()
	{
		ENGINE_CORE_ASSERT(!s_AreNavMeshBuildHooksInstalled.load(), "InstallNavMeshBuildHooks called twice without RemoveNavMeshBuildHooks");
		rcAllocSetCustom(&Utils::AllocateForRecast, &Utils::FreeForRecast);
		rcAssertFailSetCustom(&Utils::OnRecastAssertFailed);
		s_AreNavMeshBuildHooksInstalled.store(true);
	}

	void RemoveNavMeshBuildHooks()
	{
		if (!s_AreNavMeshBuildHooksInstalled.load())
			return;
		const size_t live = s_LiveNavMeshBuildAllocations.load();
		ENGINE_CORE_VERIFY(live == 0, "RemoveNavMeshBuildHooks with {} live Recast allocation(s): a navigation bake leaked its working data", live);
		rcAssertFailSetCustom(nullptr);
		// Null restores Recast's default allocator (RecastAlloc.cpp).
		rcAllocSetCustom(nullptr, nullptr);
		s_AreNavMeshBuildHooksInstalled.store(false);
	}

	bool AreNavMeshBuildHooksInstalled()
	{
		return s_AreNavMeshBuildHooksInstalled.load();
	}

	size_t GetLiveNavMeshBuildAllocationCount()
	{
		return s_LiveNavMeshBuildAllocations.load();
	}

}
