#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class PendingOperation;
	class TypeRegistry;

	enum class InputRecordAction : uint8_t
	{
		Start,
		Stop
	};

	struct InputRecordExpectation
	{
		uint32_t Tick = 0;
		std::string Luau{};
	};

	// action required. Start accepts scene/parameters/seed/restart; Stop requires path and accepts expect. Members of
	// the other action are rejected by presence, not silently ignored. Start while playing requires restart:true.
	struct InputRecordParams
	{
		InputRecordAction Action = InputRecordAction::Start;
		bool Restart = false;
		std::string Scene{};
		VariantValue Parameters{};
		uint32_t Seed = 0; // absent inherits the session/project seed
		std::string Path{};
		std::vector<InputRecordExpectation> Expect{};
	};

	struct InputRecordResult
	{
		bool Recording = false;
		uint32_t Tick = 0;
		uint32_t Events = 0;
		std::string Path{};
		std::string StateHash{};
	};

	struct InputReplayParams
	{
		std::string Path{}; // required, source replay path or cooked replay asset resolved by the host
		bool Verify = false;
		bool StrictHash = false; // requires verify:true
	};

	struct InputReplayResult
	{
		bool Passed = false;
		bool Verified = false;
		bool HashChecked = false;
		bool HashMatched = false;
		uint32_t FinalTick = 0; // ToAutomationCounter, as play.state; persistent replay ticks remain uint64_t
		std::string StateHash{};
		std::string ExpectedStateHash{};
		struct Expectation
		{
			uint32_t Tick = 0;
			bool Evaluated = false;
			bool Satisfied = false;
			VariantValue Value{};
			std::string Message{};
			std::string File{};
			uint32_t Line = 0;
			std::string JsonPointer{};
		};
		std::vector<Expectation> Expect{};
	};

	namespace Automation {

		// Recording owns lockstep from tick zero and observes every applied input, including test/device events.
		// Validates the complete request before replacing a session. A mutation/reload invalidates the recording;
		// Stop then returns its reason and writes nothing. Another lockstep owner's call is InvalidState. Writes use
		// the host's confined replay-output path and normal provenance, with no dry-run/batch promise. Stop validates
		// write authorization before consuming the recording. The host cancels the immediate Start's recorder on client
		// disconnect, without needing a pending Poll, and writes no partial/invalid recording.
		[[nodiscard]] Result<InputRecordResult> InputRecord(AutomationMethodContext& context, const InputRecordParams& params);
		// A fresh session from the validated header. Pending: at least one tick per Poll within the common 50 ms
		// budget, never an unbounded synchronous replay. Reject another time owner or active driver before replacing
		// the session. Completion, failure and cancellation pause the owned session and release this operation's input
		// suppression, stepping, audio/time and temporary lockstep ownership without another Poll. Check session serial
		// before cleanup, so a replacement session is untouched. Verification uses cooked Expect bytecode; Dist never
		// compiles a predicate. Errors identify replay/expectation/tick. A strict hash or expectation failure returns
		// Validation with the complete outcome in error data, preserving all locations and hash status.
		[[nodiscard]] Result<Scope<PendingOperation>> InputReplay(AutomationMethodContext& context, const InputReplayParams& params);

	}

	void RegisterReplayMethodTypes(TypeRegistry& registry);
	// Both are tools and available in Runtime, require an open project/session, and are neither batch nor dry-run
	// operations. Neither is Mutates (only transient state unless Stop writes); Stop explicitly checks write policy,
	// as test.run does for its optional recording. The pending replay timeout is 600 seconds.
	void RegisterReplayMethods(MethodRegistry& methods);

}
