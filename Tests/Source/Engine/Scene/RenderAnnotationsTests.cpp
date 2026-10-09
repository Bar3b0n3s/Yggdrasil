#include "TestsPCH.h"

#include "Engine/Scene/RenderAnnotations.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <variant>

namespace Engine {
	namespace {

		RenderSnapshot AnnotationView()
		{
			RenderSnapshot view;
			view.HasCamera = true;
			view.Camera.View[3].z = -10;
			return view;
		}

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
			REQUIRE(view.DebugDraw.GetSize() == 1);
			const auto* box = std::get_if<DebugBox>(&view.DebugDraw.GetCommands()[0].Shape);
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
			REQUIRE(view.DebugDraw.GetSize() == 5);
			const auto* label = std::get_if<DebugText>(&view.DebugDraw.GetCommands()[0].Shape);
			REQUIRE(label != nullptr);
			CHECK(label->Text == "Box " + entity.GetUUID().ToString().substr(0, 6));
			auto clean = AnnotationView();
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, clean));
			CHECK(clean.DebugDraw.IsEmpty());
			CHECK(fixture.GetScene().GetRevision() == revision);
			const auto before = view.DebugDraw;
			view.Alpha = -1;
			CHECK_FALSE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, view));
			CHECK(view.DebugDraw == before);
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
			CHECK(view.DebugDraw.IsEmpty());
			auto plain = AnnotationView();
			plain.Flags = RenderViewFlags::Icons;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, plain));
			CHECK(plain.Icons.empty());
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
			REQUIRE(view.DebugDraw.GetSize() == 1);
			CHECK(std::get<DebugText>(view.DebugDraw.GetCommands()[0].Shape).Text.starts_with("B "));
			auto empty = AnnotationView();
			empty.Annotations.Labels = RenderAnnotationLabels::Explicit;
			empty.Annotations.Axes = true;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, empty));
			CHECK(empty.DebugDraw.GetSize() == 6);
			auto selected = AnnotationView();
			selected.SelectedEntities = { a.GetUUID() };
			selected.Annotations.Labels = RenderAnnotationLabels::Selected;
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, selected));
			REQUIRE(selected.DebugDraw.GetSize() == 1);
			CHECK(std::get<DebugText>(selected.DebugDraw.GetCommands()[0].Shape).Text.starts_with("A "));
		}
	}

}
