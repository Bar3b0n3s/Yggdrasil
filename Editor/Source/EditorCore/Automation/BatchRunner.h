#pragma once

#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Sequential in-process request runs (Architecture §13.9, §6.7): `Editor --headless --batch f.jsonl` replays RPC calls and
// exits non-zero at the first error, and `Editor --headless --project P --upgrade` runs project.upgrade with its own
// transcript lines (§13.12, client "cli").

namespace Engine {

	class AutomationServer;

	// One request of a run.
	struct BatchRequest
	{
		std::string Method{};
		VariantValue Params{}; // an object or null; may hold {"$ref": "<line>.<path>"} for batch files (indexBase 1)
		uint32_t Line = 0;     // the 1-based line of a batch file; 0 for requests built in code
	};

	struct BatchRunOptions
	{
		std::string ClientName = "batch";
		// Append request and response lines to project://Automation/BuildLog.jsonl (TranscriptLog) and pass each request's
		// line as its transcript line, so provenance names it (--upgrade, §13.12). Needs an open project when a request runs.
		bool WriteTranscript = false;
	};

	// Runs requests one at a time through an in-process client of an AutomationServer, one per frame, waiting for each
	// response (pending operations included) before submitting the next, and stops at the first error. Main thread.
	//
	// Per frame, after AutomationServer::Pump: Advance collects the response of the request in flight (stopping on an error
	// response), substitutes "$ref" values of the next request from the earlier results (JsonReference.h; a failure stops
	// the run), and submits it. EditorApp calls Advance from its safe point until IsFinished and then exits with
	// ExitCode::Success or ExitCode::Failed (§6.7 "stops at the first error with exit code 1"). Its in-process client keeps
	// every result inline (AutomationServer::ConnectInProcess with offloading off), so a "$ref" into a large result (a big
	// scene.query, an edit.batch of hundreds of ops) finds the data instead of an offload summary.
	class BatchRunner
	{
	public:
		// The requests of a batch file (§6.7): one {"method": ..., "params": ...} object per line, UTF-8, LF or CRLF; a line
		// that is empty or only whitespace is an error, as is any other member. Errors: NotFound or Io for the file; Parse
		// (with the line) for invalid JSON; Validation (with the line) for a line of the wrong shape.
		[[nodiscard]] static Result<std::vector<BatchRequest>> LoadFile(const std::filesystem::path& file);

		BatchRunner(std::vector<BatchRequest> requests, BatchRunOptions options);
		~BatchRunner();

		BatchRunner(const BatchRunner&) = delete;
		BatchRunner& operator=(const BatchRunner&) = delete;

		// One frame of the run (see the class comment). Connects its in-process client on the first call. No effect once
		// finished.
		void Advance(AutomationServer& server);

		[[nodiscard]] bool IsFinished() const;
		// The error that stopped the run: the failing request's error (with its line as context) or a substitution error;
		// nullopt while running and after success.
		[[nodiscard]] const std::optional<Error>& GetFailure() const { return m_Failure; }
		// The failing response (the JSON-RPC error message), or null.
		[[nodiscard]] const Json& GetFailedResponse() const { return m_FailedResponse; }
		// The results of the requests completed so far, in order.
		[[nodiscard]] const std::vector<Json>& GetResults() const { return m_Results; }
	private:
		// The in-process client, the index of the next request, the request in flight and its transcript line
		// (BatchRunner.cpp).
		struct State;
	private:
		std::vector<BatchRequest> m_Requests;
		BatchRunOptions m_Options;
		std::optional<Error> m_Failure;
		Json m_FailedResponse;
		std::vector<Json> m_Results;
		Scope<State> m_State;
	};

}
