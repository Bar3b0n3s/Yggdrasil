#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Session/PlayInput.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// input.inject (Architecture §13.5 "input", §13.6) and the input event params play.step shares, typed on
// AutomationMethodContext so the Editor and the Runtime share them (§13.5 "Runtime subset: input.*"). Conventions as in
// MethodRegistry.h; frozen by the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 4).
//
// Events (§13.6) are stamped with a tick offset relative to the session's next tick (input.inject) or to the first tick of
// the play.step that carries them, queued in the session's PlayInput and applied at step 1 of their tick (§5.7), so
// injected input is replayable and recorded like any other (§13.6). Type and state names are case-insensitive and echoed
// in PascalCase; "tap" is down on its tick and up on the next, so both edges are observed. Every event is validated before
// any is queued (all or nothing).
//
// Deferred to later milestones: input.record and input.replay (§13.6) need the replay format with Luau expectations and
// the cooked replay player (M13 and M14); they are not registered in M7.

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry struct "InputEvent": one input event of input.inject or play.step (§13.6), camelCase keys:
	//   {tick?, type, name?, state?, value?, key?, button?, gamepad?, axis?, position?, delta?, text?}
	// Which members a type reads (PlayInputEvent):
	//   action        name, and either state (a button use) or value (an axis value in [-1, 1]); both is InvalidParams
	//   key           key (a KeyCodes.h key name, "Space", "A", "Escape"), state
	//   mouseButton   button ("Left", "Right", "Middle", ...), state, position? ([x, y] window coordinates)
	//   mouseMove     position
	//   mouseDelta    delta
	//   scroll        delta
	//   gamepadButton gamepad?, button (a gamepad button name, "South", "Start", ...), state
	//   gamepadAxis   gamepad?, axis ("LeftX", "LeftY", "RightTrigger", ...), value
	//   text          text (UTF-8)
	// Members a type does not read must be absent (InvalidParams at /events/<i>/<member>); "state" defaults to "down" for
	// the types that read it. Presence is read from the request's params (MethodContext::GetParams), because a nested
	// struct's members have no HasParam.
	struct InputEventParams
	{
		uint32_t Tick = 0;                                      // the tick offset (see the file comment)
		PlayInputEventType Type = PlayInputEventType::KeyInput; // required; registry enum "InputEventType"
		std::string Name{};
		PlayInputEventState State = PlayInputEventState::Down; // registry enum "InputEventState"
		float Value = 0.0f;
		std::string KeyName{};    // key "key" (a member named Key would hide the KeyCodes.h type)
		std::string ButtonName{}; // key "button"
		uint32_t Gamepad = 0;     // 0 to MaxGamepads - 1
		std::string AxisName{};   // key "axis"
		glm::vec2 Position{ 0.0f };
		glm::vec2 Delta{ 0.0f };
		std::string Text{};
	};

	// input.inject {events, releaseAll?} (§13.5).
	struct InputInjectParams
	{
		std::vector<InputEventParams> Events{}; // required (may be empty with releaseAll)
		// Before the events: release every held key, button and injected action and zero every axis, at the session's next
		// tick (PlayInput::QueueReleaseAll).
		bool ReleaseAll = false;
	};

	struct InputInjectResult
	{
		uint32_t Tick = 0;   // the tick the offsets count from: the session's next tick
		uint32_t Queued = 0; // events queued (a tap counts once)
	};

	namespace Automation {

		// Converts the event at `/<pointer>` of the request's params (`event` is its parsed struct; `pointer` such as
		// "/events/2") to a PlayInputEvent, checking member presence against the type and the names against KeyCodes.h and
		// the session's actions (ValidatePlayInputEvent). Errors: InvalidParams located below `pointer`, with "did you
		// mean" hints for key, button, axis and action names.
		[[nodiscard]] Result<PlayInputEvent> MakePlayInputEvent(const AutomationMethodContext& context, const InputEventParams& event,
			std::string_view pointer, const InputActionMap& actions);

		// input.inject. Errors: InvalidState "not playing"; InvalidState naming the owner for another client of a lockstep
		// session; InvalidParams from MakePlayInputEvent (nothing queued then).
		[[nodiscard]] Result<InputInjectResult> InputInject(AutomationMethodContext& context, const InputInjectParams& params);

	}

	// Registers InputEventType, InputEventState, InputEvent, PlayInputSummary, InputInjectParams and InputInjectResult.
	void RegisterInputMethodTypes(TypeRegistry& registry);

	// Registers input.inject: a tool (§13.8 input_inject), AvailableInRuntime, no dry run (§13.4: input.* are Unsupported for
	// dryRun), not a batch op, not available in the launcher state, not Mutates.
	void RegisterInputMethods(MethodRegistry& methods);

}
