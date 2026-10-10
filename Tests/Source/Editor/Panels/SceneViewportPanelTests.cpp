#include "TestsPCH.h"
#include "Editor/Panels/SceneViewportPanel.h"

#include "Editor/PanelInteractionFixture.h"
#include "Editor/Ui/EditorStyle.h"

#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <string>

namespace Engine {

	namespace {

		// The real toolbar only needs a measured viewport; no image, renderer or picking is involved.
		class SceneToolbarViewport final : public EditorViewportHost
		{
		public:
			[[nodiscard]] EditorViewportImage GetImage(ViewportView view) const override
			{
				CHECK(view == ViewportView::Scene);
				return {};
			}
			void SetRectangle(ViewportView view, const EditorViewportRect& rectangle) override
			{
				CHECK(view == ViewportView::Scene);
				Rectangle = rectangle;
			}
			[[nodiscard]] Status RequestPick(const EditorViewportClick&) override { return MakeError(ErrorCode::Unsupported, "toolbar test has no picking image"); }
			[[nodiscard]] Status ConfigureSceneSnapshot(RenderSnapshot&, const EditorViewportOptions&, std::span<const UUID>) override { return MakeError(ErrorCode::Unsupported, "toolbar test has no renderer"); }
			void SetGameInputFocused(bool) override { FAIL("scene toolbar must not change game input focus"); }

			EditorViewportRect Rectangle{};
		};

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("SceneViewportPanel: consecutive keyboard space toggles retain focus when the coordinate label changes")
		{
			Test::PanelInteractionFixture fixture("SceneToolbarKeyboard");
			auto& services = fixture.GetContext();
			SceneToolbarViewport viewport;
			EditorPanelContext context{ services.Editor, services.Automation, services.Actions, services.AutomationControls,
				viewport, services.InspectorEdits, services.Thumbnails, services.Gizmos };
			Test::PanelInteractionUi ui(false);
			Utils::ApplyEditorStyle();
			SceneViewportPanel panel;
			std::string text;
			bool focusScale = false;
			const char* title = Utils::EditorWindowTitle(EditorPanel::SceneViewport);
			const auto draw = [&panel, &context, &text, &focusScale, title]() -> Status
			{
				ImGuizmo::BeginFrame();
				if (focusScale)
				{
					auto* window = ImGui::FindWindowByName(title);
					REQUIRE(window != nullptr);
					ImGui::SetWindowFocus(title);
					ImGui::SetNavID(window->GetID("Scale"), ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
					// SetNavID alone leaves keyboard navigation hidden; the first Tab would only reveal Scale.
					ImGui::SetNavCursorVisible(true);
					focusScale = false;
				}
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(1000.0f, 700.0f));
				ImGui::LogToBuffer();
				const Status status = panel.Draw(context);
				text = ImGui::GetCurrentContext()->LogBuffer.c_str();
				ImGui::LogFinish();
				return status;
			};
			const auto press = [&ui, &draw](ImGuiKey key)
			{
				ImGui::GetIO().AddKeyEvent(key, true);
				REQUIRE(ui.Frame(draw));
				ImGui::GetIO().AddKeyEvent(key, false);
				REQUIRE(ui.Frame(draw));
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			CAPTURE(text);
			// ImGui logs button decorations and visible text as separate, space-delimited fragments.
			REQUIRE(text.contains("[ World ]"));
			CHECK_FALSE(text.contains("[ Local ]"));
			focusScale = true;
			REQUIRE(ui.Frame(draw));
			auto* window = ImGui::FindWindowByName(title);
			REQUIRE(window != nullptr);
			auto& gui = *ImGui::GetCurrentContext();
			REQUIRE(gui.NavIdIsAlive);
			REQUIRE(gui.NavId == window->GetID("Scale"));

			// Tab from the preceding control discovers the live space button without assuming its changing label/ID.
			// After this initial focus, both activations use only ordinary key events; no refocus repairs a stale ID.
			press(ImGuiKey_Tab);
			REQUIRE(gui.NavWindow == window);
			REQUIRE(gui.NavIdIsAlive);
			const ImGuiID spaceButton = gui.NavId;
			REQUIRE(spaceButton != window->GetID("Scale"));
			const ImRect button = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
			REQUIRE(button.GetWidth() > 0.0f);
			REQUIRE(button.GetHeight() > 0.0f);
			const auto history = context.Editor.GetHistory().GetUndoCount();
			for (const char* expected : { "Local", "World" })
			{
				press(ImGuiKey_Space);
				CAPTURE(text);
				CHECK(text.contains(std::string("[ ") + expected + " ]"));
				CHECK_FALSE(text.contains(std::string(expected) == "Local" ? "[ World ]" : "[ Local ]"));
				CHECK(gui.NavWindow == window);
				CHECK(gui.NavId == spaceButton);
				CHECK(gui.NavIdIsAlive);
			}
			CHECK(viewport.Rectangle.Size.x > 0.0f);
			CHECK(viewport.Rectangle.Size.y > 0.0f);
			CHECK(context.Editor.GetHistory().GetUndoCount() == history);
		}
	}

}
