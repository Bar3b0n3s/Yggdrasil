#include "EditorPCH.h"
#include "Editor/Ui/EditorStyle.h"

#include "Engine/AssetPipeline/Importers/FontImporter.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace Engine {

	namespace Utils {

		float EditorDisplayScale(float contentScale, float framebufferScale)
		{
			if (!std::isfinite(contentScale) || contentScale <= 0.0f || !std::isfinite(framebufferScale) || framebufferScale <= 0.0f)
				return 1.0f;
			return std::clamp(contentScale / framebufferScale, 1.0f, 3.0f);
		}

		void ApplyEditorStyle(float scale)
		{
			ENGINE_ASSERT(ImGui::GetCurrentContext() != nullptr, "Editor style needs an ImGui context");
			scale = std::isfinite(scale) ? std::clamp(scale, 1.0f, 3.0f) : 1.0f;
			// Rebuild from unscaled values: moving between monitors must never multiply yesterday's padding.
			ImGuiStyle style;
			style.FontSizeBase = 15.0f;
			style.FontScaleDpi = scale;
			style.WindowPadding = ImVec2(12.0f, 10.0f);
			style.FramePadding = ImVec2(8.0f, 5.0f);
			style.ItemSpacing = ImVec2(8.0f, 6.0f);
			style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
			style.CellPadding = ImVec2(6.0f, 5.0f);
			style.IndentSpacing = 16.0f;
			style.ScrollbarSize = 12.0f;
			style.GrabMinSize = 10.0f;
			style.WindowRounding = 5.0f;
			style.ChildRounding = 4.0f;
			style.FrameRounding = 3.0f;
			style.PopupRounding = 5.0f;
			style.ScrollbarRounding = 6.0f;
			style.GrabRounding = 3.0f;
			style.TabRounding = 3.0f;
			style.WindowBorderSize = 1.0f;
			style.ChildBorderSize = 1.0f;
			style.PopupBorderSize = 1.0f;
			style.FrameBorderSize = 0.0f;
			style.TabBorderSize = 0.0f;
			style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
			style.WindowMenuButtonPosition = ImGuiDir_None;
			style.DockingNodeHasCloseButton = false;
			style.SeparatorTextPadding = ImVec2(0.0f, 8.0f);
			style.DisabledAlpha = 0.55f;
			const ImVec4 background(0.105f, 0.114f, 0.129f, 1.0f);
			const ImVec4 surface(0.133f, 0.145f, 0.165f, 1.0f);
			const ImVec4 raised(0.176f, 0.192f, 0.216f, 1.0f);
			const ImVec4 hover(0.227f, 0.251f, 0.286f, 1.0f);
			const ImVec4 accent(0.314f, 0.596f, 0.918f, 1.0f);
			const ImVec4 selection(0.192f, 0.329f, 0.482f, 1.0f);
			auto& colors = style.Colors;
			colors[ImGuiCol_Text] = ImVec4(0.89f, 0.91f, 0.94f, 1.0f);
			colors[ImGuiCol_TextDisabled] = ImVec4(0.57f, 0.61f, 0.67f, 1.0f);
			colors[ImGuiCol_WindowBg] = background;
			colors[ImGuiCol_ChildBg] = background;
			colors[ImGuiCol_PopupBg] = surface;
			colors[ImGuiCol_Border] = ImVec4(0.23f, 0.25f, 0.28f, 0.70f);
			colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
			colors[ImGuiCol_FrameBg] = raised;
			colors[ImGuiCol_FrameBgHovered] = hover;
			colors[ImGuiCol_FrameBgActive] = selection;
			colors[ImGuiCol_TitleBg] = surface;
			colors[ImGuiCol_TitleBgActive] = surface;
			colors[ImGuiCol_TitleBgCollapsed] = background;
			colors[ImGuiCol_MenuBarBg] = background;
			colors[ImGuiCol_ScrollbarBg] = background;
			colors[ImGuiCol_ScrollbarGrab] = raised;
			colors[ImGuiCol_ScrollbarGrabHovered] = hover;
			colors[ImGuiCol_ScrollbarGrabActive] = selection;
			colors[ImGuiCol_CheckMark] = accent;
			colors[ImGuiCol_SliderGrab] = accent;
			colors[ImGuiCol_SliderGrabActive] = ImVec4(0.46f, 0.70f, 1.0f, 1.0f);
			colors[ImGuiCol_Button] = raised;
			colors[ImGuiCol_ButtonHovered] = hover;
			colors[ImGuiCol_ButtonActive] = selection;
			colors[ImGuiCol_Header] = surface;
			colors[ImGuiCol_HeaderHovered] = hover;
			colors[ImGuiCol_HeaderActive] = selection;
			colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
			colors[ImGuiCol_SeparatorHovered] = accent;
			colors[ImGuiCol_SeparatorActive] = accent;
			colors[ImGuiCol_ResizeGrip] = ImVec4(0.31f, 0.60f, 0.92f, 0.12f);
			colors[ImGuiCol_ResizeGripHovered] = accent;
			colors[ImGuiCol_ResizeGripActive] = accent;
			colors[ImGuiCol_Tab] = background;
			colors[ImGuiCol_TabHovered] = hover;
			colors[ImGuiCol_TabSelected] = raised;
			colors[ImGuiCol_TabSelectedOverline] = accent;
			colors[ImGuiCol_TabDimmed] = background;
			colors[ImGuiCol_TabDimmedSelected] = surface;
			colors[ImGuiCol_TabDimmedSelectedOverline] = selection;
			colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
			colors[ImGuiCol_DockingEmptyBg] = background;
			colors[ImGuiCol_TableHeaderBg] = surface;
			colors[ImGuiCol_TableBorderStrong] = colors[ImGuiCol_Border];
			colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.25f, 0.28f, 0.30f);
			colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
			colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
			colors[ImGuiCol_TextSelectedBg] = selection;
			colors[ImGuiCol_NavCursor] = accent;
			colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.03f, 0.05f, 0.65f);
			style.ScaleAllSizes(scale);
			ImGui::GetStyle() = style;
		}

		Status LoadEditorFont(const VirtualFileSystem& vfs)
		{
			ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Create("engine", "Fonts/Inter-Regular.ttf"));
			ENGINE_TRY_ASSIGN(const Buffer bytes, vfs.ReadFile(path));
			if (bytes.size() <= 100 || bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
				return MakeError(ErrorCode::Validation, "The editor's Inter font has an invalid size");
			ENGINE_TRY(WithContext(FontImporter::ValidateSource(bytes), "while reading the editor's Inter font"));
			// ImGui must free its font with its own allocator. The font is the committed, hash-pinned engine resource,
			// not an imported project font. Transfer the independent copy to the atlas, including its failure cleanup.
			std::unique_ptr<void, decltype(&ImGui::MemFree)> copy(ImGui::MemAlloc(bytes.size()), &ImGui::MemFree);
			if (!copy)
				return MakeError(ErrorCode::InvalidState, "Cannot allocate the editor font");
			std::memcpy(copy.get(), bytes.data(), bytes.size());
			ImFontConfig config;
			constexpr char FontName[] = "Inter Regular";
			std::copy(std::begin(FontName), std::end(FontName), config.Name);
			ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(copy.release(), static_cast<int>(bytes.size()), 15.0f, &config);
			if (font == nullptr)
				return MakeError(ErrorCode::Validation, "Cannot load the editor's Inter font");
			ImGui::GetIO().FontDefault = font;
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			// The editor applies both font and spacing scale at the safe point. The GLFW backend must not apply it twice.
			ImGui::GetIO().ConfigDpiScaleFonts = false;
			return {};
		}

		std::string EditorLabel(std::string_view identifier)
		{
			std::string label;
			label.reserve(identifier.size() + 8);
			const auto upper = [](char value)
			{
				return value >= 'A' && value <= 'Z';
			};
			const auto lower = [](char value)
			{
				return value >= 'a' && value <= 'z';
			};
			for (size_t index = 0; index < identifier.size(); ++index)
			{
				const char current = identifier[index];
				if (current == '_')
				{
					if (!label.empty() && label.back() != ' ')
						label += ' ';
					continue;
				}
				if (index != 0 && upper(current) && (lower(identifier[index - 1]) || (upper(identifier[index - 1]) && index + 1 < identifier.size() && lower(identifier[index + 1]))))
					label += ' ';
				label += current;
			}
			return label;
		}

		const char* EditorWindowTitle(EditorPanel panel)
		{
			switch (panel)
			{
				case EditorPanel::SceneHierarchy:  return "Hierarchy###SceneHierarchy";
				case EditorPanel::Inspector:       return "Inspector###Inspector";
				case EditorPanel::SceneViewport:   return "Scene###SceneViewport";
				case EditorPanel::GameViewport:    return "Game###GameViewport";
				case EditorPanel::ContentBrowser:  return "Assets###ContentBrowser";
				case EditorPanel::Console:         return "Console###Console";
				case EditorPanel::Diagnostics:     return "Problems###Diagnostics";
				case EditorPanel::ProjectSettings: return "Project Settings###ProjectSettings";
				case EditorPanel::Stats:           return "Performance###Stats";
				case EditorPanel::Automation:      return "Automation###Automation";
				case EditorPanel::UndoHistory:     return "History###UndoHistory";
				case EditorPanel::ProjectLauncher: return "Projects###ProjectLauncher";
			}
			ENGINE_ASSERT(false, "Unknown editor panel");
			return "Panel";
		}

		float EditorUiScale()
		{
			return ImGui::GetFontSize() / 15.0f;
		}

		void EditorSectionHeading(std::string_view title)
		{
			ImGui::Spacing();
			ImGui::TextUnformatted(title.data(), title.data() + title.size());
			ImGui::Separator();
			ImGui::Spacing();
		}

		void EditorEmptyState(std::string_view title, std::string_view description)
		{
			ImGui::Spacing();
			ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
			ImGui::TextUnformatted(title.data(), title.data() + title.size());
			ImGui::PopFont();
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(description.data(), description.data() + description.size());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
		}

		bool EditorToolbarButton(const char* label, const char* tooltip, bool active)
		{
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
			const bool pressed = ImGui::Button(label);
			if (active)
				ImGui::PopStyleColor();
			if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", tooltip);
			return pressed;
		}

	}

}
