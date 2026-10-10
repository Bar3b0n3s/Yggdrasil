#include "TestsPCH.h"
#include "Editor/EditorLayer.h"

#include "Editor/PanelInteractionFixture.h"
#include "Editor/Private/EditorHostThumbnails.h"
#include "Editor/Private/EditorHostViewports.h"
#include "Editor/SupportingPanelTestUi.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"

#include <imgui_internal.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

	namespace {

		struct WorkspaceDesignUi
		{
			Test::PanelInteractionUi Ui{ false };
			Test::EditorTestFixture Environment{ "WorkspaceDesign" };
			Test::AutomationTestClient Client{ Environment.GetEditor() };
			EditorActions Actions{ Environment.GetEditor(), Client.GetServer() };
			EditorAutomationControls Controls{ Environment.GetEditor(), Client.GetServer() };
			ReflectedEditController Edits{ Environment.GetEditor() };
			GizmoController Gizmos{ Environment.GetEditor() };
			EditorHostViewports Views{ Environment.GetEditor(), Gizmos };
			EditorHostThumbnails Thumbnails{ Environment.GetEditor(), {} };
			EditorPanelContext Panels{ Environment.GetEditor(), Client.GetServer(), Actions, Controls, Views, Edits, Thumbnails.GetCache(), Gizmos };
			EditorLayer Layer{ Panels };
			std::string IniFile{};

			explicit WorkspaceDesignUi(bool openProject = true)
			{
				if (openProject)
				{
					Environment.CreateAndOpenProject();
					Environment.CreateAndOpenScene();
				}
				REQUIRE(Layer.OnSafePoint(0));
				Environment.GetEditor().GetUiState().ResetLayout(openProject);
				ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
				ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
				Utils::ApplyEditorStyle();
			}

			~WorkspaceDesignUi() { ImGui::GetIO().IniFilename = nullptr; }

			void Draw(const std::function<void()>& before = {})
			{
				REQUIRE(Ui.Frame([this, &before]() -> Status
				{
					if (before)
						before();
					return Layer.OnImGuiRender();
				}));
			}

			ImGuiWindow* Window(const char* name)
			{
				auto* window = ImGui::FindWindowByName(name);
				REQUIRE(window != nullptr);
				return window;
			}

			// Ask ImGui to measure an item's actual navigation rectangle. This changes only focus, never activates it;
			// every tested action below still uses ordinary mouse input and the production safe point.
			ImRect Locate(ImGuiWindow* window, ImGuiID id, ImGuiNavLayer layer = ImGuiNavLayer_Main)
			{
				CAPTURE(window->Name);
				CAPTURE(id);
				Draw([window, id, layer]()
				{
					ImGui::SetWindowFocus(window->Name);
					ImGui::SetNavID(id, layer, window->NavRootFocusScopeId, ImRect());
				});
				REQUIRE(ImGui::GetCurrentContext()->NavIdIsAlive);
				REQUIRE(ImGui::GetCurrentContext()->NavId == id);
				// Relative navigation rectangles use the content origin, including padding and scrolling.
				const ImRect rectangle = ImGui::WindowRectRelToAbs(window, window->NavRectRel[layer]);
				REQUIRE(rectangle.GetWidth() > 0);
				REQUIRE(rectangle.GetHeight() > 0);
				return rectangle;
			}

			ImRect Locate(const char* window, const char* label, ImGuiNavLayer layer = ImGuiNavLayer_Main)
			{
				auto* owner = Window(window);
				return Locate(owner, owner->GetID(label), layer);
			}

			void Click(ImVec2 position, ImGuiMouseButton button = ImGuiMouseButton_Left)
			{
				ImGui::GetIO().AddMousePosEvent(position.x, position.y);
				Draw();
				ImGui::GetIO().AddMouseButtonEvent(button, true);
				Draw();
				ImGui::GetIO().AddMouseButtonEvent(button, false);
				Draw();
			}

			void Click(const char* window, const char* label)
			{
				Click(Locate(window, label).GetCenter());
			}

			void ReplaceText(const char* window, const char* label, const char* text)
			{
				Click(window, label);
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
				ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
				Draw();
				ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
				ImGui::GetIO().AddInputCharactersUTF8(text);
				Draw();
			}

			std::optional<ImVec2> FindText(std::string_view text)
			{
				std::vector<ImFontGlyph> glyphs;
				REQUIRE(Ui.Frame([this, text, &glyphs]() -> Status
				{
					ENGINE_TRY(Layer.OnImGuiRender());
					glyphs = Test::SupportingPanelTestUi::CaptureGlyphs(text);
					return {};
				}));
				return Test::SupportingPanelTestUi::FindRenderedText(glyphs);
			}

			void OpenSceneDialog(bool create)
			{
				const auto file = FindText("File");
				REQUIRE(file.has_value());
				Click(*file);
				const auto action = FindText(create ? "New scene" : "Open scene");
				REQUIRE(action.has_value());
				Click(*action);
				Draw();
			}

			ImGuiWindow* SceneList()
			{
				auto* dialog = Window("Open scene");
				for (auto* window : ImGui::GetCurrentContext()->Windows)
					if (window->ParentWindow == dialog && window->ChildId == dialog->GetID("##SceneList"))
						return window;
				FAIL("The scene browser must contain its scene list");
				return nullptr;
			}

			void SelectScene(std::string_view path)
			{
				auto* list = SceneList();
				const std::string identity = "project://" + std::string(path);
				const ImGuiID row = ImHashStr("##Scene", 0, ImHashData(identity.data(), identity.size(), list->ID));
				Click(Locate(list, row).GetCenter());
			}

			void OpenRename(UUID entity)
			{
				auto* hierarchy = Window(Utils::EditorWindowTitle(EditorPanel::SceneHierarchy));
				const ImGuiID row = ImHashStr("##Entity", 0, ImHashStr(entity.ToString().c_str(), 0, hierarchy->ID));
				Click(Locate(hierarchy, row).GetCenter(), ImGuiMouseButton_Right);
				const auto rename = FindText("Rename...");
				REQUIRE(rename.has_value());
				Click(*rename);
				Draw();
			}

			std::string Text()
			{
				const auto path = Environment.GetDirectory() / "WorkspaceUi.txt";
				REQUIRE(Ui.Frame([this, &path]() -> Status
				{
					ImGui::LogToFile(64, FileSystem::PathToUtf8(path).c_str());
					const auto result = Layer.OnImGuiRender();
					ImGui::LogFinish();
					return result;
				}));
				const auto text = FileSystem::ReadText(path);
				REQUIRE(text);
				return *text;
			}
		};

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorWorkspaceDesign: every friendly panel title preserves its legacy settings and tab identity")
		{
			for (uint8_t index = 0; index <= static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
			{
				const auto panel = static_cast<EditorPanel>(index);
				const auto legacyName = EditorPanelToString(panel);
				const char* title = Utils::EditorWindowTitle(panel);
				CAPTURE(legacyName);
				CHECK(ImHashStr(title) == ImHashStr(legacyName.data()));
				CHECK(ImHashStr("#TAB", 0, ImHashStr(title)) == ImHashStr("#TAB", 0, ImHashStr(legacyName.data())));
				CHECK(std::string_view(ImHashSkipUncontributingPrefix(title)) == legacyName);
			}
		}

		TEST_CASE("EditorWorkspaceDesign: the default workspace reserves chrome and usable dock widths at supported sizes")
		{
			WorkspaceDesignUi test;
			float scale = 1.0f;
			SUBCASE("1280 by 720")
			{
			}
			SUBCASE("1600 by 900")
			{
				ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
			}
			SUBCASE("scaled 1920 by 1080")
			{
				scale = 1.5f;
				Utils::ApplyEditorStyle(scale);
				ImGui::GetIO().DisplaySize = ImVec2(1920, 1080);
			}
			float layoutScale = 0.0f;
			test.Draw([&layoutScale]()
			{
				// Capture at the same point as EditorLayer: Render() restores the unscaled font afterward.
				layoutScale = Utils::EditorUiScale();
			});
			test.Draw();
			REQUIRE(layoutScale > 0.0f);
			auto* hierarchy = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneHierarchy));
			auto* inspector = test.Window(Utils::EditorWindowTitle(EditorPanel::Inspector));
			auto* scene = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
			auto* assets = test.Window(Utils::EditorWindowTitle(EditorPanel::ContentBrowser));
			auto* toolbar = test.Window("##EditorToolbar");
			auto* menu = test.Window("##MainMenuBar");
			auto* status = test.Window("##EditorStatus");
			for (const auto panel : test.Environment.GetEditor().GetUiState().GetOpenPanels())
			{
				auto* window = test.Window(Utils::EditorWindowTitle(panel));
				REQUIRE(window->DockNode != nullptr);
				CHECK(window->DockIsActive);
			}
			CHECK(hierarchy->Size.x / layoutScale >= 220.0f);
			CHECK(hierarchy->Size.x / layoutScale <= 235.0f);
			CHECK(inspector->Size.x / layoutScale >= 350.0f);
			CHECK(inspector->Size.x / layoutScale <= 365.0f);
			CHECK(scene->Size.x / scale >= 660.0f);
			CHECK(scene->Size.y / scale >= 340.0f);
			if (ImGui::GetIO().DisplaySize.y / scale >= 900.0f)
				CHECK(assets->Size.y / scale >= 250.0f);
			CHECK(assets->Size.y <= (status->Pos.y - toolbar->Pos.y - toolbar->Size.y) * 0.32f + 2.0f);
			CHECK(scene->Pos.x == doctest::Approx(assets->Pos.x));
			CHECK(scene->Size.x == doctest::Approx(assets->Size.x));
			CHECK(scene->Pos.y >= toolbar->Pos.y + toolbar->Size.y);
			CHECK(toolbar->Pos.y >= menu->Pos.y + menu->Size.y);
			CHECK(assets->Pos.y + assets->Size.y <= status->Pos.y);
			CHECK(assets->DockNode->SelectedTabId == assets->TabId);
			CHECK(scene->DockNode->SelectedTabId == scene->TabId);
			CHECK(toolbar->ScrollMax.x == 0.0f);
			for (const char* label : { "Play", "Simulate", "Pause###PauseResume", "Step", "Stop" })
			{
				const auto rectangle = test.Locate("##EditorToolbar", label);
				CHECK(rectangle.Min.x >= toolbar->InnerRect.Min.x);
				CHECK(rectangle.Max.x <= toolbar->InnerRect.Max.x);
				CHECK(rectangle.Max.y <= toolbar->InnerRect.Max.y);
			}
		}

		TEST_CASE("EditorWorkspaceDesign: serialized legacy layouts preserve window geometry and selected tabs")
		{
			std::string saved;
			ImGuiID sceneDock = 0;
			ImGuiID assetsDock = 0;
			{
				Test::PanelInteractionUi legacy{ false };
				ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
				ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
				const auto draw = [&sceneDock, &assetsDock]() -> Status
				{
					const ImGuiID dock = ImGui::DockSpaceOverViewport();
					if (sceneDock == 0)
					{
						ImGui::DockBuilderRemoveNode(dock);
						ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
						ImGui::DockBuilderSetNodeSize(dock, ImGui::GetIO().DisplaySize);
						assetsDock = ImGui::DockBuilderSplitNode(dock, ImGuiDir_Down, 0.40f, nullptr, &sceneDock);
						ImGui::DockBuilderDockWindow("SceneViewport", sceneDock);
						ImGui::DockBuilderDockWindow("GameViewport", sceneDock);
						ImGui::DockBuilderDockWindow("ContentBrowser", assetsDock);
						ImGui::DockBuilderDockWindow("Console", assetsDock);
						ImGui::DockBuilderFinish(dock);
					}
					for (const char* name : { "SceneViewport", "GameViewport", "ContentBrowser", "Console" })
					{
						ImGui::Begin(name);
						ImGui::End();
					}
					ImGui::SetNextWindowPos(ImVec2(830, 130), ImGuiCond_Once);
					ImGui::SetNextWindowSize(ImVec2(330, 450), ImGuiCond_Once);
					ImGui::Begin("Inspector");
					ImGui::End();
					return {};
				};
				REQUIRE(legacy.Frame(draw));
				REQUIRE(legacy.Frame(draw));
				ImGui::SetWindowFocus("GameViewport");
				REQUIRE(legacy.Frame(draw));
				ImGui::SetWindowFocus("Console");
				REQUIRE(legacy.Frame(draw));
				saved = ImGui::SaveIniSettingsToMemory();
				REQUIRE(saved.contains("[Window][SceneViewport]"));
			}
			std::string resaved;
			{
				WorkspaceDesignUi test;
				REQUIRE(test.Environment.GetEditor().GetUiState().SetPanelOpen(EditorPanel::SceneHierarchy, false));
				ImGui::LoadIniSettingsFromMemory(saved.data(), saved.size());
				test.Draw();
				test.Draw();
				auto* scene = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
				auto* game = test.Window(Utils::EditorWindowTitle(EditorPanel::GameViewport));
				auto* console = test.Window(Utils::EditorWindowTitle(EditorPanel::Console));
				auto* inspector = test.Window(Utils::EditorWindowTitle(EditorPanel::Inspector));
				CHECK(scene->ID == ImHashStr(Utils::EditorWindowTitle(EditorPanel::SceneViewport)));
				CHECK(scene->ID == ImHashStr("SceneViewport")); // Pinned ImGui 1.92.6+ skips the ### bytes.
				CHECK(scene->DockId == sceneDock);
				CHECK(console->DockId == assetsDock);
				CHECK(game->DockNode->SelectedTabId == game->TabId);
				CHECK(console->DockNode->SelectedTabId == console->TabId);
				CHECK(inspector->DockId == 0);
				CHECK(inspector->Pos.x == 830);
				CHECK(inspector->Pos.y == 130);
				CHECK(inspector->Size.x == 330);
				CHECK(inspector->Size.y == 450);
				auto* tabs = scene->DockNode->TabBar;
				const auto* tab = ImGui::TabBarFindTabByID(tabs, scene->TabId);
				REQUIRE(tab != nullptr);
				test.Click(ImVec2(tabs->BarRect.Min.x + tab->Offset + tab->Width * 0.5f, tabs->BarRect.GetCenter().y));
				test.Draw();
				CHECK(scene->DockNode->SelectedTabId == scene->TabId);
				resaved = ImGui::SaveIniSettingsToMemory();
				CHECK(resaved.contains("[Window][" + std::string(ImHashSkipUncontributingPrefix(Utils::EditorWindowTitle(EditorPanel::SceneViewport))) + "]"));
			}
			{
				WorkspaceDesignUi test;
				ImGui::LoadIniSettingsFromMemory(resaved.data(), resaved.size());
				test.Draw();
				test.Draw();
				auto* scene = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
				CHECK(scene->DockId == sceneDock);
				CHECK(scene->DockNode->SelectedTabId == scene->TabId);
				CHECK(test.Window(Utils::EditorWindowTitle(EditorPanel::Inspector))->DockId == 0);
			}
		}

		TEST_CASE("EditorWorkspaceDesign: switching from the launcher to a fresh project restores every default dock")
		{
			WorkspaceDesignUi test(false);
			test.Draw();
			test.Draw();
			CHECK(test.Text().contains("Build your next world."));
			test.Environment.CreateAndOpenProject();
			test.Environment.CreateAndOpenScene();
			REQUIRE(test.Layer.OnSafePoint(0));
			// ImGuiLayer::SetIniFilePath clears the launcher's ini when the project is bound, even when its file is new.
			test.IniFile = FileSystem::PathToUtf8(test.Environment.GetDirectory() / "FreshProjectLayout.ini");
			ImGui::GetIO().IniFilename = test.IniFile.c_str();
			ImGui::ClearIniSettings();
			test.Draw();
			test.Draw();
			auto* scene = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneViewport));
			auto* game = test.Window(Utils::EditorWindowTitle(EditorPanel::GameViewport));
			auto* assets = test.Window(Utils::EditorWindowTitle(EditorPanel::ContentBrowser));
			auto* console = test.Window(Utils::EditorWindowTitle(EditorPanel::Console));
			auto* hierarchy = test.Window(Utils::EditorWindowTitle(EditorPanel::SceneHierarchy));
			auto* inspector = test.Window(Utils::EditorWindowTitle(EditorPanel::Inspector));
			for (auto* window : { scene, game, assets, console, hierarchy, inspector })
			{
				REQUIRE(window->DockNode != nullptr);
				CHECK(window->DockIsActive);
			}
			CHECK(scene->DockNode == game->DockNode);
			CHECK(assets->DockNode == console->DockNode);
			CHECK(scene->DockNode->SelectedTabId == scene->TabId);
			CHECK(assets->DockNode->SelectedTabId == assets->TabId);
			CHECK(scene->DockTabIsVisible);
			CHECK_FALSE(game->DockTabIsVisible);
			CHECK(assets->DockTabIsVisible);
			CHECK_FALSE(console->DockTabIsVisible);
			CHECK(hierarchy->Pos.x + hierarchy->Size.x <= scene->Pos.x);
			CHECK(scene->Pos.x + scene->Size.x <= inspector->Pos.x);
			CHECK(scene->Pos.y + scene->Size.y <= assets->Pos.y);
			CHECK(scene->Size.x >= 660.0f);
		}

		TEST_CASE("EditorWorkspaceDesign: the scene browser selects project scenes and requires an explicit unsaved-edit choice")
		{
			WorkspaceDesignUi test;
			auto& editor = test.Environment.GetEditor();
			test.Environment.CreateAndOpenScene("Assets/Scenes/Another level.scene");
			REQUIRE(test.Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }));
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Keep this edit" } }));
			test.Draw();
			test.Draw();
			test.OpenSceneDialog(false);
			test.Click("Open scene", "##SceneSearch");
			ImGui::GetIO().AddInputCharactersUTF8("ANOTHER");
			test.Draw();
			test.SelectScene("Assets/Scenes/Another level.scene");
			test.Click("Open scene", "Open scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetScenePath()->GetFileName() == "Main.scene");
			CHECK(editor.IsSceneDirty());
			test.Click("Open scene", "Save changes first");
			test.Click("Open scene", "Open scene");
			CHECK(editor.GetScenePath()->GetFileName() == "Main.scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetScenePath()->GetFileName() == "Another level.scene");
			CHECK_FALSE(editor.IsSceneDirty());
			test.Draw();
			REQUIRE(test.Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }));
			CHECK(editor.GetScene().GetEntityCount() == 1);
		}

		TEST_CASE("EditorWorkspaceDesign: the scene browser opens scenes outside Assets with distinct literal path identities")
		{
			WorkspaceDesignUi test;
			auto& editor = test.Environment.GetEditor();
			for (const char* path : { "Levels/Main.scene", "Levels###Shared/Main.scene", "Other###Shared/Main.scene", "Main.scene" })
				test.Environment.CreateAndOpenScene(path);
			std::string path;
			SUBCASE("project level folder")
			{
				path = "Levels/Main.scene";
			}
			SUBCASE("first authored hash folder")
			{
				path = "Levels###Shared/Main.scene";
			}
			SUBCASE("second authored hash folder")
			{
				path = "Other###Shared/Main.scene";
			}
			SUBCASE("project root")
			{
				path = "Main.scene";
			}
			REQUIRE(test.Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }));
			test.Draw();
			test.Draw();
			test.OpenSceneDialog(false);
			if (path.contains("###"))
			{
				test.ReplaceText("Open scene", "##SceneSearch", "###Shared");
				CHECK(test.FindText("Levels###Shared").has_value());
				CHECK(test.FindText("Other###Shared").has_value());
			}
			test.SelectScene(path);
			test.Click("Open scene", "Open scene");
			CHECK(editor.GetScenePath()->GetPath() == "Assets/Scenes/Main.scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetScenePath()->GetPath() == path);
			CHECK_FALSE(editor.IsSceneDirty());
		}

		TEST_CASE("EditorWorkspaceDesign: new scene folders display authored hash text and keep distinct selection identities")
		{
			WorkspaceDesignUi test;
			test.Environment.CreateAndOpenScene("A###Shared/Main.scene");
			test.Environment.CreateAndOpenScene("AA###Shared/Main.scene");
			std::string folder;
			SUBCASE("first folder")
			{
				folder = "A###Shared";
			}
			SUBCASE("second folder")
			{
				folder = "AA###Shared";
			}
			test.Draw();
			test.Draw();
			test.OpenSceneDialog(true);
			test.ReplaceText("New scene", "##SceneName", "New level");
			test.Click("New scene", "##SceneFolder");
			const auto choice = test.FindText(folder);
			REQUIRE(choice.has_value());
			test.Click(*choice);
			CHECK(test.FindText(folder).has_value());
			test.Click("New scene", "Create scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(test.Environment.GetEditor().GetScenePath()->GetPath() == folder + "/New level.scene");
		}

		TEST_CASE("EditorWorkspaceDesign: new scenes use a name and folder and cannot escape the project")
		{
			WorkspaceDesignUi test;
			test.Draw();
			test.Draw();
			test.OpenSceneDialog(true);
			test.Click("New scene", "##SceneName");
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
			test.Draw();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			ImGui::GetIO().AddInputCharactersUTF8("../Outside");
			test.Draw();
			test.Click("New scene", "Create scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(test.Environment.GetEditor().GetScenePath()->GetFileName() == "Main.scene");
			test.Click("New scene", "##SceneName");
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
			test.Draw();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			ImGui::GetIO().AddInputCharactersUTF8("First level");
			test.Draw();
			test.Click("New scene", "Create scene");
			CHECK(test.Environment.GetEditor().GetScenePath()->GetFileName() == "Main.scene");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(test.Environment.GetEditor().GetScenePath()->GetPath() == "Assets/Scenes/First level.scene");
			CHECK(test.Environment.GetEditor().GetScene().GetName() == "First level");
			CHECK_FALSE(test.Environment.GetEditor().IsSceneDirty());
		}

		TEST_CASE("EditorWorkspaceDesign: hierarchy active toggles share the user undo history and do not select the row")
		{
			WorkspaceDesignUi test;
			auto& editor = test.Environment.GetEditor();
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Mixed Case Entity" } }));
			const UUID id = editor.GetScene().GetRootEntities().front();
			test.Draw();
			test.Draw();
			const char* title = Utils::EditorWindowTitle(EditorPanel::SceneHierarchy);
			test.Click(title, "##Search");
			ImGui::GetIO().AddInputCharactersUTF8("mixed case");
			test.Draw();
			auto* hierarchy = test.Window(title);
			const ImGuiID rowId = ImHashStr("##Entity", 0, ImHashStr(id.ToString().c_str(), 0, hierarchy->ID));
			const ImRect row = test.Locate(hierarchy, rowId);
			test.Click(ImVec2(row.Max.x - ImGui::GetTextLineHeight() * 0.5f, row.Min.y + ImGui::GetTextLineHeight() * 0.5f));
			CHECK(editor.GetScene().FindEntityByID(id).IsActiveSelf());
			CHECK(editor.GetSelection().empty());
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK_FALSE(editor.GetScene().FindEntityByID(id).IsActiveSelf());
			CHECK(editor.GetSelection().empty());
			const auto history = editor.GetHistory().GetEntries(10);
			REQUIRE(history.size() == 2);
			CHECK(history[0].Origin == CommandOrigin::Agent);
			CHECK(history[1].Origin == CommandOrigin::User);
			REQUIRE(test.Client.Call("edit.undo", Json::object()));
			CHECK(editor.GetScene().FindEntityByID(id).IsActiveSelf());
			REQUIRE(test.Client.Call("edit.redo", Json::object()));
			CHECK_FALSE(editor.GetScene().FindEntityByID(id).IsActiveSelf());
		}

		TEST_CASE("EditorWorkspaceDesign: pending hierarchy renames reject replaced runtime scenes with the same entity UUID")
		{
			WorkspaceDesignUi test;
			auto& editor = test.Environment.GetEditor();
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Original entity" } }));
			const UUID entity = editor.GetScene().GetRootEntities().front();
			REQUIRE(test.Client.Call("scene.save", Json::object()));
			REQUIRE(editor.GetAssets().Refresh());
			editor.GetAssets().WaitIdle();
			REQUIRE(test.Client.Call("play.start", Json{ { "paused", true } }));
			const uint64_t revision = editor.GetRevision();
			const uint64_t serial = editor.GetPlay().GetSession()->GetSerial();
			const uint64_t generation = editor.GetPlay().GetSession()->GetSceneGeneration();
			test.Draw();
			test.Draw();
			test.OpenRename(entity);
			test.ReplaceText("Rename entity", "##Name", "Renamed entity");
			bool replaced = false;
			SUBCASE("the original runtime scene still accepts the queued rename")
			{
			}
			SUBCASE("stop and restart replaces the session")
			{
				REQUIRE(test.Client.Call("play.stop", Json::object()));
				REQUIRE(test.Client.Call("play.start", Json{ { "paused", true } }));
				CHECK(editor.GetPlay().GetSession()->GetSerial() != serial);
				CHECK(editor.GetPlay().GetSession()->GetSceneGeneration() == generation);
				replaced = true;
			}
			SUBCASE("gameplay scene load replaces the scene within the same session")
			{
				REQUIRE(test.Client.Call("script.eval", Json{ { "context", "play" }, { "code", "Scene.Load(Assets.Load('Assets/Scenes/Main.scene'), {})" } }));
				REQUIRE(test.Client.Call("play.step", Json{ { "ticks", 1 } }));
				CHECK(editor.GetPlay().GetSession()->GetSerial() == serial);
				CHECK(editor.GetPlay().GetSession()->GetSceneGeneration() > generation);
				replaced = true;
			}
			CHECK(editor.GetRevision() == revision);
			REQUIRE(editor.GetPlay().GetSession()->GetScene().FindEntityByID(entity).IsValid());
			test.Draw();
			if (replaced)
				CHECK(test.Text().contains("The scene changed."));
			test.Click("Rename entity", "Rename");
			CHECK(editor.GetPlay().GetSession()->GetScene().FindEntityByID(entity).GetName() == "Original entity");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetPlay().GetSession()->GetScene().FindEntityByID(entity).GetName() == (replaced ? "Original entity" : "Renamed entity"));
			CHECK(editor.GetScene().FindEntityByID(entity).GetName() == "Original entity");
			CHECK(editor.GetRevision() == revision);
			REQUIRE(test.Client.Call("play.stop", Json::object()));
		}

		TEST_CASE("EditorWorkspaceDesign: toolbar playback queues human commands and respects lockstep ownership")
		{
			WorkspaceDesignUi test;
			auto& editor = test.Environment.GetEditor();
			test.Draw();
			test.Draw();
			test.Click("##EditorToolbar", "Simulate");
			CHECK(editor.GetPlay().GetSession() == nullptr);
			REQUIRE(test.Layer.OnSafePoint(0));
			REQUIRE(editor.GetPlay().GetSession() != nullptr);
			CHECK(editor.GetPlay().GetSession()->GetMode() == PlayMode::Simulate);
			test.Draw();
			test.Click("##EditorToolbar", "Pause###PauseResume");
			const ImGuiID pauseId = ImGui::GetCurrentContext()->NavId;
			CHECK(pauseId == test.Window("##EditorToolbar")->GetID("Pause###PauseResume"));
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetPlay().GetSession()->IsPaused());
			test.Draw();
			CHECK(ImGui::GetCurrentContext()->NavId == pauseId);
			CHECK(ImGui::GetCurrentContext()->NavIdIsAlive);
			test.Click("##EditorToolbar", "Resume###PauseResume");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK_FALSE(editor.GetPlay().GetSession()->IsPaused());
			test.Draw();
			CHECK(ImGui::GetCurrentContext()->NavId == pauseId);
			CHECK(ImGui::GetCurrentContext()->NavIdIsAlive);
			test.Click("##EditorToolbar", "Stop");
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetPlay().GetSession() == nullptr);
			REQUIRE(test.Client.Call("play.start", Json{ { "mode", "Simulate" }, { "lockstep", true } }));
			test.Draw();
			test.Click("##EditorToolbar", "Stop");
			REQUIRE(test.Layer.OnSafePoint(0));
			REQUIRE(editor.GetPlay().GetSession() != nullptr);
			CHECK(editor.GetPlay().GetSession()->IsLockstep());
			CHECK(test.Text().contains("Agent controls time"));
			REQUIRE(test.Client.Call("play.stop", Json::object()));
		}

		TEST_CASE("EditorWorkspaceDesign: read-only hierarchy keeps selection available and disables entity mutations")
		{
			WorkspaceDesignUi test;
			auto& editor = test.Environment.GetEditor();
			REQUIRE(test.Client.Call("entity.create", Json{ { "name", "Read-only entity" } }));
			REQUIRE(test.Client.Call("scene.save", Json::object()));
			const auto projectFile = editor.GetProject().GetProjectFile();
			const UUID id = editor.GetScene().GetRootEntities().front();
			REQUIRE(editor.CloseProject());
			auto project = ProjectManager::OpenProject(projectFile,
				{ .ReadOnly = true, .ReadOnlyCacheDirectory = test.Environment.GetDirectory() / "ReadOnlyCache" }, editor.GetTypeRegistry());
			REQUIRE(project);
			REQUIRE(editor.OpenProject(std::move(*project)));
			REQUIRE(test.Client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }));
			REQUIRE(test.Layer.OnSafePoint(0));
			test.Draw();
			test.Draw();
			const char* title = Utils::EditorWindowTitle(EditorPanel::SceneHierarchy);
			test.Click(title, "Create entity");
			auto* hierarchy = test.Window(title);
			const auto rowId = ImHashStr("##Entity", 0, ImHashStr(id.ToString().c_str(), 0, hierarchy->ID));
			const ImRect row = test.Locate(hierarchy, rowId);
			test.Click(ImVec2(row.Max.x - ImGui::GetTextLineHeight() * 0.5f, row.Min.y + ImGui::GetTextLineHeight() * 0.5f));
			REQUIRE(test.Layer.OnSafePoint(0));
			CHECK(editor.GetScene().GetEntityCount() == 1);
			CHECK(editor.GetScene().FindEntityByID(id).IsActiveSelf());
			CHECK(editor.GetHistory().GetEntries(10).empty());
			test.Click(row.GetCenter());
			CHECK(editor.GetSelection().empty());
			REQUIRE(test.Layer.OnSafePoint(0));
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == id);
			CHECK(test.Text().contains("Read-only project"));
		}
	}

}
