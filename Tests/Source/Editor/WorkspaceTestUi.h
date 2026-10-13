#pragma once

#include "Editor/EditorLayer.h"
#include "Editor/PanelInteractionFixture.h"
#include "Editor/Private/EditorHostThumbnails.h"
#include "Editor/Private/EditorHostViewports.h"
#include "Editor/SupportingPanelTestUi.h"
#include "Editor/Ui/EditorStyle.h"
#include "Engine/Core/FileSystem.h"

#include <imgui_internal.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace Test {

		// The production EditorLayer over an in-process editor, its automation client and the real host services, in a
		// docked 1280x720 workspace. Tests locate items through ImGui's navigation rectangles and act with ordinary input.

		struct WorkspaceTestUi
		{
			PanelInteractionUi Ui{ false };
			EditorTestFixture Environment{ "Workspace" };
			AutomationTestClient Client{ Environment.GetEditor() };
			EditorActions Actions{ Environment.GetEditor(), Client.GetServer() };
			EditorAutomationControls Controls{ Environment.GetEditor(), Client.GetServer() };
			ReflectedEditController Edits{ Environment.GetEditor() };
			GizmoController Gizmos{ Environment.GetEditor() };
			EditorHostViewports Views{ Environment.GetEditor(), Gizmos };
			EditorHostThumbnails Thumbnails{ Environment.GetEditor(), {} };
			EditorPanelContext Panels{ Environment.GetEditor(), Client.GetServer(), Actions, Controls, Views, Edits, Thumbnails.GetCache(), Gizmos };
			EditorLayer Layer{ Panels };
			std::string IniFile{};

			explicit WorkspaceTestUi(bool openProject = true)
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

			~WorkspaceTestUi() { ImGui::GetIO().IniFilename = nullptr; }

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
					glyphs = SupportingPanelTestUi::CaptureGlyphs(text);
					return {};
				}));
				return SupportingPanelTestUi::FindRenderedText(glyphs);
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

}
