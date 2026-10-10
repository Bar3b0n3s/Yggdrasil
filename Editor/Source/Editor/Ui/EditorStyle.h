#pragma once

#include "EditorCore/EditorUiState.h"

#include <string>
#include <string_view>

namespace Engine {

	class VirtualFileSystem;

	namespace Utils {

		// Presentation only; these helpers never change the scene, registry names or automation IDs.
		// Main thread with an active ImGui context, outside a frame for ApplyEditorStyle.
		void ApplyEditorStyle(float scale = 1.0f);
		// Convert monitor DPI into logical UI units; Retina already scales logical units into framebuffer pixels.
		[[nodiscard]] float EditorDisplayScale(float contentScale, float framebufferScale);
		// Loads the committed Inter resource through engine://. Atlas owns the copied bytes until context teardown.
		// Called once before the first frame; returns file errors, Validation or ImportFailed for unusable font data.
		[[nodiscard]] Status LoadEditorFont(const VirtualFileSystem& vfs);
		[[nodiscard]] std::string EditorLabel(std::string_view identifier);
		// Friendly visible title with the existing stable docking ID after ### (Dear ImGui 1.92.6+ hashing).
		[[nodiscard]] const char* EditorWindowTitle(EditorPanel panel);
		// Scale in screen coordinates relative to the editor's 15 px base type size.
		[[nodiscard]] float EditorUiScale();
		void EditorSectionHeading(std::string_view title);
		void EditorEmptyState(std::string_view title, std::string_view description);
		// Call inside a window. Tooltip also explains disabled actions when supplied.
		[[nodiscard]] bool EditorToolbarButton(const char* label, const char* tooltip, bool active = false);

	}

}
