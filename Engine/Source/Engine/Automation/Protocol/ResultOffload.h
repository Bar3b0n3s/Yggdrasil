#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Bounded output (Architecture §1.3 principle 7, §13.4): any result whose JSON is over 48 KB is written to a file and the
// response carries {path, truncated: true, summary} instead, so a large scene.get never floods an agent's context. The
// Dispatcher decides and builds the replacement with these pure helpers; the host names and writes the file
// (IMethodHost::WriteOffloadedResult) under <Project>/Library/Automation/Out/, or under user://Automation/Out/ in the
// launcher state and for read-only editors, which write nothing under the project.
//
// The files of a directory of offloaded results (Docs/Decisions/0026-m16-contract.md, friction S1-03):
//   - every file a server writes there, offloaded results and method output files (screenshots, test reports) alike, is
//     named "<serverTag>-<sequence>.<extension>" from one per-server sequence that starts at 1, so no two of a server's
//     files ever share a name;
//   - a file is created exclusively, never replaced (VirtualFileSystem::WriteNewFile): a name that exists already, also
//     one another process created a moment before, is an error (AlreadyExists), so nothing is overwritten and no
//     "<name>.bak" backup is ever made there;
//   - the first write of a server to a directory removes every file of a server whose process no longer runs
//     (ParseOffloadFileProcessId: any extension, and the ".bak" and ".tmp-*" leftovers of older or interrupted writes);
//   - a server that shuts down removes exactly the offloaded results it wrote and keeps its output files, so a --batch
//     run's screenshots and a test run's reports can be read after it exits.

namespace Engine {

	// §13.4: "any result over 48 KB". Measured on the minified JSON of the result, "_meta" excluded.
	inline constexpr size_t DefaultOffloadThresholdBytes = 48 * 1024;

	// The project-relative directory of offloaded results (Library/ is gitignored, §2.1).
	inline constexpr std::string_view OffloadDirectory = "Library/Automation/Out";

	// The extension of offloaded results (MakeOffloadFileName).
	inline constexpr std::string_view OffloadExtension = "json";

	// The largest summary member that MakeOffloadSummary repeats verbatim, measured on its minified JSON.
	inline constexpr size_t MaxOffloadSummaryVerbatimBytes = 512;

	// The most bytes of leading strings MakeOffloadSummary keeps of a longer array of strings ("first"), measured on their
	// minified JSON array: test.run's failedCases (at most 20 "<suite>/<case>" names) keeps its leading names when it is
	// too long to repeat verbatim (M16 review R26).
	inline constexpr size_t MaxOffloadSummaryListBytes = 1536;

	// The part of offload file names that tells servers apart: "<processId>-<startSeconds>", where `startSeconds` is the
	// server's start time in seconds since the Unix epoch ("4242-1791244800"). Several editors share
	// user://Automation/Out/, and a restarted editor reuses its project's Library/Automation/Out/, so neither overwrites
	// another's files (a process id is reused only after that process ended, and never within the same second).
	[[nodiscard]] std::string MakeOffloadServerTag(uint32_t processId, int64_t startSeconds);

	// "<serverTag>-<sequence>.<extension>" with the sequence written as at least 8 decimal digits
	// ("4242-1791244800-00000042.json"): the host numbers every file it writes to its offload directory from one sequence
	// (see the file comment), so one server's files sort in the order they were written and names never depend on
	// client-chosen request ids. `serverTag` comes from MakeOffloadServerTag (non-empty, asserted); `extension` is
	// lowercase letters and digits without the dot (non-empty, asserted).
	[[nodiscard]] std::string MakeOffloadFileName(std::string_view serverTag, uint64_t sequence, std::string_view extension = OffloadExtension);

	// The process id of a file a server wrote to an offload directory: a name of the form
	// "<processId>-<startSeconds>-<sequence>.<extension>" (MakeOffloadFileName; decimal numbers, an extension of
	// lowercase letters and digits), optionally followed by ".bak" (the backup an older server's overwrite left) or by
	// ".tmp-<suffix>" (the temporary file of an interrupted atomic write). Nullopt for any other name, which pruning
	// leaves alone.
	[[nodiscard]] std::optional<uint32_t> ParseOffloadFileProcessId(std::string_view fileName);

	// A small description of a large result: for an object, each member's name with the member itself when its minified
	// JSON is at most MaxOffloadSummaryVerbatimBytes (numbers, booleans, null, short strings such as paths and codes, small
	// objects and arrays such as counts and a few names), and otherwise {"type": <JSON type name>, "count": N} for arrays
	// and objects (their element or member count) or {"type": "string", "length": N} for strings; a longer array whose
	// elements are all strings adds "first", its leading strings within MaxOffloadSummaryListBytes ({"type": "array",
	// "count": 12, "first": ["Pilot/drive", ...]}), unless they do not fit the summary's remaining room; for any other
	// result, its type and count. Members are listed in the result's order and the summary itself stays below 4 KB (later
	// members are dropped and "omittedMembers": N added).
	[[nodiscard]] Json MakeOffloadSummary(const Json& result);

	// The replacement result: {"path": <path>, "truncated": true, "summary": <summary>}.
	[[nodiscard]] Json MakeOffloadedResult(std::string_view path, const Json& summary);

}
