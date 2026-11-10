#include "TestsPCH.h"
#include "Editor/Panels/AnimationPreviewPanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "EditorCore/Animation/AnimationPreview.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The editor's animation preview panel (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 32; W23.3's review): when it
// is shown, and the animation.preview requests its controls queue for the editor's safe point (EditorActions), so the panel
// previews exactly as automation does.

namespace Engine {

	namespace {

		constexpr std::string_view Armature = "/Skinned/Armature";
		// Shown only while the panel offers a preview (AnimationPreviewPanel.cpp's text without one).
		constexpr std::string_view OfferText = "Edit scenes draw skinned meshes in their bind pose";

		// The skinned glTF fixture (Tests/Data/Assets/Gltf/Skinned.gltf: "Armature" holds the Animator, "Body" the
		// SkinnedMesh) imported into the project and instantiated in the open scene as "/Skinned".
		void InstantiateSkinnedFixture(Test::UtilityPanelFixture& fixture)
		{
			const std::filesystem::path models = fixture.GetEditor().GetProject().GetRoot() / "Assets/Models";
			const Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/Skinned.gltf"));
			REQUIRE(bytes.has_value());
			REQUIRE(FileSystem::CreateDirectories(models).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(models / "Skinned.gltf", *bytes).has_value());
			REQUIRE(fixture.GetClient().Call("project.refreshAssets", Json::object()));
			REQUIRE(fixture.GetClient().Call("prefab.instantiate", Json{ { "prefab", "Assets/Models/Skinned.gltf" } }));
		}

		UUID FindEntityId(EditorContext& editor, std::string_view path)
		{
			const Entity entity = editor.GetScene().FindEntityByPath(path);
			REQUIRE(entity.IsValid());
			return entity.GetUUID();
		}

		// Clicks the control labelled `label` (its `occurrence`-th text: the window's title "Animation Preview" comes before
		// the Preview button), checks that it queued a request, and runs it at the safe point.
		void ClickAndPump(Test::UtilityPanelFixture& fixture, AnimationPreviewPanel& panel, std::string_view label, size_t occurrence = 0)
		{
			INFO(std::string(label));
			const uint64_t before = fixture.ProbeActionTicket();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, label, occurrence);
			REQUIRE(fixture.FindPendingAction(before).has_value());
			fixture.Pump();
			static_cast<void>(fixture.Draw(panel)); // takes the request's result
		}

	}

	TEST_SUITE("Editor")
	{
		// The panel is shown while the active edit scene's selection is one entity holding an Animator, or while a preview of
		// that scene runs; never without such a selection, for an entity without an Animator, for several entities, or while
		// a play session runs (the session poses its Animators itself).
		TEST_CASE("AnimationPreviewPanel: the panel is shown only for one selected Animator of the edit scene, and never while playing")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			EditorContext& editor = fixture.GetEditor();
			AnimationPreviewPanel panel;
			CHECK_FALSE(fixture.Draw(panel).contains(OfferText));
			InstantiateSkinnedFixture(fixture);
			const UUID armature = FindEntityId(editor, Armature);
			const UUID body = FindEntityId(editor, "/Skinned/Armature/Body");

			CHECK_FALSE(fixture.Draw(panel).contains(OfferText));
			REQUIRE(editor.SetSelection({ body }, SceneTarget::Edit));
			CHECK_FALSE(fixture.Draw(panel).contains(OfferText));
			REQUIRE(editor.SetSelection({ armature, body }, SceneTarget::Edit));
			CHECK_FALSE(fixture.Draw(panel).contains(OfferText));
			REQUIRE(editor.SetSelection({ armature }, SceneTarget::Edit));
			const std::string shown = fixture.Draw(panel);
			CHECK(shown.contains(OfferText));
			CHECK(shown.contains(Armature));

			// A running preview keeps the panel without the selection.
			REQUIRE(editor.GetAnimationPreview().Begin(editor, editor.GetActiveSceneId(), armature, editor.GetAssets(), 0.0f, false).has_value());
			editor.SetSelection({});
			CHECK(fixture.Draw(panel).contains("Stop"));
			editor.GetAnimationPreview().End(editor);

			REQUIRE(editor.SetSelection({ armature }, SceneTarget::Edit));
			REQUIRE(fixture.GetClient().Call("scene.save", Json::object()));
			REQUIRE(fixture.GetClient().Call("play.start", Json{ { "lockstep", true } }));
			REQUIRE(editor.GetPlay().GetSession() != nullptr);
			CHECK_FALSE(fixture.Draw(panel).contains(OfferText));
			REQUIRE(fixture.GetClient().Call("play.stop", Json::object()));
		}

		// Each control is the animation.preview request an agent sends: Preview starts a paused preview at time 0, Play and
		// Pause send the flipped playing flag alone, a time set on the slider is sent paused, and Stop ends the preview.
		TEST_CASE("AnimationPreviewPanel: Preview, Play, Pause, the time slider and Stop queue animation.preview")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			EditorContext& editor = fixture.GetEditor();
			InstantiateSkinnedFixture(fixture);
			const UUID armature = FindEntityId(editor, Armature);
			REQUIRE(editor.SetSelection({ armature }, SceneTarget::Edit));
			AnimationPreviewPanel panel;
			const std::optional<AnimationPreviewState>& state = editor.GetAnimationPreview().GetState();
			static_cast<void>(fixture.Draw(panel)); // the window's first frame only sizes it (ImGui draws nothing then)

			ClickAndPump(fixture, panel, "Preview", 1);
			REQUIRE(state.has_value());
			CHECK(state->Entity == armature);
			CHECK(state->Scene == editor.GetActiveSceneId());
			CHECK(state->Time == 0.0f);
			CHECK_FALSE(state->Playing);
			CHECK(state->Duration == 1.0f); // the fixture's "Wave" clip
			CHECK(editor.GetScene().FindEntityByID(armature).HasComponent<AnimationPoseComponent>());

			ClickAndPump(fixture, panel, "Play");
			REQUIRE(state.has_value());
			CHECK(state->Playing);
			CHECK(state->Time == 0.0f);
			ClickAndPump(fixture, panel, "Pause");
			REQUIRE(state.has_value());
			CHECK_FALSE(state->Playing);

			// The slider shows the preview's time; a click on it sets a time within the clip, sent paused.
			ClickAndPump(fixture, panel, "Play");
			REQUIRE(state.has_value());
			REQUIRE(state->Playing);
			ClickAndPump(fixture, panel, "0.000 s");
			REQUIRE(state.has_value());
			CHECK(state->Time > 0.0f);
			CHECK(state->Time < 1.0f);
			CHECK_FALSE(state->Playing);

			ClickAndPump(fixture, panel, "Stop");
			CHECK_FALSE(state.has_value());
			CHECK_FALSE(editor.GetScene().FindEntityByID(armature).HasComponent<AnimationPoseComponent>());
			CHECK(fixture.Draw(panel).contains(OfferText));
		}

		// A request the editor refuses (an Animator without a Skeleton has nothing to pose) is reported under the controls,
		// and logged.
		TEST_CASE("AnimationPreviewPanel: a refused preview shows its error")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			EditorContext& editor = fixture.GetEditor();
			Entity bare = editor.GetScene().CreateEntity("Bare");
			bare.AddComponent<AnimatorComponent>();
			REQUIRE(editor.SetSelection({ bare.GetUUID() }, SceneTarget::Edit));
			AnimationPreviewPanel panel;
			static_cast<void>(fixture.Draw(panel)); // the window's first frame only sizes it (ImGui draws nothing then)
			const uint64_t before = fixture.ProbeActionTicket();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Preview", 1); // the button, below the window's title
			REQUIRE(fixture.FindPendingAction(before).has_value());
			fixture.Pump();
			{
				const Test::ExpectLog refused(LogLevel::Error, "Animation preview");
				static_cast<void>(fixture.Draw(panel));
				CHECK(refused.GetMatchCount() == 1);
			}
			const std::string shown = fixture.Draw(panel);
			CHECK(shown.contains("no Skeleton to pose"));
			CHECK_FALSE(editor.GetAnimationPreview().GetState().has_value());
		}
	}

}
