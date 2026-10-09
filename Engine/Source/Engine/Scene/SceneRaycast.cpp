#include "EnginePCH.h"
#include "Engine/Scene/SceneRaycast.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace Engine {

	namespace {

		bool IsFiniteRayVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		uint32_t GetVisualRayLayer(ConstEntity entity, const PhysicsLayerTable& layers)
		{
			for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
				if (current.IsActive())
					if (const auto* body = current.TryGetComponent<RigidBodyComponent>())
						return layers.FindLayer(body->Layer).value_or(0);
			if (const auto* character = entity.TryGetComponent<CharacterControllerComponent>())
				return layers.FindLayer(character->Layer).value_or(0);
			return 0;
		}

		bool IntersectsRayBounds(const Aabb& bounds, const glm::dmat4& transform, const glm::dvec3& origin, const glm::dvec3& direction, double minimum, double maximum)
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

	}

	Result<std::optional<SceneRaycastHit>> RaycastScene(const Scene& scene, AssetManager& assets, const PhysicsLayerTable& layers,
		const SceneRaycastRequest& request)
	{
		if (!IsFiniteRayVector(request.Ray.Origin) || !IsFiniteRayVector(request.Ray.Direction))
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
			if (renderer == nullptr || !renderer->Visible || !renderer->Mesh.IsValid()
				|| !PhysicsLayerTable::MaskContains(request.LayerMask, GetVisualRayLayer(entity, layers)))
				continue;
			const glm::mat4 world = ComputeRenderedWorldMatrix(entity, request.Alpha);
			const double determinant = glm::determinant(glm::dmat4(world));
			if (!std::isfinite(determinant) || determinant == 0)
				continue;
			const AssetRef<MeshData> mesh = assets.GetOrPlaceholder<MeshData>(renderer->Mesh.GetHandle());
			const glm::dmat4 transform(world);
			if (!IntersectsRayBounds(mesh->Bounds, transform, origin, direction, request.MinDistance, bestDistance))
				continue;
			for (uint32_t submeshIndex = 0; submeshIndex < mesh->Submeshes.size(); ++submeshIndex)
			{
				const MeshSubmesh& submesh = mesh->Submeshes[submeshIndex];
				if (!IntersectsRayBounds(submesh.Bounds, transform, origin, direction, request.MinDistance, bestDistance))
					continue;
				for (uint32_t offset = 0; offset + 2 < submesh.IndexCount; offset += 3)
				{
					const size_t index = static_cast<size_t>(submesh.IndexOffset) + offset;
					const glm::dvec3 a(transform * glm::dvec4(mesh->Vertices[mesh->Indices[index]].Position, 1));
					const glm::dvec3 b(transform * glm::dvec4(mesh->Vertices[mesh->Indices[index + 1]].Position, 1));
					const glm::dvec3 c(transform * glm::dvec4(mesh->Vertices[mesh->Indices[index + 2]].Position, 1));
					const glm::dvec3 edge1 = b - a;
					const glm::dvec3 edge2 = c - a;
					const glm::dvec3 p = glm::cross(direction, edge2);
					const double divisor = glm::dot(edge1, p);
					if (divisor == 0)
						continue;
					const glm::dvec3 displacement = origin - a;
					const double u = glm::dot(displacement, p) / divisor;
					const glm::dvec3 q = glm::cross(displacement, edge1);
					const double v = glm::dot(direction, q) / divisor;
					const double distance = glm::dot(edge2, q) / divisor;
					if (!(u >= 0 && v >= 0 && u + v <= 1 && distance >= request.MinDistance && distance <= bestDistance))
						continue;
					const uint32_t triangle = offset / 3;
					if (hit && distance == bestDistance && std::tuple(id, submeshIndex, triangle) >= std::tuple(hit->Entity, hit->Submesh, hit->Triangle))
						continue;
					glm::dvec3 normal = glm::normalize(glm::cross(edge1, edge2));
					if (glm::dot(normal, direction) > 0)
						normal = -normal;
					bestDistance = distance;
					hit = SceneRaycastHit{ id, static_cast<float>(distance), glm::vec3(origin + distance * direction), glm::vec3(normal),
						glm::vec3(1 - u - v, u, v), submeshIndex, triangle };
				}
			}
		}
		return hit;
	}

}
