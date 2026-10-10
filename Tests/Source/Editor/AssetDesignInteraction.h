#pragma once

#include "Editor/PanelInteractionFixture.h"
#include "Engine/Core/Hash.h"

#include <imgui_internal.h>

#include <functional>
#include <string_view>

namespace Engine {

	namespace Test {

		// Locate production navigation rectangles; actions still use public mouse/keyboard events.
		// No engine internals or private panel state are read or written.
		class AssetDesignInteraction
		{
		public:
			[[nodiscard]] static ImGuiID AuthoredScope(ImGuiID parent, std::string_view text)
			{
				const int hash = static_cast<int>(FNV1a32(text));
				return ImHashData(&hash, sizeof(hash), parent);
			}

			explicit AssetDesignInteraction(bool wrapWindow = true)
				: m_Ui(wrapWindow)
			{
			}

			[[nodiscard]] Status Frame(const std::function<Status()>& draw) { return m_Ui.Frame(draw); }
			[[nodiscard]] Status Click(const std::function<Status()>& draw, ImVec2 position) { return m_Ui.Click(draw, position); }

			[[nodiscard]] ImGuiWindow* Window(std::string_view name) const
			{
				for (auto* window : ImGui::GetCurrentContext()->Windows)
					if (window->Active && std::string_view(window->Name).contains(name))
						return window;
				FAIL("Missing active UI window: " << name);
				return nullptr;
			}

			[[nodiscard]] ImGuiWindow* Child(ImGuiWindow* parent, ImGuiID id) const
			{
				for (auto* window : ImGui::GetCurrentContext()->Windows)
					if (window->Active && window->ParentWindow == parent && window->ChildId == id)
						return window;
				FAIL("Missing active child window");
				return nullptr;
			}

			[[nodiscard]] ImGuiWindow* Popup() const
			{
				const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
				REQUIRE_FALSE(popups.empty());
				REQUIRE(popups.back().Window != nullptr);
				return popups.back().Window;
			}

			[[nodiscard]] ImRect Locate(const std::function<Status()>& draw, ImGuiWindow* window, ImGuiID id)
			{
				REQUIRE(window != nullptr);
				INFO("Locating item " << id << " in " << window->Name);
				const auto focusAndDraw = [&draw, window, id]() -> Status
				{
					ImGui::SetWindowFocus(window->Name);
					ImGui::SetNavWindow(window);
					ImGui::SetNavID(id, ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
					return draw();
				};
				REQUIRE(Frame(focusAndDraw));
				REQUIRE(ImGui::GetCurrentContext()->NavId == id);
				REQUIRE(ImGui::GetCurrentContext()->NavIdIsAlive);
				ImRect rectangle = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
				if (!window->InnerClipRect.Contains(rectangle.GetCenter()))
				{
					ImGui::SetScrollFromPosY(window, rectangle.GetCenter().y - window->Pos.y, 0.5f);
					REQUIRE(Frame(focusAndDraw));
					rectangle = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
				}
				REQUIRE(rectangle.GetWidth() > 0.0f);
				REQUIRE(rectangle.GetHeight() > 0.0f);
				return rectangle;
			}

			void Click(const std::function<Status()>& draw, ImGuiWindow* window, ImGuiID id)
			{
				REQUIRE(Click(draw, Locate(draw, window, id).GetCenter()));
			}

			void Click(const std::function<Status()>& draw, ImGuiWindow* window, const char* label)
			{
				INFO("Clicking " << label);
				Click(draw, window, window->GetID(label));
			}

			void Type(const std::function<Status()>& draw, ImGuiWindow* window, ImGuiID id, const char* text)
			{
				Click(draw, window, id);
				ImGui::GetIO().AddInputCharactersUTF8(text);
				REQUIRE(Frame(draw));
			}
		private:
			PanelInteractionUi m_Ui;
		};

	}

}
