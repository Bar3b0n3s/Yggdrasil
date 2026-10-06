#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// Session files (Architecture §13.2, §13.8): how clients find a running editor. A listening automation server writes
// <UserData>/<AppName>/Automation/Sessions/<pid>.json and deletes it on clean exit; the MCP bridge scans the directory for
// a live editor whose projectPath matches before it spawns one, and ignores files of dead processes.
//
// Format (camelCase keys in this order, canonical JsonWriter, LF):
//     {"pid": 1234, "port": 50123, "token": "<64 hex>", "protocolVersion": "1.0", "engineVersion": "0.1.0",
//      "projectPath": "<absolute path of the open .eproj, '/' separators; empty in the launcher state>",
//      "headless": true, "startedAt": "2026-10-06T12:34:56Z"}
// The file holds the token, so only the user may read it: the directory is created with 0700 and the file with 0600 on
// POSIX; on Windows the per-user %LOCALAPPDATA% ACL already restricts both.

namespace Engine {

	struct SessionFileContent
	{
		uint32_t Pid = 0;
		uint16_t Port = 0;
		std::string Token{};
		std::string ProtocolVersionText{}; // "protocolVersion": ProtocolVersion::ToString()
		std::string EngineVersionText{};   // "engineVersion"
		std::string ProjectPath{};         // UTF-8, '/' separators; empty without a project
		bool Headless = false;
		std::string StartedAt{}; // FormatUtcTimestamp
	};

	class SessionFile
	{
	public:
		SessionFile() = delete;

		// <directory>/<pid>.json
		[[nodiscard]] static std::filesystem::path GetPath(const std::filesystem::path& directory, uint32_t pid);

		// The canonical text of `content` (see the file comment).
		[[nodiscard]] static std::string ToText(const SessionFileContent& content);

		// Parses a session file. Errors: Parse for invalid JSON; Validation naming the first missing or mistyped member
		// (unknown members are ignored, so a newer engine's file stays readable).
		[[nodiscard]] static Result<SessionFileContent> FromText(std::string_view text);

		// Creates `directory` (0700 on POSIX) and writes the file of content.Pid atomically (0600 on POSIX, permissions set
		// before the rename makes it visible). Rewriting replaces it (the server rewrites it when a project opens or closes).
		// Errors: Io naming the path.
		[[nodiscard]] static Status Write(const std::filesystem::path& directory, const SessionFileContent& content);

		// Deletes the file of `pid`; a missing file is success. Errors: Io.
		[[nodiscard]] static Status Remove(const std::filesystem::path& directory, uint32_t pid);

		// "YYYY-MM-DDTHH:MM:SSZ" (UTC, seconds) for `time`, computed from the seconds since the epoch with the civil-date
		// algorithm, not with <chrono> calendar or time-zone support (missing from Apple's libc++).
		[[nodiscard]] static std::string FormatUtcTimestamp(std::chrono::system_clock::time_point time);
	};

}
