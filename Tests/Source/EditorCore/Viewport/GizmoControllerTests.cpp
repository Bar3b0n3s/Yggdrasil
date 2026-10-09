#include "TestsPCH.h"
#include "EditorCore/Viewport/GizmoController.h"

#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"
#include "Support/EditorTestFixture.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <limits>

namespace Engine {

	namespace Utils {

		static std::string ViewportTestSceneText(const Scene& scene)
		{
			const auto result = SceneSerializer::SaveToString(scene);
			REQUIRE(result.has_value());
			return *result;
		}

		static glm::mat4 ViewportTestTranslation(float x, float y = 0.0f, float z = 0.0f)
		{
			return glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
		}

		static glm::vec3 ViewportTestPosition(const Scene& scene, UUID id)
		{
			return TransformSystem::GetWorldPosition(scene.FindEntityByID(id));
		}

		static void ViewportTestNear(const glm::vec3& actual, const glm::vec3& expected)
		{
			CHECK(glm::length(actual - expected) < 1.0e-4f);
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("GizmoController: a drag produces one merged undo step")
		{
			Test::EditorTestFixture fixture("GizmoUndo");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const UUID id = editor.GetScene().CreateEntity("Box").GetUUID();
			const std::array selected{ id };
			const uint64_t revision = editor.GetRevision();
			GizmoController gizmo(editor);
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			CHECK_FALSE(editor.GetScene().GetChangeTracker().IsTracking());
			for (int step = 1; step <= 10; ++step)
			{
				REQUIRE(gizmo.Update(Utils::ViewportTestTranslation(static_cast<float>(step))).has_value());
				REQUIRE(gizmo.GetPreviewScene() != nullptr);
				Utils::ViewportTestNear(Utils::ViewportTestPosition(*gizmo.GetPreviewScene(), id), { static_cast<float>(step), 0.0f, 0.0f });
				Utils::ViewportTestNear(Utils::ViewportTestPosition(editor.GetScene(), id), glm::vec3(0.0f));
				CHECK(editor.GetRevision() == revision);
				CHECK(editor.GetHistory().GetUndoCount() == 0);
				CHECK_FALSE(editor.IsSceneDirty());
			}
			const auto committed = gizmo.End();
			REQUIRE(committed.has_value());
			CHECK(*committed != 0);
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			CHECK_FALSE(gizmo.IsDragging());
			CHECK(gizmo.GetPreviewScene() == nullptr);
			Utils::ViewportTestNear(Utils::ViewportTestPosition(editor.GetScene(), id), { 10.0f, 0.0f, 0.0f });
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			Utils::ViewportTestNear(Utils::ViewportTestPosition(editor.GetScene(), id), glm::vec3(0.0f));
			REQUIRE(editor.GetHistory().Redo(editor).has_value());
			Utils::ViewportTestNear(Utils::ViewportTestPosition(editor.GetScene(), id), { 10.0f, 0.0f, 0.0f });
		}

		TEST_CASE("GizmoController: escape discards the preview without changing history")
		{
			Test::EditorTestFixture fixture("GizmoCancel");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const std::array selected{ editor.GetScene().CreateEntity("Box").GetUUID() };
			const auto before = Utils::ViewportTestSceneText(editor.GetScene());
			const uint64_t revision = editor.GetRevision();
			GizmoController gizmo(editor);
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			REQUIRE(gizmo.Update(Utils::ViewportTestTranslation(5.0f)).has_value());
			gizmo.Cancel();
			gizmo.Cancel();
			CHECK_FALSE(gizmo.IsDragging());
			CHECK(gizmo.GetPreviewScene() == nullptr);
			CHECK(Utils::ViewportTestSceneText(editor.GetScene()) == before);
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().GetUndoCount() == 0);
			CHECK_FALSE(gizmo.End().has_value());
		}

		TEST_CASE("GizmoController: an agent edit during a drag is preserved and causes Conflict")
		{
			Test::EditorTestFixture fixture("GizmoConflict");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const Entity box = editor.GetScene().CreateEntity("Box");
			const std::array selected{ box.GetUUID() };
			GizmoController gizmo(editor);
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			REQUIRE(gizmo.Update(Utils::ViewportTestTranslation(8.0f)).has_value());
			{
				SceneEdit agent(editor, "Interleaved edit");
				box.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation.x = 3.0f;
				});
				REQUIRE(agent.Commit().has_value());
			}
			CHECK(gizmo.GetPreviewScene() == nullptr);
			const auto conflict = gizmo.End();
			REQUIRE_FALSE(conflict.has_value());
			CHECK(conflict.error().GetCode() == ErrorCode::Conflict);
			CHECK_FALSE(gizmo.IsDragging());
			CHECK(box.GetComponent<TransformComponent>().Translation.x == 3.0f);
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK(box.GetComponent<TransformComponent>().Translation.x == 0.0f);
		}

		TEST_CASE("GizmoController: selecting a parent and child applies the world delta once")
		{
			Test::EditorTestFixture fixture("GizmoHierarchy");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const Entity parent = editor.GetScene().CreateEntity("Parent");
			const Entity child = editor.GetScene().CreateEntity("Child", parent);
			child.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.x = 2.0f;
			});
			const std::array selected{ child.GetUUID(), parent.GetUUID(), child.GetUUID() };
			GizmoController gizmo(editor);
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			REQUIRE(gizmo.Update(Utils::ViewportTestTranslation(3.0f)).has_value());
			REQUIRE(gizmo.End().has_value());
			Utils::ViewportTestNear(TransformSystem::GetWorldPosition(parent), { 3.0f, 0.0f, 0.0f });
			Utils::ViewportTestNear(TransformSystem::GetWorldPosition(child), { 5.0f, 0.0f, 0.0f });
			CHECK(child.GetComponent<TransformComponent>().Translation.x == 2.0f);
			CHECK(editor.GetHistory().GetUndoCount() == 1);
		}

		TEST_CASE("GizmoController: local and world manipulation preserve parent transforms")
		{
			for (const GizmoSpace space : { GizmoSpace::Local, GizmoSpace::World })
			{
				Test::EditorTestFixture fixture("GizmoParentSpace");
				fixture.CreateAndOpenProject();
				fixture.CreateAndOpenScene();
				EditorContext& editor = fixture.GetEditor();
				const Entity parent = editor.GetScene().CreateEntity("Parent");
				parent.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = { 7.0f, 2.0f, 1.0f };
					transform.Rotation = TransformSystem::QuaternionFromEulerDegrees({ 0.0f, 0.0f, 90.0f });
					transform.Scale = { 2.0f, 3.0f, 1.0f };
				});
				const Entity child = editor.GetScene().CreateEntity("Child", parent);
				const glm::mat4 before = TransformSystem::ComputeWorldMatrix(parent);
				const std::array selected{ child.GetUUID() };
				GizmoController gizmo(editor);
				REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), { .Space = space }).has_value());
				REQUIRE(gizmo.Update(Utils::ViewportTestTranslation(4.0f, 1.0f)).has_value());
				REQUIRE(gizmo.End().has_value());
				CHECK(TransformSystem::ComputeWorldMatrix(parent) == before);
				Utils::ViewportTestNear(TransformSystem::GetWorldPosition(child), { 11.0f, 3.0f, 1.0f });
			}
		}

		TEST_CASE("GizmoController: noninvertible parents and shear fail before any write")
		{
			Test::EditorTestFixture fixture("GizmoValidation");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const Entity first = editor.GetScene().CreateEntity("First");
			const Entity parent = editor.GetScene().CreateEntity("Parent");
			const Entity child = editor.GetScene().CreateEntity("Child", parent);
			parent.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale.x = 0.0f;
			});
			const std::array selected{ first.GetUUID(), child.GetUUID() };
			GizmoController gizmo(editor);
			const auto singular = gizmo.Begin(selected, glm::mat4(1.0f), {});
			REQUIRE_FALSE(singular.has_value());
			CHECK(singular.error().GetCode() == ErrorCode::Validation);
			CHECK_FALSE(gizmo.IsDragging());
			parent.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale.x = 2.0f;
			});
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), { .Operation = GizmoOperation::Rotate }).has_value());
			REQUIRE(gizmo.Update(Utils::ViewportTestTranslation(2.0f)).has_value());
			const auto previewBefore = Utils::ViewportTestSceneText(*gizmo.GetPreviewScene());
			const glm::mat4 rotate = glm::mat4_cast(TransformSystem::QuaternionFromEulerDegrees({ 0.0f, 0.0f, 45.0f }));
			const auto shear = gizmo.Update(rotate);
			REQUIRE_FALSE(shear.has_value());
			CHECK(shear.error().GetCode() == ErrorCode::Validation);
			CHECK(Utils::ViewportTestSceneText(*gizmo.GetPreviewScene()) == previewBefore);
			CHECK(gizmo.IsDragging());
			Utils::ViewportTestNear(TransformSystem::GetWorldPosition(first), glm::vec3(0.0f));
			glm::mat4 invalid = glm::mat4(1.0f);
			invalid[3][0] = std::numeric_limits<float>::quiet_NaN();
			CHECK_FALSE(gizmo.Update(invalid).has_value());
			CHECK(Utils::ViewportTestSceneText(*gizmo.GetPreviewScene()) == previewBefore);
			REQUIRE(gizmo.End().has_value());
			Utils::ViewportTestNear(TransformSystem::GetWorldPosition(first), { 2.0f, 0.0f, 0.0f });
			Utils::ViewportTestNear(TransformSystem::GetWorldPosition(child), { 2.0f, 0.0f, 0.0f });
		}

		TEST_CASE("GizmoController: read-only scenes reject a drag and play edits are transient")
		{
			SUBCASE("Read-only")
			{
				Test::EditorTestFixture fixture("GizmoReadOnly");
				fixture.CreateAndOpenProject();
				EditorContext& editor = fixture.GetEditor();
				const auto projectFile = editor.GetProject().GetProjectFile();
				REQUIRE(editor.CloseProject());
				auto project = ProjectManager::OpenProject(projectFile,
					{ .ReadOnly = true, .ReadOnlyCacheDirectory = fixture.GetDirectory() / "ReadOnlyCache" }, fixture.GetEngine().GetTypeRegistry());
				REQUIRE(project.has_value());
				REQUIRE(editor.OpenProject(std::move(*project)).has_value());
				editor.SetScene(editor.CreateScene("ReadOnly"), std::nullopt);
				const std::array selected{ editor.GetScene().CreateEntity("Box").GetUUID() };
				GizmoController gizmo(editor);
				const auto result = gizmo.Begin(selected, glm::mat4(1.0f), {});
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == ErrorCode::PermissionDenied);
			}
			for (const PlayMode mode : { PlayMode::Play, PlayMode::Simulate })
			{
				Test::EditorTestFixture fixture("GizmoPlay");
				fixture.CreateAndOpenProject();
				fixture.CreateAndOpenScene();
				EditorContext& editor = fixture.GetEditor();
				const std::array selected{ editor.GetScene().CreateEntity("Box").GetUUID() };
				const auto before = Utils::ViewportTestSceneText(editor.GetScene());
				REQUIRE(editor.GetPlay().Start({ .Mode = mode, .Lockstep = true, .Paused = true }).has_value());
				GizmoController gizmo(editor);
				REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
				REQUIRE(gizmo.Update(Utils::ViewportTestTranslation(4.0f)).has_value());
				const auto result = gizmo.End();
				REQUIRE(result.has_value());
				CHECK(*result == 0);
				Utils::ViewportTestNear(Utils::ViewportTestPosition(editor.GetPlay().GetSession()->GetScene(), selected[0]), { 4.0f, 0.0f, 0.0f });
				CHECK(Utils::ViewportTestSceneText(editor.GetScene()) == before);
				CHECK(editor.GetHistory().GetUndoCount() == 0);
				REQUIRE(editor.GetPlay().Stop().has_value());
				CHECK(Utils::ViewportTestSceneText(editor.GetScene()) == before);
			}
		}

		TEST_CASE("GizmoController: snapping uses metres degrees and scale increments")
		{
			for (const GizmoOperation operation : { GizmoOperation::Translate, GizmoOperation::Rotate, GizmoOperation::Scale })
			{
				Test::EditorTestFixture fixture("GizmoSnap");
				fixture.CreateAndOpenProject();
				fixture.CreateAndOpenScene();
				EditorContext& editor = fixture.GetEditor();
				const Entity entity = editor.GetScene().CreateEntity("Box");
				const std::array selected{ entity.GetUUID() };
				GizmoController gizmo(editor);
				REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), { .Operation = operation, .Snap = true }).has_value());
				glm::mat4 pivot(1.0f);
				switch (operation)
				{
					case GizmoOperation::Translate: pivot = Utils::ViewportTestTranslation(0.74f); break;
					case GizmoOperation::Rotate:    pivot = glm::mat4_cast(TransformSystem::QuaternionFromEulerDegrees({ 0.0f, 0.0f, 23.0f })); break;
					case GizmoOperation::Scale:     pivot = glm::scale(glm::mat4(1.0f), glm::vec3(1.24f)); break;
				}
				REQUIRE(gizmo.Update(pivot).has_value());
				REQUIRE(gizmo.End().has_value());
				const TransformComponent pose = entity.GetComponent<TransformComponent>();
				switch (operation)
				{
					case GizmoOperation::Translate: CHECK(pose.Translation.x == doctest::Approx(0.5f)); break;
					case GizmoOperation::Rotate:    CHECK(TransformSystem::EulerDegreesFromQuaternion(pose.Rotation).z == doctest::Approx(30.0f)); break;
					case GizmoOperation::Scale:     CHECK(pose.Scale.x == doctest::Approx(1.2f)); break;
				}
			}
		}

		TEST_CASE("GizmoController: scene close and focus loss discard the preview")
		{
			Test::EditorTestFixture fixture("GizmoLifecycle");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const std::array selected{ editor.GetScene().CreateEntity("Box").GetUUID() };
			GizmoController gizmo(editor);
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			gizmo.Cancel();
			CHECK_FALSE(gizmo.IsDragging());
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			editor.CloseScene();
			CHECK(gizmo.GetPreviewScene() == nullptr);
			const auto result = gizmo.Update(glm::mat4(1.0f));
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Conflict);
			CHECK_FALSE(gizmo.IsDragging());
		}

		TEST_CASE("GizmoController: returning to the initial pivot creates no history or revision")
		{
			Test::EditorTestFixture fixture("GizmoNoop");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const std::array selected{ editor.GetScene().CreateEntity("Box").GetUUID() };
			const uint64_t revision = editor.GetRevision();
			GizmoController gizmo(editor);
			CHECK_FALSE(gizmo.Begin({}, glm::mat4(1.0f), {}).has_value());
			CHECK_FALSE(gizmo.Begin(selected, glm::mat4(0.0f), {}).has_value());
			CHECK_FALSE(gizmo.Begin(selected, glm::mat4(1.0f), { .TranslationSnap = 0.0f }).has_value());
			const std::array missing{ UUID(123) };
			const auto notFound = gizmo.Begin(missing, glm::mat4(1.0f), {});
			REQUIRE_FALSE(notFound.has_value());
			CHECK(notFound.error().GetCode() == ErrorCode::NotFound);
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			CHECK_FALSE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			REQUIRE(gizmo.Update(Utils::ViewportTestTranslation(2.0f)).has_value());
			REQUIRE(gizmo.Update(glm::mat4(1.0f)).has_value());
			const auto result = gizmo.End();
			REQUIRE(result.has_value());
			CHECK(*result == 0);
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("GizmoController: play transitions and undo invalidate a captured drag")
		{
			Test::EditorTestFixture fixture("GizmoTransitions");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const Entity entity = editor.GetScene().CreateEntity("Box");
			const std::array selected{ entity.GetUUID() };
			GizmoController gizmo(editor);
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			REQUIRE(editor.GetPlay().Start({ .Lockstep = true }).has_value());
			const auto started = gizmo.End();
			REQUIRE_FALSE(started.has_value());
			CHECK(started.error().GetCode() == ErrorCode::Conflict);
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			REQUIRE(editor.GetPlay().Stop().has_value());
			REQUIRE(editor.GetPlay().Start({ .Lockstep = true }).has_value());
			const auto restarted = gizmo.End();
			REQUIRE_FALSE(restarted.has_value());
			CHECK(restarted.error().GetCode() == ErrorCode::Conflict);
			REQUIRE(editor.GetPlay().Stop().has_value());
			{
				SceneEdit edit(editor, "Rename");
				entity.SetName("Renamed");
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			const auto undone = gizmo.End();
			REQUIRE_FALSE(undone.has_value());
			CHECK(undone.error().GetCode() == ErrorCode::Conflict);
		}
		TEST_CASE("GizmoController: play ticks preserve idle poses and direct runtime writes conflict")
		{
			Test::EditorTestFixture fixture("GizmoRuntimeConflict");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const std::array selected{ editor.GetScene().CreateEntity("Box").GetUUID() };
			REQUIRE(editor.GetPlay().Start({ .Lockstep = true }).has_value());
			PlaySession& session = *editor.GetPlay().GetSession();
			GizmoController gizmo(editor);
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			session.Tick();
			REQUIRE(gizmo.Update(Utils::ViewportTestTranslation(4.0f)).has_value());
			REQUIRE(gizmo.End().has_value());
			const Entity runtime = session.GetScene().FindEntityByID(selected[0]);
			CHECK(runtime.GetComponent<TransformComponent>().Translation.x == 4.0f);
			REQUIRE(gizmo.Begin(selected, Utils::ViewportTestTranslation(4.0f), {}).has_value());
			// Physics can write a pose without going through the authored mutation counter.
			runtime.GetComponent<TransformComponent>().Translation.x = 9.0f;
			const auto conflict = gizmo.End();
			REQUIRE_FALSE(conflict.has_value());
			CHECK(conflict.error().GetCode() == ErrorCode::Conflict);
			CHECK(runtime.GetComponent<TransformComponent>().Translation.x == 9.0f);
			CHECK(editor.GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("GizmoController: a runtime parent move withdraws the preview before release")
		{
			Test::EditorTestFixture fixture("GizmoRuntimeParentConflict");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const Entity parent = editor.GetScene().CreateEntity("Parent");
			const UUID parentId = parent.GetUUID();
			const std::array selected{ editor.GetScene().CreateEntity("Child", parent).GetUUID() };
			REQUIRE(editor.GetPlay().Start({ .Lockstep = true }).has_value());
			Scene& runtime = editor.GetPlay().GetSession()->GetScene();
			const uint64_t revision = editor.GetRevision();
			GizmoController gizmo(editor);
			REQUIRE(gizmo.Begin(selected, glm::mat4(1.0f), {}).has_value());
			REQUIRE(gizmo.Update(Utils::ViewportTestTranslation(4.0f)).has_value());
			REQUIRE(gizmo.GetPreviewScene() != nullptr);
			// A parent pose written by simulation changes the child's world matrix without changing its local pose.
			runtime.FindEntityByID(parentId).GetComponent<TransformComponent>().Translation.x = 12.0f;
			CHECK(editor.GetRevision() == revision);
			CHECK(gizmo.GetPreviewScene() == nullptr);
			const auto conflict = gizmo.End();
			REQUIRE_FALSE(conflict.has_value());
			CHECK(conflict.error().GetCode() == ErrorCode::Conflict);
			CHECK_FALSE(gizmo.IsDragging());
			Utils::ViewportTestNear(Utils::ViewportTestPosition(runtime, selected[0]), { 12.0f, 0.0f, 0.0f });
			Utils::ViewportTestNear(Utils::ViewportTestPosition(editor.GetScene(), selected[0]), glm::vec3(0.0f));
			CHECK(editor.GetHistory().GetUndoCount() == 0);
			CHECK_FALSE(editor.IsSceneDirty());
		}

		TEST_CASE("GizmoController: local snapping follows the captured rotated axes")
		{
			Test::EditorTestFixture fixture("GizmoLocalSnap");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const Entity entity = editor.GetScene().CreateEntity("Box");
			const std::array selected{ entity.GetUUID() };
			const glm::mat4 initial = glm::mat4_cast(TransformSystem::QuaternionFromEulerDegrees({ 0.0f, 0.0f, 45.0f }));
			const glm::mat4 moved = initial * Utils::ViewportTestTranslation(0.74f);
			GizmoController gizmo(editor);
			REQUIRE(gizmo.Begin(selected, initial, { .Space = GizmoSpace::Local, .Snap = true }).has_value());
			REQUIRE(gizmo.Update(moved).has_value());
			REQUIRE(gizmo.End().has_value());
			Utils::ViewportTestNear(entity.GetComponent<TransformComponent>().Translation, glm::vec3(initial * glm::vec4(0.5f, 0.0f, 0.0f, 0.0f)));
		}
	}

}
