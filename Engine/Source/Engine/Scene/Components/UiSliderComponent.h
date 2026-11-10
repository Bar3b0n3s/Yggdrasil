#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/Components/UiLayoutComponent.h"

namespace Engine {

	// Registry name "UiSlider" (M22, C22.2, R-UI-WIDGETS; Docs/Decisions/0033-m22-contract.md decisions 21, 24 and 26): a
	// widget holding a number between Min and Max that the pointer drags and the keyboard and gamepad step, raising
	// OnUiValueChanged when interaction changes it.
	//   - The value as read: Value clamped to [Min, Max]; with WholeNumbers, then rounded to floor(v + 0.5) and kept to the
	//     whole numbers of the range, clamped to [ceil(Min), floor(Max)] (Min when the range holds no whole number). So it
	//     always lies in [Min, Max], and its normalized position t = (value - Min) / (Max - Min) in [0, 1]. Writes from
	//     scripts and automation keep what they write (the registry checks Min < Max only); interaction writes the value as
	//     read.
	//   - Layout (Scene/UiLayout.h): Direction Horizontal puts Min at the left edge, Vertical at the bottom edge. Fill, an
	//     element below the slider, spans its parent element's rectangle from the Min edge to t (Horizontal: its AnchorMin.x
	//     is 0 and its AnchorMax.x is t; Vertical: AnchorMin.y is 1 - t and AnchorMax.y 1); Handle, an element below the
	//     slider, sits at t (Horizontal: both x anchors are t; Vertical: both y anchors are 1 - t). The other axis's anchors
	//     and every other UiRect field are the element's own. A Fill or Handle that is null or not an element below the
	//     slider is ignored (project.validate's UI_WIDGET_PART_INVALID).
	//   - Pointer: a press on the slider (or on any element whose nearest widget it is) sets the value at the pointer and
	//     drags it until the release: t is the pointer's position along the slide rectangle (the Handle's parent element's
	//     rectangle, else the Fill's parent's, else the slider's own), clamped to [0, 1].
	//   - Keyboard and gamepad: while focused, the directions along Direction (Left and Right, or Down and Up) change the
	//     value as read by the step toward Max (Right, Up) or Min (Left, Down), and the result is read as above; the other two
	//     directions move focus as usual. The step is Step (0: a tenth of Max - Min); with WholeNumbers it is rounded to a
	//     whole number, floor(step + 0.5), and at least 1, whatever Step says, so every press moves a whole-number slider
	//     that is not at its end.
	// Requires a UiRect; excludes UiButton, UiToggle, UiScrollView and UiTextInput.
	struct UiSliderComponent
	{
		bool Interactable = true; // as UiButtonComponent::Interactable
		bool Focusable = true;    // as UiButtonComponent::Focusable
		float Value = 0.0f;
		float Min = 0.0f; // below Max (the registry's check)
		float Max = 1.0f;
		bool WholeNumbers = false;
		float Step = 0.0f; // 0 to 1,000,000; 0: a tenth of the range; with WholeNumbers rounded, at least 1
		UiLayoutDirection Direction = UiLayoutDirection::Horizontal;
		UUID Fill{};   // EntityRef: the element stretched from the Min edge to the value
		UUID Handle{}; // EntityRef: the element placed at the value
	};

}
