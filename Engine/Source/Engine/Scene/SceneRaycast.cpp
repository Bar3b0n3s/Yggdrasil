#include "EnginePCH.h"
#include "Engine/Scene/SceneRaycast.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TerrainColliderComponent.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace Engine {

	namespace Utils {

		static bool IsFiniteRayVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		static bool IsFiniteRayVector(const glm::dvec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		static uint32_t GetVisualRayLayer(ConstEntity entity, const PhysicsLayerTable& layers)
		{
			for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
				if (current.IsActive())
					if (const auto* body = current.TryGetComponent<RigidBodyComponent>())
						return layers.FindLayer(body->Layer).value_or(0);
			if (const auto* character = entity.TryGetComponent<CharacterControllerComponent>())
				return layers.FindLayer(character->Layer).value_or(0);
			// M26 (Docs/Decisions/0037-m26-contract.md decision 21): without a RigidBody at or above it, a terrain's own
			// TerrainCollider gives the implicit static body it holds its layer.
			if (const auto* collider = entity.TryGetComponent<TerrainColliderComponent>(); collider != nullptr && entity.HasComponent<TerrainComponent>())
				return layers.FindLayer(collider->Layer).value_or(0);
			return 0;
		}

		static bool IntersectsRayBounds(const Aabb& bounds, const glm::dmat4& transform, const glm::dvec3& origin, const glm::dvec3& direction, double minimum, double maximum)
		{
			if (bounds.IsEmpty() || !IsFiniteRayVector(bounds.Min) || !IsFiniteRayVector(bounds.Max))
				return false;
			// Use the same precision as the narrow phase. A float-transformed box can round inward and reject a ray on a
			// triangle boundary before the double-precision intersection gets a chance to test it.
			glm::dvec3 lower(std::numeric_limits<double>::infinity());
			glm::dvec3 upper(-std::numeric_limits<double>::infinity());
			for (const glm::vec3& corner : bounds.GetCorners())
			{
				const glm::dvec3 point(transform * glm::dvec4(corner, 1));
				if (!IsFiniteRayVector(glm::vec3(point)))
					return false;
				lower = glm::min(lower, point);
				upper = glm::max(upper, point);
			}
			for (int axis = 0; axis < 3; ++axis)
			{
				if (direction[axis] == 0)
				{
					if (origin[axis] < lower[axis] || origin[axis] > upper[axis])
						return false;
					continue;
				}
				const double first = (lower[axis] - origin[axis]) / direction[axis];
				const double second = (upper[axis] - origin[axis]) / direction[axis];
				minimum = std::max(minimum, std::min(first, second));
				maximum = std::min(maximum, std::max(first, second));
				if (minimum > maximum)
					return false;
			}
			return true;
		}

		// The ray's two-sided intersection with the world triangle (a, b, c) at a distance in [minimum, maximum]: the
		// distance, the barycentric weights u and v of b and c, and the edges b - a and c - a (Moller-Trumbore, in doubles).
		struct RayTriangleHit
		{
			double Distance = 0.0;
			double U = 0.0;
			double V = 0.0;
			glm::dvec3 Edge1{ 0.0 };
			glm::dvec3 Edge2{ 0.0 };
		};

		static std::optional<RayTriangleHit> IntersectRayTriangle(const glm::dvec3& origin, const glm::dvec3& direction, const glm::dvec3& a,
			const glm::dvec3& b, const glm::dvec3& c, double minimum, double maximum)
		{
			const glm::dvec3 edge1 = b - a;
			const glm::dvec3 edge2 = c - a;
			const glm::dvec3 p = glm::cross(direction, edge2);
			const double divisor = glm::dot(edge1, p);
			if (divisor == 0)
				return std::nullopt;
			const glm::dvec3 displacement = origin - a;
			const double u = glm::dot(displacement, p) / divisor;
			const glm::dvec3 q = glm::cross(displacement, edge1);
			const double v = glm::dot(direction, q) / divisor;
			const double distance = glm::dot(edge2, q) / divisor;
			if (!(u >= 0 && v >= 0 && u + v <= 1 && distance >= minimum && distance <= maximum))
				return std::nullopt;
			return RayTriangleHit{ .Distance = distance, .U = u, .V = v, .Edge1 = edge1, .Edge2 = edge2 };
		}

		// Keeps `candidate`, a hit no farther than the nearest so far, as the nearest unless it ties with it and comes after it
		// by (entity, submesh, triangle); its normal is the triangle's geometric one, turned against the ray.
		static void KeepNearest(std::optional<SceneRaycastHit>& hit, double& bestDistance, UUID id, uint32_t submesh, uint32_t triangle,
			const RayTriangleHit& candidate, const glm::dvec3& origin, const glm::dvec3& direction)
		{
			if (hit && candidate.Distance == bestDistance && std::tuple(id, submesh, triangle) >= std::tuple(hit->Entity, hit->Submesh, hit->Triangle))
				return;
			glm::dvec3 normal = glm::normalize(glm::cross(candidate.Edge1, candidate.Edge2));
			if (glm::dot(normal, direction) > 0)
				normal = -normal;
			bestDistance = candidate.Distance;
			hit = SceneRaycastHit{ id, static_cast<float>(candidate.Distance), glm::vec3(origin + candidate.Distance * direction), glm::vec3(normal),
				glm::vec3(1 - candidate.U - candidate.V, candidate.U, candidate.V), submesh, triangle };
		}

		// The cell, of `cells` cells of side `cell` along one axis, holding the local coordinate `local`, clamped to the
		// terrain's cells.
		static uint32_t FindRayCell(double local, double cell, uint32_t cells)
		{
			const double index = std::floor(local / cell);
			if (!(index > 0.0))
				return 0;
			return index >= static_cast<double>(cells - 1) ? cells - 1 : static_cast<uint32_t>(index);
		}

		// M26 (W26.2's review; Docs/Decisions/0037-m26-contract.md decisions 21 and 22): the ray against the surface of the
		// Terrain `terrain` of entity `id` at the rendered world matrix `transform`: the triangles Asset/TerrainGeometry.h lays
		// out (the renderer's and the collider's), each cell's two from GetTerrainSamplePosition carried into world space and
		// intersected as a mesh's are, submesh 0 and triangle 2 * (z * (Resolution - 1) + x) for cell (x, z)'s (x, z + 1)
		// triangle, one more for its (x + 1, z) triangle. Only the cells under the ray's stretch over the terrain's square are
		// tested: the rows its local Z crosses, and in each row the columns its local X crosses there, each range widened by a
		// cell, so that no rounding skips a cell the ray touches. A terrain whose heightfield does not load, or whose extent is
		// not positive and finite, has no surface (it draws nothing).
		static void RaycastTerrain(UUID id, const TerrainComponent& terrain, AssetManager& assets, const glm::dmat4& transform, const glm::dvec3& origin,
			const glm::dvec3& direction, double minimum, double& bestDistance, std::optional<SceneRaycastHit>& hit)
		{
			const TerrainExtent extent{ .Size = terrain.Size, .HeightScale = terrain.HeightScale };
			if (!(extent.Size.x > 0.0f) || !(extent.Size.y > 0.0f) || !(extent.HeightScale > 0.0f) || !std::isfinite(extent.Size.x)
				|| !std::isfinite(extent.Size.y) || !std::isfinite(extent.HeightScale))
				return;
			const Aabb box{ .Min = glm::vec3(0.0f), .Max = glm::vec3(extent.Size.x, extent.HeightScale, extent.Size.y) };
			if (!IntersectsRayBounds(box, transform, origin, direction, minimum, bestDistance))
				return;
			const Result<AssetRef<Asset>> loaded = assets.Load(terrain.Heightfield.GetHandle());
			const AssetRef<HeightfieldData> heightfield = loaded.has_value() ? AssetCast<HeightfieldData>(*loaded) : nullptr;
			if (heightfield == nullptr || !ValidateHeightfieldData(*heightfield).has_value())
				return;

			// The ray in the terrain's local space; its parameter stays the world distance, the transform being affine.
			const glm::dmat4 toLocal = glm::inverse(transform);
			const glm::dvec3 localOrigin(toLocal * glm::dvec4(origin, 1.0));
			const glm::dvec3 localDirection(toLocal * glm::dvec4(direction, 0.0));
			if (!IsFiniteRayVector(localOrigin) || !IsFiniteRayVector(localDirection))
				return;
			// The ray's stretch over the square [0, Size.x] x [0, Size.y].
			const glm::dvec2 size(extent.Size);
			double enter = minimum;
			double leave = bestDistance;
			for (const glm::length_t axis : { glm::length_t(0), glm::length_t(2) })
			{
				const double side = axis == 0 ? size.x : size.y;
				if (localDirection[axis] == 0.0)
				{
					if (localOrigin[axis] < 0.0 || localOrigin[axis] > side)
						return;
					continue;
				}
				const double first = (0.0 - localOrigin[axis]) / localDirection[axis];
				const double second = (side - localOrigin[axis]) / localDirection[axis];
				enter = std::max(enter, std::min(first, second));
				leave = std::min(leave, std::max(first, second));
				if (enter > leave)
					return;
			}

			const uint32_t cells = heightfield->Resolution - 1;
			const double cellX = size.x / static_cast<double>(cells);
			const double cellZ = size.y / static_cast<double>(cells);
			const double startZ = localOrigin.z + enter * localDirection.z;
			const double endZ = localOrigin.z + leave * localDirection.z;
			const uint32_t firstRow = FindRayCell(std::min(startZ, endZ) - cellZ, cellZ, cells);
			const uint32_t lastRow = FindRayCell(std::max(startZ, endZ) + cellZ, cellZ, cells);
			const auto worldSample = [&heightfield, &extent, &transform](uint32_t x, uint32_t z)
			{
				return glm::dvec3(transform * glm::dvec4(GetTerrainSamplePosition(*heightfield, extent, x, z), 1.0));
			};
			for (uint32_t row = firstRow; row <= lastRow; ++row)
			{
				// The ray's stretch over the row, widened by half a cell on each side.
				double rowEnter = enter;
				double rowLeave = leave;
				if (localDirection.z != 0.0)
				{
					const double first = ((static_cast<double>(row) - 0.5) * cellZ - localOrigin.z) / localDirection.z;
					const double second = ((static_cast<double>(row) + 1.5) * cellZ - localOrigin.z) / localDirection.z;
					rowEnter = std::max(rowEnter, std::min(first, second));
					rowLeave = std::min(rowLeave, std::max(first, second));
					if (rowEnter > rowLeave)
						continue;
				}
				const double startX = localOrigin.x + rowEnter * localDirection.x;
				const double endX = localOrigin.x + rowLeave * localDirection.x;
				const uint32_t firstColumn = FindRayCell(std::min(startX, endX) - cellX, cellX, cells);
				const uint32_t lastColumn = FindRayCell(std::max(startX, endX) + cellX, cellX, cells);
				for (uint32_t column = firstColumn; column <= lastColumn; ++column)
				{
					// The cell's two triangles, split along its (x, z) to (x + 1, z + 1) diagonal (TerrainGeometry.h).
					const glm::dvec3 corner = worldSample(column, row);
					const glm::dvec3 diagonal = worldSample(column + 1, row + 1);
					const std::array<glm::dvec3, 2> sides = { worldSample(column, row + 1), worldSample(column + 1, row) };
					const uint32_t first = 2 * (row * cells + column);
					if (const std::optional<RayTriangleHit> candidate = IntersectRayTriangle(origin, direction, corner, sides[0], diagonal, minimum, bestDistance))
						KeepNearest(hit, bestDistance, id, 0, first, *candidate, origin, direction);
					if (const std::optional<RayTriangleHit> candidate = IntersectRayTriangle(origin, direction, corner, diagonal, sides[1], minimum, bestDistance))
						KeepNearest(hit, bestDistance, id, 0, first + 1, *candidate, origin, direction);
				}
			}
		}

	}

	Result<std::optional<SceneRaycastHit>> RaycastScene(const Scene& scene, AssetManager& assets, const PhysicsLayerTable& layers,
		const SceneRaycastRequest& request)
	{
		if (!Utils::IsFiniteRayVector(request.Ray.Origin) || !Utils::IsFiniteRayVector(request.Ray.Direction))
			return MakeError(ErrorCode::InvalidArgument, "ray origin and direction must be finite");
		const double length = glm::length(glm::dvec3(request.Ray.Direction));
		if (length == 0 || !std::isfinite(length))
			return MakeError(ErrorCode::InvalidArgument, "ray direction must be nonzero");
		if (!std::isfinite(request.MaxDistance) || !std::isfinite(request.MinDistance)
			|| request.MinDistance < 0 || request.MaxDistance <= request.MinDistance)
			return MakeError(ErrorCode::InvalidArgument, "ray distances must define a finite nonnegative increasing interval");
		if (!std::isfinite(request.Alpha) || request.Alpha < 0 || request.Alpha > 1)
			return MakeError(ErrorCode::InvalidArgument, "ray alpha must be in [0,1]");
		const glm::dvec3 origin(request.Ray.Origin);
		const glm::dvec3 direction = glm::dvec3(request.Ray.Direction) / length;
		std::optional<SceneRaycastHit> hit;
		double bestDistance = request.MaxDistance;
		if (request.LayerMask == 0)
			return hit;
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || !entity.IsActive())
				continue;
			const auto* renderer = entity.TryGetComponent<MeshRendererComponent>();
			const bool drawsMesh = renderer != nullptr && renderer->Visible && renderer->Mesh.IsValid();
			const auto* terrain = entity.TryGetComponent<TerrainComponent>();
			const bool drawsTerrain = terrain != nullptr && terrain->Heightfield.IsValid();
			if ((!drawsMesh && !drawsTerrain) || !PhysicsLayerTable::MaskContains(request.LayerMask, Utils::GetVisualRayLayer(entity, layers)))
				continue;
			const glm::mat4 world = ComputeRenderedWorldMatrix(entity, request.Alpha);
			const double determinant = glm::determinant(glm::dmat4(world));
			if (!std::isfinite(determinant) || determinant == 0)
				continue;
			const glm::dmat4 transform(world);
			if (drawsMesh)
			{
				const AssetRef<MeshData> mesh = assets.GetOrPlaceholder<MeshData>(renderer->Mesh.GetHandle());
				if (Utils::IntersectsRayBounds(mesh->Bounds, transform, origin, direction, request.MinDistance, bestDistance))
				{
					for (uint32_t submeshIndex = 0; submeshIndex < mesh->Submeshes.size(); ++submeshIndex)
					{
						const MeshSubmesh& submesh = mesh->Submeshes[submeshIndex];
						if (!Utils::IntersectsRayBounds(submesh.Bounds, transform, origin, direction, request.MinDistance, bestDistance))
							continue;
						for (uint32_t offset = 0; offset + 2 < submesh.IndexCount; offset += 3)
						{
							const size_t index = static_cast<size_t>(submesh.IndexOffset) + offset;
							const glm::dvec3 a(transform * glm::dvec4(mesh->Vertices[mesh->Indices[index]].Position, 1));
							const glm::dvec3 b(transform * glm::dvec4(mesh->Vertices[mesh->Indices[index + 1]].Position, 1));
							const glm::dvec3 c(transform * glm::dvec4(mesh->Vertices[mesh->Indices[index + 2]].Position, 1));
							if (const std::optional<Utils::RayTriangleHit> candidate =
									Utils::IntersectRayTriangle(origin, direction, a, b, c, request.MinDistance, bestDistance))
								Utils::KeepNearest(hit, bestDistance, id, submeshIndex, offset / 3, *candidate, origin, direction);
						}
					}
				}
			}
			if (drawsTerrain)
				Utils::RaycastTerrain(id, *terrain, assets, transform, origin, direction, request.MinDistance, bestDistance, hit);
		}
		return hit;
	}

}
