#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string_view>

// The editor's side of the transcript (Architecture §13.8 "Transcript", §13.12). Together with
// Automation/Provenance.json (ProvenanceRecorder.h) it lets the demo audit (Scripts/AuditProvenance.py, M16) prove that
// every engine-consumed file of a game project was written by the editor through automation.
//
// Automation/BuildLog.jsonl (shared with Tools/MCP/engine_mcp/transcript.py; ADR 0008 decision 11): one JSON object per
// line, camelCase, appended, never rewritten:
//     {"type": "request", "time": "<UTC ISO 8601>", "client": "<name>", "id": <id>, "method": "<method>", "params": {...}}
//     {"type": "response", "time": "...", "client": "<name>", "id": <id>, "requestLine": <line of the request>,
//      "ok": true|false, "summary": "<one line>", "error"?: {"code": <int>, "errorCode": "<name>", "detail": "<text>"}}
// The bridge writes both lines for every MCP call; the editor appends them itself only for its CLI runs (--upgrade,
// BatchRunOptions::WriteTranscript).

namespace Engine {

	class VirtualFileSystem;

	// The editor's own transcript lines (format in the file comment), for CLI runs. Static functions only; main thread.
	class TranscriptLog
	{
	public:
		// Project-relative.
		static constexpr std::string_view FilePath = "Automation/BuildLog.jsonl";

		TranscriptLog() = delete;

		// Appends a request line to project://Automation/BuildLog.jsonl (creating it) and returns its 1-based line number.
		// The file is rewritten atomically with the line added, so a crash never leaves half a line. Errors: Validation when
		// the existing file does not end with a newline (a torn line); the VFS errors.
		[[nodiscard]] static Result<uint64_t> AppendRequest(VirtualFileSystem& vfs, std::string_view client, const Json& id,
			std::string_view method, const Json& params);

		// Appends the response line of the request at `requestLine`. `error` is null on success, else the response's error
		// object (code, data.errorCode, data.detail are copied). Errors: as AppendRequest.
		[[nodiscard]] static Status AppendResponse(VirtualFileSystem& vfs, std::string_view client, const Json& id, uint64_t requestLine,
			std::string_view summary, const Json& error);
	};

}
