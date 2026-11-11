#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiInteraction.h"
#include "Engine/Scene/UiLayout.h"
#include "Engine/Scene/UiSystem.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Scripting/ScriptProxy.h"

#include <lua.h>

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
			return {};
		}

	}

}
