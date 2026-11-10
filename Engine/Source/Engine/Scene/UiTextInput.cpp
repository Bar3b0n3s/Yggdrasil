#include "EnginePCH.h"
#include "Engine/Scene/UiTextInput.h"

#include "Engine/Core/Utf8.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

// The text input's editing (UiTextInput.h; Docs/Decisions/0033-m22-contract.md decision 47). Text is walked one codepoint at
// a time: a well-formed UTF-8 sequence (GetUtf8SequenceLength) is one codepoint, and so is every byte that starts none (an
// ill-formed byte, kept as it is), as the text-measurement layer counts them. Only integer operations on the bytes.

namespace Engine {

	namespace Utils {

		// The byte length of the codepoint at `offset` of `text` (below its size): its well-formed sequence's, or 1 for a byte
		// that starts none.
		static size_t GetUiTextCodepointLength(std::string_view text, size_t offset)
		{
			return std::max<size_t>(GetUtf8SequenceLength(text, offset), 1);
		}

		// The byte offset in `text` of the codepoint `index` (its size when `index` is at or past its codepoint count).
		static size_t FindUiTextByte(std::string_view text, uint32_t index)
		{
			size_t offset = 0;
			for (uint32_t codepoint = 0; codepoint < index && offset < text.size(); ++codepoint)
				offset += GetUiTextCodepointLength(text, offset);
			return offset;
		}

		static uint32_t CountUiTextCodepoints(std::string_view text)
		{
			uint32_t count = 0;
			for (size_t offset = 0; offset < text.size(); offset += GetUiTextCodepointLength(text, offset))
				++count;
			return count;
		}

		// Whether the well-formed sequence `sequence` encodes a control character typing never inserts: C0 (U+0000 to U+001F,
		// one byte), DEL (U+007F) or C1 (U+0080 to U+009F, the two bytes C2 80 to C2 9F).
		static bool IsUiTextControl(std::string_view sequence)
		{
			const auto lead = static_cast<unsigned char>(sequence[0]);
			if (sequence.size() == 1)
				return lead < 0x20 || lead == 0x7F;
			return sequence.size() == 2 && lead == 0xC2 && static_cast<unsigned char>(sequence[1]) <= 0x9F;
		}

	}

	uint32_t ReadUiTextInputCaret(const UiTextInputComponent& input)
	{
		return std::min(input.Caret, Utils::CountUiTextCodepoints(input.Text));
	}

	uint32_t GetUiTextInputCaretByte(const UiTextInputComponent& input)
	{
		return static_cast<uint32_t>(Utils::FindUiTextByte(input.Text, input.Caret));
	}

	bool EditUiTextInput(Scene& scene, UUID input, std::string_view typed, UiTextKeys keys, std::vector<UiEvent>& events)
	{
		const Entity entity = scene.FindEntityByID(input);
		UiTextInputComponent* component = entity ? entity.TryGetComponent<UiTextInputComponent>() : nullptr;
		if (component == nullptr || !component->Interactable)
			return false;

		std::string text = component->Text;
		uint32_t count = Utils::CountUiTextCodepoints(text);
		uint32_t caret = std::min(component->Caret, count);

		// 1. Typing: well-formed codepoints other than control characters, while the text holds fewer than MaxLength, inserted
		// together at the caret as read.
		const size_t insertAt = Utils::FindUiTextByte(text, caret);
		std::string inserted;
		for (size_t offset = 0; offset < typed.size() && count < component->MaxLength;)
		{
			const size_t length = GetUtf8SequenceLength(typed, offset);
			if (length == 0)
			{
				++offset;
				continue;
			}
			const std::string_view sequence = typed.substr(offset, length);
			offset += length;
			if (Utils::IsUiTextControl(sequence))
				continue;
			inserted += sequence;
			++count;
			++caret;
		}
		text.insert(insertAt, inserted);

		// 2. The editing keys, each once, in their fixed order.
		if (HasFlag(keys, UiTextKeys::Backspace) && caret > 0)
		{
			const size_t begin = Utils::FindUiTextByte(text, caret - 1);
			text.erase(begin, Utils::GetUiTextCodepointLength(text, begin));
			--caret;
			--count;
		}
		if (HasFlag(keys, UiTextKeys::Delete) && caret < count)
		{
			const size_t begin = Utils::FindUiTextByte(text, caret);
			text.erase(begin, Utils::GetUiTextCodepointLength(text, begin));
			--count;
		}
		if (HasFlag(keys, UiTextKeys::Left) && caret > 0)
			--caret;
		if (HasFlag(keys, UiTextKeys::Right) && caret < count)
			++caret;
		if (HasFlag(keys, UiTextKeys::Home))
			caret = 0;
		if (HasFlag(keys, UiTextKeys::End))
			caret = count;

		// 3. Text and Caret written; TextChanged when Text changed, then Submit for Enter.
		const bool textChanged = text != component->Text;
		const bool changed = textChanged || caret != component->Caret;
		if (textChanged)
			component->Text = std::move(text);
		component->Caret = caret;
		if (textChanged)
			events.push_back(UiEvent{ .Kind = UiEventKind::TextChanged, .Target = input });
		if (HasFlag(keys, UiTextKeys::Enter))
			events.push_back(UiEvent{ .Kind = UiEventKind::Submit, .Target = input });
		return changed;
	}

}
