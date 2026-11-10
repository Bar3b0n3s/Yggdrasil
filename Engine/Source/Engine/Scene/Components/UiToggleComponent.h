#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

namespace Engine {

	// Registry name "UiToggle" (M22, C22.2, R-UI-WIDGETS; Docs/Decisions/0033-m22-contract.md decisions 21, 24 and 26): a
	// widget whose IsOn flips when it is activated (a pointer press and release over it, or Submit while it has focus),
	// raising OnUiValueChanged with the new value. Graphic names a UI element below the toggle (its check mark) that is
	// shown only while IsOn: while off, the layout marks that element and its subtree Hidden (UiLayoutNode::Hidden): laid
	// out, neither drawn, hit nor focusable. A Graphic that is null or not an element below the toggle is ignored
	// (project.validate's UI_WIDGET_PART_INVALID). Requires a UiRect; excludes UiButton, UiSlider, UiScrollView and
	// UiTextInput. Toggle groups (radio buttons) are not built: a script keeps one of several toggles on from OnUiValueChanged.
	struct UiToggleComponent
	{
		bool Interactable = true; // as UiButtonComponent::Interactable
		bool Focusable = true;    // as UiButtonComponent::Focusable
		bool IsOn = false;
		UUID Graphic{}; // EntityRef: the element shown while IsOn
	};

}
