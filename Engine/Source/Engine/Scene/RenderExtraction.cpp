#include "EnginePCH.h"
#include "Engine/Scene/RenderExtraction.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/FogComponent.h"
#include "Engine/Scene/Components/IrradianceVolumeComponent.h"
#include "Engine/Scene/Components/Light2DComponent.h"
#include "Engine/Scene/Components/MaterialParametersComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/ParticleEmitterComponent.h"
#include "Engine/Scene/Components/ParticleStateComponent.h"
#include "Engine/Scene/Components/PixelPerfectCameraComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/PostProcessComponent.h"
#include "Engine/Scene/Components/ReflectionProbeComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Components/SpotLightComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Components/TextComponent.h"
#include "Engine/Scene/Components/TilemapRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/ParticleSimulation.h"
#include "Engine/Scene/PixelPerfectCamera.h"
#include "Engine/Scene/Private/AnimationTargets.h"
#include "Engine/Scene/Private/SpriteDrawing.h"
#include "Engine/Scene/Private/TilemapDrawing.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scene/UiExtraction.h"
#include "Engine/Scene/UiLayout.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>

// Simulation path (§4.12): only + - * /, sqrt (glm's normalize, cross, inverse and mat4_cast are built from them) and
// Core/DetMath, so a snapshot is bit-identical in every configuration.

namespace Engine {

	namespace Utils {

		// cos(theta) above which the rendered rotation falls back to a normalized lerp: sin(theta) is too small to divide by.
		// The same threshold as TransformSystem's slerp, so the rendered pose and Transform.RenderRotation agree.
		constexpr double RenderSlerpLinearThreshold = 0.9995;
		// The length below which a camera's up vector is parallel to its view direction (ExplicitRenderCamera looking straight
		// up or down), or a light's -Z axis is degenerate (a zero scale).
		constexpr float DegenerateAxisLength = 1e-6f;

		// The previous world matrix to interpolate from, or nullptr when the entity renders at its current pose (§5.2):
		// outside a runtime scene, when it or an ancestor has InterpolationResetTag, or before its first snapshot. The rule of
		// TransformSystem::GetRenderPosition and GetRenderRotation.
		static const PreviousWorldTransformComponent* FindInterpolationStart(ConstEntity entity)
		{
			if (!entity.GetScene()->IsRuntime())
				return nullptr;
			for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
			{
				if (current.HasComponent<InterpolationResetTag>())
					return nullptr;
			}
			return entity.TryGetComponent<PreviousWorldTransformComponent>();
		}

		// Spherical interpolation along the shorter arc through DetMath, `alpha` in (0, 1): TransformSystem's slerp, so the
		// rendered rotation equals Transform.RenderRotation up to the decomposition's rounding.
		static glm::quat SlerpRotation(const glm::quat& from, const glm::quat& to, float alpha)
		{
			const glm::dquat start(from.w, from.x, from.y, from.z);
			glm::dquat end(to.w, to.x, to.y, to.z);
			double cosTheta = glm::dot(start, end);
			if (cosTheta < 0.0)
			{
				end = -end;
				cosTheta = -cosTheta;
			}

			const double t = alpha;
			double startWeight = 1.0 - t;
			double endWeight = t;
			if (cosTheta < RenderSlerpLinearThreshold)
			{
				const double theta = DetMath::ACos(cosTheta);
				const double sinTheta = DetMath::Sin(theta);
				startWeight = DetMath::Sin((1.0 - t) * theta) / sinTheta;
				endWeight = DetMath::Sin(t * theta) / sinTheta;
			}
			const glm::dquat blended = glm::normalize(start * startWeight + end * endWeight);
			return glm::quat(static_cast<float>(blended.w), static_cast<float>(blended.x), static_cast<float>(blended.y), static_cast<float>(blended.z));
		}

		// T(translation) * R(rotation) * S(scale).
		static glm::mat4 ComposeMatrix(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
		{
			glm::mat4 matrix = glm::mat4_cast(rotation);
			matrix[0] *= scale.x;
			matrix[1] *= scale.y;
			matrix[2] *= scale.z;
			matrix[3] = glm::vec4(translation, 1.0f);
			return matrix;
		}

		// The world matrix extraction reads: the WorldTransformComponent, or the chain walk before the first
		// TransformSystem::Update gave the entity one.
		static glm::mat4 GetCurrentWorldMatrix(ConstEntity entity)
		{
			if (const WorldTransformComponent* world = entity.TryGetComponent<WorldTransformComponent>())
				return world->Matrix;
			return TransformSystem::ComputeWorldMatrix(entity);
		}

		static bool IsEffectivelyEnabled(ConstEntity entity)
		{
			return !entity.HasComponent<HierarchyDisabledTag>();
		}

		// M23 (C23.3; Docs/Decisions/0034-m23-contract.md decisions 27 and 29): a SkinnedMesh's deformation. Its driving
		// Animator's pose (the runtime AnimationPoseComponent the AnimationSystem phase or the editor's preview wrote) gives the
		// palette, each skin matrix taken into the item's space at the rendered poses (inverse(World) * the Animator entity's
		// rendered world matrix * Skin[j]), and the morph weights of the entity's path from the Animator, which replace the
		// component's at their indices; without a pose the item has no palette (its bind pose) and the component's weights.
		static void ExtractSkinnedItem(ConstEntity entity, const SkinnedMeshComponent& skinned, float alpha, MeshDrawItem& item)
		{
			std::span<const float> animated;
			const ConstEntity animator = FindDrivingAnimator(entity);
			if (const AnimationPoseComponent* pose = animator.IsValid() ? animator.TryGetComponent<AnimationPoseComponent>() : nullptr)
			{
				const glm::mat4 toItem = glm::inverse(item.World) * ComputeRenderedWorldMatrix(animator, alpha);
				item.JointPalette.reserve(pose->Skin.size());
				for (const glm::mat4& skin : pose->Skin)
					item.JointPalette.push_back(toItem * skin);
				if (const std::optional<std::string> target = Detail::GetAnimationTargetPath(animator, entity))
				{
					if (const AnimationMorphWeights* morph = Detail::FindPoseMorphWeights(pose->Morphs, *target))
						animated = morph->Weights;
				}
			}
			item.MorphWeights = Detail::MergeAnimatedMorphWeights(skinned.MorphWeights, animated);
		}

		// M26 (C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 7; stream P-B): appends one emitter's live particles
		// (its runtime state's, in emission order) to snapshot.Particles and its item to snapshot.ParticleEmitters, even when
		// none is alive. A particle is drawn at `alpha` between its PreviousPosition and its Position (its Position at 1), in
		// world space, through `emitterWorld` (the emitter's rendered world matrix) for a Local emitter; its size is its
		// StartSize times SizeOverLife and its colour ColorOverLife, both at its age over its lifetime as of its last step
		// (clamped to [0, 1]; extraction does not know the fixed step, so the curves are not interpolated between steps).
		static void AppendParticleItems(const ParticleEmitterComponent& emitter, const ParticleStateComponent& state, const glm::mat4& emitterWorld,
			float alpha, UUID entity, RenderSnapshot& snapshot)
		{
			const auto first = static_cast<uint32_t>(snapshot.Particles.size());
			const bool local = emitter.SimulationSpace == ParticleSimulationSpace::Local;
			for (const Particle& particle : state.State.Particles)
			{
				glm::vec3 position = alpha >= 1.0f ? particle.Position : particle.PreviousPosition + (particle.Position - particle.PreviousPosition) * alpha;
				if (local)
					position = glm::vec3(emitterWorld * glm::vec4(position, 1.0f));
				// A NaN ratio (a zero age over a zero lifetime, which the fields' range excludes) takes the curves' start.
				const float ratio = particle.Age / particle.Lifetime;
				const float normalizedAge = ratio >= 1.0f ? 1.0f : (ratio > 0.0f ? ratio : 0.0f);
				snapshot.Particles.push_back(ParticleItem{
					.Position = position,
					.Size = particle.StartSize * EvaluateParticleSize(emitter, normalizedAge),
					.Color = EvaluateParticleColor(emitter, normalizedAge),
				});
			}
			snapshot.ParticleEmitters.push_back(ParticleEmitterItem{
				.Texture = emitter.Texture.GetHandle(),
				.Blend = emitter.Blend == ParticleBlend::Additive ? RenderParticleBlend::Additive : RenderParticleBlend::Alpha,
				.First = first,
				.Count = static_cast<uint32_t>(snapshot.Particles.size()) - first,
				.Entity = entity,
			});
		}

		static RenderProjection ToRenderProjection(ProjectionType projection)
		{
			return projection == ProjectionType::Orthographic ? RenderProjection::Orthographic : RenderProjection::Perspective;
		}

		static RenderTonemapper ToRenderTonemapper(Tonemapper tonemapper)
		{
			switch (tonemapper)
			{
				case Tonemapper::AgX:        return RenderTonemapper::AgX;
				case Tonemapper::ACES:       return RenderTonemapper::Aces;
				case Tonemapper::PbrNeutral: return RenderTonemapper::PbrNeutral;
				case Tonemapper::Linear:     return RenderTonemapper::Linear;
			}
			return RenderTonemapper::AgX;
		}

		static RenderSsaoQuality ToRenderSsaoQuality(SsaoQuality quality)
		{
			switch (quality)
			{
				case SsaoQuality::Low:    return RenderSsaoQuality::Low;
				case SsaoQuality::Medium: return RenderSsaoQuality::Medium;
				case SsaoQuality::High:   return RenderSsaoQuality::High;
			}
			return RenderSsaoQuality::Medium;
		}

		// M26 (Docs/Decisions/0037-m26-contract.md decision 12): PostProcess version 2's enums.
		static RenderExposureMode ToRenderExposureMode(ExposureMode mode)
		{
			switch (mode)
			{
				case ExposureMode::Manual: return RenderExposureMode::Manual;
				case ExposureMode::Auto:   return RenderExposureMode::Auto;
			}
			return RenderExposureMode::Manual;
		}

		static RenderAntiAliasing ToRenderAntiAliasing(AntiAliasing antiAliasing)
		{
			switch (antiAliasing)
			{
				case AntiAliasing::None: return RenderAntiAliasing::None;
				case AntiAliasing::Fxaa: return RenderAntiAliasing::Fxaa;
				case AntiAliasing::Taa:  return RenderAntiAliasing::Taa;
			}
			return RenderAntiAliasing::Fxaa;
		}

		// The snapshot's post-processing of a PostProcess component: every member of version 2, its enums mapped.
		static PostProcessSettings MakePostProcessSettings(const PostProcessComponent& component)
		{
			return PostProcessSettings{
				.ExposureEV = component.ExposureEV,
				.ExposureMode = ToRenderExposureMode(component.ExposureMode),
				.AutoExposureMinEV = component.AutoExposureMinEV,
				.AutoExposureMaxEV = component.AutoExposureMaxEV,
				.AutoExposureSpeedUp = component.AutoExposureSpeedUp,
				.AutoExposureSpeedDown = component.AutoExposureSpeedDown,
				.Tonemap = ToRenderTonemapper(component.Tonemap),
				.Lut = component.Lut.GetHandle(),
				.LutIntensity = component.LutIntensity,
				.SsaoEnabled = component.SsaoEnabled,
				.SsaoRadius = component.SsaoRadius,
				.SsaoIntensity = component.SsaoIntensity,
				.SsaoQuality = ToRenderSsaoQuality(component.SsaoQuality),
				.SsrEnabled = component.SsrEnabled,
				.SsrIntensity = component.SsrIntensity,
				.SsrMaxDistance = component.SsrMaxDistance,
				.SsrThickness = component.SsrThickness,
				.SsrMaxRoughness = component.SsrMaxRoughness,
				.BloomEnabled = component.BloomEnabled,
				.BloomIntensity = component.BloomIntensity,
				.AntiAliasing = ToRenderAntiAliasing(component.AntiAliasing),
			};
		}

		static RenderTextSpace ToRenderTextSpace(TextSpace space)
		{
			switch (space)
			{
				case TextSpace::Screen: return RenderTextSpace::Screen;
				case TextSpace::World:  return RenderTextSpace::World;
			}
			return RenderTextSpace::Screen;
		}

		static RenderTextAlignment ToRenderTextAlignment(TextAlignment alignment)
		{
			switch (alignment)
			{
				case TextAlignment::Left:   return RenderTextAlignment::Left;
				case TextAlignment::Center: return RenderTextAlignment::Center;
				case TextAlignment::Right:  return RenderTextAlignment::Right;
			}
			return RenderTextAlignment::Center;
		}

		// The world -> view matrix of a camera at `position` whose rotation is `rotation` (it looks down its local -Z with
		// +Y up): the inverse of T(position) * R(rotation).
		static glm::mat4 MakeViewMatrix(const glm::vec3& position, const glm::quat& rotation)
		{
			const glm::mat3 inverseRotation = glm::transpose(glm::mat3_cast(rotation));
			glm::mat4 view(inverseRotation);
			view[3] = glm::vec4(-(inverseRotation * position), 1.0f);
			return view;
		}

		// The view matrix of a camera at `position` turned as `world`, a camera entity's rendered world matrix, with its scale
		// removed: the rotation of its decomposition, or the identity rotation for a matrix that does not decompose (a zero
		// scale, which the Transform.Scale field rule already rejects for authored data).
		static glm::mat4 MakeEntityViewMatrix(const glm::mat4& world, const glm::vec3& position)
		{
			const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(world);
			return MakeViewMatrix(position, decomposed.has_value() ? decomposed->Rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		}

		// The view matrix of a camera at `position` looking at `target` with +Y up. Looking straight up or down, where +Y
		// is the view direction, the view's up is -Z (looking down) or +Z (looking up), so the image keeps the world's -Z
		// at its top when seen from above.
		static glm::mat4 MakeLookAtViewMatrix(const glm::vec3& position, const glm::vec3& target)
		{
			const glm::vec3 forward = glm::normalize(target - position);
			glm::vec3 up(0.0f, 1.0f, 0.0f);
			if (glm::length(glm::cross(forward, up)) < DegenerateAxisLength)
				up = forward.y < 0.0f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
			const glm::vec3 right = glm::normalize(glm::cross(forward, up));
			const glm::vec3 cameraUp = glm::cross(right, forward);
			// The camera's basis in world space: +X right, +Y up, -Z forward; the view matrix is its transpose.
			glm::mat4 view(1.0f);
			view[0][0] = right.x;
			view[1][0] = right.y;
			view[2][0] = right.z;
			view[0][1] = cameraUp.x;
			view[1][1] = cameraUp.y;
			view[2][1] = cameraUp.z;
			view[0][2] = -forward.x;
			view[1][2] = -forward.y;
			view[2][2] = -forward.z;
			view[3][0] = -glm::dot(right, position);
			view[3][1] = -glm::dot(cameraUp, position);
			view[3][2] = glm::dot(forward, position);
			return view;
		}

		// The direction a light shines: the normalized -Z axis of its rendered world matrix, or -Z for a degenerate one.
		static glm::vec3 GetLightDirection(const glm::mat4& world)
		{
			const glm::vec3 axis(world[2]);
			const float length = glm::length(axis);
			if (!(length >= DegenerateAxisLength))
				return glm::vec3(0.0f, 0.0f, -1.0f);
			return -axis / length;
		}

		// The camera data of a CameraComponent seen through `world` (its rendered world matrix) in a `width` x `height` view.
		// Through the entity's PixelPerfectCamera when it holds one (Docs/Decisions/0032-m21-contract.md decision 6): at the
		// position ComputePixelPerfectView snaps to the pixel grid, orthographic with its half height at the reference
		// resolution's aspect whatever the Camera's own Projection and OrthographicSize, with the reference resolution as the
		// viewport (the scene resolution), and `output` set to the view's output stage; otherwise `output` is left as it is.
		static CameraData MakeEntityCameraData(ConstEntity entity, const CameraComponent& camera, const glm::mat4& world, uint32_t width,
			uint32_t height, RenderOutputSettings& output)
		{
			CameraData data;
			data.Position = glm::vec3(world[3]);
			data.ProjectionKind = ToRenderProjection(camera.Projection);
			data.VerticalFov = camera.VerticalFov;
			data.OrthographicSize = camera.OrthographicSize;
			data.ViewportWidth = width;
			data.ViewportHeight = height;
			if (const PixelPerfectCameraComponent* pixelPerfect = entity.TryGetComponent<PixelPerfectCameraComponent>())
			{
				const PixelPerfectView view = ComputePixelPerfectView(*pixelPerfect, data.Position);
				data.Position = view.Position;
				data.ProjectionKind = RenderProjection::Orthographic;
				data.OrthographicSize = view.OrthographicSize;
				// M22's landing: the output stage's settings from the one function the play session's UI takes them from too.
				output = GetCameraOutputSettings(entity);
				data.ViewportWidth = output.SceneWidth;
				data.ViewportHeight = output.SceneHeight;
			}
			data.View = MakeEntityViewMatrix(world, data.Position);
			data.NearClip = camera.NearClip;
			data.FarClip = camera.FarClip;
			data.Projection = ComputeReverseZProjection(data.ProjectionKind, data.VerticalFov, data.OrthographicSize, data.NearClip, data.FarClip,
				data.ViewportWidth, data.ViewportHeight);
			data.ClearColor = camera.ClearColor;
			data.ClearToSkybox = camera.Clear == ClearMode::Skybox;
			data.Entity = entity.GetUUID();
			return data;
		}

		static CameraData MakeExplicitCameraData(const ExplicitRenderCamera& camera, uint32_t width, uint32_t height)
		{
			CameraData data;
			data.View = MakeLookAtViewMatrix(camera.Position, camera.Target);
			data.Position = camera.Position;
			data.ProjectionKind = camera.Projection;
			data.VerticalFov = camera.VerticalFov;
			data.OrthographicSize = camera.OrthographicSize;
			data.NearClip = camera.NearClip;
			data.FarClip = camera.FarClip;
			data.Projection = ComputeReverseZProjection(data.ProjectionKind, data.VerticalFov, data.OrthographicSize, data.NearClip, data.FarClip,
				width, height);
			data.ViewportWidth = width;
			data.ViewportHeight = height;
			data.ClearColor = camera.ClearColor;
			data.ClearToSkybox = false;
			data.Entity = UUID();
			return data;
		}

		static void AddLight(RenderSnapshot& snapshot, LightData light, const glm::mat4& world, UUID entity)
		{
			light.Position = glm::vec3(world[3]);
			light.Direction = GetLightDirection(world);
			light.Entity = entity;
			snapshot.Lights.push_back(light);
		}

		// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16; stream Q-B): the irradiance volume and the
		// reflection probe of `entity` (`id`) whose Data is set, at the rendered world matrix `getWorld` gives.
		template<typename GetWorld>
		static void AddProbeItems(ConstEntity entity, UUID id, const GetWorld& getWorld, RenderSnapshot& snapshot)
		{
			if (const IrradianceVolumeComponent* volume = entity.TryGetComponent<IrradianceVolumeComponent>(); volume != nullptr && volume->Data.IsValid())
			{
				snapshot.IrradianceVolumes.push_back(IrradianceVolumeItem{
					.World = getWorld(),
					.Size = volume->Size,
					.Data = volume->Data.GetHandle(),
					.Intensity = volume->Intensity,
					.BlendDistance = volume->BlendDistance,
					.Entity = id,
				});
			}
			if (const ReflectionProbeComponent* probe = entity.TryGetComponent<ReflectionProbeComponent>(); probe != nullptr && probe->Data.IsValid())
			{
				snapshot.ReflectionProbes.push_back(ReflectionProbeItem{
					.World = getWorld(),
					.Size = probe->Size,
					.Data = probe->Data.GetHandle(),
					.BoxProjection = probe->BoxProjection,
					.Intensity = probe->Intensity,
					.BlendDistance = probe->BlendDistance,
					.Entity = id,
				});
			}
		}

		// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22; stream T-C): the drawing of `terrain` (whose
		// Heightfield is set) at the rendered world matrix `world`, with its first MaxTerrainLayers layers; its PickId is
		// assigned after the canonical loop (AssignTerrainPickIds).
		static TerrainItem MakeTerrainItem(const TerrainComponent& terrain, const glm::mat4& world, UUID id)
		{
			TerrainItem item;
			item.World = world;
			item.Heightfield = terrain.Heightfield.GetHandle();
			item.Size = terrain.Size;
			item.HeightScale = terrain.HeightScale;
			const size_t layerCount = std::min<size_t>(terrain.Layers.size(), MaxTerrainLayers);
			item.Layers.reserve(layerCount);
			for (size_t index = 0; index < layerCount; ++index)
			{
				const TerrainLayer& layer = terrain.Layers[index];
				item.Layers.push_back(TerrainLayerItem{
					.BaseColorMap = layer.BaseColorMap.GetHandle(),
					.NormalMap = layer.NormalMap.GetHandle(),
					.Tint = layer.Tint,
					.Tiling = layer.Tiling,
					.Roughness = layer.Roughness,
					.Metallic = layer.Metallic,
				});
			}
			item.SplatMap = terrain.SplatMap.GetHandle();
			item.ReceiveShadows = terrain.ReceiveShadows;
			item.LodBias = terrain.LodBias;
			item.Entity = id;
			return item;
		}

		// M26 (decision 22): each terrain's PickId after the meshes' (RenderSnapshot::Terrains): its entity's entry when its
		// MeshRenderer made one, so the table never lists an entity twice, else a new entry appended in the terrains' order.
		static void AssignTerrainPickIds(RenderSnapshot& snapshot)
		{
			for (TerrainItem& terrain : snapshot.Terrains)
			{
				const auto found = std::find(snapshot.PickTable.begin(), snapshot.PickTable.end(), terrain.Entity);
				if (found == snapshot.PickTable.end())
				{
					snapshot.PickTable.push_back(terrain.Entity);
					terrain.PickId = static_cast<uint32_t>(snapshot.PickTable.size());
				}
				else
					terrain.PickId = static_cast<uint32_t>(found - snapshot.PickTable.begin()) + 1;
			}
		}

		// The camera of `request` in `snapshot`. Errors: those of ExtractRenderSnapshot for Entity and Explicit requests.
		static Status ExtractCamera(const Scene& scene, const RenderExtractionRequest& request, RenderSnapshot& snapshot)
		{
			snapshot.Camera.ViewportWidth = request.Width;
			snapshot.Camera.ViewportHeight = request.Height;
			switch (request.Camera)
			{
				case RenderCameraSource::Primary:
				{
					const ConstEntity camera = FindPrimaryCamera(scene);
					if (!camera.IsValid())
						return {}; // a game view without a primary camera: no camera, the default clear colour
					snapshot.Camera = MakeEntityCameraData(camera, camera.GetComponent<CameraComponent>(), ComputeRenderedWorldMatrix(camera, request.Alpha),
						request.Width, request.Height, snapshot.Output);
					snapshot.HasCamera = true;
					return {};
				}
				case RenderCameraSource::Entity:
				{
					const ConstEntity camera = scene.FindEntityByID(request.CameraEntity);
					if (!camera.IsValid())
						return MakeError(ErrorCode::NotFound, "no entity of the scene has the id {}", request.CameraEntity);
					const CameraComponent* component = camera.TryGetComponent<CameraComponent>();
					if (component == nullptr)
					{
						return std::unexpected(Error(ErrorCode::InvalidArgument,
							std::format("entity {} '{}' has no Camera component, so it cannot render a view", request.CameraEntity, camera.GetName()))
								.WithHint("add a Camera component to it, or name a camera entity"));
					}
					snapshot.Camera = MakeEntityCameraData(camera, *component, ComputeRenderedWorldMatrix(camera, request.Alpha), request.Width,
						request.Height, snapshot.Output);
					snapshot.HasCamera = true;
					return {};
				}
				case RenderCameraSource::Explicit:
				{
					const ExplicitRenderCamera& camera = request.ExplicitCamera;
					if (camera.Target == camera.Position)
					{
						return MakeError(ErrorCode::InvalidArgument, "an explicit camera must look at a target other than its position ({}, {}, {})",
							camera.Position.x, camera.Position.y, camera.Position.z);
					}
					snapshot.Camera = MakeExplicitCameraData(camera, request.Width, request.Height);
					snapshot.HasCamera = true;
					return {};
				}
			}
			return MakeError(ErrorCode::InvalidArgument, "unknown camera source {}", std::to_underlying(request.Camera));
		}

	}

	ConstEntity FindPrimaryCamera(const Scene& scene)
	{
		ConstEntity primary;
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || !Utils::IsEffectivelyEnabled(entity))
				continue;
			const CameraComponent* camera = entity.TryGetComponent<CameraComponent>();
			if (camera != nullptr && camera->Primary)
			{
				primary = entity;
				break;
			}
		}
		return primary;
	}

	RenderOutputSettings GetCameraOutputSettings(ConstEntity camera)
	{
		ENGINE_CORE_ASSERT(camera.IsValid(), "GetCameraOutputSettings needs a valid entity");
		const PixelPerfectCameraComponent* pixelPerfect = camera.TryGetComponent<PixelPerfectCameraComponent>();
		if (pixelPerfect == nullptr)
			return RenderOutputSettings{};
		// The view's Output depends on the component alone (PixelPerfectCamera.h), so the origin stands for any position.
		return ComputePixelPerfectView(*pixelPerfect, glm::vec3(0.0f)).Output;
	}

	RenderOutputMapping ComputeGameViewOutputMapping(const Scene& scene, glm::uvec2 viewSize)
	{
		const ConstEntity camera = FindPrimaryCamera(scene);
		return ComputeRenderOutputMapping(viewSize.x, viewSize.y, camera.IsValid() ? GetCameraOutputSettings(camera) : RenderOutputSettings{});
	}

	glm::mat4 ComputeRenderedWorldMatrix(ConstEntity entity, float alpha)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "ComputeRenderedWorldMatrix needs a valid entity");
		ENGINE_CORE_ASSERT(alpha >= 0.0f && alpha <= 1.0f, "ComputeRenderedWorldMatrix needs an alpha in [0, 1], got {}", alpha);

		const glm::mat4 current = Utils::GetCurrentWorldMatrix(entity);
		const PreviousWorldTransformComponent* previous = Utils::FindInterpolationStart(entity);
		if (previous == nullptr || alpha >= 1.0f || previous->Matrix == current)
			return current;

		// The translation always interpolates (TransformSystem::GetRenderPosition's arithmetic).
		const glm::vec3 startTranslation(previous->Matrix[3]);
		const glm::vec3 currentTranslation(current[3]);
		const glm::vec3 translation = startTranslation + (currentTranslation - startTranslation) * alpha;

		// Rotation and scale interpolate when both poses decompose; otherwise the current ones are kept (GetRenderRotation's
		// fallback for a previous pose that does not decompose).
		const Result<TransformDecomposition> start = TransformSystem::DecomposeMatrix(previous->Matrix);
		const Result<TransformDecomposition> end = TransformSystem::DecomposeMatrix(current);
		if (!start.has_value() || !end.has_value())
		{
			glm::mat4 rendered = current;
			rendered[3] = glm::vec4(translation, 1.0f);
			return rendered;
		}
		const glm::quat rotation = alpha <= 0.0f ? start->Rotation : Utils::SlerpRotation(start->Rotation, end->Rotation, alpha);
		const glm::vec3 scale = start->Scale + (end->Scale - start->Scale) * alpha;
		return Utils::ComposeMatrix(translation, rotation, scale);
	}

	MeshMaterialParameters ResolveMaterialParameters(const MaterialParametersComponent& component, double time)
	{
		// One offset component scrolled to `time` and wrapped into [0, 1).
		const auto scroll = [time](float offset, float speed)
		{
			const double value = static_cast<double>(offset) + static_cast<double>(speed) * time;
			const auto wrapped = static_cast<float>(value - std::floor(value));
			return wrapped < 1.0f ? wrapped : 0.0f;
		};
		MeshMaterialParameters result;
		result.Tint = component.Tint;
		result.Emissive = component.Emissive * component.EmissiveStrength;
		result.UVOffset = glm::vec2(scroll(component.UVOffset.x, component.UVScroll.x), scroll(component.UVOffset.y, component.UVScroll.y));
		result.DissolveAmount = component.DissolveAmount;
		if (component.DissolveAmount > 0.0f)
		{
			result.DissolveEdge = component.DissolveEdgeColor * component.DissolveEdgeStrength;
			result.DissolveEdgeWidth = component.DissolveEdgeWidth;
			result.DissolveNoiseScale = component.DissolveNoiseScale;
		}
		result.Opacity = component.Opacity;
		return result;
	}

	Result<RenderSnapshot> ExtractRenderSnapshot(const Scene& scene, const RenderExtractionRequest& request, std::pmr::memory_resource* resource)
	{
		ENGINE_PROFILE_SCOPE("ExtractRenderSnapshot");
		ENGINE_CORE_ASSERT(resource != nullptr, "ExtractRenderSnapshot needs a memory resource");
		constexpr uint32_t ValidFlags = (1u << 7) - 1u;
		if ((std::to_underlying(request.Flags) & ~ValidFlags) != 0)
			return MakeError(ErrorCode::InvalidArgument, "render extraction has unknown view flags");
		if (request.Annotations.Labels > RenderAnnotationLabels::Explicit
			|| (request.Annotations.Labels != RenderAnnotationLabels::Explicit && !request.Annotations.LabelEntities.empty()))
			return MakeError(ErrorCode::InvalidArgument, "render extraction has invalid annotation labels");
		const uint32_t shadowSize = request.Quality.ShadowMapSize;
		if (shadowSize < 256 || shadowSize > 8192 || (shadowSize & (shadowSize - 1)) != 0)
			return MakeError(ErrorCode::InvalidArgument, "shadow map size must be a power of two from 256 to 8192");
		if (!IsValidMaxShadedLights(request.Quality.MaxShadedLights))
			return MakeError(ErrorCode::InvalidArgument, "the shaded-light cap must be 256, 512, 1024 or 2048, got {}", request.Quality.MaxShadedLights);
		if (request.Width == 0 || request.Height == 0)
			return MakeError(ErrorCode::InvalidArgument, "a render extraction needs a view of at least 1x1 pixels, got {}x{}", request.Width, request.Height);
		if (!std::isfinite(request.Alpha) || request.Alpha < 0.0f || request.Alpha > 1.0f)
			return MakeError(ErrorCode::InvalidArgument, "a render extraction needs an interpolation alpha in [0, 1], got {}", request.Alpha);
		if (!std::isfinite(request.SimulationTime) || request.SimulationTime < 0.0)
			return MakeError(ErrorCode::InvalidArgument, "a render extraction needs a finite simulation time of at least 0 s, got {}", request.SimulationTime);

		RenderSnapshot snapshot(resource != nullptr ? resource : std::pmr::get_default_resource());
		snapshot.Alpha = request.Alpha;
		snapshot.Flags = request.Flags;
		snapshot.Quality = request.Quality;
		snapshot.Annotations = request.Annotations;
		snapshot.SelectedEntities = request.SelectedEntities;
		const auto canonicalize = [&scene](std::vector<UUID>& ids)
		{
			std::erase_if(ids, [&scene](UUID id)
			{
				return !scene.FindEntityByID(id).IsValid();
			});
			std::ranges::sort(ids);
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		};
		canonicalize(snapshot.SelectedEntities);
		canonicalize(snapshot.Annotations.LabelEntities);
		ENGINE_TRY(Utils::ExtractCamera(scene, request, snapshot));

		std::optional<RenderEnvironment> environment;
		std::optional<PostProcessSettings> post;
		std::optional<RenderFog> fog;
		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 24): the sprites' places in the one 2D draw order and the
		// tilemaps' canonical positions, from which the tilemaps are placed among the sprites below.
		std::vector<Utils::SpriteDrawKey> spriteKeys;
		std::vector<uint32_t> tilemapPositions;
		uint32_t canonicalPosition = 0;
		scene.ForEachCanonical([&snapshot, &environment, &post, &fog, &request, &spriteKeys, &tilemapPositions, &canonicalPosition](ConstEntity entity)
		{
			if (!Utils::IsEffectivelyEnabled(entity))
				return;
			const uint32_t position = canonicalPosition++;
			const UUID id = entity.GetUUID();
			// The rendered matrix is computed once per entity, and only for entities that draw or light.
			std::optional<glm::mat4> world;
			const auto getWorld = [&world, entity, &request]() -> const glm::mat4&
			{
				if (!world.has_value())
					world = ComputeRenderedWorldMatrix(entity, request.Alpha);
				return *world;
			};

			if (const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>(); renderer != nullptr && renderer->Visible && renderer->Mesh.IsValid())
			{
				MeshDrawItem item;
				item.World = getWorld();
				item.Mesh = renderer->Mesh.GetHandle();
				item.Materials.reserve(renderer->Materials.size());
				for (const TypedAssetHandle<AssetType::Material>& material : renderer->Materials)
					item.Materials.push_back(material.GetHandle());
				item.CastShadows = renderer->CastShadows;
				item.ReceiveShadows = renderer->ReceiveShadows;
				item.Entity = id;
				if (const SkinnedMeshComponent* skinned = entity.TryGetComponent<SkinnedMeshComponent>())
					Utils::ExtractSkinnedItem(entity, *skinned, request.Alpha, item);
				if (const MaterialParametersComponent* parameters = entity.TryGetComponent<MaterialParametersComponent>())
					item.Parameters = ResolveMaterialParameters(*parameters, request.SimulationTime);
				snapshot.PickTable.push_back(id);
				item.PickId = static_cast<uint32_t>(snapshot.PickTable.size());
				snapshot.Meshes.push_back(std::move(item));
			}
			if (const DirectionalLightComponent* light = entity.TryGetComponent<DirectionalLightComponent>())
			{
				Utils::AddLight(snapshot,
					LightData{
						.Type = RenderLightType::Directional,
						.Color = light->Color,
						.Intensity = light->Intensity,
						.LightAngle = light->LightAngle,
						.CastShadows = light->CastShadows,
						.ShadowDistance = light->ShadowDistance,
						.CascadeCount = light->CascadeCount,
						.CascadeSplitLambda = light->CascadeSplitLambda,
						.DepthBias = light->DepthBias,
						.NormalBias = light->NormalBias,
					},
					getWorld(), id);
			}
			if (const PointLightComponent* light = entity.TryGetComponent<PointLightComponent>())
			{
				Utils::AddLight(snapshot,
					LightData{
						.Type = RenderLightType::Point,
						.Color = light->Color,
						.Intensity = light->Intensity,
						.Range = light->Range,
						.SourceRadius = light->SourceRadius,
						.CastShadows = light->CastShadows,
					},
					getWorld(), id);
			}
			if (const SpotLightComponent* light = entity.TryGetComponent<SpotLightComponent>())
			{
				Utils::AddLight(snapshot,
					LightData{
						.Type = RenderLightType::Spot,
						.Color = light->Color,
						.Intensity = light->Intensity,
						.Range = light->Range,
						.InnerConeAngle = light->InnerConeAngle,
						.OuterConeAngle = light->OuterConeAngle,
						.SourceRadius = light->SourceRadius,
						.CastShadows = light->CastShadows,
					},
					getWorld(), id);
			}
			if (const TextComponent* text = entity.TryGetComponent<TextComponent>(); text != nullptr && !text->Text.empty())
			{
				snapshot.Texts.push_back(TextItem{
					.Text = text->Text,
					.Font = text->Font.GetHandle(),
					.Size = text->Size,
					.Color = text->Color,
					.Space = Utils::ToRenderTextSpace(text->Space),
					.Anchor = text->Anchor,
					.Pivot = text->Pivot,
					.Offset = text->Offset,
					.Alignment = Utils::ToRenderTextAlignment(text->Alignment),
					.Billboard = text->Billboard,
					.World = getWorld(),
					.Entity = id,
				});
			}
			if (const EnvironmentComponent* component = entity.TryGetComponent<EnvironmentComponent>(); component != nullptr && !environment.has_value())
			{
				environment = RenderEnvironment{
					.Environment = component->Environment.GetHandle(),
					.Intensity = component->Intensity,
					.Rotation = component->Rotation,
					.ShowSkybox = component->ShowSkybox,
					.SkyboxBlur = component->SkyboxBlur,
					.FallbackColor = component->FallbackColor,
				};
			}
			if (const PostProcessComponent* component = entity.TryGetComponent<PostProcessComponent>(); component != nullptr && !post.has_value())
				post = Utils::MakePostProcessSettings(*component);
			// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 4): the first effectively enabled Fog, as Environment and PostProcess.
			if (const FogComponent* component = entity.TryGetComponent<FogComponent>(); component != nullptr && !fog.has_value())
			{
				fog = RenderFog{
					.Enabled = true,
					.Color = component->Color,
					.AmbientTint = component->AmbientTint,
					.Density = component->Density,
					.StartDistance = component->StartDistance,
					.MaxOpacity = component->MaxOpacity,
					.HeightDensity = component->HeightDensity,
					.HeightFalloff = component->HeightFalloff,
					.BaseHeight = component->BaseHeight,
					.SunGlow = component->SunGlow,
					.SunGlowExponent = component->SunGlowExponent,
					.FogSkybox = component->FogSkybox,
				};
			}
			// M21 (Docs/Decisions/0032-m21-contract.md decision 4): listed in canonical order, sorted into draw order below.
			if (const SpriteRendererComponent* sprite = entity.TryGetComponent<SpriteRendererComponent>())
			{
				snapshot.Sprites.push_back(Utils::MakeSpriteItem(*sprite, getWorld(), id, request.SortingLayers));
				const SpriteItem& item = snapshot.Sprites.back();
				spriteKeys.push_back(Utils::SpriteDrawKey{ .SortingLayer = item.SortingLayer, .OrderInLayer = item.OrderInLayer, .Canonical = position });
			}
			// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 22): in canonical order, the order the Lights2D pass adds
			// them in, at the rendered world position.
			if (const Light2DComponent* light = entity.TryGetComponent<Light2DComponent>())
			{
				snapshot.Lights2D.push_back(Light2DItem{
					.Type = light->Type == Light2DType::Global ? RenderLight2DType::Global : RenderLight2DType::Point,
					.Position = glm::vec3(getWorld()[3]),
					.Color = light->Color,
					.Intensity = light->Intensity,
					.Radius = light->Radius,
					.Falloff = light->Falloff,
					.Height = light->Height,
					.Entity = id,
				});
			}
			// M21 (C21.2, decision 24): listed in canonical order, placed among the sprites below.
			if (const TilemapRendererComponent* tilemap = entity.TryGetComponent<TilemapRendererComponent>(); tilemap != nullptr && tilemap->Tilemap.IsValid())
			{
				snapshot.Tilemaps.push_back(Utils::MakeTilemapItem(*tilemap, getWorld(), id, request.SortingLayers));
				tilemapPositions.push_back(position);
			}
			// M26 (C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 7): an emitter with a runtime state (a play scene's,
			// once its first Particles phase made it; an edit scene holds none, so its views draw no particles), in canonical
			// order. Only a Local emitter's particles need its rendered matrix.
			if (const ParticleEmitterComponent* emitter = entity.TryGetComponent<ParticleEmitterComponent>(); emitter != nullptr)
			{
				if (const ParticleStateComponent* particles = entity.TryGetComponent<ParticleStateComponent>(); particles != nullptr)
				{
					const bool local = emitter->SimulationSpace == ParticleSimulationSpace::Local;
					Utils::AppendParticleItems(*emitter, *particles, local ? getWorld() : glm::mat4(1.0f), request.Alpha, id, snapshot);
				}
			}
			// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16): the baked probes, in canonical order, in
			// every view (an edit scene's included: baked lighting shows while editing).
			Utils::AddProbeItems(entity, id, getWorld, snapshot);
			// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22): a terrain with a heightfield, in canonical
			// order, in every view (an edit scene's included: terrain is edited in the scene view).
			if (const TerrainComponent* terrain = entity.TryGetComponent<TerrainComponent>(); terrain != nullptr && terrain->Heightfield.IsValid())
				snapshot.Terrains.push_back(Utils::MakeTerrainItem(*terrain, getWorld(), id));
		});
		Utils::AssignTerrainPickIds(snapshot);
		snapshot.Environment = environment.value_or(RenderEnvironment{});
		snapshot.Post = post.value_or(PostProcessSettings{});
		snapshot.Fog = fog.value_or(RenderFog{});
		Utils::SortSpritesForDrawing(snapshot.Sprites);
		Utils::PlaceTilemapsForDrawing(snapshot.Tilemaps, tilemapPositions, std::move(spriteKeys));
		// M22 (C22.1, Docs/Decisions/0033-m22-contract.md decision 7): the view's UI, laid out at the scene resolution of the
		// snapshot's Output (the view's size by default), for the game view and a camera's view. Its widgets take their theme's
		// tints from the session's interaction state (C22.2, decision 25), null outside a play session. C22.3 (decision 51):
		// World canvases draw in every view that draws the world, so the editor's scene view (Explicit) draws them and no
		// Screen canvas, each at the request's alpha, as the meshes (the milestone review).
		const RenderOutputMapping mapping = ComputeRenderOutputMapping(request.Width, request.Height, snapshot.Output);
		AppendUiDrawItems(scene, ComputeUiLayout(scene, glm::uvec2(mapping.SceneWidth, mapping.SceneHeight)), snapshot.Ui, request.UiInteraction,
			request.Camera == RenderCameraSource::Explicit, request.Alpha);
		// Moved, so the containers keep `resource` (a copy would allocate from the default resource).
		return Result<RenderSnapshot>(std::move(snapshot));
	}

}
