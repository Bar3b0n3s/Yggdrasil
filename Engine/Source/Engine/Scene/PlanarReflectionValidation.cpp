#include "EnginePCH.h"
#include "Engine/Scene/PlanarReflectionValidation.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PlanarReflectionComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <utility>

namespace Engine {

	namespace Utils {

		// The local mesh bounds of `renderer`'s Mesh when it loads through `assets` as a Mesh with bounds.
		[[nodiscard]] static std::optional<Aabb> LoadMeshBounds(const MeshRendererComponent& renderer, AssetManager& assets)
		{
			const AssetHandle handle = renderer.Mesh.GetHandle();
			if (!handle.IsValid() || assets.GetAssetType(handle) != AssetType::Mesh)
				return std::nullopt;
			const Result<AssetRef<Asset>> loaded = assets.Load(handle);
			if (!loaded.has_value())
				return std::nullopt;
			const AssetRef<MeshData> mesh = AssetCast<MeshData>(*loaded);
			if (mesh == nullptr || mesh->Bounds.IsEmpty())
				return std::nullopt;
			return mesh->Bounds;
		}

		[[nodiscard]] static std::string FormatAxis(int axis, double sign)
		{
			std::array<int, 3> components{};
			components[static_cast<size_t>(axis)] = sign < 0.0 ? -1 : 1;
			return std::format("({}, {}, {})", components[0], components[1], components[2]);
		}

		// The finding for a mesh whose bounds leave the plane, or none: the plane's world distance of each corner of `bounds`
		// at `world` is its local distance (dot(unit, p) - offset) over |inverse transpose(world) * unit|. A `unit` that is not
		// finite (Normal's validator refuses a zero Normal on every write path, and a load drops a component that fails it, so
		// none reaches here) gives none.
		[[nodiscard]] static std::optional<std::pair<std::string, std::string>> CheckMeshOnPlane(const Aabb& bounds, const glm::dmat4& world,
			const glm::dvec3& unit, double offset)
		{
			const glm::dmat3 linear(world);
			const double determinant = glm::determinant(linear);
			if (determinant == 0.0 || !std::isfinite(determinant))
				return std::nullopt;
			const glm::dmat3 normals = glm::transpose(glm::inverse(linear));
			const double normalScale = glm::length(normals * unit);
			if (!(normalScale > 0.0) || !std::isfinite(normalScale))
				return std::nullopt;
			double farthest = 0.0;
			for (uint32_t corner = 0; corner < 8; ++corner)
			{
				const glm::dvec3 point((corner & 1U) != 0 ? bounds.Max.x : bounds.Min.x, (corner & 2U) != 0 ? bounds.Max.y : bounds.Min.y,
					(corner & 4U) != 0 ? bounds.Max.z : bounds.Min.z);
				farthest = std::max(farthest, std::abs(glm::dot(unit, point) - offset) / normalScale);
			}
			if (!(farthest > PlanarReflectionMaxMeshDistance))
				return std::nullopt;

			// The local axis along which the bounds are thinnest in the world (the world distance between their two faces across
			// the axis: the local extent over |inverse transpose(world) * axis|), ties going to the axis Normal points along
			// most, its sign kept from Normal where Normal has one.
			const glm::dvec3 extent = glm::dvec3(bounds.Max) - glm::dvec3(bounds.Min);
			const glm::dvec3 magnitudes = glm::abs(unit);
			const int preferred = magnitudes.z >= magnitudes.x && magnitudes.z >= magnitudes.y ? 2 : (magnitudes.y >= magnitudes.x ? 1 : 0);
			int thinnest = preferred;
			double thinnestExtent = 0.0;
			for (const int axis : { preferred, (preferred + 1) % 3, (preferred + 2) % 3 })
			{
				const double worldExtent = extent[axis] / glm::length(normals[axis]);
				if (axis == preferred || worldExtent < thinnestExtent)
				{
					thinnest = axis;
					thinnestExtent = worldExtent;
				}
			}
			std::string message = std::format("its mesh reaches {:.3g} m from the plane, farther than {} m, so the glass is not on its plane", farthest,
				PlanarReflectionMaxMeshDistance);
			// A mesh thicker than the distance across every axis: no plane on any face holds the whole glass.
			if (thinnestExtent > PlanarReflectionMaxMeshDistance)
			{
				std::string thick = std::format("the mesh is {:.3g} m thick even along local {}, its thinnest axis, more than {} m: put the "
												"PlanarReflection on a flat glass, such as the built-in Quad, whose own entity draws it",
					thinnestExtent, "XYZ"[thinnest], PlanarReflectionMaxMeshDistance);
				return std::make_pair(std::move(message), std::move(thick));
			}
			// The Offset of the bounds' face on the reflecting side, so following the hint puts every corner on the plane's side
			// within the mesh's thickness.
			const double sign = unit[thinnest] < 0.0 ? -1.0 : 1.0;
			const double face = sign > 0.0 ? static_cast<double>(bounds.Max[thinnest]) : -static_cast<double>(bounds.Min[thinnest]);
			const double suggestedOffset = face == 0.0 ? 0.0 : face; // never -0
			std::string hint = std::format("set the PlanarReflection's Normal to the glass's facing axis and Offset to its surface: the mesh is "
										   "thinnest along local {} (Normal {}, Offset {:.4g})",
				"XYZ"[thinnest], FormatAxis(thinnest, sign), suggestedOffset);
			return std::make_pair(std::move(message), std::move(hint));
		}

		static void FindIssues(const Scene& scene, ConstEntity entity, const PlanarReflectionComponent& reflection, AssetManager& assets,
			std::vector<PlanarReflectionSceneIssue>& issues)
		{
			// M47 (Docs/Decisions/0065-m47-water.md decision 3, W47.3): a Water's reflection is its base plane, which needs no mesh.
			if (entity.HasComponent<WaterComponent>())
				return;
			const std::string path = scene.GetEntityPath(entity);
			const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>();
			if (renderer == nullptr || !renderer->Mesh.IsValid())
			{
				issues.push_back(PlanarReflectionSceneIssue{ .Code = std::string(RenderReflectionInvalidCode),
					.Severity = DiagnosticSeverity::Warning,
					.Entity = entity.GetUUID(),
					.Component = renderer == nullptr ? "PlanarReflection" : "MeshRenderer",
					.Field = renderer == nullptr ? "" : "Mesh",
					.Message = std::format("the PlanarReflection of '{}' reflects nothing: {}", path,
						renderer == nullptr ? "the entity has no MeshRenderer" : "its MeshRenderer has no Mesh"),
					.Hint = "put the PlanarReflection on the entity whose MeshRenderer draws the glass (the built-in Quad faces +Z, the Plane +Y)" });
				return;
			}

			const glm::dvec3 normal(reflection.Normal);
			const std::optional<Aabb> bounds = LoadMeshBounds(*renderer, assets);
			if (!bounds.has_value())
				return;
			const glm::dmat4 world(TransformSystem::ComputeWorldMatrix(entity));
			std::optional<std::pair<std::string, std::string>> found =
				CheckMeshOnPlane(*bounds, world, normal / glm::length(normal), static_cast<double>(reflection.Offset));
			if (!found.has_value())
				return;
			issues.push_back(PlanarReflectionSceneIssue{ .Code = std::string(RenderReflectionInvalidCode),
				.Severity = DiagnosticSeverity::Warning,
				.Entity = entity.GetUUID(),
				.Component = "PlanarReflection",
				.Field = "Normal",
				.Message = std::format("the PlanarReflection of '{}' does not lie on its glass: {}", path, found->first),
				.Hint = std::move(found->second) });
		}

	}

	std::vector<PlanarReflectionSceneIssue> FindPlanarReflectionIssues(const Scene& scene, AssetManager& assets)
	{
		std::vector<PlanarReflectionSceneIssue> issues;
		if (scene.GetRegistry().view<PlanarReflectionComponent>().empty())
			return issues;
		scene.ForEachCanonical([&scene, &assets, &issues](ConstEntity entity)
		{
			if (entity.HasComponent<HierarchyDisabledTag>())
				return;
			if (const PlanarReflectionComponent* reflection = entity.TryGetComponent<PlanarReflectionComponent>())
				Utils::FindIssues(scene, entity, *reflection, assets, issues);
		});
		return issues;
	}

}
