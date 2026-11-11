#include "TestsPCH.h"

#include "Engine/Scene/UiTextInput.h"

#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <string>
#include <vector>

// The text input's editing (M22, C22.3, R-UI-TEXTINPUT; Docs/Decisions/0033-m22-contract.md decisions 45 to 47; stream T):
// committed characters inserted at the caret up to MaxLength, the editing keys by codepoint, and the events they raise.

namespace Engine {

	namespace {

		// A text input below a canvas with `text` and `caret`.
		Entity AddUiTextInput(Scene& scene, std::string text, uint32_t caret, uint32_t maxLength = 256)
		{
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity input = scene.CreateEntity("Input", canvas);
			input.AddComponent<UiRectComponent>(UiRectComponent{ .Size = glm::vec2(200.0f, 30.0f) });
			input.AddComponent<UiTextInputComponent>(UiTextInputComponent{ .Text = std::move(text), .MaxLength = maxLength, .Caret = caret });
			return input;
		}

	}

	TEST_SUITE("Scene")
	{
		// The review of C22.3 (Docs/Decisions/0033-m22-contract.md, "Review of C22.3", the text input's limits): the longest
		// Text typing reaches, MaxUiTextInputLength codepoints of at most four UTF-8 bytes each, fits what a script's write of
		// a component holds (Scripting/ScriptLimits.h), MaxLength is registered up to that limit, and Caret, read clamped to
		// Text's codepoint count, is registered without an upper bound, since a Text a scene or automation writes may be
		// longer than any MaxLength.
		TEST_CASE("UiTextInput: a Text of the longest MaxLength fits a script's write, and Caret is registered for any Text")
		{
			CHECK(static_cast<size_t>(MaxUiTextInputLength) * 4 <= MaxScriptComponentStringBytes);
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* component = registry->FindComponent<UiTextInputComponent>();
			REQUIRE(component != nullptr);
			const FieldInfo* maxLength = component->FindField("MaxLength");
			const FieldInfo* caret = component->FindField("Caret");
			REQUIRE(maxLength != nullptr);
			REQUIRE(caret != nullptr);
			CHECK(maxLength->GetMeta().Min == 1.0);
			CHECK(maxLength->GetMeta().Max == static_cast<double>(MaxUiTextInputLength));
			CHECK(caret->GetMeta().Min == 0.0);
			CHECK_FALSE(caret->GetMeta().Max.has_value());
		}

		TEST_CASE("UiTextInput: typed characters insert at the caret up to MaxLength, and the editing keys remove and move by codepoint")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// "a", e with an acute accent (2 bytes), "b": three codepoints; the caret after the accented e.
			Entity input = AddUiTextInput(scene, "a\xC3\xA9"
												 "b",
				2, 6);
			const UUID id = input.GetUUID();
			CHECK(ReadUiTextInputCaret(input.GetComponent<UiTextInputComponent>()) == 2u);
			CHECK(GetUiTextInputCaretByte(input.GetComponent<UiTextInputComponent>()) == 3u);

			// Typing inserts at the caret, drops control characters and ill-formed bytes, and stops at MaxLength (6 codepoints):
			// "x\ty" and an Alef insert "x", "y" and the Alef; a sixth codepoint "z" is dropped.
			std::vector<UiEvent> events;
			CHECK(EditUiTextInput(scene, id, "x\ty\xFF\xD7\x90z", UiTextKeys::None, events));
			CHECK(input.GetComponent<UiTextInputComponent>().Text == "a\xC3\xA9xy\xD7\x90"
																	 "b");
			CHECK(input.GetComponent<UiTextInputComponent>().Caret == 5u);
			REQUIRE(events.size() == 1);
			CHECK(events[0].Kind == UiEventKind::TextChanged);
			CHECK(events[0].Target == id);

			// The keys apply in their fixed order: Backspace (removes the Alef), Delete (removes "b"), then Left and Home.
			events.clear();
			CHECK(EditUiTextInput(scene, id, {}, UiTextKeys::Backspace | UiTextKeys::Delete | UiTextKeys::Left | UiTextKeys::Home, events));
			CHECK(input.GetComponent<UiTextInputComponent>().Text == "a\xC3\xA9xy");
			CHECK(input.GetComponent<UiTextInputComponent>().Caret == 0u);
			REQUIRE(events.size() == 1);
			CHECK(events[0].Kind == UiEventKind::TextChanged);

			// Moving the caret changes no text and raises nothing; End goes past the last codepoint; Enter raises Submit.
			events.clear();
			CHECK(EditUiTextInput(scene, id, {}, UiTextKeys::Right | UiTextKeys::End, events));
			CHECK(input.GetComponent<UiTextInputComponent>().Caret == 4u);
			CHECK(events.empty());
			CHECK_FALSE(EditUiTextInput(scene, id, {}, UiTextKeys::Right | UiTextKeys::Enter, events));
			REQUIRE(events.size() == 1);
			CHECK(events[0].Kind == UiEventKind::Submit);

			// A caret beyond the text reads clamped, and a backspace at 0 removes nothing.
			input.GetComponent<UiTextInputComponent>().Caret = 99;
			CHECK(ReadUiTextInputCaret(input.GetComponent<UiTextInputComponent>()) == 4u);
			CHECK(GetUiTextInputCaretByte(input.GetComponent<UiTextInputComponent>()) == 5u);
			input.GetComponent<UiTextInputComponent>().Caret = 0;
			events.clear();
			CHECK_FALSE(EditUiTextInput(scene, id, {}, UiTextKeys::Backspace, events));
			CHECK(events.empty());

			// A text input that is not interactable, or an entity that is not one, takes nothing.
			input.GetComponent<UiTextInputComponent>().Interactable = false;
			CHECK_FALSE(EditUiTextInput(scene, id, "q", UiTextKeys::None, events));
			CHECK(input.GetComponent<UiTextInputComponent>().Text == "a\xC3\xA9xy");
			CHECK_FALSE(EditUiTextInput(scene, scene.FindEntityByPath("/Canvas").GetUUID(), "q", UiTextKeys::None, events));
			CHECK(events.empty());
		}
	}

}
