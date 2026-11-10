#include "TestsPCH.h"

#include "EditorCore/Automation/AnimationMethods.h"

#include "EditorCore/Animation/AnimationPreview.h"
#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>

// animation.preview (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 32) through the in-process server: the
// editor's animation preview over an imported skinned glTF, as automation drives it.

namespace Engine {

	namespace {

		constexpr std::string_view Armature = "/Skinned/Armature";

		Json CallOrFail(Test::AutomationFixture& setup, std::string_view method, const Json& params)
		{
			const Result<Json> result = setup.Call(method, params);
			INFO(method);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return *result;
		}

		// Copies the skinned glTF fixture (Tests/Data/Assets/Gltf/Skinned.gltf: "Armature" holds the Animator, "Body" the
		// SkinnedMesh) into the project, imports it and instantiates it in the open scene as "/Skinned".
		void InstantiateSkinnedFixture(Test::AutomationFixture& setup)
		{
			const std::filesystem::path models = setup.GetEditorFixture().GetProjectRoot() / "Assets/Models";
			const Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/Skinned.gltf"));
			REQUIRE(bytes.has_value());
			REQUIRE(FileSystem::CreateDirectories(models).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(models / "Skinned.gltf", *bytes).has_value());
			CallOrFail(setup, "project.refreshAssets", Json::object());
			CallOrFail(setup, "prefab.instantiate", Json{ { "prefab", "Assets/Models/Skinned.gltf" } });
		}

		[[nodiscard]] bool IsPosed(EditorContext& editor)
		{
			const Entity armature = editor.GetScene().FindEntityByPath(Armature);
			REQUIRE(armature.IsValid());
			return armature.HasComponent<AnimationPoseComponent>();
		}

	}

	TEST_SUITE("Automation")
	{
		// The round trip of decision 32's wire format: a preview poses the edit scene without a revision or an undo step, keeps
		// its time and playing flag when they are absent, advances with the editor's frames while playing, ends with stop,
		// and refuses a non-Animator entity, a time below 0, stop with a time and, while playing, the play scene.
		TEST_CASE("AnimationMethods: animation.preview poses the target scene's Animator, keeps its time and refuses the play scene")
		{
			Test::AutomationFixture setup("AnimationPreviewMethod");
			EditorContext& editor = setup.GetEditor();
			InstantiateSkinnedFixture(setup);
			const uint64_t revision = editor.GetRevision();
			const size_t undoCount = editor.GetHistory().GetUndoCount();
			CHECK_FALSE(IsPosed(editor));

			const Json started = CallOrFail(setup, "animation.preview", Json{ { "entity", Armature }, { "time", 0.25 } });
			CHECK(started["entity"]["path"] == Json(Armature));
			CHECK(started["previewing"] == Json(true));
			CHECK(started["time"] == Json(0.25));
			CHECK(started["playing"] == Json(false));
			CHECK(started["duration"] == Json(1.0)); // the fixture's "Wave" clip
			CHECK(IsPosed(editor));
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().GetUndoCount() == undoCount);

			// An absent time and playing keep the running preview's; playing, the editor's frames advance the time.
			const Json kept = CallOrFail(setup, "animation.preview", Json{ { "entity", Armature }, { "playing", true } });
			CHECK(kept["time"] == Json(0.25));
			CHECK(kept["playing"] == Json(true));
			editor.GetAnimationPreview().Update(editor, editor.GetAssets(), 0.5);
			const Json advanced = CallOrFail(setup, "animation.preview", Json{ { "entity", Armature } });
			CHECK(advanced["time"] == Json(0.75));
			CHECK(advanced["playing"] == Json(true));
			// The fixture's clip loops, so the time wraps at its end.
			editor.GetAnimationPreview().Update(editor, editor.GetAssets(), 0.5);
			REQUIRE(editor.GetAnimationPreview().GetState().has_value());
			CHECK(editor.GetAnimationPreview().GetState()->Time == doctest::Approx(0.25f));

			// Refusals, each before anything changes.
			const Result<Json> body = setup.Call("animation.preview", Json{ { "entity", "/Skinned/Armature/Body" }, { "time", 0.0 } });
			REQUIRE_FALSE(body.has_value());
			CHECK(body.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(body.error().GetIssues().empty());
			CHECK(body.error().GetIssues().front().JsonPointer == "/entity");
			CHECK_FALSE(setup.Call("animation.preview", Json{ { "entity", Armature }, { "time", -1.0 } }).has_value());
			const Result<Json> stopTimed = setup.Call("animation.preview", Json{ { "entity", Armature }, { "stop", true }, { "time", 0.5 } });
			REQUIRE_FALSE(stopTimed.has_value());
			CHECK(stopTimed.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(stopTimed.error().GetIssues().empty());
			CHECK(stopTimed.error().GetIssues().front().JsonPointer == "/time");
			CHECK(IsPosed(editor));
			CHECK(editor.GetAnimationPreview().GetState().has_value());

			const Json stopped = CallOrFail(setup, "animation.preview", Json{ { "entity", Armature }, { "stop", true } });
			CHECK(stopped["previewing"] == Json(false));
			CHECK_FALSE(IsPosed(editor));
			CHECK_FALSE(editor.GetAnimationPreview().GetState().has_value());
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().GetUndoCount() == undoCount);

			// While playing, a request without a scene target reads the play scene, which the session poses itself.
			CallOrFail(setup, "scene.save", Json::object());
			CallOrFail(setup, "play.start", Json{ { "lockstep", true } });
			const Json playing = setup.Request("animation.preview", Json{ { "entity", Armature } });
			REQUIRE(playing.contains("error"));
			CHECK(playing["error"]["data"]["errorCode"] == Json("InvalidState"));
			CHECK(playing["error"].dump().contains("play.stop"));
			CHECK_FALSE(editor.GetAnimationPreview().GetState().has_value());
			CallOrFail(setup, "play.stop", Json::object());

			const auto* method = setup.GetClient().GetServer().GetMethods().Find("animation.preview");
			REQUIRE(method != nullptr);
			CHECK(method->Specification.ExposeAsTool);
			CHECK(method->Specification.SupportsSceneTarget);
			CHECK_FALSE(method->Specification.Mutates);
			CHECK_FALSE(method->Specification.SupportsDryRun);
			CHECK_FALSE(method->Specification.AllowedInBatch);
			CHECK_FALSE(method->Specification.AvailableInRuntime);
		}

		// W23.3's review: the preview's time is bounded like a layer's own time (MaxAnimatorTime), so the time times a layer's
		// Speed stays finite: a time far beyond it, which a looping layer at Speed 2 would turn into an infinite step, is
		// refused at /time before anything changes, and the bound itself previews.
		TEST_CASE("AnimationMethods: animation.preview refuses a time above a layer time's bound")
		{
			Test::AutomationFixture setup("AnimationPreviewTimeBound");
			EditorContext& editor = setup.GetEditor();
			InstantiateSkinnedFixture(setup);
			Entity armature = editor.GetScene().FindEntityByPath(Armature);
			REQUIRE(armature.IsValid());
			AnimatorComponent& animator = armature.GetComponent<AnimatorComponent>();
			REQUIRE(animator.Layers.size() == 1);
			REQUIRE(animator.Layers[0].Loop);
			animator.Layers[0].Speed = 2.0f;

			const Result<Json> refused = setup.Call("animation.preview", Json{ { "entity", Armature }, { "time", 3.0e38 } });
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(refused.error().GetIssues().empty());
			CHECK(refused.error().GetIssues().front().JsonPointer == "/time");
			CHECK_FALSE(editor.GetAnimationPreview().GetState().has_value());
			CHECK_FALSE(IsPosed(editor));

			const Json bound = CallOrFail(setup, "animation.preview", Json{ { "entity", Armature }, { "time", static_cast<double>(MaxAnimatorTime) } });
			CHECK(bound["previewing"] == Json(true));
			const AnimationPoseComponent* pose = armature.TryGetComponent<AnimationPoseComponent>();
			REQUIRE(pose != nullptr);
			for (const glm::mat4& joint : pose->JointModel)
				CHECK(std::isfinite(joint[3][0] + joint[3][1] + joint[3][2]));
		}

		// AnimationMethods.h's other branches: stop ends only a preview of the target scene's entity (stop naming another
		// Animator keeps the running preview and its pose); an entity the target scene does not hold is InvalidParams at
		// /entity and changes nothing; an Animator without a Skeleton is NotFound, and leaves no preview (the running one of
		// another entity ended first) and no pose; and an explicit scene target previews an open scene that is not active.
		TEST_CASE("AnimationMethods: animation.preview's stop ends only its own preview, and an unknown entity or a missing Skeleton previews nothing")
		{
			Test::AutomationFixture setup("AnimationPreviewBranches");
			EditorContext& editor = setup.GetEditor();
			InstantiateSkinnedFixture(setup);
			Entity bare = editor.GetScene().CreateEntity("Bare");
			bare.AddComponent<AnimatorComponent>();
			CallOrFail(setup, "animation.preview", Json{ { "entity", Armature }, { "time", 0.25 } });
			REQUIRE(editor.GetAnimationPreview().GetState().has_value());

			const Json otherStop = CallOrFail(setup, "animation.preview", Json{ { "entity", "/Bare" }, { "stop", true } });
			CHECK(otherStop["previewing"] == Json(false));
			REQUIRE(editor.GetAnimationPreview().GetState().has_value());
			CHECK(editor.GetAnimationPreview().GetState()->Entity == editor.GetScene().FindEntityByPath(Armature).GetUUID());
			CHECK(editor.GetAnimationPreview().GetState()->Time == 0.25f);
			CHECK(IsPosed(editor));

			const Result<Json> unknown = setup.Call("animation.preview", Json{ { "entity", "/Nobody" }, { "time", 0.5 } });
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
			REQUIRE_FALSE(unknown.error().GetIssues().empty());
			CHECK(unknown.error().GetIssues().front().JsonPointer == "/entity");
			REQUIRE(editor.GetAnimationPreview().GetState().has_value());
			CHECK(editor.GetAnimationPreview().GetState()->Time == 0.25f);
			CHECK(IsPosed(editor));

			const Result<Json> unposed = setup.Call("animation.preview", Json{ { "entity", "/Bare" }, { "time", 0.5 } });
			REQUIRE_FALSE(unposed.has_value());
			CHECK(unposed.error().GetCode() == ErrorCode::NotFound);
			CHECK(unposed.error().GetMessageText().contains("Skeleton"));
			CHECK_FALSE(editor.GetAnimationPreview().GetState().has_value());
			CHECK_FALSE(IsPosed(editor));
			CHECK_FALSE(bare.HasComponent<AnimationPoseComponent>());

			// A second open scene, active, holding its own copy: the scene target previews the first one's.
			const OpenSceneId main = editor.GetActiveSceneId();
			CallOrFail(setup, "scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } });
			CallOrFail(setup, "prefab.instantiate", Json{ { "prefab", "Assets/Models/Skinned.gltf" } });
			REQUIRE(editor.GetActiveSceneId() != main);
			const Json targeted = CallOrFail(setup, "animation.preview", Json{ { "entity", Armature }, { "time", 0.5 }, { "scene", "Assets/Scenes/Main.scene" } });
			CHECK(targeted["previewing"] == Json(true));
			REQUIRE(editor.GetAnimationPreview().GetState().has_value());
			CHECK(editor.GetAnimationPreview().GetState()->Scene == main);
			Scene* mainScene = editor.FindScene(main);
			REQUIRE(mainScene != nullptr);
			CHECK(mainScene->FindEntityByPath(Armature).HasComponent<AnimationPoseComponent>());
			CHECK_FALSE(IsPosed(editor));
		}
	}

}
