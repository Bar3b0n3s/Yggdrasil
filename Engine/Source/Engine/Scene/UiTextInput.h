#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/UiInteraction.h"

#include <cstdint>
#include <string_view>
#include <vector>

// The text input's editing (M22, C22.3, R-UI-TEXTINPUT; Docs/Decisions/0033-m22-contract.md decisions 45 to 47; stream T):
// what UiSystem::ProcessInput (Scene/UiSystem.h, its text step) does to the focused UiTextInput with the tick's latched text
// and editing keys (UiInputFrame::Text and TextKeys). Text is UTF-8 and the caret counts codepoints; an ill-formed byte
// counts as one codepoint and is kept as it is, as the text-measurement layer skips one (Renderer/TextMeasurement.h), so
// editing never fails and never reinterprets bytes it did not insert. Editing works on codepoints, not grapheme clusters
// (an accepted limit, Backlog §6): Backspace after a combining mark removes the mark. Integer operations only, from latched
// input only, so a deterministic session edits identically in every configuration. Writes go to the scene's component
// (its serialization, so the state hash, covers them). Main thread.

namespace Engine {

	class Scene;

	// The largest MaxLength a text input takes (its registered range), in codepoints: at most four UTF-8 bytes each, so the
	// longest Text typing reaches, 4,096 bytes, fits what a script's write of a component holds
	// (Scripting/ScriptLimits.h's MaxScriptComponentStringBytes; the review of C22.3).
	inline constexpr uint32_t MaxUiTextInputLength = 1024;

	// The caret as read: Caret clamped to Text's codepoint count. Pure.
	[[nodiscard]] uint32_t ReadUiTextInputCaret(const UiTextInputComponent& input);

	// The UTF-8 byte offset in Text of the caret as read (Text's size at its end): where the Ui pass draws it
	// (UiDrawItem::CaretByte). Pure.
	[[nodiscard]] uint32_t GetUiTextInputCaretByte(const UiTextInputComponent& input);

	// One UiInput's editing of the text input `input` (its widget has focus and is interactable; anything else does nothing
	// and returns false), in this order:
	//   1. `typed`, the tick's committed characters (UTF-8, in the order typed), without its control characters (U+0000 to
	//      U+001F, U+007F to U+009F) and ill-formed bytes, inserted at the caret as read, as many codepoints as keep Text's
	//      codepoint count at most MaxLength (the rest dropped); the caret moves past them;
	//   2. the editing keys of `keys`, each once, in the order Backspace (removes the codepoint before the caret), Delete (the
	//      one after it), Left, Right (the caret one codepoint, kept within Text), Home (to 0), End (to the codepoint count);
	//      the step view carries no order between a step's keys and characters, so this order is the rule;
	//   3. when Text changed, TextChanged at `input`; then, when `keys` holds Enter, Submit at `input`.
	// Text and Caret are written (Caret as read after the edit). Returns whether Text or Caret changed.
	bool EditUiTextInput(Scene& scene, UUID input, std::string_view typed, UiTextKeys keys, std::vector<UiEvent>& events);

}
