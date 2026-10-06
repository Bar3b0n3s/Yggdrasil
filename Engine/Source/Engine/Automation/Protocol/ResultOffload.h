#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Bounded output (Architecture §1.3 principle 7, §13.4): any result whose JSON is over 48 KB is written to a file and the
// response carries {path, truncated: true, summary} instead, so a large scene.get never floods an agent's context. The
// Dispatcher decides and builds the replacement with these pure helpers; the host writes the file
// (IMethodHost::WriteOffloadedResult) under <Project>/Library/Automation/Out/, or under user://Automation/Out/ in the
// launcher state and for read-only editors, which write nothing under the project.

namespace Engine {

	// §13.4: "any result over 48 KB". Measured on the minified JSON of the result, "_meta" excluded.
	inline constexpr size_t DefaultOffloadThresholdBytes = 48 * 1024;

	// The project-relative directory of offloaded results (Library/ is gitignored, §2.1).
	inline constexpr std::string_view OffloadDirectory = "Library/Automation/Out";

	// The part of offload file names that tells servers apart: "<processId>-<startSeconds>", where `startSeconds` is the
	// server's start time in seconds since the Unix epoch ("4242-1791244800"). Several editors share
	// user://Automation/Out/, and a restarted editor reuses its project's Library/Automation/Out/, so neither overwrites
	// another's files (a process id is reused only after that process ended, and never within the same second).
	[[nodiscard]] std::string MakeOffloadServerTag(uint32_t processId, int64_t startSeconds);

	// "<serverTag>-<sequence>.json" with the sequence written as at least 8 decimal digits ("4242-1791244800-00000042.json"):
	// the Dispatcher numbers offloaded results per server from 1, so one server's files sort in order and names never
	// depend on client-chosen request ids. `serverTag` comes from MakeOffloadServerTag (non-empty, asserted).
	[[nodiscard]] std::string MakeOffloadFileName(std::string_view serverTag, uint64_t sequence);

	// A small description of a large result: for an object, each member's name with {"type": <JSON type name>, "count": N}
	// for arrays and objects (their element or member count) or {"type": "string", "length": N} for strings, and the value
	// itself for numbers, booleans and null; for any other result, its type and count. Members are listed in the result's
	// order and the summary itself stays below 4 KB (later members are dropped and "omittedMembers": N added).
	[[nodiscard]] Json MakeOffloadSummary(const Json& result);

	// The replacement result: {"path": <path>, "truncated": true, "summary": <summary>}.
	[[nodiscard]] Json MakeOffloadedResult(std::string_view path, const Json& summary);

}
