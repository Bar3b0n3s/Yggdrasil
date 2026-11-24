#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

// Requests that take more than one frame (Architecture §13.2 "Pending operations"): play.step, play.waitFor,
// asset.import, project.export, test.run and input.replay return a PendingOperation, which the Dispatcher polls once per
// frame on the main thread until it resolves. In M4 only the test hook debug.pend uses one.

namespace Engine {

	class MethodContext;

	// What a run reports it is doing (PendingProgress::Stage; Architecture §13.2 "Progress"). A recording's runs are
	// Record, Verify and, for one with input events, InputCheck, in that order (PendingProgress::Runs).
	enum class PendingProgressStage : uint8_t
	{
		Suite,     // test.run: a selected suite's cases run
		Replay,    // test.run: a committed replay plays; input.replay
		Record,    // test.run {record}: the recorded run, the suite whose input the replay keeps (run 1)
		Verify,    // the recording replayed with its input, strictly (run 2): test.run {record}, input.record stop
		InputCheck // the recording replayed without its input (run 3, only with input events): the same
	};

	// A long operation's report of its work (Architecture §13.2 "Progress"), which the Dispatcher sends its client as a
	// $/progress notification (MakeProgressNotification): when the handler returns the operation, after a Poll that reached
	// a new stage or step, and otherwise at most once per DispatcherSpecification::ProgressInterval. A client waits for the
	// call while Work grows, so the call is bounded by whether its work advances, not by a fixed deadline (the MCP bridge,
	// §13.8).
	struct PendingProgress
	{
		// Grows whenever the operation's work advances (a tick simulated, a session set up or torn down, a step of its
		// state machine) and never otherwise: a client that sees it stay the same for the method's timeoutSeconds ends
		// the call.
		uint64_t Work = 0;
		PendingProgressStage Stage = PendingProgressStage::Suite;
		// What the stage works on: "<suite>/<case>" while a case runs, else the suite's name, or the replay's path. Text from
		// scripts is sanitized by the operation (SanitizeTestReportText), so it always serializes.
		std::string Subject{};
		uint64_t Tick = 0;      // the tick the stage's session reached
		uint64_t FinalTick = 0; // the tick the stage ends at when known (a replay's final tick), else 0
		uint32_t Step = 0;      // which of Steps the stage belongs to (1-based: the selected suites, then the replays)
		uint32_t Steps = 0;     // 0 when the operation has no such count
		// A recording's runs once known: 3, or 2 for one without input events, which has no input check (Verify ends
		// it); 0 while not known (test.run's recorded run, whose input events are known only at its end) and for any
		// other stage.
		uint32_t Runs = 0;
	};

	// One running request. Created by a pending handler (MethodRegistry::AddPending) and owned by the Dispatcher until it
	// resolves or is cancelled. Main thread only.
	class PendingOperation
	{
	public:
		virtual ~PendingOperation() = default;

		// Advances the operation by one frame's worth of work. Returns nullopt while it is still running, otherwise its
		// outcome: the JSON of the method's registered result struct (MethodContext::SerializeResult) or an error. Called once
		// per AutomationServer::Pump (once per frame) with the request's own context, and never again after it returned a
		// value or after Cancel.
		[[nodiscard]] virtual std::optional<Result<Json>> Poll(MethodContext& context) = 0;

		// The requesting client disconnected, or the server stops (§13.2 "Disconnect"): the operation releases what it holds
		// (a lockstep it owns, a recording it started) and is destroyed without another Poll. It resolves internally as
		// Cancelled; no response is sent. The default does nothing.
		virtual void Cancel(MethodContext& /*context*/) {}

		// The phase marker text while it runs ("Play:step 120/600"); empty: "Automation:<method>".
		[[nodiscard]] virtual std::string GetPhase() const { return {}; }

		// What it reports in its $/progress notifications, read when the handler returns it and after each Poll that leaves it
		// running (§13.2 "Progress"; Dispatcher::Pump says when one is sent); nullopt, the default, sends none, and its client
		// bounds the call by the method's timeoutSeconds alone.
		[[nodiscard]] virtual std::optional<PendingProgress> GetProgress() const { return std::nullopt; }
	};

	// What a handler produces, type-erased (§13.2): an immediate result (the result struct's JSON), an error, or an operation
	// that resolves later. MethodRegistry builds it from the typed handlers.
	using MethodResult = std::variant<Json, Error, Scope<PendingOperation>>;

}
