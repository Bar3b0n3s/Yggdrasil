#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <variant>

// Requests that take more than one frame (Architecture §13.2 "Pending operations"): play.step, play.waitFor,
// asset.import, project.export, test.run and input.replay return a PendingOperation, which the Dispatcher polls once per
// frame on the main thread until it resolves. In M4 only the test hook debug.pend uses one.

namespace Engine {

	class MethodContext;

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
	};

	// What a handler produces, type-erased (§13.2): an immediate result (the result struct's JSON), an error, or an operation
	// that resolves later. MethodRegistry builds it from the typed handlers.
	using MethodResult = std::variant<Json, Error, Scope<PendingOperation>>;

}
