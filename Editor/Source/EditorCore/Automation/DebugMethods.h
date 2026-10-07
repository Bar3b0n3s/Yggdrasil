#pragma once

#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>

// Test hooks (Roadmap M4): registered only with --automation-test-hooks (MethodSpecification::TestHook), never tools,
// never in the catalogue, excluded from method coverage. They let the Python suite reach states it otherwise cannot:
// a frozen main thread (the watchdog's Busy answer) and a pending operation (cancellation on disconnect).

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// debug.stall {ms}: blocks the main thread for `ms` milliseconds inside the handler, with the phase marker
	// "Debug:stall <ms> ms", so another client's requests meet the watchdog (test_busy_watchdog_reports_phase).
	struct DebugStallParams
	{
		uint32_t Ms = 0; // 0 to 60000
	};

	struct DebugStallResult
	{
		uint32_t StalledMs = 0;
	};

	// debug.pend {frames?}: a pending operation that resolves after `frames` polls (frames, since it is polled once per
	// frame), or never when 0, until its client disconnects (test_disconnect_cancels_pending_operations). The handler logs
	// "Started debug.pend of client '<name>'" at Info when the operation becomes pending, and its Cancel logs "Cancelled
	// debug.pend of client '<name>'" at Info. The test waits for the first line through log.read from another client before
	// it disconnects (requests of different clients have no order), then waits for the second.
	struct DebugPendParams
	{
		uint32_t Frames = 0;
	};

	struct DebugPendResult
	{
		uint32_t Frames = 0; // the polls it took
	};

	namespace Automation {

		[[nodiscard]] Result<DebugStallResult> DebugStall(EditorMethodContext& context, const DebugStallParams& params);
		[[nodiscard]] Result<Scope<PendingOperation>> DebugPend(EditorMethodContext& context, const DebugPendParams& params);

	}

	// The structs are always registered (the registry is built before the command line is known to the methods).
	void RegisterDebugMethodTypes(TypeRegistry& registry);

	// Registers debug.stall and debug.pend (TestHook; available in the launcher state, so the hooks work before a project
	// opens; not AllowedInBatch). Called by RegisterEditorMethods only with EditorMethodOptions::TestHooks.
	void RegisterDebugMethods(MethodRegistry& methods);

}
