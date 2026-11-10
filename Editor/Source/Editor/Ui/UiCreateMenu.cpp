#include "EditorPCH.h"
#include "Editor/Ui/UiCreateMenu.h"

#include <imgui.h>

#include <array>
#include <utility>

namespace Engine {

	std::optional<Json> DrawUiCreateMenu(const char* label, UUID parent, bool enabled)
	{
		// The menu labels and UiWidgetKind's registry names, in the enum's order.
		static constexpr std::array<std::pair<const char*, const char*>, 9> Kinds = { { { "Canvas", "Canvas" },
			{ "Panel", "Panel" },
			{ "Image", "Image" },
			{ "Text", "Text" },
			{ "Button", "Button" },
			{ "Toggle", "Toggle" },
			{ "Slider", "Slider" },
			{ "Scroll view", "ScrollView" },
			{ "Text input", "TextInput" } } };
		if (!ImGui::BeginMenu(label, enabled))
			return std::nullopt;
		std::optional<Json> chosen;
		for (const auto& [item, kind] : Kinds)
		{
			if (!ImGui::MenuItem(item))
				continue;
			Json params{ { "kind", kind } };
			if (parent.IsValid())
				params["parent"] = parent.ToString();
			chosen = std::move(params);
		}
		ImGui::EndMenu();
		return chosen;
	}

}
