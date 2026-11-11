#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <string>

namespace Engine {

	// Registry name "UiTextInput" (M22, C22.3, R-UI-TEXTINPUT; Docs/Decisions/0033-m22-contract.md decisions 45 to 48): a
	// single-line text field. While it has focus (a pointer press, navigation, Ui.SetFocus), the tick's committed characters
	// (the text latch: PlayInput's step text, the Text events of the tick, from GLFW's character callback or injected) are
	// inserted at its Caret, up to MaxLength codepoints, and the editing keys edit it (Scene/UiTextInput.h): Backspace and
	// Delete remove the codepoint before and after the caret, Left, Right, Home and End move the caret; Enter (or the
	// keypad's) and Submit while focused raise OnUiSubmit. A change of Text by input raises OnUiTextChanged; writes by
	// scripts or automation raise none. There is no IME composition display, no selection and no clipboard (accepted limits,
	// Backlog §6): a game sees committed characters only.
	//
	// What it shows: TextElement names a UI element below the input whose UiText draws the input's Text in place of its own
	// (its font, size, colour, alignments and line spacing are that UiText's), with the caret while the input has focus;
	// Placeholder names an element below the input shown only while Text is empty (Hidden otherwise, UiLayoutNode::Hidden).
	// The input clips its descendants as ClipChildren does, whatever its UiRect says, so a long text never spills out; the
	// Ui pass keeps the caret inside the TextElement's rectangle by moving the text. A part that is null or not an element
	// below the input in the same canvas is ignored (project.validate's UI_WIDGET_PART_INVALID).
	//
	// Simulation state: Text and Caret are the scene's (serialized, so in the state hash); interaction writes them in the
	// UiInput phase from latched input only. Requires a UiRect; excludes UiButton, UiToggle, UiSlider and UiScrollView (and
	// each of them excludes it).
	struct UiTextInputComponent
	{
		bool Interactable = true; // as UiButtonComponent::Interactable
		bool Focusable = true;    // as UiButtonComponent::Focusable
		std::string Text{};       // UTF-8; typing never inserts a control character (U+0000 to U+001F, U+007F to U+009F)
		// The most codepoints typing reaches, 1 to MaxUiTextInputLength (Scene/UiTextInput.h, 1,024); characters past it are
		// dropped. A script, automation or the scene may write a longer Text (a script's write holds at most 4,096 bytes),
		// which is kept: typing then inserts nothing until the editing keys bring it below MaxLength.
		uint32_t MaxLength = 256;
		// The caret, in codepoints from the start of Text, read clamped to Text's codepoint count (ReadUiTextInputCaret);
		// registered without an upper bound, since a Text may be longer than any MaxLength.
		uint32_t Caret = 0;
		UUID TextElement{}; // EntityRef: the element whose UiText shows Text, with the caret while focused
		UUID Placeholder{}; // EntityRef: the element shown only while Text is empty
	};

}
