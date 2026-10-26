#pragma once

#include "Engine/Core/Json/Json.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	enum class ScriptErrorKind : uint8_t
	{
		Compile,
		Type,
		Runtime,
		Timeout,
		Memory
	};

	struct ScriptTraceFrame
	{
		std::string Script{};
		uint32_t Line = 0;
		std::string Function{};
	};

	// Owned diagnostic data: no VM strings or entity pointers survive a protected call or a scene restart (§11.7).
	struct ScriptError
	{
		uint64_t ID = 0;
		ScriptErrorKind Kind = ScriptErrorKind::Runtime;
		std::string Script{};
		uint32_t Line = 0;
		uint32_t Column = 0;
		std::string Message{};
		std::string Callback{};
		UUID Entity{};
		std::string EntityName{};
		// M20 (S-MULTI; Docs/Decisions/0031-m20-contract.md decision 17): the faulting instance's slot in its entity's Script
		// list (for an evaluation, the slot it bound self to). ScriptErrorToJson writes it as "slot" after "entity" for an
		// error that has an entity. Not part of the deduplication key: a repeat from another slot updates the entry.
		uint32_t Slot = 0;
		uint64_t Tick = 0;
		uint64_t Count = 1;
		std::vector<ScriptTraceFrame> Traceback{};
		std::string JsonPointer{}; // embedded source, e.g. /Expect/0/Luau; Line/Column refer to its decoded Luau bytes
	};

	// Lowercase protocol spelling and the §11.7 JSON representation. Pure; ToJson never consults a live scene.
	[[nodiscard]] std::string_view ScriptErrorKindToString(ScriptErrorKind kind);
	[[nodiscard]] Json ScriptErrorToJson(const ScriptError& error);

	// Applies the bounds of a retained script error (ScriptLimits.h): a Message longer than MaxScriptLogMessageBytes is cut at
	// a UTF-8 sequence boundary and ends with " [truncated: <n> bytes]" (n its full byte count; the marker counts toward the
	// cap), and a Traceback longer than MaxScriptErrorTracebackFrames keeps its innermost frames and ends with the
	// "[truncated: <n> more frames]" frame. Pure and idempotent: a bounded error is left as it is.
	void BoundScriptError(ScriptError& error);

	// Main-thread error stream of a VM, a session or a host's history. Each Add bounds the error (BoundScriptError) and gives
	// it a new monotonically increasing ID, including repeats. Deduplicates by (Script, JsonPointer, Line, Message): replaces
	// that entry's location/context with its latest occurrence and increments Count. Retains at most MaxScriptErrorEntries
	// distinct errors: adding another evicts the one whose latest occurrence is the oldest, and an evicted error that occurs
	// again starts a new count. Read returns retained entries with ID > since in ascending ID order, so a repeated error is
	// observable after a previous cursor; HasEvictedAfter(since) tells whether an error whose latest occurrence is newer than
	// since was evicted, which no Read can return any more. GetErrors has the same order; references/spans expire on the
	// next Add.
	class ScriptErrorStream
	{
	public:
		[[nodiscard]] const ScriptError& Add(ScriptError error);
		[[nodiscard]] std::vector<ScriptError> Read(uint64_t since = 0, uint32_t limit = 100) const;
		// The ID of the latest occurrence, 0 before the first; evictions never lower it.
		[[nodiscard]] uint64_t GetCursor() const;
		[[nodiscard]] std::span<const ScriptError> GetErrors() const;
		// Distinct errors evicted so far.
		[[nodiscard]] uint64_t GetEvictedCount() const;
		[[nodiscard]] bool HasEvictedAfter(uint64_t since) const;
	private:
		std::vector<ScriptError> m_Errors{};
		uint64_t m_Cursor = 0;
		uint64_t m_EvictedCount = 0;
		uint64_t m_EvictedThrough = 0; // the latest occurrence ID of the error evicted last; evictions go oldest first
	};

}
