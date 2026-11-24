#include "TestsPCH.h"

#include "Engine/Scene/RenderExtraction.h"

#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Core/Hash.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/BoneAttachments.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/BoneAttachmentComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/FogComponent.h"
#include "Engine/Scene/Components/IrradianceVolumeComponent.h"
#include "Engine/Scene/Components/Light2DComponent.h"
#include "Engine/Scene/Components/MaterialParametersComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/ParticleEmitterComponent.h"
#include "Engine/Scene/Components/ParticleLookComponent.h"
#include "Engine/Scene/Components/ParticleStateComponent.h"
#include "Engine/Scene/Components/PixelPerfectCameraComponent.h"
#include "Engine/Scene/Components/PlanarReflectionComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/PostProcessComponent.h"
#include "Engine/Scene/Components/ReflectionProbeComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Components/SkyComponent.h"
#include "Engine/Scene/Components/SpotLightComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Components/TextComponent.h"
#include "Engine/Scene/Components/TilemapRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/ParticleSimulation.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/GlmApprox.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>
#include <format>
#include <limits>
#include <memory_resource>
#include <string_view>
#include <utility>
#include <vector>

// Render extraction (Architecture §5.2, §8.2): the snapshot of one view built from a scene, with the interpolation rule
// shared with TransformSystem's render pose (Docs/Decisions/0012-m7-decisions.md decisions 5 and 6).

namespace Engine {

	namespace {

		// A MeshRenderer drawing `mesh` with its default materials.
		MeshRendererComponent MakeMeshRenderer(AssetHandle mesh)
		{
			MeshRendererComponent renderer;
			renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(mesh);
			return renderer;
		}

		Entity CreateCamera(Scene& scene, std::string_view name, bool primary)
		{
			Entity camera = scene.CreateEntity(name);
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = primary });
			return camera;
		}

		void SetTranslation(Entity entity, const glm::vec3& translation)
		{
			entity.Patch<TransformComponent>([translation](TransformComponent& transform)
			{
				transform.Translation = translation;
			});
		}

		// Gives `entity` its current world matrix as its previous pose, as the play session's interpolation snapshot does.
		void SnapshotPreviousPose(Entity entity)
		{
			entity.AddComponent<PreviousWorldTransformComponent>(PreviousWorldTransformComponent{ .Matrix = entity.GetComponent<WorldTransformComponent>().Matrix });
		}

		// M41 (Docs/Decisions/0058-m41-bone-attachments.md decision R): a rig of hips one unit up and a hand one above them, a
		// one-second walk of the hips 2 m along +Z and a one-second swing of the hand to 90 degrees about +Z and back.
		constexpr AssetHandle RigSkeletonHandle(0xb0e5000000000101);
		constexpr AssetHandle RigWalkHandle(0xb0e5000000000102);
		constexpr AssetHandle RigSwingHandle(0xb0e5000000000103);

		void PublishWalkingRig(Test::InMemoryAssetManager& assets)
		{
			Ref<SkeletonData> skeleton = CreateRef<SkeletonData>();
			skeleton->Joints.push_back({ .Name = "Hips", .Translation = { 0.0f, 1.0f, 0.0f }, .InverseBind = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)) });
			skeleton->Joints.push_back({ .Name = "Hand", .Parent = 0, .Translation = { 0.0f, 1.0f, 0.0f }, .InverseBind = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -2.0f, 0.0f)) });
			assets.Publish(RigSkeletonHandle, skeleton, "Assets/Models/Warden.gltf");
			Ref<AnimationClipData> walk = CreateRef<AnimationClipData>();
			walk->Duration = 1.0f;
			walk->Channels.push_back(AnimationChannel{ .Target = "Hips", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 1.0f }, .Values = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 2.0f } });
			assets.Publish(RigWalkHandle, walk, "Assets/Models/Warden.gltf");
			const float half = 0.70710678f;
			Ref<AnimationClipData> swing = CreateRef<AnimationClipData>();
			swing->Duration = 1.0f;
			swing->Channels.push_back(AnimationChannel{ .Target = "Hand", .Path = AnimationPath::Rotation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 0.5f, 1.0f }, .Values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, half, half, 0.0f, 0.0f, 0.0f, 1.0f } });
			assets.Publish(RigSwingHandle, swing, "Assets/Models/Warden.gltf");
		}

		// An Animator walking with root motion Apply (so its entity moves between ticks) and swinging the hand fast.
		AnimatorComponent MakeWalkingAnimator()
		{
			AnimatorComponent animator;
			animator.Skeleton = TypedAssetHandle<AssetType::Skeleton>(RigSkeletonHandle);
			animator.Layers.push_back(AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(RigWalkHandle) });
			animator.Layers.push_back(AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(RigSwingHandle), .Time = 0.1f, .Speed = 4.0f });
			animator.RootMotion = AnimatorRootMotion::Apply;
			return animator;
		}

		// Every entity's current world matrix as its previous pose (the play session's interpolation snapshot, phase 0).
		void SnapshotPreviousPoses(Scene& scene)
		{
			entt::registry& registry = scene.GetRegistry();
			for (const auto [entity, world] : registry.view<WorldTransformComponent>().each())
				registry.emplace_or_replace<PreviousWorldTransformComponent>(entity, world.Matrix);
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderExtraction: the primary camera is the first effectively enabled one in canonical order")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(CreateCamera(scene, "Secondary", false));
			Entity disabled = CreateCamera(scene, "DisabledPrimary", true);
			disabled.SetActive(false);
			const Entity primary = CreateCamera(scene, "Primary", true);
			static_cast<void>(CreateCamera(scene, "LaterPrimary", true));
			CHECK(FindPrimaryCamera(scene) == ConstEntity(primary));

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 640, .Height = 360 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->HasCamera);
			CHECK(snapshot->Camera.Entity == primary.GetUUID());
			CHECK(snapshot->Camera.ViewportWidth == 640);
		}

		TEST_CASE("RenderExtraction: a game view without a primary camera has no camera and is no error")
		{
			Test::SceneTestFixture fixture;
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK_FALSE(snapshot->HasCamera);
			CHECK_FALSE(FindPrimaryCamera(fixture.GetScene()).IsValid());
		}

		TEST_CASE("RenderExtraction: visible meshes and lights of enabled entities are extracted in canonical order")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity first = scene.CreateEntity("First");
			first.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			Entity hidden = scene.CreateEntity("Hidden");
			MeshRendererComponent& invisible = hidden.AddComponent<MeshRendererComponent>();
			invisible.Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			invisible.Visible = false;
			Entity disabled = scene.CreateEntity("Disabled");
			disabled.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::SphereMesh);
			disabled.SetActive(false);
			Entity sun = scene.CreateEntity("Sun");
			sun.AddComponent<DirectionalLightComponent>();
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 1);
			CHECK(snapshot->Meshes[0].Entity == first.GetUUID());
			REQUIRE(snapshot->Lights.size() == 1);
			CHECK(snapshot->Lights[0].Type == RenderLightType::Directional);
			CHECK(Test::ApproxEqual(snapshot->Lights[0].Direction, glm::vec3(0.0f, 0.0f, -1.0f), 1e-6f));
		}

		TEST_CASE("RenderExtraction: meshes keep their materials and flags, and children of disabled entities are left out")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity parent = scene.CreateEntity("Parent");
			SetTranslation(parent, glm::vec3(1.0f, 2.0f, 3.0f));
			Entity child = scene.CreateEntity("Child", parent);
			MeshRendererComponent& renderer = child.AddComponent<MeshRendererComponent>();
			renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::SphereMesh);
			renderer.Materials = { TypedAssetHandle<AssetType::Material>(BuiltinAssetHandles::ErrorMaterial), {} };
			renderer.CastShadows = false;
			Entity noMesh = scene.CreateEntity("NoMesh");
			static_cast<void>(noMesh.AddComponent<MeshRendererComponent>());
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 1);
			const MeshDrawItem& item = snapshot->Meshes[0];
			CHECK(item.Entity == child.GetUUID());
			CHECK(item.Mesh == BuiltinAssetHandles::SphereMesh);
			REQUIRE(item.Materials.size() == 2);
			CHECK(item.Materials[0] == BuiltinAssetHandles::ErrorMaterial);
			CHECK_FALSE(item.Materials[1].IsValid());
			CHECK_FALSE(item.CastShadows);
			CHECK(item.ReceiveShadows);
			CHECK(Test::ApproxEqual(glm::vec3(item.World[3]), glm::vec3(1.0f, 2.0f, 3.0f), 1e-6f));

			parent.SetActive(false);
			const Result<RenderSnapshot> withoutChild = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(withoutChild.has_value(), withoutChild.error().ToString());
			CHECK(withoutChild->Meshes.empty());
		}

		TEST_CASE("RenderExtraction: lights take their pose from the rendered world matrix and their fields from the component")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity sun = scene.CreateEntity("Sun");
			sun.AddComponent<DirectionalLightComponent>(DirectionalLightComponent{ .Color = glm::vec3(1.0f, 0.5f, 0.25f), .Intensity = 2.0f });
			// Rotated 90 degrees about +Y: local -Z points along world -X.
			sun.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 90.0f, 0.0f));
			});
			Entity lamp = scene.CreateEntity("Lamp");
			SetTranslation(lamp, glm::vec3(0.0f, 4.0f, 0.0f));
			lamp.AddComponent<PointLightComponent>(PointLightComponent{ .Range = 7.0f, .CastShadows = true });
			Entity spot = scene.CreateEntity("Spot");
			spot.AddComponent<SpotLightComponent>(SpotLightComponent{ .InnerConeAngle = 10.0f, .OuterConeAngle = 25.0f, .CastShadows = true });
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Lights.size() == 3);
			const LightData& directional = snapshot->Lights[0];
			CHECK(directional.Type == RenderLightType::Directional);
			CHECK(directional.Entity == sun.GetUUID());
			CHECK(directional.Intensity == 2.0f);
			CHECK(Test::ApproxEqual(directional.Color, glm::vec3(1.0f, 0.5f, 0.25f), 1e-6f));
			CHECK(Test::ApproxEqual(directional.Direction, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-5f));
			const LightData& point = snapshot->Lights[1];
			CHECK(point.Type == RenderLightType::Point);
			CHECK(point.Range == 7.0f);
			CHECK(Test::ApproxEqual(point.Position, glm::vec3(0.0f, 4.0f, 0.0f), 1e-6f));
			CHECK(point.CastShadows); // PointLight version 2 (M23, R-PLS)
			const LightData& cone = snapshot->Lights[2];
			CHECK(cone.Type == RenderLightType::Spot);
			CHECK(cone.InnerConeAngle == 10.0f);
			CHECK(cone.OuterConeAngle == 25.0f);
			CHECK(cone.CastShadows);
		}

		TEST_CASE("RenderExtraction: the environment and post-process settings are the first enabled components, or the defaults")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Result<RenderSnapshot> defaults = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(defaults.has_value(), defaults.error().ToString());
			CHECK(defaults->Environment.FallbackColor == RenderEnvironment{}.FallbackColor);
			CHECK(defaults->Post.Tonemap == RenderTonemapper::AgX);

			Entity disabled = scene.CreateEntity("DisabledLook");
			disabled.AddComponent<PostProcessComponent>(PostProcessComponent{ .ExposureEV = 5.0f });
			disabled.SetActive(false);
			Entity look = scene.CreateEntity("Look");
			look.AddComponent<EnvironmentComponent>(EnvironmentComponent{ .Environment = {}, .Intensity = 0.5f, .FallbackColor = glm::vec3(0.1f, 0.2f, 0.3f) });
			look.AddComponent<PostProcessComponent>(PostProcessComponent{ .ExposureEV = -1.0f, .Tonemap = Tonemapper::Linear });

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Environment.Intensity == 0.5f);
			CHECK(snapshot->Environment.FallbackColor == glm::vec3(0.1f, 0.2f, 0.3f));
			CHECK(snapshot->Post.ExposureEV == -1.0f);
			CHECK(snapshot->Post.Tonemap == RenderTonemapper::Linear);
		}

		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 4): the first effectively enabled Fog in canonical order is the
		// snapshot's, every field as it is; without one (or with only a disabled one) the fog is disabled, at its defaults.
		TEST_CASE("RenderExtraction: the first enabled Fog is the snapshot's fog, and a scene without one has none")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity disabled = scene.CreateEntity("DisabledFog");
			disabled.AddComponent<FogComponent>(FogComponent{ .Density = 5.0f });
			disabled.SetActive(false);
			TransformSystem::Update(scene);
			Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK_FALSE(snapshot->Fog.Enabled);
			CHECK(snapshot->Fog.Density == RenderFog{}.Density);

			Entity haze = scene.CreateEntity("Haze");
			haze.AddComponent<FogComponent>(FogComponent{ .Color = glm::vec3(0.25f, 0.5f, 0.75f),
				.AmbientTint = 0.5f,
				.Density = 0.1f,
				.StartDistance = 4.0f,
				.MaxOpacity = 0.8f,
				.HeightDensity = 0.3f,
				.HeightFalloff = 2.0f,
				.BaseHeight = -1.5f,
				.SunGlow = 3.0f,
				.SunGlowExponent = 16.0f,
				.FogSkybox = false });
			Entity second = scene.CreateEntity("SecondFog");
			second.AddComponent<FogComponent>(FogComponent{ .Density = 9.0f });
			TransformSystem::Update(scene);
			snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			const RenderFog& fog = snapshot->Fog;
			CHECK(fog.Enabled);
			CHECK(fog.Color == glm::vec3(0.25f, 0.5f, 0.75f));
			CHECK(fog.AmbientTint == 0.5f);
			CHECK(fog.Density == 0.1f);
			CHECK(fog.StartDistance == 4.0f);
			CHECK(fog.MaxOpacity == 0.8f);
			CHECK(fog.HeightDensity == 0.3f);
			CHECK(fog.HeightFalloff == 2.0f);
			CHECK(fog.BaseHeight == -1.5f);
			CHECK(fog.SunGlow == 3.0f);
			CHECK(fog.SunGlowExponent == 16.0f);
			CHECK_FALSE(fog.FogSkybox);

			haze.SetActive(false);
			TransformSystem::Update(scene);
			snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Fog.Enabled);
			CHECK(snapshot->Fog.Density == 9.0f);
		}

		// M26 (C26.1, R-AEX; Docs/Decisions/0037-m26-contract.md decision 12, stream X-A): every member of PostProcess version 2
		// reaches the snapshot, its enums mapped to the renderer's.
		TEST_CASE("RenderExtraction: PostProcess version 2's members reach the snapshot with their enums mapped")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity look = scene.CreateEntity("Look");
			look.AddComponent<PostProcessComponent>(PostProcessComponent{ .ExposureEV = 1.5f,
				.ExposureMode = ExposureMode::Auto,
				.AutoExposureMinEV = -3.0f,
				.AutoExposureMaxEV = 5.0f,
				.AutoExposureSpeedUp = 4.0f,
				.AutoExposureSpeedDown = 0.5f,
				.Tonemap = Tonemapper::PbrNeutral,
				.Lut = TypedAssetHandle<AssetType::Lut>(UUID(0x0281)),
				.LutIntensity = 0.75f,
				.SsaoEnabled = false,
				.SsaoRadius = 0.8f,
				.SsaoIntensity = 1.5f,
				.SsaoQuality = SsaoQuality::High,
				.SsrEnabled = true,
				.SsrIntensity = 0.5f,
				.SsrMaxDistance = 20.0f,
				.SsrThickness = 0.1f,
				.SsrMaxRoughness = 0.4f,
				.BloomEnabled = false,
				.BloomIntensity = 0.1f,
				.AntiAliasing = AntiAliasing::Taa });

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			const PostProcessSettings& post = snapshot->Post;
			CHECK(post.ExposureEV == 1.5f);
			CHECK(post.ExposureMode == RenderExposureMode::Auto);
			CHECK(post.AutoExposureMinEV == -3.0f);
			CHECK(post.AutoExposureMaxEV == 5.0f);
			CHECK(post.AutoExposureSpeedUp == 4.0f);
			CHECK(post.AutoExposureSpeedDown == 0.5f);
			CHECK(post.Tonemap == RenderTonemapper::PbrNeutral);
			CHECK(post.Lut == AssetHandle(UUID(0x0281)));
			CHECK(post.LutIntensity == 0.75f);
			CHECK_FALSE(post.SsaoEnabled);
			CHECK(post.SsaoRadius == 0.8f);
			CHECK(post.SsaoIntensity == 1.5f);
			CHECK(post.SsaoQuality == RenderSsaoQuality::High);
			CHECK(post.SsrEnabled);
			CHECK(post.SsrIntensity == 0.5f);
			CHECK(post.SsrMaxDistance == 20.0f);
			CHECK(post.SsrThickness == 0.1f);
			CHECK(post.SsrMaxRoughness == 0.4f);
			CHECK_FALSE(post.BloomEnabled);
			CHECK(post.BloomIntensity == 0.1f);
			CHECK(post.AntiAliasing == RenderAntiAliasing::Taa);

			look.Patch<PostProcessComponent>([](PostProcessComponent& component)
			{
				component.ExposureMode = ExposureMode::Manual;
				component.AntiAliasing = AntiAliasing::None;
			});
			const Result<RenderSnapshot> manual = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(manual.has_value(), manual.error().ToString());
			CHECK(manual->Post.ExposureMode == RenderExposureMode::Manual);
			CHECK(manual->Post.AntiAliasing == RenderAntiAliasing::None);
		}

		TEST_CASE("RenderExtraction: a camera's view matrix is the inverse of its pose without scale, and its projection reverse-Z")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity camera = CreateCamera(scene, "Camera", true);
			camera.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 1.0f, 5.0f);
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 90.0f, 0.0f));
				transform.Scale = glm::vec3(2.0f);
			});
			camera.Patch<CameraComponent>([](CameraComponent& component)
			{
				component.ClearColor = glm::vec3(0.5f, 0.25f, 0.125f);
				component.Clear = ClearMode::Color;
			});
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 200, .Height = 100 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			const CameraData& data = snapshot->Camera;
			CHECK(Test::ApproxEqual(data.Position, glm::vec3(0.0f, 1.0f, 5.0f), 1e-6f));
			// The camera's position maps to the view-space origin, and its -Z axis (world -X here) to view -Z, at unit scale.
			CHECK(Test::ApproxEqual(glm::vec3(data.View * glm::vec4(data.Position, 1.0f)), glm::vec3(0.0f), 1e-5f));
			CHECK(Test::ApproxEqual(glm::vec3(data.View * glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f)), glm::vec3(0.0f, 0.0f, -1.0f), 1e-5f));
			CHECK(Test::ApproxEqual(data.Projection, ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 1000.0f, 200, 100), 1e-6f));
			CHECK(data.ClearColor == glm::vec3(0.5f, 0.25f, 0.125f));
			CHECK_FALSE(data.ClearToSkybox);
		}

		TEST_CASE("RenderExtraction: entity and explicit cameras render through their own pose, and invalid ones are errors")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity camera = CreateCamera(scene, "Side", false);
			const Entity notCamera = scene.CreateEntity("NotACamera");

			RenderExtractionRequest request{ .Camera = RenderCameraSource::Entity, .CameraEntity = camera.GetUUID(), .Width = 32, .Height = 32 };
			const Result<RenderSnapshot> side = ExtractRenderSnapshot(scene, request);
			REQUIRE_MESSAGE(side.has_value(), side.error().ToString());
			CHECK(side->Camera.Entity == camera.GetUUID());

			request.CameraEntity = notCamera.GetUUID();
			const Result<RenderSnapshot> wrong = ExtractRenderSnapshot(scene, request);
			REQUIRE_FALSE(wrong.has_value());
			CHECK(wrong.error().GetCode() == ErrorCode::InvalidArgument);

			request.CameraEntity = UUID(0x1234567890abcdefULL);
			const Result<RenderSnapshot> missing = ExtractRenderSnapshot(scene, request);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);

			const Result<RenderSnapshot> editor = ExtractRenderSnapshot(scene, { .Camera = RenderCameraSource::Explicit, .Width = 32, .Height = 32 });
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			CHECK(editor->HasCamera);
			CHECK(editor->Camera.Entity == UUID());
			CHECK(Test::ApproxEqual(editor->Camera.Position, ExplicitRenderCamera{}.Position, 1e-6f));

			const Result<RenderSnapshot> empty = ExtractRenderSnapshot(scene, { .Width = 0, .Height = 32 });
			REQUIRE_FALSE(empty.has_value());
			CHECK(empty.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("RenderExtraction: an explicit camera looks at its target, also straight down, and may not sit on it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			RenderExtractionRequest request{ .Camera = RenderCameraSource::Explicit, .Width = 16, .Height = 16 };
			const Result<RenderSnapshot> standard = ExtractRenderSnapshot(scene, request);
			REQUIRE_MESSAGE(standard.has_value(), standard.error().ToString());
			// The default camera at (0, 3, 10) looks at the origin: the origin is straight ahead, on the view's -Z axis.
			const glm::vec3 origin(standard->Camera.View * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
			CHECK(Test::ApproxEqual(glm::vec2(origin), glm::vec2(0.0f), 1e-5f));
			CHECK(origin.z < 0.0f);

			// Looking straight down: the view's up is world -Z, and the target is still ahead.
			request.ExplicitCamera.Position = glm::vec3(0.0f, 10.0f, 0.0f);
			const Result<RenderSnapshot> top = ExtractRenderSnapshot(scene, request);
			REQUIRE_MESSAGE(top.has_value(), top.error().ToString());
			CHECK(Test::ApproxEqual(glm::vec3(top->Camera.View * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)), glm::vec3(0.0f, 1.0f, 0.0f), 1e-5f));
			CHECK(Test::ApproxEqual(glm::vec3(top->Camera.View * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)), glm::vec3(0.0f, 0.0f, -10.0f), 1e-5f));

			request.ExplicitCamera.Target = request.ExplicitCamera.Position;
			const Result<RenderSnapshot> degenerate = ExtractRenderSnapshot(scene, request);
			REQUIRE_FALSE(degenerate.has_value());
			CHECK(degenerate.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("RenderExtraction: an alpha outside [0, 1] or not finite is InvalidArgument")
		{
			Test::SceneTestFixture fixture;
			for (const float alpha : { -0.25f, 1.5f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
			{
				CAPTURE(alpha);
				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), { .Width = 8, .Height = 8, .Alpha = alpha });
				REQUIRE_FALSE(snapshot.has_value());
				CHECK(snapshot.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), { .Width = 8, .Height = 8, .Alpha = 0.75f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Alpha == 0.75f);
		}

		// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 3): the request's simulation time is validated as its
		// alpha is.
		TEST_CASE("RenderExtraction: a simulation time that is negative or not finite is InvalidArgument")
		{
			Test::SceneTestFixture fixture;
			for (const double time : { -0.5, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
			{
				CAPTURE(time);
				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), { .Width = 8, .Height = 8, .SimulationTime = time });
				REQUIRE_FALSE(snapshot.has_value());
				CHECK(snapshot.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), { .Width = 8, .Height = 8, .SimulationTime = 12.5 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
		}

		// M31 (decision 3): a MeshRenderer's MaterialParameters reach its item resolved at the request's simulation time: the
		// emissions times their strengths, the offset scrolled and wrapped into [0, 1), the edge only while it dissolves; an
		// entity without the component, and one with the component's defaults at any time, get the neutral parameters.
		TEST_CASE("RenderExtraction: a mesh's MaterialParameters reach its item, its offset scrolled by the simulation time and wrapped")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity plain = scene.CreateEntity("Plain");
			plain.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::CubeMesh));
			Entity lava = scene.CreateEntity("Lava");
			lava.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::PlaneMesh));
			lava.AddComponent<MaterialParametersComponent>(MaterialParametersComponent{ .Tint = glm::vec3(1.0f, 0.5f, 0.25f),
				.Emissive = glm::vec3(1.0f, 0.5f, 0.0f),
				.EmissiveStrength = 4.0f,
				.UVOffset = glm::vec2(0.25f, -0.5f),
				.UVScroll = glm::vec2(0.5f, -0.25f),
				.DissolveAmount = 0.5f,
				.DissolveEdgeColor = glm::vec3(0.0f, 0.5f, 1.0f),
				.DissolveEdgeStrength = 2.0f,
				.DissolveEdgeWidth = 0.125f,
				.DissolveNoiseScale = 3.0f,
				.Opacity = 0.75f });
			Entity defaults = scene.CreateEntity("Defaults");
			defaults.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::SphereMesh));
			static_cast<void>(defaults.AddComponent<MaterialParametersComponent>());
			TransformSystem::Update(scene);

			const auto extract = [&scene](double time)
			{
				Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8, .SimulationTime = time });
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				REQUIRE(snapshot->Meshes.size() == 3);
				return std::move(*snapshot);
			};
			const RenderSnapshot atStart = extract(0.0);
			CHECK(atStart.Meshes[0].Parameters.IsNeutral());
			CHECK(atStart.Meshes[2].Parameters.IsNeutral());
			const MeshMaterialParameters& resolved = atStart.Meshes[1].Parameters;
			CHECK(resolved.Tint == glm::vec3(1.0f, 0.5f, 0.25f));
			CHECK(resolved.Emissive == glm::vec3(4.0f, 2.0f, 0.0f));
			CHECK(resolved.UVOffset == glm::vec2(0.25f, 0.5f)); // -0.5 wraps to 0.5
			CHECK(resolved.DissolveAmount == 0.5f);
			CHECK(resolved.DissolveEdge == glm::vec3(0.0f, 1.0f, 2.0f));
			CHECK(resolved.DissolveEdgeWidth == 0.125f);
			CHECK(resolved.DissolveNoiseScale == 3.0f);
			CHECK(resolved.Opacity == 0.75f);

			// 3 s later the offset moved (1.5, -0.75) and wrapped: (0.75, 0.75); after an hour of 60 Hz ticks it stays exact.
			const RenderSnapshot later = extract(3.0);
			CHECK(later.Meshes[1].Parameters.UVOffset == glm::vec2(0.75f, 0.75f));
			CHECK(later.Meshes[2].Parameters.IsNeutral());
			const RenderSnapshot hour = extract(3600.0);
			CHECK(hour.Meshes[1].Parameters.UVOffset == glm::vec2(0.25f, 0.5f));
			CHECK(hour.Meshes[2].Parameters.IsNeutral());

			// Without a dissolve the edge is neutral whatever its fields hold, so the parameters are neutral unless something
			// else is set.
			MaterialParametersComponent undissolved;
			undissolved.DissolveEdgeColor = glm::vec3(0.0f, 1.0f, 0.0f);
			undissolved.DissolveEdgeWidth = 0.5f;
			undissolved.DissolveNoiseScale = 8.0f;
			CHECK(ResolveMaterialParameters(undissolved, 7.25).IsNeutral());
		}

		// M31 (decision 3): a wrapped offset never reaches 1, also where the scrolled value lies a hair below a whole number.
		TEST_CASE("RenderExtraction: a scrolled offset wraps into [0, 1) at any time")
		{
			MaterialParametersComponent component;
			component.UVScroll = glm::vec2(-1.0f / 3.0f, 0.1f);
			for (const double time : { 0.0, 1.0 / 60.0, 3.0, 2.5e-8, 1000.0 / 3.0, 12345.678 })
			{
				CAPTURE(time);
				const glm::vec2 offset = ResolveMaterialParameters(component, time).UVOffset;
				CHECK(offset.x >= 0.0f);
				CHECK(offset.x < 1.0f);
				CHECK(offset.y >= 0.0f);
				CHECK(offset.y < 1.0f);
			}
		}

		TEST_CASE("RenderExtraction: runtime scenes interpolate between the previous and the current pose")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity moving = scene.CreateEntity("Moving");
			Entity child = scene.CreateEntity("Child", moving);
			TransformSystem::Update(scene);
			SnapshotPreviousPose(moving);
			SnapshotPreviousPose(child);
			SetTranslation(moving, glm::vec3(4.0f, 0.0f, 0.0f));
			TransformSystem::Update(scene);

			CHECK(ComputeRenderedWorldMatrix(moving, 0.25f)[3].x == doctest::Approx(1.0f));
			CHECK(ComputeRenderedWorldMatrix(child, 0.25f)[3].x == doctest::Approx(1.0f));

			// A reset tag on the parent stops interpolation for its subtree.
			moving.AddComponent<InterpolationResetTag>();
			CHECK(ComputeRenderedWorldMatrix(moving, 0.25f)[3].x == doctest::Approx(4.0f));
			CHECK(ComputeRenderedWorldMatrix(child, 0.25f)[3].x == doctest::Approx(4.0f));
		}

		TEST_CASE("RenderExtraction: edit scenes and entities without a previous pose render at their current pose")
		{
			// An edit scene with a previous pose never interpolates.
			Test::SceneTestFixture edit;
			Entity edited = edit.GetScene().CreateEntity("Edited");
			TransformSystem::Update(edit.GetScene());
			SnapshotPreviousPose(edited);
			SetTranslation(edited, glm::vec3(4.0f, 0.0f, 0.0f));
			TransformSystem::Update(edit.GetScene());
			CHECK(ComputeRenderedWorldMatrix(edited, 0.25f)[3].x == doctest::Approx(4.0f));

			// A runtime entity created since the last snapshot has no previous pose; one without a world matrix yet renders at
			// its chain-walked pose.
			Test::SceneTestFixture runtime(1, true);
			Entity created = runtime.GetScene().CreateEntity("Created");
			SetTranslation(created, glm::vec3(0.0f, 2.0f, 0.0f));
			CHECK_FALSE(created.HasComponent<WorldTransformComponent>());
			CHECK(ComputeRenderedWorldMatrix(created, 0.5f)[3].y == doctest::Approx(2.0f));
			TransformSystem::Update(runtime.GetScene());
			CHECK(ComputeRenderedWorldMatrix(created, 0.5f)[3].y == doctest::Approx(2.0f));

			// Extraction uses the request's alpha for meshes and cameras alike.
			created.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			SnapshotPreviousPose(created);
			SetTranslation(created, glm::vec3(0.0f, 6.0f, 0.0f));
			TransformSystem::Update(runtime.GetScene());
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(runtime.GetScene(), { .Width = 8, .Height = 8, .Alpha = 0.5f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 1);
			CHECK(snapshot->Meshes[0].World[3].y == doctest::Approx(4.0f));
		}

		TEST_CASE("RenderExtraction: the rendered pose is TransformSystem's render position and rotation")
		{
			// One rule for the image and for scripts' Transform.RenderPosition/RenderRotation (RenderExtraction.h), the
			// fallback for a previous pose that does not decompose included.
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity turning = scene.CreateEntity("Turning");
			Entity shrunk = scene.CreateEntity("Shrunk");
			TransformSystem::Update(scene);
			SnapshotPreviousPose(turning);
			// A previous pose with a zero scale cannot be decomposed: the translation still interpolates, the rotation is the
			// current one.
			shrunk.AddComponent<PreviousWorldTransformComponent>(PreviousWorldTransformComponent{ .Matrix = glm::mat4(0.0f) });
			for (Entity entity : { turning, shrunk })
			{
				entity.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(2.0f, 4.0f, -6.0f);
					transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 90.0f, 0.0f));
				});
			}
			TransformSystem::Update(scene);
			scene.SetInterpolationAlpha(0.5f);

			for (const Entity entity : { turning, shrunk })
			{
				INFO(std::string(entity.GetName()));
				const glm::mat4 rendered = ComputeRenderedWorldMatrix(entity, 0.5f);
				CHECK(Test::ApproxEqual(glm::vec3(rendered[3]), TransformSystem::GetRenderPosition(entity), 1e-5f));
				const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(rendered);
				REQUIRE(decomposed.has_value());
				// q and -q are the same rotation: compare what they do to a vector.
				const glm::vec3 axis(1.0f, 0.0f, 0.0f);
				CHECK(Test::ApproxEqual(decomposed->Rotation * axis, TransformSystem::GetRenderRotation(entity) * axis, 1e-5f));
			}
			CHECK(Test::ApproxEqual(glm::vec3(ComputeRenderedWorldMatrix(shrunk, 0.5f)[3]), glm::vec3(1.0f, 2.0f, -3.0f), 1e-5f));
		}

		TEST_CASE("RenderExtraction: interpolated scale lerps between the poses")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity growing = scene.CreateEntity("Growing");
			TransformSystem::Update(scene);
			SnapshotPreviousPose(growing);
			growing.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(3.0f);
			});
			TransformSystem::Update(scene);
			const Result<TransformDecomposition> halfway = TransformSystem::DecomposeMatrix(ComputeRenderedWorldMatrix(growing, 0.5f));
			REQUIRE(halfway.has_value());
			CHECK(Test::ApproxEqual(halfway->Scale, glm::vec3(2.0f), 1e-5f));
			// Alpha 0 is the previous pose and alpha 1 the current one.
			CHECK(Test::ApproxEqual(ComputeRenderedWorldMatrix(growing, 0.0f), glm::mat4(1.0f), 1e-6f));
			CHECK(ComputeRenderedWorldMatrix(growing, 1.0f) == growing.GetComponent<WorldTransformComponent>().Matrix);
		}

		// M8 (Docs/Decisions/0013-m8-decisions.md decision 11): texts.

		TEST_CASE("RenderExtraction: texts of enabled entities are extracted in canonical order with every field")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity title = scene.CreateEntity("Title");
			title.AddComponent<TextComponent>(TextComponent{ .Text = "Score: 10",
				.Font = TypedAssetHandle<AssetType::Font>(AssetHandle(0x5555)),
				.Size = 48.0f,
				.Color = glm::vec4(1.0f, 0.5f, 0.25f, 0.75f),
				.Space = TextSpace::Screen,
				.Anchor = glm::vec2(0.5f, 0.0f),
				.Pivot = glm::vec2(0.5f, 0.0f),
				.Offset = glm::vec2(0.0f, 24.0f),
				.Alignment = TextAlignment::Left,
				.Billboard = false });
			Entity empty = scene.CreateEntity("Empty");
			empty.AddComponent<TextComponent>(); // no text: not extracted
			Entity hidden = scene.CreateEntity("Hidden");
			hidden.AddComponent<TextComponent>().Text = "Hidden";
			hidden.SetActive(false);
			Entity label = scene.CreateEntity("Label");
			label.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			});
			TextComponent& labelText = label.AddComponent<TextComponent>();
			labelText.Text = "World";
			labelText.Space = TextSpace::World;
			labelText.Alignment = TextAlignment::Right;
			labelText.Billboard = true;
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Texts.size() == 2);
			const TextItem& screen = snapshot->Texts[0];
			CHECK(screen.Entity == title.GetUUID());
			CHECK(screen.Text == "Score: 10");
			CHECK(screen.Font == AssetHandle(0x5555));
			CHECK(screen.Size == 48.0f);
			CHECK(screen.Color == glm::vec4(1.0f, 0.5f, 0.25f, 0.75f));
			CHECK(screen.Space == RenderTextSpace::Screen);
			CHECK(screen.Anchor == glm::vec2(0.5f, 0.0f));
			CHECK(screen.Pivot == glm::vec2(0.5f, 0.0f));
			CHECK(screen.Offset == glm::vec2(0.0f, 24.0f));
			CHECK(screen.Alignment == RenderTextAlignment::Left);
			const TextItem& world = snapshot->Texts[1];
			CHECK(world.Entity == label.GetUUID());
			CHECK(world.Space == RenderTextSpace::World);
			CHECK(world.Alignment == RenderTextAlignment::Right);
			CHECK(world.Billboard);
			CHECK(Test::ApproxEqual(glm::vec3(world.World[3]), glm::vec3(1.0f, 2.0f, 3.0f), 1e-6f));
			// Extraction adds no debug primitives and leaves the debug view Lit.
			CHECK(snapshot->DebugDraw.IsEmpty());
			CHECK(snapshot->DebugView == RenderDebugView::Lit);

			// M38 (Docs/Decisions/0055-m38-runtime-locale.md decision 11): the Text component lays out unshaped and left to right,
			// so its Start extracts as Left and its End as Right.
			title.Patch<TextComponent>([](TextComponent& text)
			{
				text.Alignment = TextAlignment::Start;
			});
			label.Patch<TextComponent>([](TextComponent& text)
			{
				text.Alignment = TextAlignment::End;
			});
			const Result<RenderSnapshot> directional = ExtractRenderSnapshot(scene, { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(directional.has_value(), directional.error().ToString());
			REQUIRE(directional->Texts.size() == 2);
			CHECK(directional->Texts[0].Alignment == RenderTextAlignment::Left);
			CHECK(directional->Texts[1].Alignment == RenderTextAlignment::Right);
		}

		TEST_CASE("RenderExtraction: world texts in a play scene render at the interpolated pose")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity label = scene.CreateEntity("Label");
			TextComponent& moving = label.AddComponent<TextComponent>();
			moving.Text = "Moving";
			moving.Space = TextSpace::World;
			TransformSystem::Update(scene);
			SnapshotPreviousPose(label);
			SetTranslation(label, glm::vec3(4.0f, 0.0f, 0.0f));
			TransformSystem::Update(scene);
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8, .Alpha = 0.25f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Texts.size() == 1);
			CHECK(Test::ApproxEqual(glm::vec3(snapshot->Texts[0].World[3]), glm::vec3(1.0f, 0.0f, 0.0f), 1e-6f));
		}

		TEST_CASE("RenderExtraction: a snapshot extracted over a memory resource allocates from it and equals the default one")
		{
			// R-ARENA (Docs/Decisions/0029-m18-contract.md decision 9): a host extracts into a per-frame arena through the
			// resource; the values are the same as over the global heap.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(CreateCamera(scene, "Camera", true));
			Entity mesh = scene.CreateEntity("Mesh");
			mesh.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			mesh.AddComponent<PointLightComponent>();
			TextComponent& text = mesh.AddComponent<TextComponent>();
			text.Text = "Arena";
			TransformSystem::Update(scene);

			std::pmr::monotonic_buffer_resource arena;
			const Result<RenderSnapshot> overArena = ExtractRenderSnapshot(scene, { .Width = 64, .Height = 64 }, &arena);
			REQUIRE_MESSAGE(overArena.has_value(), overArena.error().ToString());
			CHECK(overArena->Meshes.get_allocator().resource() == &arena);
			CHECK(overArena->Lights.get_allocator().resource() == &arena);
			CHECK(overArena->Texts.get_allocator().resource() == &arena);
			CHECK(overArena->PickTable.get_allocator().resource() == &arena);
			CHECK(overArena->DebugDraw.GetMemoryResource() == &arena);
			const Result<RenderSnapshot> overHeap = ExtractRenderSnapshot(scene, { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(overHeap.has_value(), overHeap.error().ToString());
			CHECK(overHeap->Meshes.get_allocator().resource() == std::pmr::get_default_resource());
			REQUIRE(overArena->Meshes.size() == 1);
			REQUIRE(overHeap->Meshes.size() == 1);
			CHECK(overArena->Meshes[0].Entity == overHeap->Meshes[0].Entity);
			CHECK(overArena->Meshes[0].World == overHeap->Meshes[0].World);
			CHECK(std::ranges::equal(overArena->PickTable, overHeap->PickTable));
			REQUIRE(overArena->Lights.size() == 1);
			CHECK(overArena->Lights[0].Position == overHeap->Lights[0].Position);
			REQUIRE(overArena->Texts.size() == 1);
			CHECK(overArena->Texts[0].Text == "Arena");
			CHECK(overArena->Camera.View == overHeap->Camera.View);
		}

		// M23 (C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decisions 27 and 29, stream B): a SkinnedMesh's
		// item carries its driving Animator's skin matrices taken into the item's own space (the inverse of its rendered world
		// matrix times the Animator entity's, times each Skin matrix), and its morph weights, the pose's channel for its path
		// overriding the component's; an item without a pose has no palette (its bind pose), a MeshRenderer without a
		// SkinnedMesh neither.
		TEST_CASE("RenderExtraction: a SkinnedMesh's item carries its Animator's palette in its own space and its morph weights")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(CreateCamera(scene, "Camera", true));
			Entity rig = scene.CreateEntity("Rig");
			SetTranslation(rig, glm::vec3(2.0f, 0.0f, 0.0f));
			rig.AddComponent<AnimatorComponent>();
			Entity body = scene.CreateEntity("Body", rig);
			SetTranslation(body, glm::vec3(0.0f, 1.0f, 0.0f));
			body.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c1)));
			body.AddComponent<SkinnedMeshComponent>(SkinnedMeshComponent{ .MorphWeights = { 0.25f, 0.5f } });
			Entity plain = scene.CreateEntity("Plain");
			plain.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c2)));
			const glm::mat4 joint = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 3.0f));
			AnimationPoseComponent pose;
			pose.JointModel = { glm::mat4(1.0f), joint };
			pose.Skin = { glm::mat4(1.0f), joint };
			pose.Morphs = { AnimationMorphWeights{ .Target = "Body", .Weights = { 1.0f } } };
			rig.AddComponent<AnimationPoseComponent>(pose);
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, RenderExtractionRequest{ .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 2);
			const MeshDrawItem& skinned = snapshot->Meshes[0];
			REQUIRE(skinned.JointPalette.size() == 2);
			// The body sits one unit above the rig: its palette undoes that offset, so joint 0's matrix moves down by one.
			CHECK(skinned.JointPalette[0] == glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)));
			CHECK(skinned.JointPalette[1] == glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 3.0f)));
			// The pose's channel for "Body" overrides the first weight; the second keeps the component's.
			CHECK(skinned.MorphWeights == std::vector<float>{ 1.0f, 0.5f });
			CHECK(snapshot->Meshes[1].JointPalette.empty());
			CHECK(snapshot->Meshes[1].MorphWeights.empty());

			// Without a pose the skinned item draws its bind pose: no palette, the component's weights.
			rig.RemoveComponent<AnimationPoseComponent>();
			const Result<RenderSnapshot> unposed = ExtractRenderSnapshot(scene, RenderExtractionRequest{ .Width = 64, .Height = 64 });
			REQUIRE(unposed.has_value());
			CHECK(unposed->Meshes[0].JointPalette.empty());
			CHECK(unposed->Meshes[0].MorphWeights == std::vector<float>{ 0.25f, 0.5f });
		}

		// The Animator's own entity is its clips' empty path, and a pose's morph list longer than the SkinnedMesh's weights is
		// kept whole (the item holds the longer of the two; a sampled weight may leave [-1, 1]).
		TEST_CASE("RenderExtraction: a SkinnedMesh on its Animator's entity takes the pose's whole morph list at the empty path")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(CreateCamera(scene, "Camera", true));
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>();
			hero.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c3)));
			hero.AddComponent<SkinnedMeshComponent>(SkinnedMeshComponent{ .MorphWeights = { 0.25f } });
			AnimationPoseComponent pose;
			pose.JointModel = { glm::mat4(1.0f) };
			pose.Skin = { glm::mat4(1.0f) };
			pose.Morphs = { AnimationMorphWeights{ .Target = "Elsewhere", .Weights = { 1.0f } }, AnimationMorphWeights{ .Target = "", .Weights = { 0.5f, -0.5f, 1.5f } } };
			hero.AddComponent<AnimationPoseComponent>(pose);
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, RenderExtractionRequest{ .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 1);
			CHECK(snapshot->Meshes[0].JointPalette == std::vector<glm::mat4>{ glm::mat4(1.0f) });
			CHECK(snapshot->Meshes[0].MorphWeights == std::vector<float>{ 0.5f, -0.5f, 1.5f });
		}

		// M41 (Docs/Decisions/0058-m41-bone-attachments.md decision R): between ticks an attached subtree renders at its
		// Animator's rendered pose times its current pose relative to the Animator's entity, as the skinned palette does, so
		// the staff's item, one unit along the hand, lies on the hand the palette draws at alpha 0.5, while the walking
		// Animator's entity interpolates and the hand swings, whatever reset tag the staff has; Transform.RenderPosition and
		// RenderRotation give the same pose (the one rule of RenderExtraction.h), and so does a child of the staff. Plain
		// interpolation of the staff would leave it off the hand. At alpha 1 the subtree renders at its current pose, bit for
		// bit.
		TEST_CASE("RenderExtraction: an attached subtree renders at its Animator's rendered pose")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishWalkingRig(assets);
			static_cast<void>(CreateCamera(scene, "Camera", true));
			Entity warden = scene.CreateEntity("Warden");
			warden.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 0.0f, -2.0f);
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 30.0f, 0.0f));
			});
			warden.AddComponent<AnimatorComponent>(MakeWalkingAnimator());
			Entity body = scene.CreateEntity("Body", warden);
			body.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c4)));
			body.AddComponent<SkinnedMeshComponent>();
			Entity staff = scene.CreateEntity("Staff", warden);
			staff.AddComponent<BoneAttachmentComponent>(BoneAttachmentComponent{ .Bone = "Hand", .Offset = glm::vec3(0.0f, 1.0f, 0.0f) });
			staff.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c5)));
			Entity gem = scene.CreateEntity("Gem", staff);
			SetTranslation(gem, glm::vec3(0.0f, 0.5f, 0.0f));
			gem.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c6)));
			TransformSystem::Update(scene);
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			(*system)->Step(SimStep::FromTick(0, 1.0 / 60.0));
			TransformSystem::Update(scene);
			SnapshotPreviousPoses(scene);
			(*system)->Step(SimStep::FromTick(1, 1.0 / 60.0));
			TransformSystem::Update(scene);
			REQUIRE(staff.HasComponent<BoneAttachmentStateComponent>());
			// A frame-phase write's tag on the staff changes nothing for the attached subtree.
			staff.AddComponent<InterpolationResetTag>();
			scene.SetInterpolationAlpha(0.5f);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, RenderExtractionRequest{ .Width = 64, .Height = 64, .Alpha = 0.5f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 3);
			const MeshDrawItem& bodyItem = snapshot->Meshes[0];
			const MeshDrawItem& staffItem = snapshot->Meshes[1];
			const MeshDrawItem& gemItem = snapshot->Meshes[2];
			REQUIRE(bodyItem.JointPalette.size() == 2);
			// The hand as the skin draws it: the body's item times its palette entry, undoing the hand's bind; the staff one unit
			// along it.
			const glm::mat4 hand = bodyItem.World * bodyItem.JointPalette[1] * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f));
			const glm::vec3 held(hand * glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
			CHECK(Test::ApproxEqual(glm::vec3(staffItem.World[3]), held, 1e-5f));
			CHECK(Test::ApproxEqual(glm::vec3(staffItem.World[0]), glm::vec3(hand[0]), 1e-5f));
			CHECK(Test::ApproxEqual(glm::vec3(staffItem.World[1]), glm::vec3(hand[1]), 1e-5f));
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderPosition(staff), held, 1e-5f));
			CHECK(Test::ApproxEqual(gemItem.World, staffItem.World * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f)), 1e-5f));
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderPosition(gem), glm::vec3(gemItem.World[3]), 1e-5f));
			for (const Entity entity : { staff, gem })
			{
				INFO(std::string(entity.GetName()));
				const Result<TransformDecomposition> rendered = TransformSystem::DecomposeMatrix(ComputeRenderedWorldMatrix(entity, 0.5f));
				REQUIRE(rendered.has_value());
				for (const glm::vec3 axis : { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f) })
					CHECK(Test::ApproxEqual(rendered->Rotation * axis, TransformSystem::GetRenderRotation(entity) * axis, 1e-5f));
			}
			// Interpolating the staff like any entity would leave it off the hand by about half a tick of the swing.
			const glm::vec3 previous(staff.GetComponent<PreviousWorldTransformComponent>().Matrix[3]);
			const glm::vec3 current(staff.GetComponent<WorldTransformComponent>().Matrix[3]);
			CHECK(glm::length(previous + (current - previous) * 0.5f - held) > 1e-2f);
			// At alpha 1 the subtree renders at its current pose.
			CHECK(ComputeRenderedWorldMatrix(staff, 1.0f) == staff.GetComponent<WorldTransformComponent>().Matrix);
			CHECK(ComputeRenderedWorldMatrix(gem, 1.0f) == gem.GetComponent<WorldTransformComponent>().Matrix);
		}

		// The review of W41.1 (finding 4): a rider holding its own Animator, attached to its mount's hips, is itself rendered
		// by the rule (its Animator's rendered pose is its mount's times its pose relative to it), so its lance, attached to
		// the rider's hand, lies on the hand the rider's skin draws, and Transform.RenderPosition and RenderRotation agree.
		TEST_CASE("RenderExtraction: a rider on a mount renders with the mount, and its lance on the rider's rendered hand")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishWalkingRig(assets);
			static_cast<void>(CreateCamera(scene, "Camera", true));
			Entity mount = scene.CreateEntity("Mount");
			mount.AddComponent<AnimatorComponent>(MakeWalkingAnimator());
			Entity rider = scene.CreateEntity("Rider", mount);
			rider.AddComponent<BoneAttachmentComponent>(BoneAttachmentComponent{ .Bone = "Hips", .Offset = glm::vec3(0.0f, 0.3f, 0.0f) });
			AnimatorComponent riding = MakeWalkingAnimator();
			riding.RootMotion = AnimatorRootMotion::None;
			riding.Layers[1].Time = 0.3f;
			rider.AddComponent<AnimatorComponent>(riding);
			Entity body = scene.CreateEntity("Body", rider);
			body.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c4)));
			body.AddComponent<SkinnedMeshComponent>();
			Entity lance = scene.CreateEntity("Lance", rider);
			lance.AddComponent<BoneAttachmentComponent>(BoneAttachmentComponent{ .Bone = "Hand", .Offset = glm::vec3(0.0f, 1.0f, 0.0f) });
			lance.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c5)));
			TransformSystem::Update(scene);
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			(*system)->Step(SimStep::FromTick(0, 1.0 / 60.0));
			TransformSystem::Update(scene);
			SnapshotPreviousPoses(scene);
			(*system)->Step(SimStep::FromTick(1, 1.0 / 60.0));
			TransformSystem::Update(scene);
			REQUIRE(rider.HasComponent<BoneAttachmentStateComponent>());
			REQUIRE(lance.HasComponent<BoneAttachmentStateComponent>());
			scene.SetInterpolationAlpha(0.5f);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, RenderExtractionRequest{ .Width = 64, .Height = 64, .Alpha = 0.5f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 2);
			const MeshDrawItem& bodyItem = snapshot->Meshes[0];
			const MeshDrawItem& lanceItem = snapshot->Meshes[1];
			REQUIRE(bodyItem.JointPalette.size() == 2);
			const glm::mat4 hand = bodyItem.World * bodyItem.JointPalette[1] * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f));
			const glm::vec3 held(hand * glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
			CHECK(Test::ApproxEqual(glm::vec3(lanceItem.World[3]), held, 1e-5f));
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderPosition(lance), held, 1e-5f));
			// The rider is itself rendered on the mount's rendered hips, and the two functions agree for it too.
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderPosition(rider), glm::vec3(ComputeRenderedWorldMatrix(rider, 0.5f)[3]), 1e-5f));
			for (const Entity entity : { rider, lance })
			{
				INFO(std::string(entity.GetName()));
				const Result<TransformDecomposition> rendered = TransformSystem::DecomposeMatrix(ComputeRenderedWorldMatrix(entity, 0.5f));
				REQUIRE(rendered.has_value());
				for (const glm::vec3 axis : { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f) })
					CHECK(Test::ApproxEqual(rendered->Rotation * axis, TransformSystem::GetRenderRotation(entity) * axis, 1e-5f));
			}
		}

		// The review of W41.1 (finding 4): a state whose recorded Animator is no longer an ancestor (the staff moved, after
		// the pass, into a holster another Animator holds on its hips) is passed over, and the walk goes on to the holster's,
		// so the staff renders with the still mount, not by plain interpolation from where the walking hand had it.
		TEST_CASE("RenderExtraction: an attachment moved away from its Animator renders by the next attachment above it")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishWalkingRig(assets);
			static_cast<void>(CreateCamera(scene, "Camera", true));
			Entity walker = scene.CreateEntity("Walker");
			walker.AddComponent<AnimatorComponent>(MakeWalkingAnimator());
			Entity staff = scene.CreateEntity("Staff", walker);
			staff.AddComponent<BoneAttachmentComponent>(BoneAttachmentComponent{ .Bone = "Hand", .Offset = glm::vec3(0.0f, 1.0f, 0.0f) });
			staff.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c5)));
			Entity stand = scene.CreateEntity("Stand");
			SetTranslation(stand, glm::vec3(3.0f, 0.0f, 0.0f));
			AnimatorComponent still;
			still.Skeleton = TypedAssetHandle<AssetType::Skeleton>(RigSkeletonHandle);
			stand.AddComponent<AnimatorComponent>(still);
			Entity holster = scene.CreateEntity("Holster", stand);
			holster.AddComponent<BoneAttachmentComponent>(BoneAttachmentComponent{ .Bone = "Hips" });
			TransformSystem::Update(scene);
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			(*system)->Step(SimStep::FromTick(0, 1.0 / 60.0));
			TransformSystem::Update(scene);
			SnapshotPreviousPoses(scene);
			(*system)->Step(SimStep::FromTick(1, 1.0 / 60.0));
			TransformSystem::Update(scene);
			REQUIRE(scene.SetParent(staff, holster, std::nullopt, true).has_value());
			TransformSystem::Update(scene);
			REQUIRE(staff.GetComponent<BoneAttachmentStateComponent>().Animator == walker.GetUUID());
			CHECK(FindAttachedRenderAnimator(staff) == ConstEntity(stand));
			scene.SetInterpolationAlpha(0.5f);

			const glm::vec3 current(staff.GetComponent<WorldTransformComponent>().Matrix[3]);
			const glm::vec3 previous(staff.GetComponent<PreviousWorldTransformComponent>().Matrix[3]);
			const glm::vec3 rendered(ComputeRenderedWorldMatrix(staff, 0.5f)[3]);
			CHECK(Test::ApproxEqual(rendered, current, 1e-5f));
			CHECK(glm::length(previous + (current - previous) * 0.5f - rendered) > 1e-2f);
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderPosition(staff), rendered, 1e-5f));
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, RenderExtractionRequest{ .Width = 64, .Height = 64, .Alpha = 0.5f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 1);
			CHECK(Test::ApproxEqual(glm::vec3(snapshot->Meshes[0].World[3]), rendered, 1e-5f));
		}

		// M41 (decision R): a scene without attachments takes exactly the rule it had: the digest of the meshes' rendered
		// matrices and palettes and the lights' poses at alpha 0.375, over a walking, swinging rig with a skinned body, a
		// mesh that turns, grows and moves between the ticks, a child of a reset-tagged entity and a light, is the one its
		// extraction gave before M41 (the committed value, checked against the extraction without M41's change).
		TEST_CASE("RenderExtraction: a scene without attachments extracts as before")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishWalkingRig(assets);
			static_cast<void>(CreateCamera(scene, "Camera", true));
			Entity warden = scene.CreateEntity("Warden");
			warden.AddComponent<AnimatorComponent>(MakeWalkingAnimator());
			Entity body = scene.CreateEntity("Body", warden);
			body.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c4)));
			body.AddComponent<SkinnedMeshComponent>();
			Entity spinner = scene.CreateEntity("Spinner");
			spinner.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c5)));
			spinner.AddComponent<PointLightComponent>();
			Entity written = scene.CreateEntity("Written");
			Entity child = scene.CreateEntity("Child", written);
			child.AddComponent<MeshRendererComponent>(MakeMeshRenderer(AssetHandle(0x5c6)));
			TransformSystem::Update(scene);
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			(*system)->Step(SimStep::FromTick(0, 1.0 / 60.0));
			TransformSystem::Update(scene);
			SnapshotPreviousPoses(scene);
			(*system)->Step(SimStep::FromTick(1, 1.0 / 60.0));
			spinner.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.5f, 1.0f, -0.25f);
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(10.0f, 70.0f, -5.0f));
				transform.Scale = glm::vec3(1.0f, 2.0f, 1.5f);
			});
			SetTranslation(written, glm::vec3(3.0f, 0.0f, 0.0f));
			TransformSystem::Update(scene);
			written.AddComponent<InterpolationResetTag>();
			CHECK(scene.GetRegistry().view<BoneAttachmentStateComponent>().empty());

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, RenderExtractionRequest{ .Width = 64, .Height = 64, .Alpha = 0.375f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Meshes.size() == 3);
			XXH64Hasher hasher;
			for (const MeshDrawItem& item : snapshot->Meshes)
			{
				hasher.Update(std::as_bytes(std::span(&item.World, 1)));
				hasher.Update(std::as_bytes(std::span(item.JointPalette)));
			}
			REQUIRE(snapshot->Lights.size() == 1);
			hasher.Update(std::as_bytes(std::span(&snapshot->Lights[0].Position, 1)));
			hasher.Update(std::as_bytes(std::span(&snapshot->Lights[0].Direction, 1)));
			CHECK(std::format("{:016x}", hasher.Digest()) == "f9f2df085179fed9");
		}

		// Docs/Decisions/0032-m21-contract.md decision 4 (stream A): sprites in draw order, their layers resolved by name.
		TEST_CASE("RenderExtraction: sprites are extracted in sorting-layer, order-in-layer and canonical order")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(CreateCamera(scene, "Camera", true));
			const auto addSprite = [&scene](std::string_view name, std::string layer, int32_t order)
			{
				Entity entity = scene.CreateEntity(name);
				entity.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Texture = TypedAssetHandle<AssetType::Texture>(BuiltinAssetHandles::SpriteSquareTexture),
					.SortingLayer = std::move(layer),
					.OrderInLayer = order });
				return entity.GetUUID();
			};
			const UUID front = addSprite("Front", "Foreground", 0);
			const UUID backLate = addSprite("BackLate", "Background", 5);
			const UUID backEarly = addSprite("BackEarly", "Background", -1);
			const UUID backTie = addSprite("BackTie", "Background", 5);
			const UUID unknown = addSprite("Unknown", "NoSuchLayer", 0);
			Entity hidden = scene.CreateEntity("Hidden");
			hidden.AddComponent<SpriteRendererComponent>();
			hidden.SetActive(false);
			TransformSystem::Update(scene);

			RenderExtractionRequest request{ .Width = 640, .Height = 360 };
			request.SortingLayers = { "Default", "Background", "Foreground" };
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, request);
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Sprites.size() == 5);
			// An undeclared layer draws in the first one (SPRITE_UNKNOWN_SORTING_LAYER); equal keys keep canonical order.
			CHECK(snapshot->Sprites[0].Entity == unknown);
			CHECK(snapshot->Sprites[0].SortingLayer == 0);
			CHECK(snapshot->Sprites[1].Entity == backEarly);
			CHECK(snapshot->Sprites[2].Entity == backLate);
			CHECK(snapshot->Sprites[3].Entity == backTie);
			CHECK(snapshot->Sprites[3].SortingLayer == 1);
			CHECK(snapshot->Sprites[4].Entity == front);
			CHECK(snapshot->Sprites[4].SortingLayer == 2);
			CHECK(snapshot->Sprites[4].Texture == BuiltinAssetHandles::SpriteSquareTexture);
		}

		// Docs/Decisions/0032-m21-contract.md decision 6 (stream C): the pixel-perfect camera drives the output stage.
		TEST_CASE("RenderExtraction: a PixelPerfectCamera sets the output stage and an orthographic view of its reference height")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A perspective camera (the Camera's default) whose own orthographic size the pixel-perfect view replaces.
			Entity camera = CreateCamera(scene, "Camera", true);
			camera.Patch<CameraComponent>([](CameraComponent& component)
			{
				component.OrthographicSize = 3.0f;
			});
			camera.AddComponent<PixelPerfectCameraComponent>(PixelPerfectCameraComponent{ .ReferenceWidth = 320, .ReferenceHeight = 180, .PixelsPerUnit = 16.0f, .LetterboxColor = glm::vec3(0.25f, 0.0f, 0.0f) });
			SetTranslation(camera, glm::vec3(0.03f, 0.0f, 10.0f));
			const Entity plain = CreateCamera(scene, "Plain", false);
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 960, .Height = 540 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Output.SceneWidth == 320);
			CHECK(snapshot->Output.SceneHeight == 180);
			CHECK(snapshot->Output.PixelArt);
			CHECK(snapshot->Output.LetterboxColor == glm::vec3(0.25f, 0.0f, 0.0f));
			CHECK(snapshot->Camera.ProjectionKind == RenderProjection::Orthographic);
			CHECK(snapshot->Camera.OrthographicSize == 180.0f / 32.0f);
			// Snapped to the pixel grid: the view's left edge at a multiple of 1 / 16, and the view matrix looks from there.
			CHECK(snapshot->Camera.Position == glm::vec3(0.0f, 0.0f, 10.0f));
			CHECK(glm::vec3(snapshot->Camera.View[3]) == glm::vec3(0.0f, 0.0f, -10.0f));
			// The projection has the reference resolution's aspect, which is the camera's viewport (the scene resolution).
			CHECK(snapshot->Camera.ViewportWidth == 320);
			CHECK(snapshot->Camera.ViewportHeight == 180);
			const CameraComponent& component = camera.GetComponent<CameraComponent>();
			CHECK(snapshot->Camera.Projection
				== ComputeReverseZProjection(RenderProjection::Orthographic, component.VerticalFov, 180.0f / 32.0f, component.NearClip, component.FarClip, 320, 180));

			// An Entity request through the camera takes the same view; a camera without one keeps the default output stage.
			const Result<RenderSnapshot> entityView = ExtractRenderSnapshot(scene, { .Camera = RenderCameraSource::Entity, .CameraEntity = camera.GetUUID(), .Width = 960, .Height = 540 });
			REQUIRE_MESSAGE(entityView.has_value(), entityView.error().ToString());
			CHECK(entityView->Output == snapshot->Output);
			CHECK(entityView->Camera.Projection == snapshot->Camera.Projection);
			const Result<RenderSnapshot> plainView = ExtractRenderSnapshot(scene, { .Camera = RenderCameraSource::Entity, .CameraEntity = plain.GetUUID(), .Width = 960, .Height = 540 });
			REQUIRE_MESSAGE(plainView.has_value(), plainView.error().ToString());
			CHECK(plainView->Output == RenderOutputSettings{});
			CHECK(plainView->Camera.ProjectionKind == RenderProjection::Perspective);
			CHECK(plainView->Camera.ViewportWidth == 960);

			// The editor's explicit scene-view camera ignores it.
			const Result<RenderSnapshot> sceneView = ExtractRenderSnapshot(scene, { .Camera = RenderCameraSource::Explicit, .Width = 960, .Height = 540 });
			REQUIRE(sceneView.has_value());
			CHECK(sceneView->Output == RenderOutputSettings{});
		}

		// C21.2 (Docs/Decisions/0032-m21-contract.md decisions 21 and 22): the 2D lights in canonical order with every field, at
		// their rendered position, and a lit sprite's normal map.
		TEST_CASE("RenderExtraction: Light2D components become 2D lights in canonical order, and sprites carry their normal maps")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(CreateCamera(scene, "Camera", true));
			Entity torch = scene.CreateEntity("Torch");
			torch.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(2.0f, -1.0f, 0.5f);
			});
			torch.AddComponent<Light2DComponent>(Light2DComponent{ .Type = Light2DType::Point, .Color = glm::vec3(1.0f, 0.5f, 0.25f), .Intensity = 3.0f, .Radius = 6.0f, .Falloff = 0.25f, .Height = 2.0f });
			Entity sky = scene.CreateEntity("Sky");
			sky.AddComponent<Light2DComponent>(Light2DComponent{ .Type = Light2DType::Global, .Color = glm::vec3(0.1f), .Intensity = 0.5f });
			Entity off = scene.CreateEntity("Off");
			off.AddComponent<Light2DComponent>();
			off.SetActive(false);
			Entity sprite = scene.CreateEntity("Sprite");
			const AssetHandle normalMap(0x4e4d000000000001ull);
			sprite.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .Lit = true, .NormalMap = TypedAssetHandle<AssetType::Texture>(normalMap) });
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 320, .Height = 180 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Lights2D.size() == 2);
			const Light2DItem& point = snapshot->Lights2D[0];
			CHECK(point.Entity == torch.GetUUID());
			CHECK(point.Type == RenderLight2DType::Point);
			CHECK(point.Position == glm::vec3(2.0f, -1.0f, 0.5f));
			CHECK(point.Color == glm::vec3(1.0f, 0.5f, 0.25f));
			CHECK(point.Intensity == 3.0f);
			CHECK(point.Radius == 6.0f);
			CHECK(point.Falloff == 0.25f);
			CHECK(point.Height == 2.0f);
			CHECK(snapshot->Lights2D[1].Entity == sky.GetUUID());
			CHECK(snapshot->Lights2D[1].Type == RenderLight2DType::Global);
			REQUIRE(snapshot->Sprites.size() == 1);
			CHECK(snapshot->Sprites[0].Lit);
			CHECK(snapshot->Sprites[0].NormalMap == normalMap);
		}

		// C21.2 (decision 24, stream T1): tilemaps sort with the sprites by (sorting layer, order in layer, canonical order), each
		// placed among them by its DrawPosition.
		TEST_CASE("RenderExtraction: tilemaps are extracted in sorting-layer order and placed among the sprites")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(CreateCamera(scene, "Camera", true));
			const TypedAssetHandle<AssetType::Tilemap> map(AssetHandle(0x7113000000000002ull));
			const auto addSprite = [&scene](std::string_view name, std::string layer, int32_t order)
			{
				Entity entity = scene.CreateEntity(name);
				entity.AddComponent<SpriteRendererComponent>(SpriteRendererComponent{ .SortingLayer = std::move(layer), .OrderInLayer = order });
				return entity.GetUUID();
			};
			const auto addTilemap = [&scene, map](std::string_view name, std::string layer, int32_t order)
			{
				Entity entity = scene.CreateEntity(name);
				entity.AddComponent<TilemapRendererComponent>(TilemapRendererComponent{ .Tilemap = map, .Color = glm::vec4(0.5f), .PixelsPerUnit = 8.0f, .SortingLayer = std::move(layer), .OrderInLayer = order, .Lit = true });
				return entity.GetUUID();
			};
			const UUID ground = addTilemap("Ground", "Background", 0);
			const UUID hero = addSprite("Hero", "Foreground", 0);
			const UUID sky = addSprite("Sky", "Background", -5);
			const UUID decor = addTilemap("Decor", "Foreground", 0); // ties with Hero: canonical order puts it after
			const UUID tree = addSprite("Tree", "Background", 0);    // ties with Ground: canonical order puts it after
			Entity empty = scene.CreateEntity("Empty");
			empty.AddComponent<TilemapRendererComponent>(); // a null Tilemap draws nothing and is not extracted
			TransformSystem::Update(scene);

			RenderExtractionRequest request{ .Width = 640, .Height = 360 };
			request.SortingLayers = { "Default", "Background", "Foreground" };
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, request);
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			// The one order: Sky, Ground, Tree, Hero, Decor.
			REQUIRE(snapshot->Sprites.size() == 3);
			CHECK(snapshot->Sprites[0].Entity == sky);
			CHECK(snapshot->Sprites[1].Entity == tree);
			CHECK(snapshot->Sprites[2].Entity == hero);
			REQUIRE(snapshot->Tilemaps.size() == 2);
			const TilemapItem& first = snapshot->Tilemaps[0];
			CHECK(first.Entity == ground);
			CHECK(first.DrawPosition == 1);
			CHECK(first.Tilemap == map.GetHandle());
			CHECK(first.Color == glm::vec4(0.5f));
			CHECK(first.PixelsPerUnit == 8.0f);
			CHECK(first.SortingLayer == 1);
			CHECK(first.Lit);
			CHECK(snapshot->Tilemaps[1].Entity == decor);
			CHECK(snapshot->Tilemaps[1].DrawPosition == 3);
			CHECK(snapshot->Tilemaps[1].SortingLayer == 2);
		}

		// M26 (C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 7, stream P-B): every effectively enabled emitter
		// whose entity holds its runtime state gives an item and its live particles in emission order, in canonical order: a
		// World emitter's at the extraction's alpha between each particle's last two steps (its Position at alpha 1), a Local
		// emitter's through the emitter's rendered world matrix; each sized and coloured by the emitter's over-life curves at
		// its age over its lifetime. An emitter without a state (an edit scene's, or one before its first Particles phase)
		// gives none, and neither does a disabled one.
		TEST_CASE("RenderExtraction: emitters with a particle state give their live particles at the extraction's alpha, in canonical order")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			const TypedAssetHandle<AssetType::Texture> spark(AssetHandle(0x7114000000000001ull));
			const ParticleEmitterComponent worldEmitter{ .Texture = spark,
				.Blend = ParticleBlend::Additive,
				.ColorOverLife = { ParticleColorKey{ .Time = 0.0f, .Color = glm::vec4(1.0f, 0.5f, 0.0f, 1.0f) },
					ParticleColorKey{ .Time = 1.0f, .Color = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f) } },
				.SizeOverLife = { ParticleSizeKey{ .Time = 0.0f, .Size = 1.0f }, ParticleSizeKey{ .Time = 1.0f, .Size = 3.0f } } };
			const ParticleEmitterComponent localEmitter{ .SimulationSpace = ParticleSimulationSpace::Local };
			const auto addEmitter = [&scene](std::string_view name, const ParticleEmitterComponent& emitter, std::vector<Particle> particles)
			{
				Entity entity = scene.CreateEntity(name);
				entity.AddComponent<ParticleEmitterComponent>(emitter);
				if (!particles.empty())
					entity.AddComponent<ParticleStateComponent>().State.Particles = std::move(particles);
				return entity;
			};
			Entity unstepped = scene.CreateEntity("Unstepped");
			unstepped.AddComponent<ParticleEmitterComponent>();
			const Particle moving{ .Position = glm::vec3(8.0f, 0.0f, 0.0f),
				.PreviousPosition = glm::vec3(4.0f, 0.0f, 0.0f),
				.Age = 0.5f,
				.Lifetime = 2.0f,
				.StartSize = 0.25f };
			const Particle newborn{ .Position = glm::vec3(1.0f), .PreviousPosition = glm::vec3(1.0f), .Age = 0.0f, .Lifetime = 1.0f, .StartSize = 0.5f };
			const Entity world = addEmitter("World", worldEmitter, { moving, newborn });
			// A state without live particles still gives its emitter's item.
			Entity empty = scene.CreateEntity("Empty");
			empty.AddComponent<ParticleEmitterComponent>();
			static_cast<void>(empty.AddComponent<ParticleStateComponent>());
			const Particle carried{ .Position = glm::vec3(0.0f, 3.0f, 0.0f), .PreviousPosition = glm::vec3(0.0f, 1.0f, 0.0f), .Age = 3.0f, .Lifetime = 2.0f };
			Entity local = addEmitter("Local", localEmitter, { carried });
			Entity hidden = addEmitter("Hidden", worldEmitter, { newborn });
			hidden.SetActive(false);
			// The Local emitter moves from the origin to (2, 0, 0) over the step.
			TransformSystem::Update(scene);
			SnapshotPreviousPose(local);
			SetTranslation(local, glm::vec3(2.0f, 0.0f, 0.0f));
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8, .Alpha = 0.5f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->ParticleEmitters.size() == 3);
			REQUIRE(snapshot->Particles.size() == 3);
			const ParticleEmitterItem& first = snapshot->ParticleEmitters[0];
			CHECK(first.Entity == world.GetUUID());
			CHECK(first.Texture == spark.GetHandle());
			CHECK(first.Blend == RenderParticleBlend::Additive);
			CHECK(first.First == 0);
			CHECK(first.Count == 2);
			CHECK(snapshot->ParticleEmitters[1].Entity == empty.GetUUID());
			CHECK(snapshot->ParticleEmitters[1].First == 2);
			CHECK(snapshot->ParticleEmitters[1].Count == 0);
			const ParticleEmitterItem& last = snapshot->ParticleEmitters[2];
			CHECK(last.Entity == local.GetUUID());
			CHECK_FALSE(last.Texture.IsValid());
			CHECK(last.Blend == RenderParticleBlend::Alpha);
			CHECK(last.First == 2);
			CHECK(last.Count == 1);

			// Halfway between the last two steps; a newborn particle stays where it was born.
			CHECK(snapshot->Particles[0].Position == glm::vec3(6.0f, 0.0f, 0.0f));
			CHECK(snapshot->Particles[1].Position == glm::vec3(1.0f));
			// The Local particle halfway at (0, 2, 0) in the emitter's space, rendered halfway along its move at (1, 0, 0).
			CHECK(Test::ApproxEqual(snapshot->Particles[2].Position, glm::vec3(1.0f, 2.0f, 0.0f), 1e-6f));
			// The curves at age over lifetime: 0.25, 0, and 1 for the particle past its lifetime (clamped).
			CHECK(snapshot->Particles[0].Size == 0.25f * EvaluateParticleSize(worldEmitter, 0.25f));
			CHECK(snapshot->Particles[0].Color == EvaluateParticleColor(worldEmitter, 0.25f));
			CHECK(snapshot->Particles[1].Size == 0.5f * EvaluateParticleSize(worldEmitter, 0.0f));
			CHECK(snapshot->Particles[1].Color == EvaluateParticleColor(worldEmitter, 0.0f));
			CHECK(snapshot->Particles[2].Size == carried.StartSize * EvaluateParticleSize(localEmitter, 1.0f));
			CHECK(snapshot->Particles[2].Color == EvaluateParticleColor(localEmitter, 1.0f));

			// At alpha 1 a particle is at its Position exactly, and the Local emitter at its current pose.
			const Result<RenderSnapshot> current = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8, .Alpha = 1.0f });
			REQUIRE_MESSAGE(current.has_value(), current.error().ToString());
			REQUIRE(current->Particles.size() == 3);
			CHECK(current->Particles[0].Position == moving.Position);
			CHECK(Test::ApproxEqual(current->Particles[2].Position, glm::vec3(2.0f, 3.0f, 0.0f), 1e-6f));

			// An edit scene holds no particle state, so its views draw no particles.
			Test::SceneTestFixture edit;
			Entity editEmitter = edit.GetScene().CreateEntity("Emitter");
			editEmitter.AddComponent<ParticleEmitterComponent>();
			TransformSystem::Update(edit.GetScene());
			const Result<RenderSnapshot> editSnapshot = ExtractRenderSnapshot(edit.GetScene(), { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(editSnapshot.has_value(), editSnapshot.error().ToString());
			CHECK(editSnapshot->ParticleEmitters.empty());
			CHECK(editSnapshot->Particles.empty());
		}

		// M33 (Docs/Decisions/0048-m33-particle-look.md decision 3): a look's particles carry their angle at the extraction's
		// alpha, their world velocity, their flipbook phase and their serial, their colour brightened by the look's intensity,
		// and its item the look's settings; an emitter without a look keeps the defaults that draw as M26 did.
		TEST_CASE("RenderExtraction: a ParticleLook's particles carry their angle, velocity, frame phase and intensity, and its item the look")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			const TypedAssetHandle<AssetType::SpriteSheet> sheet(AssetHandle(0x7114000000000002ull));
			const ParticleEmitterComponent emitter{ .SimulationSpace = ParticleSimulationSpace::Local,
				.ColorOverLife = { ParticleColorKey{ .Time = 0.0f, .Color = glm::vec4(0.5f, 0.25f, 0.125f, 0.75f) } } };
			const Particle spinning{ .Position = glm::vec3(0.0f, 2.0f, 0.0f),
				.PreviousPosition = glm::vec3(0.0f, 1.0f, 0.0f),
				.Velocity = glm::vec3(0.0f, 0.0f, -3.0f),
				.Age = 0.5f,
				.Lifetime = 2.0f,
				.Rotation = 1.0f,
				.PreviousRotation = 0.5f,
				.FrameDraw = 0.625f,
				.Serial = 41 };
			// The emitter turned 90 degrees about +Y and moved to (5, 0, 0): its local -Z is world -X and its local X world -Z.
			Entity looked = scene.CreateEntity("Looked");
			looked.AddComponent<ParticleEmitterComponent>(emitter);
			looked.AddComponent<ParticleStateComponent>().State.Particles = { spinning };
			looked.AddComponent<ParticleLookComponent>(ParticleLookComponent{ .SpriteSheet = sheet,
				.FirstFrame = 2,
				.FrameCount = 6,
				.FrameCycles = 3.0f,
				.BlendFrames = true,
				.Alignment = ParticleAlignment::Flat,
				.VelocityStretch = 0.5f,
				.SoftDistance = 0.25f,
				.Intensity = 4.0f });
			looked.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(5.0f, 0.0f, 0.0f);
				transform.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			});
			Entity plain = scene.CreateEntity("Plain");
			plain.AddComponent<ParticleEmitterComponent>(emitter);
			plain.AddComponent<ParticleStateComponent>().State.Particles = { spinning };
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8, .Alpha = 0.5f });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->ParticleEmitters.size() == 2);
			REQUIRE(snapshot->Particles.size() == 2);
			const ParticleEmitterItem& item = snapshot->ParticleEmitters[0];
			CHECK(item.Sheet == sheet.GetHandle());
			CHECK(item.FirstFrame == 2);
			CHECK(item.FrameCount == 6);
			CHECK(item.BlendFrames);
			CHECK(item.Alignment == RenderParticleAlignment::Flat);
			CHECK(item.VelocityStretch == 0.5f);
			CHECK(item.SoftDistance == 0.25f);
			CHECK(item.FrameCycles == 3.0f);
			CHECK(item.Incarnation == looked.GetComponent<ParticleStateComponent>().Incarnation);
			CHECK(Test::ApproxEqual(item.PlaneRight, glm::vec3(0.0f, 0.0f, -1.0f), 1e-6f));
			CHECK(Test::ApproxEqual(item.PlaneUp, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-6f));
			const ParticleItem& particle = snapshot->Particles[0];
			CHECK(particle.Rotation == 0.75f);
			CHECK(Test::ApproxEqual(particle.Velocity, glm::vec3(-3.0f, 0.0f, 0.0f), 1e-5f));
			CHECK(particle.FramePhase == 0.25f * 3.0f);
			CHECK(particle.Serial == 41);
			CHECK_FALSE(particle.Previous.has_value());
			CHECK(particle.Color == glm::vec4(2.0f, 1.0f, 0.5f, 0.75f));

			// Without a look: M26's look, and the particle's own angle, velocity and serial all the same.
			const ParticleEmitterItem& bare = snapshot->ParticleEmitters[1];
			CHECK_FALSE(bare.Sheet.IsValid());
			CHECK_FALSE(bare.BlendFrames);
			CHECK(bare.Alignment == RenderParticleAlignment::Camera);
			CHECK(bare.SoftDistance == 0.0f);
			CHECK(snapshot->Particles[1].Color == glm::vec4(0.5f, 0.25f, 0.125f, 0.75f));
			CHECK(snapshot->Particles[1].FramePhase == 0.25f);

			// Random frames take the particle's pick and never blend; a dying particle's OverLife phase stays below its cycles.
			looked.GetComponent<ParticleLookComponent>().FrameMode = ParticleFrameMode::Random;
			plain.GetComponent<ParticleStateComponent>().State.Particles[0].Age = 2.0f;
			const Result<RenderSnapshot> random = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8, .Alpha = 1.0f });
			REQUIRE_MESSAGE(random.has_value(), random.error().ToString());
			CHECK_FALSE(random->ParticleEmitters[0].BlendFrames);
			CHECK(random->Particles[0].FramePhase == 0.625f);
			CHECK(random->Particles[0].Rotation == 1.0f);
			CHECK(random->Particles[1].FramePhase < 1.0f);
		}

		// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16; stream Q-B).
		TEST_CASE("RenderExtraction: baked probes are extracted in canonical order at their rendered pose, unbaked and disabled ones left out")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const TypedAssetHandle<AssetType::IrradianceVolume> volumeData(AssetHandle(0x1a2b000000000001ull));
			const TypedAssetHandle<AssetType::Environment> probeData(AssetHandle(0x1a2b000000000002ull));
			Entity room = scene.CreateEntity("Room");
			SetTranslation(room, glm::vec3(1.0f, 2.0f, 3.0f));
			room.AddComponent<IrradianceVolumeComponent>(IrradianceVolumeComponent{ .Data = volumeData, .Size = glm::vec3(6.0f, 3.0f, 5.0f), .Intensity = 0.5f, .BlendDistance = 0.25f });
			room.AddComponent<ReflectionProbeComponent>(ReflectionProbeComponent{ .Data = probeData, .Size = glm::vec3(4.0f, 2.0f, 7.0f), .BoxProjection = false, .Intensity = 2.0f, .BlendDistance = 0.5f });
			Entity unbaked = scene.CreateEntity("Unbaked");
			unbaked.AddComponent<IrradianceVolumeComponent>();
			unbaked.AddComponent<ReflectionProbeComponent>();
			Entity hall = scene.CreateEntity("Hall");
			hall.AddComponent<ReflectionProbeComponent>(ReflectionProbeComponent{ .Data = probeData });
			Entity off = scene.CreateEntity("Off");
			off.AddComponent<IrradianceVolumeComponent>(IrradianceVolumeComponent{ .Data = volumeData });
			off.SetActive(false);
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->IrradianceVolumes.size() == 1);
			const IrradianceVolumeItem& volume = snapshot->IrradianceVolumes[0];
			CHECK(volume.Entity == room.GetUUID());
			CHECK(volume.Data == volumeData.GetHandle());
			CHECK(volume.Size == glm::vec3(6.0f, 3.0f, 5.0f));
			CHECK(volume.Intensity == 0.5f);
			CHECK(volume.BlendDistance == 0.25f);
			CHECK(Test::ApproxEqual(glm::vec3(volume.World[3]), glm::vec3(1.0f, 2.0f, 3.0f), 1e-6f));
			REQUIRE(snapshot->ReflectionProbes.size() == 2);
			const ReflectionProbeItem& probe = snapshot->ReflectionProbes[0];
			CHECK(probe.Entity == room.GetUUID());
			CHECK(probe.Data == probeData.GetHandle());
			CHECK(probe.Size == glm::vec3(4.0f, 2.0f, 7.0f));
			CHECK_FALSE(probe.BoxProjection);
			CHECK(probe.Intensity == 2.0f);
			CHECK(probe.BlendDistance == 0.5f);
			CHECK(Test::ApproxEqual(glm::vec3(probe.World[3]), glm::vec3(1.0f, 2.0f, 3.0f), 1e-6f));
			CHECK(snapshot->ReflectionProbes[1].Entity == hall.GetUUID());
			CHECK(snapshot->ReflectionProbes[1].BoxProjection);
		}

		// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22; stream T-C): terrains with a heightfield, in
		// canonical order, every field mapped and at most four layers; each pickable after the meshes, sharing its entity's
		// mesh entry when it has one.
		TEST_CASE("RenderExtraction: terrains with a heightfield are extracted in canonical order, pickable after the meshes")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const TypedAssetHandle<AssetType::Heightfield> heights(AssetHandle(0x1a2c000000000001ull));
			const TypedAssetHandle<AssetType::Texture> splat(AssetHandle(0x1a2c000000000002ull));
			const TypedAssetHandle<AssetType::Texture> grass(AssetHandle(0x1a2c000000000003ull));
			const TypedAssetHandle<AssetType::Texture> grassNormal(AssetHandle(0x1a2c000000000004ull));
			Entity field = scene.CreateEntity("Field");
			SetTranslation(field, glm::vec3(-4.0f, 1.0f, 2.0f));
			TerrainComponent fieldTerrain{ .Heightfield = heights, .Size = glm::vec2(120.0f, 80.0f), .HeightScale = 30.0f, .SplatMap = splat, .ReceiveShadows = false, .LodBias = 2.0f };
			fieldTerrain.Layers = {
				TerrainLayer{ .BaseColorMap = grass, .NormalMap = grassNormal, .Tint = glm::vec3(0.5f, 0.75f, 0.25f), .Tiling = 6.0f, .Roughness = 0.7f, .Metallic = 0.1f },
				TerrainLayer{ .Tiling = 2.0f },
				TerrainLayer{},
				TerrainLayer{},
				TerrainLayer{ .Tiling = 9.0f }, // past MaxTerrainLayers (the registry refuses it; extraction drops it)
			};
			field.AddComponent<TerrainComponent>(std::move(fieldTerrain));
			Entity rock = scene.CreateEntity("Rock");
			rock.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			Entity hill = scene.CreateEntity("Hill");
			hill.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::SphereMesh);
			hill.AddComponent<TerrainComponent>(TerrainComponent{ .Heightfield = heights });
			Entity flat = scene.CreateEntity("Flat");
			flat.AddComponent<TerrainComponent>();
			Entity off = scene.CreateEntity("Off");
			off.AddComponent<TerrainComponent>(TerrainComponent{ .Heightfield = heights });
			off.SetActive(false);
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			REQUIRE(snapshot->Terrains.size() == 2);
			const TerrainItem& first = snapshot->Terrains[0];
			CHECK(first.Entity == field.GetUUID());
			CHECK(first.Heightfield == heights.GetHandle());
			CHECK(first.Size == glm::vec2(120.0f, 80.0f));
			CHECK(first.HeightScale == 30.0f);
			CHECK(first.SplatMap == splat.GetHandle());
			CHECK_FALSE(first.ReceiveShadows);
			CHECK(first.LodBias == 2.0f);
			CHECK(Test::ApproxEqual(glm::vec3(first.World[3]), glm::vec3(-4.0f, 1.0f, 2.0f), 1e-6f));
			REQUIRE(first.Layers.size() == MaxTerrainLayers);
			CHECK(first.Layers[0].BaseColorMap == grass.GetHandle());
			CHECK(first.Layers[0].NormalMap == grassNormal.GetHandle());
			CHECK(first.Layers[0].Tint == glm::vec3(0.5f, 0.75f, 0.25f));
			CHECK(first.Layers[0].Tiling == 6.0f);
			CHECK(first.Layers[0].Roughness == 0.7f);
			CHECK(first.Layers[0].Metallic == 0.1f);
			CHECK_FALSE(first.Layers[1].BaseColorMap.IsValid());
			CHECK(first.Layers[1].Tiling == 2.0f);
			const TerrainItem& second = snapshot->Terrains[1];
			CHECK(second.Entity == hill.GetUUID());
			CHECK(second.Layers.empty());
			CHECK_FALSE(second.SplatMap.IsValid());
			CHECK(second.ReceiveShadows);
			// The meshes' entries first, in canonical order; the hill's terrain shares its mesh's, the field's follows them.
			REQUIRE(snapshot->PickTable.size() == 3);
			CHECK(snapshot->PickTable[0] == rock.GetUUID());
			CHECK(snapshot->PickTable[1] == hill.GetUUID());
			CHECK(snapshot->PickTable[2] == field.GetUUID());
			CHECK(second.PickId == 2);
			CHECK(first.PickId == 3);
		}

		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): an entity with a PlanarReflection and a visible mesh
		// gives one item in canonical order, in the game view and the editor's (explicit) view of an edit scene alike, at the
		// rendered pose its mesh item draws at; a mirror without a drawn mesh, or disabled, gives none.
		TEST_CASE("RenderExtraction: a planar reflection is extracted at its rendered pose in every view")
		{
			Test::SceneTestFixture edit;
			Scene& scene = edit.GetScene();
			CreateCamera(scene, "Camera", true);
			Entity wall = scene.CreateEntity("Wall");
			SetTranslation(wall, glm::vec3(0.0f, 1.5f, -4.0f));
			wall.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::QuadMesh));
			wall.AddComponent<PlanarReflectionComponent>(PlanarReflectionComponent{ .Normal = glm::vec3(0.0f, 0.0f, 2.0f), .Offset = 0.25f, .ResolutionScale = 0.75f });
			Entity bare = scene.CreateEntity("Bare");
			bare.AddComponent<PlanarReflectionComponent>();
			Entity hidden = scene.CreateEntity("Hidden");
			MeshRendererComponent hiddenRenderer = MakeMeshRenderer(BuiltinAssetHandles::QuadMesh);
			hiddenRenderer.Visible = false;
			hidden.AddComponent<MeshRendererComponent>(hiddenRenderer);
			hidden.AddComponent<PlanarReflectionComponent>();
			Entity unmeshed = scene.CreateEntity("Unmeshed");
			unmeshed.AddComponent<MeshRendererComponent>();
			unmeshed.AddComponent<PlanarReflectionComponent>();
			Entity off = scene.CreateEntity("Off");
			off.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::QuadMesh));
			off.AddComponent<PlanarReflectionComponent>();
			off.SetActive(false);
			Entity floor = scene.CreateEntity("Floor");
			floor.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::PlaneMesh));
			floor.AddComponent<PlanarReflectionComponent>(PlanarReflectionComponent{ .Normal = glm::vec3(0.0f, 1.0f, 0.0f) });
			TransformSystem::Update(scene);

			for (const RenderCameraSource source : { RenderCameraSource::Primary, RenderCameraSource::Explicit })
			{
				CAPTURE(static_cast<int>(source));
				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Camera = source, .Width = 8, .Height = 8 });
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				REQUIRE(snapshot->PlanarReflections.size() == 2);
				const PlanarReflectionItem& mirror = snapshot->PlanarReflections[0];
				CHECK(mirror.Entity == wall.GetUUID());
				CHECK(mirror.Normal == glm::vec3(0.0f, 0.0f, 2.0f));
				CHECK(mirror.Offset == 0.25f);
				CHECK(mirror.ResolutionScale == 0.75f);
				const auto wallItem = std::ranges::find(snapshot->Meshes, wall.GetUUID(), &MeshDrawItem::Entity);
				REQUIRE(wallItem != snapshot->Meshes.end());
				CHECK(mirror.World == wallItem->World);
				CHECK(snapshot->PlanarReflections[1].Entity == floor.GetUUID());
				CHECK(snapshot->PlanarReflections[1].ResolutionScale == DefaultPlanarReflectionResolutionScale);
			}

			// In a runtime scene the item takes the interpolated pose its mesh draws at.
			Test::SceneTestFixture runtime(1, true);
			Entity moving = runtime.GetScene().CreateEntity("Moving");
			moving.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::QuadMesh));
			moving.AddComponent<PlanarReflectionComponent>();
			TransformSystem::Update(runtime.GetScene());
			SnapshotPreviousPose(moving);
			SetTranslation(moving, glm::vec3(0.0f, 0.0f, -8.0f));
			TransformSystem::Update(runtime.GetScene());
			const Result<RenderSnapshot> between = ExtractRenderSnapshot(runtime.GetScene(), { .Width = 8, .Height = 8, .Alpha = 0.5f });
			REQUIRE_MESSAGE(between.has_value(), between.error().ToString());
			REQUIRE(between->PlanarReflections.size() == 1);
			REQUIRE(between->Meshes.size() == 1);
			CHECK(between->PlanarReflections[0].World[3].z == doctest::Approx(-4.0f));
			CHECK(between->PlanarReflections[0].World == between->Meshes[0].World);
		}

		TEST_CASE("RenderExtraction: a Sky is extracted with its Sun and Moon in every view, and a scene without one extracts as before")
		{
			// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Scene/SkyExtraction.h).
			Test::SceneTestFixture edit;
			Scene& scene = edit.GetScene();
			CreateCamera(scene, "Camera", true);
			Entity first = scene.CreateEntity("First");
			first.AddComponent<DirectionalLightComponent>(DirectionalLightComponent{ .Color = glm::vec3(0.5f, 0.5f, 1.0f) });
			Entity sun = scene.CreateEntity("Sun");
			sun.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(-60.0f, 0.0f, 0.0f));
			});
			sun.AddComponent<DirectionalLightComponent>(DirectionalLightComponent{ .Color = glm::vec3(1.0f, 0.9f, 0.8f), .Intensity = 4.0f });
			// A moon without a light: its local +Z turned 30 degrees up about X (toward +Y and +Z).
			Entity moon = scene.CreateEntity("Moon");
			moon.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(-30.0f, 0.0f, 0.0f));
			});
			TransformSystem::Update(scene);

			// Without a Sky: disabled, and the lights as authored.
			const Result<RenderSnapshot> plain = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(plain.has_value(), plain.error().ToString());
			CHECK_FALSE(plain->Sky.Enabled);
			CHECK(plain->Lights.size() == 2);
			CHECK(plain->Lights[1].Color == glm::vec3(1.0f, 0.9f, 0.8f));

			const SkyComponent authored{ .Sun = sun.GetUUID(),
				.Moon = moon.GetUUID(),
				.Intensity = 2.5f,
				.DriveLights = false,
				.Haze = 3.0f,
				.Overcast = 0.25f,
				.GroundAlbedo = glm::vec3(0.1f, 0.2f, 0.3f),
				.SunDisc = 4.0f,
				.MoonSize = 1.5f,
				.Stars = 7.0f,
				.Latitude = -20.0f,
				.NorthAngle = 15.0f,
				.Clouds = 0.5f,
				.CloudSpeed = glm::vec2(-3.0f, 2.0f) };
			Entity skyEntity = scene.CreateEntity("Sky");
			skyEntity.AddComponent<SkyComponent>(authored);
			for (const RenderCameraSource source : { RenderCameraSource::Primary, RenderCameraSource::Explicit })
			{
				CAPTURE(static_cast<int>(source));
				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Camera = source, .Width = 8, .Height = 8, .SimulationTime = 12.5 });
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				const RenderSky& sky = snapshot->Sky;
				CHECK(sky.Enabled);
				CHECK(sky.Entity == skyEntity.GetUUID());
				CHECK(sky.Sun == sun.GetUUID());
				CHECK(sky.Moon == moon.GetUUID());
				CHECK(sky.Intensity == 2.5f);
				CHECK_FALSE(sky.DriveLights);
				CHECK(sky.Haze == 3.0f);
				CHECK(sky.Overcast == 0.25f);
				CHECK(sky.GroundAlbedo == glm::vec3(0.1f, 0.2f, 0.3f));
				CHECK(sky.SunDisc == 4.0f);
				CHECK(sky.MoonSize == 1.5f);
				CHECK(sky.Stars == 7.0f);
				CHECK(sky.Latitude == -20.0f);
				CHECK(sky.NorthAngle == 15.0f);
				CHECK(sky.Clouds == 0.5f);
				CHECK(sky.CloudSpeed == glm::vec2(-3.0f, 2.0f));
				CHECK(sky.SimulationTime == 12.5);
				// The Sun is the second light, the named one; the Moon holds no light, so its direction is its local +Z.
				CHECK(sky.SunLight == std::optional<uint32_t>(1));
				CHECK(sky.SunLightColor == glm::vec3(1.0f, 0.9f, 0.8f));
				CHECK_FALSE(sky.MoonLight.has_value());
				CHECK(sky.HasMoon);
				const glm::vec3 forward = glm::normalize(glm::vec3(moon.GetComponent<WorldTransformComponent>().Matrix[2]));
				CHECK(glm::length(sky.MoonDirection - forward) < 1e-6f);
				CHECK(sky.MoonDirection.y > 0.4f);
			}

			// A null Sun takes the first directional light; a Moon that is the Sun's entity has no moon.
			skyEntity.Patch<SkyComponent>([&first](SkyComponent& sky)
			{
				sky.Sun = UUID();
				sky.Moon = first.GetUUID();
			});
			Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Sky.SunLight == std::optional<uint32_t>(0));
			CHECK_FALSE(snapshot->Sky.MoonLight.has_value());
			CHECK_FALSE(snapshot->Sky.HasMoon);
			CHECK(snapshot->Sky.SimulationTime == 0.0);

			// A Moon light: its light's index, colour and direction.
			skyEntity.Patch<SkyComponent>([&sun](SkyComponent& sky)
			{
				sky.Sun = sun.GetUUID();
			});
			snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Sky.MoonLight == std::optional<uint32_t>(0));
			CHECK(snapshot->Sky.MoonLightColor == glm::vec3(0.5f, 0.5f, 1.0f));
			CHECK(snapshot->Sky.HasMoon);
			CHECK(glm::length(snapshot->Sky.MoonDirection + snapshot->Lights[0].Direction) < 1e-6f);

			// A disabled Sky is not extracted.
			skyEntity.SetActive(false);
			snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK_FALSE(snapshot->Sky.Enabled);
			CHECK(snapshot->Lights[1].Color == glm::vec3(1.0f, 0.9f, 0.8f));
		}
	}

}
