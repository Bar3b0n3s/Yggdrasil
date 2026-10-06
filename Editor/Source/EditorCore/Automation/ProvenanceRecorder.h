#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// File provenance (Architecture §13.4 "Provenance"). The demo audit (Scripts/AuditProvenance.py, M16) proves from this
// file and the transcript (TranscriptLog.h), both committed, that every engine-consumed file of a game project was written
// by the editor through automation.
//
// Automation/Provenance.json, frozen by the M4 contract (ADR 0008 decision 10), canonical JsonWriter (tabs, LF), entries
// sorted by Path (byte-wise), one per path:
//     {
//         "Format": "Provenance", "Version": 1,
//         "Entries": [
//             { "Path": "Assets/Scenes/Main.scene", "XXH64": "<16 lowercase hex digits of the file's bytes>",
//               "Method": "scene.save", "RequestId": 17, "Client": "engine-mcp", "TranscriptLine": 42 }
//         ]
//     }
// "RequestId" is the JSON-RPC id as sent (an integer or a string), null for "ui" and "cli" writes; "TranscriptLine" is the
// request's 1-based line in Automation/BuildLog.jsonl, null when the request carried none. "Method" is the automation method,
// "ui" for writes by the windowed UI (the audit rejects those for demo projects) and "project.upgrade" for --upgrade
// (client "cli"). Dry runs and read-only editors never record.

namespace Engine {

	class VirtualFileSystem;

	// Who wrote a file: the request being served (set per request by the AutomationServer, EditorContext::SetWriteAttribution),
	// or the UI when no request is being served.
	struct WriteAttribution
	{
		std::string Method = "ui";
		VariantValue RequestId{};                 // the JSON-RPC id; null for "ui" and "cli"
		std::string Client = "ui";                // session.hello client.name, "batch", "cli", "test" or "ui"
		std::optional<uint64_t> TranscriptLine{}; // params._meta.transcriptLine
	};

	// One recorded file.
	struct ProvenanceEntry
	{
		std::string Path{}; // project-relative, '/' separators
		uint64_t Hash = 0;  // XXH64 (seed 0) of the bytes written
		std::string Method{};
		VariantValue RequestId{};
		std::string Client{};
		std::optional<uint64_t> TranscriptLine{};
	};

	// The in-memory provenance of one open project, saved after every recorded write. Owned by EditorContext while a writable
	// project is open. Main thread only. A value type.
	class ProvenanceRecorder
	{
	public:
		static constexpr std::string_view FormatName = "Provenance";
		static constexpr uint32_t CurrentVersion = 1;
		// Project-relative.
		static constexpr std::string_view FilePath = "Automation/Provenance.json";

		// Whether writes to `path` (project-relative) are recorded: a ".eproj" file at the project root and every file under
		// "Assets/" (§13.4). Library/, Automation/ and anything else are not.
		[[nodiscard]] static bool IsRecordedPath(std::string_view path);

		// Reads project://Automation/Provenance.json; a missing file gives an empty recorder. Errors: Parse (line and column)
		// or Validation (located) for a malformed file, UnsupportedVersion for a newer one; the VFS read errors.
		[[nodiscard]] static Result<ProvenanceRecorder> Load(const VirtualFileSystem& vfs);

		// The canonical document of `entries` (sorted by Path, asserted) and its inverse. FromText errors: Parse; Validation
		// for a wrong header, a mistyped member, an XXH64 that is not 16 hex digits, unsorted or duplicate paths.
		[[nodiscard]] static std::string ToText(std::span<const ProvenanceEntry> entries);
		[[nodiscard]] static Result<std::vector<ProvenanceEntry>> FromText(std::string_view text);

		// Records that `path` (asserted IsRecordedPath) now holds bytes with `hash`, written by `attribution`, replacing the
		// path's previous entry.
		void Record(std::string_view path, uint64_t hash, const WriteAttribution& attribution);

		// Writes ToText(GetEntries()) atomically to project://Automation/Provenance.json (creating Automation/). This write is
		// not itself recorded. Errors: the VFS write errors.
		[[nodiscard]] Status Save(VirtualFileSystem& vfs) const;

		// Every entry, sorted by Path.
		[[nodiscard]] std::span<const ProvenanceEntry> GetEntries() const { return m_Entries; }
		// The entry of `path`, or nullptr.
		[[nodiscard]] const ProvenanceEntry* Find(std::string_view path) const;
	private:
		std::vector<ProvenanceEntry> m_Entries;
	};

}
