#include "TestsPCH.h"
#include "Editor/Private/EditorHostClose.h"

#include "Editor/PanelInteractionFixture.h"
#include "EditorCore/Autosave/Autosave.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"

#include <doctest/doctest.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Engine {

	namespace {

		// Real editor, automation server, queued UI actions and autosave service; only the safe points are driven here.
		struct HostCloseSetup
		{
			Test::AutomationFixture Automation{ "HostClose" };
			EditorActions Actions{ Automation.GetEditor(), Automation.GetClient().GetServer() };
			Autosave Saves{ Automation.GetEditor() };
			EditorHostClose Close{ Automation.GetEditor(), Actions, Saves };

			EditorContext& GetEditor() { return Automation.GetEditor(); }
			std::filesystem::path Source() { return Automation.GetEditorFixture().GetProjectRoot() / "Assets/Scenes/Main.scene"; }
			void Dirty(std::string name)
			{
				SceneEdit edit(GetEditor(), "Unsaved edit");
				(void)GetEditor().GetScene().CreateEntity(std::move(name));
				REQUIRE(edit.Commit().has_value());
			}
			// One host safe point: the layer pumps EditorActions, then the close helper completes its choice.
			bool SafePoint()
			{
				Actions.Pump();
				return Close.Pump();
			}
			std::string ReadSource() { return FileSystem::ReadText(Source()).value_or(std::string()); }
		};

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorHostClose: a clean, scene-less or read-only editor closes at once")
		{
			HostCloseSetup setup;
			EditorContext& editor = setup.GetEditor();
			CHECK_FALSE(setup.Close.RequestClose(true));
			CHECK_FALSE(setup.Close.GetPrompt());
			editor.CloseScene();
			CHECK_FALSE(setup.Close.RequestClose(true));
			const auto projectFile = editor.GetProject().GetProjectFile();
			REQUIRE(setup.Saves.Reset());
			REQUIRE(editor.CloseProject().has_value());
			const ProjectOpenOptions options{ .ReadOnly = true, .ReadOnlyCacheDirectory = setup.Automation.GetEditorFixture().GetDirectory().GetPath() / "ReadOnly" };
			auto project = ProjectManager::OpenProject(projectFile, options, editor.GetTypeRegistry());
			REQUIRE(project.has_value());
			REQUIRE(editor.OpenProject(std::move(*project)).has_value());
			// A repaired load is the one way a read-only scene is dirty: it can never be saved, so nothing is asked.
			editor.SetScene(editor.CreateScene("Repaired"), std::nullopt, true);
			REQUIRE(editor.IsSceneDirty());
			CHECK_FALSE(setup.Close.RequestClose(true));
			CHECK_FALSE(setup.Close.GetPrompt());
			CHECK_FALSE(setup.SafePoint());
		}

		TEST_CASE("EditorHostClose: Save writes the scene through scene.save and then closes")
		{
			HostCloseSetup setup;
			setup.Dirty("SavedOnClose");
			REQUIRE(setup.Close.RequestClose(true));
			const auto prompt = setup.Close.GetPrompt();
			REQUIRE(prompt.has_value());
			CHECK(prompt->SceneName == "Main");
			CHECK(prompt->CanSave);
			CHECK_FALSE(prompt->Pending);
			// A second close request while the prompt is open is held as well.
			CHECK(setup.Close.RequestClose(true));
			REQUIRE(setup.Close.Choose(EditorCloseChoice::Save).has_value());
			CHECK(setup.Close.GetPrompt()->Pending);
			CHECK(setup.Close.Choose(EditorCloseChoice::Discard).error().GetCode() == ErrorCode::InvalidState);
			CHECK_FALSE(setup.ReadSource().contains("SavedOnClose"));
			REQUIRE(setup.SafePoint());
			CHECK_FALSE(setup.Close.GetPrompt());
			CHECK_FALSE(setup.GetEditor().IsSceneDirty());
			CHECK(setup.ReadSource().contains("SavedOnClose"));
			CHECK_FALSE(FileSystem::Exists(setup.Automation.GetEditorFixture().GetProjectRoot() / "Library/Autosave/Manifest.json"));
		}

		// Docs/Decisions/0031-m20-contract.md decision 15: the prompt lists every open scene with unsaved changes, in tab
		// order, and Don't save keeps each of them recoverable.
		TEST_CASE("EditorHostClose: the prompt lists every dirty open scene and Don't save keeps each recoverable")
		{
			HostCloseSetup setup;
			EditorContext& editor = setup.GetEditor();
			setup.Dirty("InMain");
			const Result<VfsPath> second = VfsPath::Parse("project://Assets/Scenes/Second.scene");
			REQUIRE(second.has_value());
			REQUIRE(editor.AddScene(editor.CreateScene("Second"), *second).has_value());
			setup.Dirty("InSecond");
			const Result<VfsPath> clean = VfsPath::Parse("project://Assets/Scenes/Clean.scene");
			REQUIRE(clean.has_value());
			REQUIRE(editor.AddScene(editor.CreateScene("Clean"), *clean).has_value());
			REQUIRE(setup.Close.RequestClose(true));
			const auto prompt = setup.Close.GetPrompt();
			REQUIRE(prompt.has_value());
			CHECK(prompt->SceneNames == std::vector<std::string>{ "Main", "Second" });
			CHECK(prompt->SceneName == "Main, Second");
			CHECK(prompt->CanSave);
			REQUIRE(setup.Close.Choose(EditorCloseChoice::Discard).has_value());
			REQUIRE(setup.SafePoint());
			const auto offers = setup.Saves.FindRecoveries();
			REQUIRE_MESSAGE(offers.has_value(), offers.error().ToString());
			REQUIRE(offers->size() == 2);
			const auto root = setup.Automation.GetEditorFixture().GetProjectRoot() / "Library/Autosave";
			CHECK(FileSystem::ReadText(root / (*offers)[0].Generation / "Scene.json").value_or("").contains("InMain"));
			CHECK(FileSystem::ReadText(root / (*offers)[1].Generation / "Scene.json").value_or("").contains("InSecond"));
		}

		// Decision 15: Save writes every dirty open scene (project.save) when several are dirty, and the editor closes once
		// they are written.
		TEST_CASE("EditorHostClose: Save writes every dirty open scene and then closes")
		{
			HostCloseSetup setup;
			EditorContext& editor = setup.GetEditor();
			setup.Dirty("InMain");
			const Result<VfsPath> second = VfsPath::Parse("project://Assets/Scenes/Second.scene");
			REQUIRE(second.has_value());
			REQUIRE(editor.AddScene(editor.CreateScene("Second"), *second).has_value());
			setup.Dirty("InSecond");
			REQUIRE(setup.Close.RequestClose(true));
			REQUIRE(setup.Close.GetPrompt()->SceneNames == std::vector<std::string>{ "Main", "Second" });
			REQUIRE(setup.Close.Choose(EditorCloseChoice::Save).has_value());
			REQUIRE(setup.SafePoint());
			CHECK_FALSE(setup.Close.GetPrompt());
			CHECK_FALSE(editor.IsAnySceneDirty());
			CHECK(setup.ReadSource().contains("InMain"));
			const auto root = setup.Automation.GetEditorFixture().GetProjectRoot();
			CHECK(FileSystem::ReadText(root / "Assets/Scenes/Second.scene").value_or("").contains("InSecond"));
		}

		// Decision 15: the one dirty open scene is saved also when it is not the active one, whose scene.save would write
		// the clean active scene instead, and the editor then closes.
		TEST_CASE("EditorHostClose: Save writes the one dirty open scene that is not active and then closes")
		{
			HostCloseSetup setup;
			EditorContext& editor = setup.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			const std::string before = setup.ReadSource();
			const Result<VfsPath> second = VfsPath::Parse("project://Assets/Scenes/Second.scene");
			REQUIRE(second.has_value());
			REQUIRE(editor.AddScene(editor.CreateScene("Second"), *second).has_value());
			setup.Dirty("InSecond");
			REQUIRE(editor.ActivateScene(main).has_value());
			REQUIRE(setup.Close.RequestClose(true));
			REQUIRE(setup.Close.GetPrompt()->SceneNames == std::vector<std::string>{ "Second" });
			REQUIRE(setup.Close.Choose(EditorCloseChoice::Save).has_value());
			REQUIRE(setup.SafePoint());
			CHECK_FALSE(setup.Close.GetPrompt());
			CHECK_FALSE(editor.IsAnySceneDirty());
			CHECK(setup.ReadSource() == before);
			const auto root = setup.Automation.GetEditorFixture().GetProjectRoot();
			CHECK(FileSystem::ReadText(root / "Assets/Scenes/Second.scene").value_or("").contains("InSecond"));
		}

		TEST_CASE("EditorHostClose: Don't save keeps a final recovery generation and closes")
		{
			HostCloseSetup setup;
			const std::string before = setup.ReadSource();
			setup.Dirty("DiscardedOnClose");
			REQUIRE(setup.Close.RequestClose(true));
			REQUIRE(setup.Close.Choose(EditorCloseChoice::Discard).has_value());
			REQUIRE(setup.SafePoint());
			CHECK_FALSE(setup.Close.GetPrompt());
			CHECK(setup.ReadSource() == before);
			const auto offer = setup.Saves.FindRecovery();
			REQUIRE_MESSAGE(offer.has_value(), offer.error().ToString());
			REQUIRE(offer->has_value());
			const auto root = setup.Automation.GetEditorFixture().GetProjectRoot() / "Library/Autosave" / (**offer).Generation;
			CHECK(FileSystem::ReadText(root / "Scene.json").value_or("").contains("DiscardedOnClose"));
			const auto metadata = JsonReader::Parse(FileSystem::ReadText(root / "Metadata.json").value_or(""));
			REQUIRE(metadata.has_value());
			CHECK(JsonReader(*metadata).ReadMember<uint32_t>("Reason").value_or(0) == static_cast<uint32_t>(AutosaveReason::Exit));
		}

		TEST_CASE("EditorHostClose: without a UI to show the prompt, a close with unsaved changes keeps a final recovery generation and closes at once")
		{
			HostCloseSetup setup;
			const std::string before = setup.ReadSource();
			setup.Dirty("ClosedWithoutUi");
			const Test::ExpectLog closing(LogLevel::Warn, "no UI to ask");
			CHECK_FALSE(setup.Close.RequestClose(false));
			CHECK(closing.GetMatchCount() == 1);
			CHECK_FALSE(setup.Close.GetPrompt());
			CHECK_FALSE(setup.SafePoint());
			CHECK(setup.ReadSource() == before);
			const auto offer = setup.Saves.FindRecovery();
			REQUIRE_MESSAGE(offer.has_value(), offer.error().ToString());
			REQUIRE(offer->has_value());
			const auto root = setup.Automation.GetEditorFixture().GetProjectRoot() / "Library/Autosave" / (**offer).Generation;
			CHECK(FileSystem::ReadText(root / "Scene.json").value_or("").contains("ClosedWithoutUi"));
			const auto metadata = JsonReader::Parse(FileSystem::ReadText(root / "Metadata.json").value_or(""));
			REQUIRE(metadata.has_value());
			CHECK(JsonReader(*metadata).ReadMember<uint32_t>("Reason").value_or(0) == static_cast<uint32_t>(AutosaveReason::Exit));
		}

		TEST_CASE("EditorHostClose: a prompt that can no longer be shown closes at once and drops its queued save")
		{
			HostCloseSetup setup;
			const std::string before = setup.ReadSource();
			setup.Dirty("PromptLost");
			REQUIRE(setup.Close.RequestClose(true));
			REQUIRE(setup.Close.Choose(EditorCloseChoice::Save).has_value());
			const Test::ExpectLog closing(LogLevel::Warn, "no UI to ask");
			CHECK_FALSE(setup.Close.RequestClose(false));
			CHECK_FALSE(setup.Close.GetPrompt());
			CHECK_FALSE(setup.SafePoint());
			CHECK(setup.ReadSource() == before);
			const auto offer = setup.Saves.FindRecovery();
			REQUIRE_MESSAGE(offer.has_value(), offer.error().ToString());
			CHECK(offer->has_value());
		}

		TEST_CASE("EditorHostClose: Cancel keeps editing and drops a queued save")
		{
			HostCloseSetup setup;
			const std::string before = setup.ReadSource();
			setup.Dirty("KeptOpen");
			REQUIRE(setup.Close.RequestClose(true));
			REQUIRE(setup.Close.Choose(EditorCloseChoice::Save).has_value());
			REQUIRE(setup.Close.Choose(EditorCloseChoice::Cancel).has_value());
			CHECK_FALSE(setup.Close.GetPrompt());
			CHECK_FALSE(setup.SafePoint());
			CHECK(setup.ReadSource() == before);
			CHECK(setup.GetEditor().IsSceneDirty());
			CHECK(setup.Close.Choose(EditorCloseChoice::Save).error().GetCode() == ErrorCode::NotFound);
			// The next close request asks again.
			CHECK(setup.Close.RequestClose(true));
			CHECK(setup.Close.GetPrompt().has_value());
		}

		TEST_CASE("EditorHostClose: a failed or impossible save stays in the prompt")
		{
			HostCloseSetup setup;
			EditorContext& editor = setup.GetEditor();
			setup.Dirty("CannotBeWritten");
			// A directory where the scene file was makes the atomic write fail.
			REQUIRE(FileSystem::Remove(setup.Source()).has_value());
			REQUIRE(FileSystem::CreateDirectories(setup.Source()).has_value());
			REQUIRE(setup.Close.RequestClose(true));
			REQUIRE(setup.Close.Choose(EditorCloseChoice::Save).has_value());
			CHECK_FALSE(setup.SafePoint());
			auto prompt = setup.Close.GetPrompt();
			REQUIRE(prompt.has_value());
			CHECK_FALSE(prompt->Pending);
			REQUIRE(prompt->Failure.has_value());
			CHECK(editor.IsSceneDirty());
			REQUIRE(setup.Close.Choose(EditorCloseChoice::Cancel).has_value());

			// An untitled scene has no file to save to: only Don't save or Cancel are possible.
			editor.SetScene(editor.CreateScene("Untitled"), std::nullopt, true);
			REQUIRE(setup.Close.RequestClose(true));
			prompt = setup.Close.GetPrompt();
			REQUIRE(prompt.has_value());
			CHECK_FALSE(prompt->CanSave);
			CHECK(setup.Close.Choose(EditorCloseChoice::Save).error().GetCode() == ErrorCode::InvalidState);
			REQUIRE(setup.Close.GetPrompt()->Failure.has_value());
			CHECK_FALSE(setup.SafePoint());
			REQUIRE(setup.Close.Choose(EditorCloseChoice::Discard).has_value());
			CHECK(setup.SafePoint());
		}

		TEST_CASE("EditorHostClose: a scene saved by another path while the prompt is open completes the close")
		{
			HostCloseSetup setup;
			setup.Dirty("SavedByTheAgent");
			REQUIRE(setup.Close.RequestClose(true));
			CHECK_FALSE(setup.SafePoint());
			REQUIRE(setup.Automation.Call("scene.save", Json::object()).has_value());
			CHECK(setup.SafePoint());
			CHECK_FALSE(setup.Close.GetPrompt());
		}

		TEST_CASE("EditorHostClose: the prompt's Don't save button queues the decision")
		{
			Test::PanelInteractionUi ui(false);
			HostCloseSetup setup;
			setup.Dirty("ClickedAway");
			const auto draw = [&setup]() -> Status
			{
				setup.Close.Draw();
				return {};
			};
			// Nothing is drawn until a close is held.
			REQUIRE(ui.Frame(draw));
			const ImGuiWindow* hidden = ImGui::FindWindowByName("Unsaved changes");
			CHECK((hidden == nullptr || !hidden->Active));
			REQUIRE(setup.Close.RequestClose(true));
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			const ImGuiWindow* modal = ImGui::FindWindowByName("Unsaved changes");
			REQUIRE(modal != nullptr);
			REQUIRE(modal->Active);
			// Read the buttons' geometry only; the choice itself goes through real ImGui mouse events.
			const ImGuiStyle& style = ImGui::GetStyle();
			const float saveWidth = ImGui::CalcTextSize("Save").x + 2 * style.FramePadding.x;
			const float discardWidth = ImGui::CalcTextSize("Don't save").x + 2 * style.FramePadding.x;
			const ImVec2 discard{ modal->WorkRect.Min.x + saveWidth + style.ItemSpacing.x + discardWidth / 2, modal->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() / 2 };
			REQUIRE(ui.Click(draw, discard));
			const auto prompt = setup.Close.GetPrompt();
			REQUIRE(prompt.has_value());
			CHECK(prompt->Pending);
			CHECK(setup.SafePoint());
			CHECK_FALSE(setup.Close.GetPrompt());
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(ImGui::FindWindowByName("Unsaved changes")->Active);
		}
	}

}
