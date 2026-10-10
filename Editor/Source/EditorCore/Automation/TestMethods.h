#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Testing/FeatureTestRunner.h"
#include "Engine/Testing/TestResults.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class PendingOperation;
	class TypeRegistry;

	// The runner's inventory already has the exact payload: suites in settings order, cases in declaration order
	// with locations, and unique sorted replay paths. Register this type once as TestListResult.
	using TestListResult = TestInventory;

	struct TestRunParams
	{
		std::string Filter{};      // Case-insensitive substring of suite/case; replays match their path.
		uint32_t TimeoutTicks = 0; // Omitted: no additional run limit; when present require 1..UINT32_MAX.
		std::string Record{};      // Optional project://Assets/... .replay destination; exactly one selected suite required.
	};

	// The RPC returns the runner's exact schema (§11.10), without nesting it in a second result envelope. The alias
	// follows the method-name collision convention; reflection continues to identify this one type as TestRunResult.
	using TestRunMethodResult = TestRunResult;

	inline constexpr std::chrono::milliseconds TestRunFrameBudget{ 50 };

	namespace Automation {

		// Collects suites in scratch sessions without executing case bodies; restores editor selection/scene/play state.
		// InvalidState while another owner controls time or a run is active. Type errors block discovery with structured
		// diagnostics, not a partial inventory. Missing scenes/scripts/replays retain located NotFound/Validation errors.
		[[nodiscard]] Result<TestListResult> TestList(EditorMethodContext& context, const NoParams& params);

		// Pending, main-thread adapter over FeatureTestRunner. Preflight validates scripts, all requested paths, filter
		// selection and recording destination before acquiring the exclusive run lease or replacing any session.
		// Each Poll advances at least one bounded runner unit and then stops when the 50 ms wall budget is consumed;
		// simulated clocks, tick deadlines and outcomes never depend on this partition. OwnsAudioTime is set on every
		// suite session, including ScriptedClock suites. The regular editor frame loop must not also advance it.
		// Cases failing/timing out are successful RPC reports with Passed=false and locations, not transport failures.
		// Writes JSON/JUnit outputs and the optional replay through the host's output/project writer with attribution.
		// Cancel/disconnect unpublishes/destroys the runner sessions, cancels case/tasks/captures, releases audio/time
		// ownership and restores the prior editor state without another Poll; it never publishes an incomplete replay.
		// Errors: InvalidArgument at /timeoutTicks or /record; Validation for script/check/selection failures;
		// InvalidState for a conflicting run/time owner; PermissionDenied for recording in a read-only project; I/O errors.
		[[nodiscard]] Result<Scope<PendingOperation>> TestRun(EditorMethodContext& context, const TestRunParams& params);

	}

	// Calls RegisterTestResultTypes exactly once, plus the discovery types and TestRunParams; no duplicate run schema.
	void RegisterTestMethodTypes(TypeRegistry& registry);
	// Editor only, no launcher, no dry run, no batch, no Mutates flag (record's optional write checks permissions itself).
	// test.list is immediate and not a tool; test.run is pending, exposed as test_run, TimeoutSeconds=900.
	void RegisterTestMethods(MethodRegistry& methods);

}
