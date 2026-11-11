#pragma once

#include "Engine/Core/Base.h"

namespace Engine {

	// Registry name "UiButton" (M22, C22.2, R-UI-WIDGETS; Docs/Decisions/0033-m22-contract.md decisions 21 and 24): a widget
	// that raises OnUiClick when it is activated: a pointer press and release over it (the release over the button the
	// press began on), or Submit while it has focus (Scene/UiInteraction.h). Requires a UiRect; one widget per element (it
	// excludes UiToggle, UiSlider, UiScrollView and UiTextInput). Its UiImage, if any, takes its canvas's state tints (UiTheme).
	struct UiButtonComponent
	{
		// False: the button is Disabled (its tint), takes no hover, press or focus and raises no event; it still blocks
		// the pointer.
		bool Interactable = true;
		// Keyboard and gamepad navigation, Ui.SetFocus and a pointer press on it may focus it. False: nothing focuses it,
		// so a pointer press clicks it without taking keyboard focus (a HUD button in a game whose play uses the arrows and
		// Space), and Submit never reaches it.
		bool Focusable = true;
	};

}
