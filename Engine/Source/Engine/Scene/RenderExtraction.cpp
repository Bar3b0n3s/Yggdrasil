#include "EnginePCH.h"
#include "Engine/Scene/RenderExtraction.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/PostProcessComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SpotLightComponent.h"
#include "Engine/Scene/Components/TextComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <algorithm>
#include <optional>

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

		// The view matrix of `world`, a camera entity's rendered world matrix, with its scale removed: the rotation of its
		// decomposition, or the identity rotation for a matrix that does not decompose (a zero scale, which the
		// Transform.Scale field rule already rejects for authored data).
		static glm::mat4 MakeEntityViewMatrix(const glm::mat4& world)
		{
			const glm::vec3 position(world[3]);
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

		// The camera data of a CameraComponent seen through `world` (its rendered world matrix).
		static CameraData MakeEntityCameraData(ConstEntity entity, const CameraComponent& camera, const glm::mat4& world, uint32_t width,
			uint32_t height)
		{
			CameraData data;
			data.View = MakeEntityViewMatrix(world);
			data.Position = glm::vec3(world[3]);
			data.ProjectionKind = ToRenderProjection(camera.Projection);
			data.VerticalFov = camera.VerticalFov;
			data.OrthographicSize = camera.OrthographicSize;
			data.NearClip = camera.NearClip;
			data.FarClip = camera.FarClip;
			data.Projection = ComputeReverseZProjection(data.ProjectionKind, data.VerticalFov, data.OrthographicSize, data.NearClip, data.FarClip,
				width, height);
			data.ViewportWidth = width;
			data.ViewportHeight = height;
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
						request.Width, request.Height);
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
						request.Height);
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

	Result<RenderSnapshot> ExtractRenderSnapshot(const Scene& scene, const RenderExtractionRequest& request)
	{
		constexpr uint32_t ValidFlags = (1u << 7) - 1u;
		if ((std::to_underlying(request.Flags) & ~ValidFlags) != 0)
			return MakeError(ErrorCode::InvalidArgument, "render extraction has unknown view flags");
		if (request.Annotations.Labels > RenderAnnotationLabels::Explicit
			|| (request.Annotations.Labels != RenderAnnotationLabels::Explicit && !request.Annotations.LabelEntities.empty()))
			return MakeError(ErrorCode::InvalidArgument, "render extraction has invalid annotation labels");
		const uint32_t shadowSize = request.Quality.ShadowMapSize;
		if (shadowSize < 256 || shadowSize > 8192 || (shadowSize & (shadowSize - 1)) != 0)
			return MakeError(ErrorCode::InvalidArgument, "shadow map size must be a power of two from 256 to 8192");
		if (request.Width == 0 || request.Height == 0)
			return MakeError(ErrorCode::InvalidArgument, "a render extraction needs a view of at least 1x1 pixels, got {}x{}", request.Width, request.Height);
		if (!std::isfinite(request.Alpha) || request.Alpha < 0.0f || request.Alpha > 1.0f)
			return MakeError(ErrorCode::InvalidArgument, "a render extraction needs an interpolation alpha in [0, 1], got {}", request.Alpha);

		RenderSnapshot snapshot;
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
		scene.ForEachCanonical([&snapshot, &environment, &post, &request](ConstEntity entity)
		{
			if (!Utils::IsEffectivelyEnabled(entity))
				return;
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
						.CastShadows = false,
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
			{
				post = PostProcessSettings{
					.ExposureEV = component->ExposureEV,
					.Tonemap = Utils::ToRenderTonemapper(component->Tonemap),
					.SsaoEnabled = component->SsaoEnabled,
					.SsaoRadius = component->SsaoRadius,
					.SsaoIntensity = component->SsaoIntensity,
					.SsaoQuality = Utils::ToRenderSsaoQuality(component->SsaoQuality),
					.BloomEnabled = component->BloomEnabled,
					.BloomIntensity = component->BloomIntensity,
					.FxaaEnabled = component->FxaaEnabled,
				};
			}
		});
		snapshot.Environment = environment.value_or(RenderEnvironment{});
		snapshot.Post = post.value_or(PostProcessSettings{});
		return snapshot;
	}

}
