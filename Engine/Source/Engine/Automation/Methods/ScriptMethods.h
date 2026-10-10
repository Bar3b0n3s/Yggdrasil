#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"
#include "Engine/Scripting/Sandbox.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	enum class ScriptEvaluationContext : uint8_t
	{
		Edit,
		Play
	};

	struct ScriptTraceFrameSummary
	{
		std::string Script{};
		uint32_t Line = 0;
		std::string Function{};
	};

	// The detached §11.7 error schema. Counters saturate with ToAutomationCounter like other automation summaries;
	// pagination uses the full 64-bit sequence in nextCursor and never uses the displayed id as a cursor. Kind is one
	// of the mandated lowercase output spellings compile, type, runtime, timeout, memory; it is not user input.
	struct ScriptErrorSummary
	{
		uint32_t Id = 0;
		std::string Kind{};
		std::string Script{};
		uint32_t Line = 0;
		uint32_t Column = 0;
		std::string Message{};
		std::string Callback{};
		EntitySummary Entity{};
		uint32_t Tick = 0;
		uint32_t Count = 0;
		std::vector<ScriptTraceFrameSummary> Traceback{};
		std::string JsonPointer{}; // Preserves ScriptError::JsonPointer, e.g. /Expect/0/Luau; empty for standalone scripts.
	};

	struct ScriptErrorsParams
	{
		// Exclusive sequence cursor: empty starts at the beginning, "end" takes the current cursor without entries.
		// Other values are unsigned decimal 64-bit sequences returned in nextCursor; malformed input is located at /since.
		std::string Since{};
		uint32_t Limit = 100; // 1..1000
	};

	struct ScriptErrorsResult
	{
		std::vector<ScriptErrorSummary> Errors{};
		std::string NextCursor{}; // Always a decimal cursor, including when no entries remain; reuse as since.
	};

	struct ScriptEvalParams
	{
		std::string Code{};                                              // Required UTF-8 Luau chunk; the first returned value is reported.
		ScriptEvaluationContext Context = ScriptEvaluationContext::Edit; // Required, case-insensitive registry enum.
		std::string Entity{};                                            // Optional EntityRef; binds self to the selected entity's behaviour instance.
	};

	// Reuses the detached VM result exactly; its Value is VariantValue and Prints retains call order. Registered once
	// by RegisterSharedScriptMethodTypes as ScriptEvalResult, without introducing a second conversion/schema.
	using ScriptEvalResult = ScriptEvaluation;

	namespace Automation {

		// Main-thread reads over the host's script error stream, including retained editor errors outside Play.
		// Duplicate occurrences remain observable by cursor with their updated count. Does not clear or consume errors.
		// Errors: InvalidArgument at /since; Unsupported if the host has no script-error service.
		[[nodiscard]] Result<ScriptErrorsResult> ScriptErrors(AutomationMethodContext& context, const ScriptErrorsParams& params);

		// Main-thread sandbox evaluation. Edit uses a separate read-only VM: host writes, deferred work, scene loads,
		// audio, input, shared randomness and application controls are rejected before side effects; local values may
		// change. Edit self exposes resolved fields without running lifecycle callbacks. Play uses the session VM and
		// checks the requesting lockstep owner before executing anything. Entity
		// resolves in the requested scene; NotFound at /entity for an absent entity/instance. Runtime supports Play only.
		// Errors: InvalidState without the requested scene/VM, for Simulate, or another owner's lockstep; Unsupported at
		// /context for Runtime Edit; Script with a structured §11.7 error in context.SetErrorData on evaluation failure.
		// A successful mutating Play eval marks recording/replay state modified through the session's normal write path.
		[[nodiscard]] Result<ScriptEvalResult> ScriptEval(AutomationMethodContext& context, const ScriptEvalParams& params);

	}

	// Explicit registration only; no global/static registration. Shared functions deliberately include Shared in their
	// names to coexist with EditorCore/Automation/ScriptMethods.h in one translation unit.
	void RegisterSharedScriptMethodTypes(TypeRegistry& registry);
	// Both are tools, available in Runtime, never launcher methods and never support dry run. errors is a batch read;
	// eval is not allowed in a batch, since Play effects cannot be rolled back. Neither edits project content (Mutates=false).
	void RegisterSharedScriptMethods(MethodRegistry& methods);

}
