#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiInteraction.h"
#include "Engine/Scene/UiLayout.h"
#include "Engine/Scene/UiSystem.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Scripting/ScriptProxy.h"

#include <lua.h>

#include <format>
#include <optional>
#include <string>
#include <utility>

// The Ui module (M22, C22.2, R-UI-EVENTS; Docs/Decisions/0033-m22-contract.md decision 28): the game UI's interaction state
// and layout in the running scene, read through the play session's UiSystem (IScriptHost::GetUi), and Ui.SetFocus. A host
// without one (an edit evaluation) reports no focus, hover or press and no rectangle, and refuses Ui.SetFocus.

namespace Engine {

	namespace Utils {

		// The element `id` names, or nil when it is null or the scene no longer holds it live (a destroyed element has lost
		// its place in the interaction state; the next UiInput clears it).
		static void PushUiElement(ScriptCall& call, UUID id)
		{
			IScriptHost& host = call.Engine->GetHost();
			if (!id.IsValid() || !host.GetScene().FindEntityByID(id))
				Lua::PushNil(call);
			else
				Lua::Push(call, ScriptEntityIdentity{ id, host.GetSceneGeneration() });
		}

		// The session's interaction state, or null without a session's UI.
		static const UiInteractionState* GetUiInteraction(ScriptCall& call)
		{
			const UiSystem* ui = call.Engine->GetHost().GetUi();
			return ui == nullptr ? nullptr : &ui->GetInteraction();
		}

		static int UiGetFocus(ScriptCall& call)
		{
			const UiInteractionState* interaction = GetUiInteraction(call);
			PushUiElement(call, interaction == nullptr ? UUID{} : interaction->Focused);
			return 1;
		}

		static int UiGetHovered(ScriptCall& call)
		{
			const UiInteractionState* interaction = GetUiInteraction(call);
			PushUiElement(call, interaction == nullptr ? UUID{} : interaction->Hovered);
			return 1;
		}

		static int UiGetPressed(ScriptCall& call)
		{
			const UiInteractionState* interaction = GetUiInteraction(call);
			PushUiElement(call, interaction == nullptr ? UUID{} : interaction->Pressed);
			return 1;
		}

		static int UiIsPointerOverUi(ScriptCall& call)
		{
			const UiInteractionState* interaction = GetUiInteraction(call);
			Lua::Push(call, interaction != nullptr && interaction->PointerOverUi);
			return 1;
		}

		static int UiGetRect(ScriptCall& call)
		{
			IScriptHost& host = call.Engine->GetHost();
			const auto identity = Lua::Check<ScriptEntityIdentity>(call, 1);
			const Status readable = ScriptProxy::ValidateEntityForRead(host, identity);
			if (!readable)
				return Lua::RaiseError(call, readable.error());
			const UiSystem* ui = host.GetUi();
			const UiLayoutNode* node = ui == nullptr ? nullptr : FindUiLayoutNode(ui->GetLayout(), identity.ID);
			if (node == nullptr)
			{
				Lua::PushNil(call);
				return 1;
			}
			// C22.3 (the review of C22.3, decision 56): in the pixels of the element's canvas (a World canvas's own), with the
			// canvas. The canvas is read as the element is (W22.3's review): an element pending destruction (M20, W-P02) keeps
			// reporting its canvas, which a destroyed ancestor leaves pending until the same flush, and no script runs between
			// a flush and the next layout, so a laid-out readable element's canvas is readable too (nil is only a guard).
			lua_createtable(call.State, 0, 3);
			Lua::Push(call, glm::vec3(node->Min, 0.0f));
			lua_setfield(call.State, -2, "Min");
			Lua::Push(call, glm::vec3(node->Max, 0.0f));
			lua_setfield(call.State, -2, "Max");
			if (std::as_const(host.GetScene()).FindReadableEntityByID(node->Canvas).IsReadable())
				Lua::Push(call, ScriptEntityIdentity{ node->Canvas, host.GetSceneGeneration() });
			else
				Lua::PushNil(call);
			lua_setfield(call.State, -2, "Canvas");
			return 1;
		}

		static int UiSetFocus(ScriptCall& call)
		{
			IScriptHost& host = call.Engine->GetHost();
			UUID element{};
			if (!Lua::IsNoneOrNil(call, 1))
			{
				const auto identity = Lua::Check<ScriptEntityIdentity>(call, 1);
				const Status valid = ScriptProxy::ValidateEntity(host, identity);
				if (!valid)
					return Lua::RaiseError(call, valid.error());
				element = identity.ID;
			}
			UiSystem* ui = host.GetUi();
			if (ui == nullptr)
			{
				return Lua::RaiseError(call,
					Error(ErrorCode::InvalidState, "there is no game UI to focus: Ui.SetFocus works in a running play session")
						.WithHint("call it from a behaviour or a test case, not from an edit-scene evaluation"));
			}
			Scene& scene = host.GetScene();
			// A change of focus is a gameplay mutation (it queues Focus events and may scroll a scroll view), so an evaluation
			// reports it before it happens; a refused element raises its refusal, and the focus it already has changes nothing.
			const Result<bool> changes = ui->CheckFocus(scene, element);
			if (!changes)
				return Lua::RaiseError(call, changes.error());
			if (!*changes)
				return 0;
			const Status permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const Status focused = ui->SetFocus(scene, element);
			if (!focused)
				return Lua::RaiseError(call, focused.error());
			return 0;
		}

		// M28 (Docs/Decisions/0043-m28-game-services.md decision 5): the receiver of a UiText presentation method, a UiText on
		// a live entity (or, to read, one still readable), and the session's UI, which holds the presentation texts. Both
		// methods are frame-paced like Application.GetStats: a fixed-step gameplay callback raises, so a frame-paced value
		// shown through them never reaches the fixed step.
		static std::pair<Entity, UiSystem*> UiTextSelf(ScriptCall& call, bool read)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			const Status valid = read ? ScriptProxy::ValidateComponentForRead(host, proxy) : ScriptProxy::ValidateComponent(host, proxy);
			if (!valid)
				Lua::RaiseError(call, valid.error());
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != "UiText")
				Lua::RaiseError(call, "wrong component receiver");
			const bool gameplay = call.Execution != nullptr && call.Execution->Origin == ScriptExecutionOrigin::Gameplay;
			if (gameplay && host.GetFrameState().Phase == InputPhase::Step)
			{
				Lua::RaiseError(call, "presentation text is frame-paced and outside the state hash, so a fixed-step callback (OnFixedUpdate, a task, "
									  "a physics, animation or UI event) cannot set or read it; use it in OnUpdate or OnLateUpdate, or write "
									  "UiText.Text in the fixed step");
			}
			Entity entity = read ? host.GetScene().FindReadableEntityByID(proxy.Entity.ID) : host.GetScene().FindEntityByID(proxy.Entity.ID);
			return { entity, host.GetUi() };
		}

		static int UiTextSetPresentationText(ScriptCall& call)
		{
			const auto [entity, ui] = UiTextSelf(call, false);
			std::optional<std::string> text;
			if (!Lua::IsNoneOrNil(call, 2))
			{
				text = Lua::Check<std::string>(call, 2);
				if (text->size() > MaxScriptPresentationTextBytes)
					return Lua::RaiseError(call, std::format("a presentation text holds at most {} bytes; this one has {}", MaxScriptPresentationTextBytes, text->size()));
				if (!IsValidUtf8(*text))
					return Lua::RaiseError(call, std::format("the presentation text is not valid UTF-8 (at byte {})", FindInvalidUtf8(*text)));
			}
			if (ui == nullptr)
			{
				return Lua::RaiseError(call,
					Error(ErrorCode::InvalidState, "there is no game UI to show presentation text: it works in a running play session")
						.WithHint("call it from a behaviour or a test case, not from an edit-scene evaluation"));
			}
			ui->SetPresentationText(entity.GetUUID(), std::move(text));
			return 0;
		}

		static int UiTextGetPresentationText(ScriptCall& call)
		{
			const auto [entity, ui] = UiTextSelf(call, true);
			const std::string* text = ui == nullptr ? nullptr : ui->FindPresentationText(entity.GetUUID());
			if (text == nullptr)
				Lua::PushNil(call);
			else
				Lua::PushString(call, *text);
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterUi(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			ENGINE_TRY(api.RegisterAlias("UiElementRect", "{Min: vector, Max: vector, Canvas: Entity}",
				"A UI element's laid-out rectangle: its top-left (Min) and bottom-right (Max) corners as (x, y, 0), x right and y down, in scene "
				"pixels for an element of a Screen canvas and in its canvas's own pixels for one of a World canvas, and the canvas it belongs to "
				"(Canvas)."));
			api.Module("Ui",
				   "The game UI's interaction state in the running scene: keyboard and gamepad focus, the widget under the pointer and the one it "
				   "presses, and where elements were laid out. UI events reach behaviours through the OnUi callbacks, which bubble from the element "
				   "to its ancestors.")
				.Function("GetFocus", &Utils::UiGetFocus, "() -> Entity?", "Returns the UI element that has keyboard and gamepad focus, or nil.",
					read)
				.Function("SetFocus", &Utils::UiSetFocus, "(element: Entity?) -> ()",
					"Moves focus to element, a laid-out, interactable and focusable UI widget, or clears it with nil; OnUiFocus follows in the next "
					"UiEvents phase. Any other element raises a located error saying why.")
				.Function("GetHovered", &Utils::UiGetHovered, "() -> Entity?", "Returns the interactable UI widget under the pointer, or nil.", read)
				.Function("GetPressed", &Utils::UiGetPressed, "() -> Entity?", "Returns the UI widget the held pointer pressed, or nil.", read)
				.Function("IsPointerOverUi", &Utils::UiIsPointerOverUi, "() -> boolean",
					"Returns whether the pointer was over a UI image, text or widget that catches it at this tick, so gameplay can ignore clicks "
					"meant for the UI.",
					read)
				.Function("GetRect", &Utils::UiGetRect, "(element: Entity) -> UiElementRect?",
					"Returns element's rectangle in the current UI layout, in scene pixels (which are view coordinates unless the output is "
					"upscaled) or, for an element of a World canvas, in that canvas's own pixels, with its canvas; nil when it is not laid out.",
					read);
			// M28 (Docs/Decisions/0043-m28-game-services.md decision 5): presentation text, frame-paced values a UiText shows
			// without reaching the state hash.
			const ScriptMemberOptions write{};
			api.Type("UiText")
				.Method("SetPresentationText", Utils::UiTextSetPresentationText, "(self: UiText, text: string?) -> ()",
					std::format("Shows text (at most {} bytes of UTF-8) in place of the element's Text until it is cleared with nil, the element or its "
								"UiText is removed, or the scene is replaced: frame-paced text such as a frame time or a performance readout, never "
								"serialized, hashed or recorded, so it never splits a replay. Layout and hit testing keep the authored Text's "
								"element. A fixed-step gameplay callback (OnFixedUpdate, a task, a physics, animation or UI event) raises an error: "
								"set it in OnUpdate or OnLateUpdate.",
						MaxScriptPresentationTextBytes),
					write)
				.Method("GetPresentationText", Utils::UiTextGetPresentationText, "(self: UiText) -> string?",
					"Returns the presentation text SetPresentationText set, or nil. Frame-paced: a fixed-step gameplay callback raises an error.",
					read);
			return {};
		}

	}

}
