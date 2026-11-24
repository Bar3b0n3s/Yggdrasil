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
#include "Engine/Scene/Components/Light2DComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/SpotLightComponent.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/JointDebugDraw.h"
#include "Engine/Scene/NavigationDebugDraw.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiExtraction.h"
#include "Engine/Scene/WaterQueries.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace Engine {

	namespace Utils {

		static bool IsFiniteAnnotationVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		static bool HasPositiveComponent(const glm::vec3& value)
		{
			return std::max({ value.x, value.y, value.z }) > 0;
		}

		// M47 (Docs/Decisions/0065-m47-water.md decision 3): the outline's colour, a water blue no collider category uses.
		constexpr glm::vec4 WaterVolumeColor = glm::vec4(0.2f, 0.65f, 1.0f, 1.0f);

		// A Rectangle water's volume as a box: its footprint (the frame's axes, the half extents in world metres) from Depth
		// below its base to Headroom above it. The frame's X and Z axes and +Y are a rotation (MakeWaterFrame), turned into a
		// quaternion without a CRT transcendental (Scene is on the simulation path's lint rule).
		static void AddWaterVolumeOutline(DebugDrawList& debug, const ResolvedWater& water)
		{
			const WaterFrame& frame = water.Surface.Frame;
			const glm::vec3 center(glm::dvec3(frame.Origin.x, frame.Origin.y + (water.Headroom - water.Depth) * 0.5, frame.Origin.z));
			const glm::vec3 halfExtents(glm::dvec3(water.HalfExtents.x, (water.Depth + water.Headroom) * 0.5, water.HalfExtents.y));
			const glm::mat3 axes(glm::vec3(static_cast<float>(frame.AxisX.x), 0.0f, static_cast<float>(frame.AxisX.y)), glm::vec3(0.0f, 1.0f, 0.0f),
				glm::vec3(static_cast<float>(frame.AxisZ.x), 0.0f, static_cast<float>(frame.AxisZ.y)));
			if (IsFiniteAnnotationVector(center) && IsFiniteAnnotationVector(halfExtents))
				debug.AddBox(center, halfExtents, glm::normalize(glm::quat_cast(axes)), WaterVolumeColor);
		}

		static bool IsContributingRenderLight(const LightData& light)
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
		const PhysicsSystem* physics, RenderSnapshot& snapshot, const Physics2DSystem* physics2D)
	{
		const RenderAnnotations& options = snapshot.Annotations;
		if (!std::isfinite(snapshot.Alpha) || snapshot.Alpha < 0 || snapshot.Alpha > 1
			|| std::to_underlying(options.Labels) > std::to_underlying(RenderAnnotationLabels::Explicit)
			|| (options.Labels != RenderAnnotationLabels::Explicit && !options.LabelEntities.empty()))
			return MakeError(ErrorCode::InvalidArgument, "invalid render annotation options or alpha");
		// M22 (Docs/Decisions/0033-m22-contract.md decision 9): the UI elements' outlines and labels, in scene pixels, whether or
		// not the view has a camera (a view without a primary camera draws its UI), so the call precedes the camera early return
		// that the mesh and light paths follow (the ADR's W22.1 review). It cannot fail, so the append stays atomic.
		AppendUiRenderAnnotations(scene, snapshot);
		if (!snapshot.HasCamera)
			return {};
		DebugDrawList debug = snapshot.AnnotationDraw;
		// A copy from the snapshot's memory resource (RenderSnapshot.h), so the assignment below moves it back without copying.
		std::pmr::vector<RenderIcon> icons(snapshot.Icons, snapshot.Icons.get_allocator());
		const bool addIcons = HasFlag(snapshot.Flags, RenderViewFlags::EditorOverlays) && HasFlag(snapshot.Flags, RenderViewFlags::Icons);
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || !entity.IsActive())
				continue;
			const glm::mat4 world = ComputeRenderedWorldMatrix(entity, snapshot.Alpha);
			const glm::vec3 position(world[3]);
			if (!Utils::IsFiniteAnnotationVector(position))
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
					if (!bounds.IsEmpty() && Utils::IsFiniteAnnotationVector(bounds.Min) && Utils::IsFiniteAnnotationVector(bounds.Max))
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
				// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 20): a 2D light shows where it is in its own colour.
				if (const auto* light2D = entity.TryGetComponent<Light2DComponent>())
					add(RenderIconKind::PointLight, glm::vec4(light2D->Color, 1.0f));
			}
			// M47 (Docs/Decisions/0065-m47-water.md decision 3; phase 8's ledger: after M45's region): with the editor's overlays, a
			// Rectangle Water's volume is outlined at its rendered pose, where it masks the Ocean and holds its own water; an
			// Ocean is unbounded and draws no outline.
			if (HasFlag(snapshot.Flags, RenderViewFlags::EditorOverlays))
			{
				if (const auto* water = entity.TryGetComponent<WaterComponent>(); water != nullptr && water->Shape == WaterShape::Rectangle)
					Utils::AddWaterVolumeOutline(debug, ResolveWater(*water, world, id));
			}
		}
		if (HasFlag(snapshot.Flags, RenderViewFlags::Colliders))
		{
			AppendColliderDebugDraw(BuildColliderDebugDraw(scene, layers, physics, &assets, { .Alpha = snapshot.Alpha }), debug);
			// M21 (decision 28): the 2D colliders after the 3D ones, told asleep from awake by the session's 2D system.
			AppendColliderDebugDraw(BuildCollider2DDebugDraw(scene, layers, physics2D, &assets, { .Alpha = snapshot.Alpha }), debug);
			// M37 (Docs/Decisions/0054-m37-joints.md decision 11): the joints after the colliders, at the snapshot's alpha.
			AppendJointDebugDraw(BuildJointDebugDraw(scene, layers, physics, snapshot.Alpha), debug);
		}
		// M30 (Docs/Decisions/0045-m30-navigation.md decision 12): the baked navigation mesh's polygon edges, after the colliders,
		// at their own colour: the mesh has no pose, so the snapshot's interpolation alpha does not apply to it.
		if (HasFlag(snapshot.Flags, RenderViewFlags::Navigation))
			static_cast<void>(AppendNavigationDebugDraw(scene, assets, debug));
		snapshot.AnnotationDraw = std::move(debug);
		snapshot.Icons = std::move(icons);
		return {};
	}

	Result<RenderSceneValidation> EvaluateRenderSceneValidation(const Scene& scene, AssetManager& assets)
	{
		ENGINE_TRY_ASSIGN(const RenderSnapshot snapshot, ExtractRenderSnapshot(scene, {}));
		RenderSceneValidation result;
		bool illumination = false;
		for (const LightData& light : snapshot.Lights)
			if (Utils::IsContributingRenderLight(light))
			{
				illumination = true;
				if (light.Type == RenderLightType::Spot && light.CastShadows)
					++result.ShadowedSpotLights;
				if (light.Type == RenderLightType::Point && light.CastShadows)
					++result.ShadowedPointLights;
			}
		const RenderEnvironment& environment = snapshot.Environment;
		if (!std::isfinite(environment.Intensity) || !Utils::IsFiniteAnnotationVector(environment.FallbackColor))
			return MakeError(ErrorCode::InvalidArgument, "environment illumination must be finite");
		glm::vec3 ambient = environment.FallbackColor;
		if (environment.Environment.IsValid())
		{
			const auto loaded = assets.Load(environment.Environment);
			if (loaded)
				if (const auto map = AssetCast<EnvironmentData>(*loaded))
					ambient = map->IrradianceSH9[0];
		}
		if (!Utils::IsFiniteAnnotationVector(ambient) || !Utils::IsFiniteAnnotationVector(ambient * environment.Intensity))
			return MakeError(ErrorCode::InvalidArgument, "environment irradiance must be finite");
		illumination = illumination || (environment.Intensity > 0 && Utils::HasPositiveComponent(ambient));
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
				if (!Utils::IsFiniteAnnotationVector(data->Emissive) || !std::isfinite(data->EmissiveStrength) || !Utils::IsFiniteAnnotationVector(emission))
					return MakeError(ErrorCode::InvalidArgument, "material emission must be finite");
				needsLighting = needsLighting || !Utils::HasPositiveComponent(emission);
			}
		}
		result.NoLighting = needsLighting && !illumination;
		return result;
	}

}
