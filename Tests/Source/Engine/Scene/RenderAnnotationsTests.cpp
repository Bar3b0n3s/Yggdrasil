#include "TestsPCH.h"

#include "Engine/Scene/RenderAnnotations.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Time.h"
#include "Engine/Scene/ColliderDebugDraw.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/Light2DComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SkyComponent.h"
#include "Engine/Scene/Components/SpotLightComponent.h"
#include "Engine/Scene/Components/StaticComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/RenderStaticCache.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <variant>

namespace Engine {

	static RenderSnapshot AnnotationView()
	{
		RenderSnapshot view;
		view.HasCamera = true;
		view.Camera.View[3].z = -10;
		return view;
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderAnnotations: colliders use snapshot alpha and match rendered meshes")
		{
			Test::SceneTestFixture fixture(1, true);
			Test::InMemoryAssetManager assets;
			Entity parent = fixture.GetScene().CreateEntity("Parent");
			Entity child = fixture.GetScene().CreateEntity("Child", parent);
			child.AddComponent<BoxColliderComponent>();
			child.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			parent.Patch<TransformComponent>([](TransformComponent& t)
			{
				t.Translation.x = 4;
			});
			TransformSystem::Update(fixture.GetScene());
			parent.AddComponent<PreviousWorldTransformComponent>();
			child.AddComponent<PreviousWorldTransformComponent>();
			auto view = AnnotationView();
			view.Alpha = 0.25f;
			view.Flags = RenderViewFlags::Colliders;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, view));
			REQUIRE(view.AnnotationDraw.GetSize() == 1);
			const auto* box = std::get_if<DebugBox>(&view.AnnotationDraw.GetCommands()[0].Shape);
			REQUIRE(box != nullptr);
			CHECK(box->Center == glm::vec3(ComputeRenderedWorldMatrix(child, view.Alpha)[3]));
			CHECK(box->Center.x == 1);
		}

		TEST_CASE("RenderAnnotations: labels bounds and axes are capture-local")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Entity entity = fixture.GetScene().CreateEntity("Box");
			entity.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			const uint64_t revision = fixture.GetScene().GetRevision();
			auto view = AnnotationView();
			view.Annotations = { .Labels = RenderAnnotationLabels::All, .Bounds = true, .Axes = true };
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, view));
			REQUIRE(view.AnnotationDraw.GetSize() == 5);
			const auto* label = std::get_if<DebugText>(&view.AnnotationDraw.GetCommands()[0].Shape);
			REQUIRE(label != nullptr);
			CHECK(label->Text == "Box " + entity.GetUUID().ToString().substr(0, 6));
			auto clean = AnnotationView();
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, clean));
			CHECK(clean.AnnotationDraw.IsEmpty());
			CHECK(fixture.GetScene().GetRevision() == revision);
			const auto before = view.AnnotationDraw;
			view.Alpha = -1;
			CHECK_FALSE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, view));
			CHECK(view.AnnotationDraw == before);
		}

		TEST_CASE("RenderAnnotations: bounds are the mesh bounds transformed by the rendered pose")
		{
			// A moving play entity between ticks: its box must sit on the mesh drawn at the snapshot's alpha, not on the current
			// world transform up to a step ahead (M9-F24).
			Test::SceneTestFixture fixture(1, true);
			Test::InMemoryAssetManager assets;
			Entity mover = fixture.GetScene().CreateEntity("Mover");
			mover.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			mover.Patch<TransformComponent>([](TransformComponent& t)
			{
				t.Translation = glm::vec3(4.0f, 0.0f, -2.0f);
				t.Scale = glm::vec3(2.0f, 1.0f, 3.0f);
			});
			TransformSystem::Update(fixture.GetScene());
			mover.AddComponent<PreviousWorldTransformComponent>();
			auto view = AnnotationView();
			view.Alpha = 0.25f;
			view.Annotations.Bounds = true;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, view));
			REQUIRE(view.AnnotationDraw.GetSize() == 1);
			const auto* box = std::get_if<DebugBox>(&view.AnnotationDraw.GetCommands()[0].Shape);
			REQUIRE(box != nullptr);
			const glm::mat4 rendered = ComputeRenderedWorldMatrix(mover, view.Alpha);
			const Aabb expected = assets.GetOrPlaceholder<MeshData>(BuiltinAssetHandles::CubeMesh)->Bounds.Transformed(rendered);
			CHECK(box->Center == expected.GetCenter());
			CHECK(box->HalfExtents == expected.GetSize() * 0.5f);
			CHECK(box->Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			CHECK(view.AnnotationDraw.GetCommands()[0].Color == glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
			// A quarter of the way from the previous pose (the origin) to the current one.
			CHECK(box->Center.x == doctest::Approx(1.0f));
			CHECK(box->Center.z == doctest::Approx(-0.5f));
			const glm::vec3 current(mover.GetComponent<WorldTransformComponent>().Matrix[3]);
			CHECK(box->Center != current);
			// At alpha 1 the box follows the current pose.
			auto settled = AnnotationView();
			settled.Annotations.Bounds = true;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, settled));
			const auto* full = std::get_if<DebugBox>(&settled.AnnotationDraw.GetCommands()[0].Shape);
			REQUIRE(full != nullptr);
			CHECK(full->Center == glm::vec3(4.0f, 0.0f, -2.0f));
			CHECK(full->HalfExtents == glm::vec3(1.0f, 0.5f, 1.5f));
		}

		TEST_CASE("RenderAnnotations: plain glyph records need no editor UI or font")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Entity camera = fixture.GetScene().CreateEntity("Camera");
			camera.AddComponent<CameraComponent>();
			Entity light = fixture.GetScene().CreateEntity("Light");
			light.AddComponent<PointLightComponent>();
			auto view = AnnotationView();
			view.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Icons;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, view));
			REQUIRE(view.Icons.size() == 2);
			CHECK(view.Icons[0].Kind == RenderIconKind::Camera);
			CHECK(view.Icons[1].Kind == RenderIconKind::PointLight);
			CHECK(view.Icons[0].Entity == camera.GetUUID());
			CHECK(view.AnnotationDraw.IsEmpty());
			auto plain = AnnotationView();
			plain.Flags = RenderViewFlags::Icons;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, plain));
			CHECK(plain.Icons.empty());
		}

		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 20): where the view's icons are on, a Light2D shows the
		// PointLight icon in its Color, so the scene view shows where each 2D light is.
		TEST_CASE("RenderAnnotations: a Light2D shows the PointLight icon in its colour when icons are on")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Entity torch = fixture.GetScene().CreateEntity("Torch");
			torch.AddComponent<Light2DComponent>(Light2DComponent{ .Color = glm::vec3(1.0f, 0.5f, 0.25f) });
			auto view = AnnotationView();
			view.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Icons;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, view));
			REQUIRE(view.Icons.size() == 1);
			CHECK(view.Icons[0].Kind == RenderIconKind::PointLight);
			CHECK(view.Icons[0].Entity == torch.GetUUID());
			CHECK(view.Icons[0].Color == glm::vec4(1.0f, 0.5f, 0.25f, 1.0f));
			// Icons off, or a view without the editor overlays: none.
			auto plain = AnnotationView();
			plain.Flags = RenderViewFlags::Icons;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, plain));
			CHECK(plain.Icons.empty());
			auto hidden = AnnotationView();
			hidden.Flags = RenderViewFlags::EditorOverlays;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, hidden));
			CHECK(hidden.Icons.empty());
		}

		// M21 (C21.2, decision 28, stream P3): with Colliders, AppendRenderAnnotations appends the 2D colliders after the 3D ones
		// (BuildCollider2DDebugDraw's records through AppendColliderDebugDraw), told asleep from awake by the session's
		// Physics2DSystem the host passes; without it they draw as an edit scene's.
		TEST_CASE("RenderAnnotations: with Colliders, a play scene's 2D colliders follow the 3D ones")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			// A 3D box first in canonical order, then a 2D ground and a 2D crate resting on it.
			Entity crate3D = scene.CreateEntity("Crate3D");
			crate3D.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 5.0f, 0.0f);
			});
			crate3D.AddComponent<BoxColliderComponent>();
			const auto addBox = [&scene](std::string_view name, const glm::vec2& position, const glm::vec2& halfExtents, BodyType type)
			{
				Entity entity = scene.CreateEntity(name);
				entity.Patch<TransformComponent>([position](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(position, 0.0f);
				});
				entity.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = type });
				entity.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = halfExtents });
				return entity;
			};
			static_cast<void>(addBox("Ground", glm::vec2(0.0f, -0.5f), glm::vec2(20.0f, 0.5f), BodyType::Static));
			const Entity crate = addBox("Crate", glm::vec2(0.0f, 0.5f), glm::vec2(0.5f), BodyType::Dynamic);
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> physics2D = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(physics2D.has_value(), physics2D.error().ToString());
			for (uint64_t tick = 0; tick < 180; ++tick)
			{
				TransformSystem::Update(scene);
				const SimStep step = SimStep::FromTick(tick, 1.0 / 60.0);
				(*physics2D)->PreStep(step);
				(*physics2D)->Step(step);
				(*physics2D)->PostStep(step);
			}
			TransformSystem::Update(scene);
			REQUIRE((*physics2D)->IsSleeping(crate.GetUUID()).value_or(false));

			// The 3D box, then the ground's four edges, then the crate's four edges, in the Sleeping colour.
			const auto annotate = [&scene, &assets](const Physics2DSystem* system)
			{
				auto view = AnnotationView();
				view.Flags = RenderViewFlags::Colliders;
				REQUIRE(AppendRenderAnnotations(scene, assets, {}, nullptr, view, system));
				return view;
			};
			const RenderSnapshot played = annotate(physics2D->get());
			const auto& commands = played.AnnotationDraw.GetCommands();
			REQUIRE(commands.size() == 9);
			CHECK(std::holds_alternative<DebugBox>(commands[0].Shape));
			for (size_t index = 1; index < commands.size(); ++index)
			{
				CAPTURE(index);
				CHECK(std::holds_alternative<DebugLine>(commands[index].Shape));
				const ColliderDebugCategory category = index < 5 ? ColliderDebugCategory::Static : ColliderDebugCategory::Sleeping;
				CHECK(commands[index].Color == GetColliderDebugColor(category));
			}
			// Without the session's system the crate is Dynamic, as in an edit scene.
			const RenderSnapshot edited = annotate(nullptr);
			REQUIRE(edited.AnnotationDraw.GetCommands().size() == 9);
			CHECK(edited.AnnotationDraw.GetCommands()[8].Color == GetColliderDebugColor(ColliderDebugCategory::Dynamic));
		}

		TEST_CASE("RenderAnnotations: explicit label IDs are independent of editor selection")
		{
			Test::SceneTestFixture fixture(1, true);
			Test::InMemoryAssetManager assets;
			Entity a = fixture.GetScene().CreateEntity("A");
			Entity b = fixture.GetScene().CreateEntity("B");
			Entity disabled = fixture.GetScene().CreateEntity("Disabled");
			disabled.SetActive(false);
			Entity pending = fixture.GetScene().CreateEntity("Pending");
			const UUID pendingId = pending.GetUUID();
			fixture.GetScene().DestroyEntity(pending);
			auto view = AnnotationView();
			view.SelectedEntities = { a.GetUUID() };
			view.Annotations = { .Labels = RenderAnnotationLabels::Explicit, .LabelEntities = { b.GetUUID(), disabled.GetUUID(), pendingId, UUID(9) } };
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, view));
			REQUIRE(view.AnnotationDraw.GetSize() == 1);
			CHECK(std::get<DebugText>(view.AnnotationDraw.GetCommands()[0].Shape).Text.starts_with("B "));
			auto empty = AnnotationView();
			empty.Annotations.Labels = RenderAnnotationLabels::Explicit;
			empty.Annotations.Axes = true;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, empty));
			CHECK(empty.AnnotationDraw.GetSize() == 6);
			auto selected = AnnotationView();
			selected.SelectedEntities = { a.GetUUID() };
			selected.Annotations.Labels = RenderAnnotationLabels::Selected;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, selected));
			REQUIRE(selected.AnnotationDraw.GetSize() == 1);
			CHECK(std::get<DebugText>(selected.AnnotationDraw.GetCommands()[0].Shape).Text.starts_with("A "));
		}

		TEST_CASE("RenderSceneValidation: default ambient and emissive-only scenes avoid false warnings")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			Entity mesh = scene.CreateEntity("Cube");
			mesh.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>().Intensity = 0;
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			auto emission = CreateRef<MaterialData>();
			emission->Emissive = { 1, 0, 0 };
			emission->EmissiveStrength = 2;
			assets.Publish(AssetHandle(2000), emission);
			mesh.Patch<MeshRendererComponent>([](MeshRendererComponent& r)
			{
				r.Materials.resize(1);
				r.Materials[0].SetHandle(AssetHandle(2000));
			});
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			Entity other = scene.CreateEntity("Unlit");
			other.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			other.SetActive(false);
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
		}

		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): a static entity's mesh, which the snapshot holds in its
		// static set rather than its Meshes, still needs lighting when it emits nothing.
		TEST_CASE("RenderSceneValidation: a static mesh without emission or light needs lighting")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>().Intensity = 0;
			Entity island = scene.CreateEntity("Island");
			island.AddComponent<StaticComponent>();
			Entity rock = scene.CreateEntity("Rock", island);
			rock.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			REQUIRE(scene.GetStaticCache() != nullptr);
			CHECK(scene.GetStaticCache()->HoldsItem(rock.GetHandle()));
			auto emission = CreateRef<MaterialData>();
			emission->Emissive = { 1, 0, 0 };
			emission->EmissiveStrength = 2;
			assets.Publish(AssetHandle(2002), emission);
			rock.Patch<MeshRendererComponent>([](MeshRendererComponent& r)
			{
				r.Materials.resize(1);
				r.Materials[0].SetHandle(AssetHandle(2002));
			});
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
		}

		TEST_CASE("RenderSceneValidation: an enabled Sky of positive Intensity illuminates")
		{
			// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): a Sky lights the scene in the Environment's place,
			// while its entity is enabled and its Intensity positive.
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			Entity mesh = scene.CreateEntity("Cube");
			mesh.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>().Intensity = 0;
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			Entity sky = scene.CreateEntity("Sky");
			sky.AddComponent<SkyComponent>();
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			sky.Patch<SkyComponent>([](SkyComponent& component)
			{
				component.Intensity = 0.0f;
			});
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			sky.Patch<SkyComponent>([](SkyComponent& component)
			{
				component.Intensity = 1.0f;
			});
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			sky.SetActive(false);
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			// The W45.2 review's finding 3: beside an enabled Sky the Environment lights nothing, so a scene without an Environment
			// entity, whose default fallback colour lights it without a Sky, is unlit under a Sky of Intensity 0.
			scene.DestroyEntity(environment);
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			sky.SetActive(true);
			sky.Patch<SkyComponent>([](SkyComponent& component)
			{
				component.Intensity = 0.0f;
			});
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			sky.Patch<SkyComponent>([](SkyComponent& component)
			{
				component.Intensity = 1.0f;
			});
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
		}

		TEST_CASE("RenderSceneValidation: missing maps use fallback and usable maps use SH illumination")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			Entity mesh = scene.CreateEntity("Cube");
			mesh.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>().Environment.SetHandle(AssetHandle(2001));
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			environment.Patch<EnvironmentComponent>([](EnvironmentComponent& e)
			{
				e.FallbackColor = glm::vec3(0);
			});
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			auto map = CreateRef<EnvironmentData>();
			map->IrradianceSH9[0] = glm::vec3(1);
			assets.Publish(AssetHandle(2001), map);
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
			map = CreateRef<EnvironmentData>();
			assets.Publish(AssetHandle(2001), map);
			environment.Patch<EnvironmentComponent>([](EnvironmentComponent& e)
			{
				e.FallbackColor = glm::vec3(1);
			});
			CHECK(EvaluateRenderSceneValidation(scene, assets)->NoLighting);
		}

		TEST_CASE("RenderSceneValidation: only enabled contributing shadowed spots consume the global budget")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			Entity mesh = scene.CreateEntity("Cube");
			mesh.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>().Intensity = 0;
			for (int i = 0; i < 9; ++i)
				scene.CreateEntity("Spot").AddComponent<SpotLightComponent>().CastShadows = true;
			Entity invalid = scene.CreateEntity("Invalid");
			invalid.AddComponent<SpotLightComponent>().CastShadows = true;
			invalid.Patch<SpotLightComponent>([](SpotLightComponent& s)
			{
				s.Intensity = std::numeric_limits<float>::quiet_NaN();
			});
			Entity disabled = scene.CreateEntity("Disabled");
			disabled.AddComponent<SpotLightComponent>().CastShadows = true;
			disabled.SetActive(false);
			Entity dark = scene.CreateEntity("Dark");
			dark.AddComponent<SpotLightComponent>().CastShadows = true;
			dark.Patch<SpotLightComponent>([](SpotLightComponent& s)
			{
				s.Range = 0;
			});
			const auto result = EvaluateRenderSceneValidation(scene, assets);
			REQUIRE(result);
			CHECK(result->ShadowedSpotLights == 9);
			CHECK(result->ShadowedPointLights == 0);
			CHECK_FALSE(result->NoLighting);
		}

		// M23 (R-PLS; Docs/Decisions/0034-m23-contract.md decision 18): PointLight version 2's CastShadows reaches the scan, which
		// counts the point lights the way it counts the spots.
		TEST_CASE("RenderSceneValidation: only enabled contributing shadowed point lights consume the point budget")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			for (int i = 0; i < 5; ++i)
				scene.CreateEntity("Bulb").AddComponent<PointLightComponent>().CastShadows = true;
			scene.CreateEntity("Unshadowed").AddComponent<PointLightComponent>();
			Entity disabled = scene.CreateEntity("Disabled");
			disabled.AddComponent<PointLightComponent>().CastShadows = true;
			disabled.SetActive(false);
			Entity dark = scene.CreateEntity("Dark");
			dark.AddComponent<PointLightComponent>().CastShadows = true;
			dark.Patch<PointLightComponent>([](PointLightComponent& light)
			{
				light.Intensity = 0;
			});
			Entity invalid = scene.CreateEntity("Invalid");
			invalid.AddComponent<PointLightComponent>().CastShadows = true;
			invalid.Patch<PointLightComponent>([](PointLightComponent& light)
			{
				light.Range = std::numeric_limits<float>::quiet_NaN();
			});
			const auto result = EvaluateRenderSceneValidation(scene, assets);
			REQUIRE(result);
			CHECK(result->ShadowedPointLights == 5);
			CHECK(result->ShadowedSpotLights == 0);
		}

		TEST_CASE("RenderSceneValidation: nonfinite environment and emission return classification errors")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Scene& scene = fixture.GetScene();
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>().Intensity = std::numeric_limits<float>::infinity();
			const auto invalid = EvaluateRenderSceneValidation(scene, assets);
			REQUIRE_FALSE(invalid);
			CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
			environment.Patch<EnvironmentComponent>([](EnvironmentComponent& e)
			{
				e.Intensity = 0;
			});
			Entity mesh = scene.CreateEntity("Cube");
			mesh.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			mesh.Patch<MeshRendererComponent>([](MeshRendererComponent& r)
			{
				r.Materials.resize(1);
				r.Materials[0].SetHandle(AssetHandle(2000));
			});
			auto emission = CreateRef<MaterialData>();
			emission->EmissiveStrength = std::numeric_limits<float>::quiet_NaN();
			assets.Publish(AssetHandle(2000), emission);
			CHECK_FALSE(EvaluateRenderSceneValidation(scene, assets));
		}
	}

}
