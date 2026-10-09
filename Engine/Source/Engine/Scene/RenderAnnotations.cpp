#include "EnginePCH.h"
#include "Engine/Scene/RenderAnnotations.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Scene/ColliderDebugDraw.h"
#include "Engine/Scene/Components/AudioListenerComponent.h"
#include "Engine/Scene/Components/AudioSourceComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/SpotLightComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace Engine {

	namespace {

		bool IsFiniteAnnotationVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool HasPositiveComponent(const glm::vec3& value)
		{
			return std::max({ value.x, value.y, value.z }) > 0;
		}

		bool IsContributingRenderLight(const LightData& light)
		{
			const glm::vec3 radiance = light.Color * light.Intensity;
			if (!IsFiniteAnnotationVector(light.Color) || !std::isfinite(light.Intensity)
				|| !IsFiniteAnnotationVector(radiance) || !HasPositiveComponent(radiance))
				return false;
			if (light.Type != RenderLightType::Point && (!IsFiniteAnnotationVector(light.Direction) || !(glm::length(light.Direction) > 0)))
				return false;
			if (light.Type == RenderLightType::Directional)
				return true;
			return IsFiniteAnnotationVector(light.Position) && std::isfinite(light.Range) && light.Range > 0 && std::isfinite(light.SourceRadius)
				&& (light.Type != RenderLightType::Spot || (std::isfinite(light.InnerConeAngle) && std::isfinite(light.OuterConeAngle)));
		}

	}

	Status AppendRenderAnnotations(const Scene& scene, AssetManager& assets, const PhysicsLayerTable& layers,
		const PhysicsSystem* physics, RenderSnapshot& snapshot)
	{
		const RenderAnnotations& options = snapshot.Annotations;
		if (!std::isfinite(snapshot.Alpha) || snapshot.Alpha < 0 || snapshot.Alpha > 1
			|| std::to_underlying(options.Labels) > std::to_underlying(RenderAnnotationLabels::Explicit)
			|| (options.Labels != RenderAnnotationLabels::Explicit && !options.LabelEntities.empty()))
			return MakeError(ErrorCode::InvalidArgument, "invalid render annotation options or alpha");
		if (!snapshot.HasCamera)
			return {};
		DebugDrawList debug = snapshot.DebugDraw;
		std::vector<RenderIcon> icons = snapshot.Icons;
		const bool addIcons = HasFlag(snapshot.Flags, RenderViewFlags::EditorOverlays) && HasFlag(snapshot.Flags, RenderViewFlags::Icons);
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || !entity.IsActive())
				continue;
			const glm::mat4 world = ComputeRenderedWorldMatrix(entity, snapshot.Alpha);
			const glm::vec3 position(world[3]);
			if (!IsFiniteAnnotationVector(position))
				continue;
			const bool inFront = (snapshot.Camera.View * glm::vec4(position, 1)).z < 0;
			const bool label = options.Labels == RenderAnnotationLabels::All
				|| (options.Labels == RenderAnnotationLabels::Selected && std::ranges::find(snapshot.SelectedEntities, id) != snapshot.SelectedEntities.end())
				|| (options.Labels == RenderAnnotationLabels::Explicit && std::ranges::find(options.LabelEntities, id) != options.LabelEntities.end());
			if (label && inFront)
				debug.AddText(position, std::format("{} {}", entity.GetName(), id.ToString().substr(0, 6)), 16, glm::vec4(1), 0, DebugDepthMode::OnTop);
			if (options.Axes)
			{
				debug.AddLine(position, position + glm::vec3(1, 0, 0), glm::vec4(1, 0, 0, 1));
				debug.AddLine(position, position + glm::vec3(0, 1, 0), glm::vec4(0, 1, 0, 1));
				debug.AddLine(position, position + glm::vec3(0, 0, 1), glm::vec4(0, 0, 1, 1));
			}
			if (options.Bounds)
				if (const auto* mesh = entity.TryGetComponent<MeshRendererComponent>(); mesh != nullptr && mesh->Visible && mesh->Mesh.IsValid())
				{
					const Aabb bounds = assets.GetOrPlaceholder<MeshData>(mesh->Mesh.GetHandle())->Bounds.Transformed(world);
					if (!bounds.IsEmpty() && IsFiniteAnnotationVector(bounds.Min) && IsFiniteAnnotationVector(bounds.Max))
						debug.AddBox(bounds.GetCenter(), bounds.GetSize() * 0.5f, glm::quat(1, 0, 0, 0), glm::vec4(1, 1, 0, 1));
				}
			if (addIcons && inFront)
			{
				const auto add = [&icons, position, id](RenderIconKind kind, const glm::vec4& color)
				{
					icons.push_back({ .Kind = kind, .Position = position, .Color = color, .Entity = id });
				};
				if (entity.HasComponent<CameraComponent>())
					add(RenderIconKind::Camera, glm::vec4(0.5f, 0.8f, 1, 1));
				if (entity.HasComponent<DirectionalLightComponent>())
					add(RenderIconKind::DirectionalLight, glm::vec4(1, 0.9f, 0.3f, 1));
				if (entity.HasComponent<PointLightComponent>())
					add(RenderIconKind::PointLight, glm::vec4(1, 0.9f, 0.3f, 1));
				if (entity.HasComponent<SpotLightComponent>())
					add(RenderIconKind::SpotLight, glm::vec4(1, 0.9f, 0.3f, 1));
				if (entity.HasComponent<AudioSourceComponent>())
					add(RenderIconKind::AudioSource, glm::vec4(0.4f, 1, 0.6f, 1));
				if (entity.HasComponent<AudioListenerComponent>())
					add(RenderIconKind::AudioListener, glm::vec4(0.4f, 1, 0.6f, 1));
			}
		}
		if (HasFlag(snapshot.Flags, RenderViewFlags::Colliders))
			AppendColliderDebugDraw(BuildColliderDebugDraw(scene, layers, physics, &assets, { .Alpha = snapshot.Alpha }), debug);
		snapshot.DebugDraw = std::move(debug);
		snapshot.Icons = std::move(icons);
		return {};
	}

	Result<RenderSceneValidation> EvaluateRenderSceneValidation(const Scene& scene, AssetManager& assets)
	{
		ENGINE_TRY_ASSIGN(const RenderSnapshot snapshot, ExtractRenderSnapshot(scene, {}));
		RenderSceneValidation result;
		bool illumination = false;
		for (const LightData& light : snapshot.Lights)
			if (IsContributingRenderLight(light))
			{
				illumination = true;
				if (light.Type == RenderLightType::Spot && light.CastShadows)
					++result.ShadowedSpotLights;
			}
		const RenderEnvironment& environment = snapshot.Environment;
		if (!std::isfinite(environment.Intensity) || !IsFiniteAnnotationVector(environment.FallbackColor))
			return MakeError(ErrorCode::InvalidArgument, "environment illumination must be finite");
		glm::vec3 ambient = environment.FallbackColor;
		if (environment.Environment.IsValid())
		{
			const auto loaded = assets.Load(environment.Environment);
			if (loaded)
				if (const auto map = AssetCast<EnvironmentData>(*loaded))
					ambient = map->IrradianceSH9[0];
		}
		if (!IsFiniteAnnotationVector(ambient) || !IsFiniteAnnotationVector(ambient * environment.Intensity))
			return MakeError(ErrorCode::InvalidArgument, "environment irradiance must be finite");
		illumination = illumination || (environment.Intensity > 0 && HasPositiveComponent(ambient));
		bool needsLighting = false;
		for (const MeshDrawItem& item : snapshot.Meshes)
		{
			const auto mesh = assets.GetOrPlaceholder<MeshData>(item.Mesh);
			for (const MeshSubmesh& submesh : mesh->Submeshes)
			{
				if (submesh.IndexCount == 0)
					continue;
				AssetHandle material = BuiltinAssetHandles::DefaultMaterial;
				if (submesh.MaterialSlot < mesh->Slots.size() && mesh->Slots[submesh.MaterialSlot].DefaultMaterial.IsValid())
					material = mesh->Slots[submesh.MaterialSlot].DefaultMaterial;
				if (submesh.MaterialSlot < item.Materials.size() && item.Materials[submesh.MaterialSlot].IsValid())
					material = item.Materials[submesh.MaterialSlot];
				const auto data = assets.GetOrPlaceholder<MaterialData>(material);
				const glm::vec3 emission = data->Emissive * data->EmissiveStrength;
				if (!IsFiniteAnnotationVector(data->Emissive) || !std::isfinite(data->EmissiveStrength) || !IsFiniteAnnotationVector(emission))
					return MakeError(ErrorCode::InvalidArgument, "material emission must be finite");
				needsLighting = needsLighting || !HasPositiveComponent(emission);
			}
		}
		result.NoLighting = needsLighting && !illumination;
		return result;
	}

}
