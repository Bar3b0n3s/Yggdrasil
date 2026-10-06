#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The structured in-memory log (Architecture §4.4): the last N entries of every logger, read by monotonic sequence
// number by the editor console, automation log.read, the _meta diagnostics delta, crash reports and Test::ExpectLog.
// This header is spdlog-free on purpose (Architecture §3 rule 3: spdlog appears only in Log.h among public headers);
// Log.cpp adapts spdlog's sink interface onto RingBufferSink::Append.

namespace Engine {

	enum class LogLevel : uint8_t
	{
		Trace,
		Info,
		Warn,
		Error,
		Critical
	};

	// The three loggers (Architecture §4.4): Engine (ENGINE_CORE_* macros), App (ENGINE_* macros, Editor and Runtime) and
	// Script (Luau Log.* and print).
	enum class LogChannel : uint8_t
	{
		Engine,
		App,
		Script
	};

	// "Trace", "Info", "Warn", "Error", "Critical".
	[[nodiscard]] std::string_view LogLevelToString(LogLevel level);
	// Case-insensitive inverse of LogLevelToString (automation parses enums case-insensitively, §6); nullopt otherwise.
	[[nodiscard]] std::optional<LogLevel> LogLevelFromString(std::string_view text);
	// "Engine", "App", "Script".
	[[nodiscard]] std::string_view LogChannelToString(LogChannel channel);
	// Case-insensitive inverse of LogChannelToString; nullopt otherwise.
	[[nodiscard]] std::optional<LogChannel> LogChannelFromString(std::string_view text);

	// One structured log entry (Architecture §4.4). The context fields come from the logging thread's LogContextScope
	// (LogContext.h) at the time of the call.
	struct LogEntry
	{
		uint64_t Seq = 0;             // assigned by RingBufferSink::Append: 1, 2, 3 ... per sink, never reused
		uint64_t TimeNs = 0;          // steady-clock nanoseconds since Log::Initialize (diagnostic only, never simulated)
		std::optional<uint64_t> Tick; // the simulation tick, when the logging code ran inside a session step
		LogLevel Level = LogLevel::Info;
		LogChannel Logger = LogChannel::Engine;
		std::string Message;
		std::string File; // source file of the log statement (__FILE__); empty when unknown
		uint32_t Line = 0;
		UUID EntityId;          // invalid when there is no entity context
		std::string ScriptFile; // empty when there is no script context
		uint32_t ScriptLine = 0;
	};

	// A read request. Entries with Seq >= Cursor are scanned in Seq order.
	struct LogQuery
	{
		uint64_t Cursor = 0;                     // first Seq of interest; 0 = the oldest entry still held
		LogLevel MinimumLevel = LogLevel::Trace; // entries below this level are skipped
		std::vector<LogChannel> Channels;        // empty = every channel
		std::string Contains;                    // case-sensitive substring of Message; empty = any
		size_t Limit = 100;                      // maximum number of entries returned (automation default; max 1,000)
	};

	struct LogReadResult
	{
		std::vector<LogEntry> Entries; // matching entries, ascending Seq
		// The cursor to pass next time: one past the last scanned entry, so that entries skipped by the filters are not
		// scanned again. When Limit stopped the scan it is one past the last returned entry.
		uint64_t NextCursor = 0;
		// Entries with Seq >= the requested cursor that were overwritten before this read (the reader fell behind).
		uint64_t DroppedCount = 0;
	};

	// A fixed-capacity ring of LogEntry. When full, appending overwrites the oldest entry. Thread-safe: every member may
	// be called concurrently from any thread.
	class RingBufferSink
	{
	public:
		static constexpr size_t DefaultCapacity = 16384;

		// `capacity` must be at least 1 (asserted).
		explicit RingBufferSink(size_t capacity = DefaultCapacity);

		RingBufferSink(const RingBufferSink&) = delete;
		RingBufferSink& operator=(const RingBufferSink&) = delete;

		// Stores `entry`, overwriting its Seq with the next sequence number, and returns that number.
		uint64_t Append(LogEntry entry);

		// The entries matching `query`, see LogQuery and LogReadResult.
		[[nodiscard]] LogReadResult Read(const LogQuery& query) const;

		// The last `count` entries (fewer when fewer are held), ascending Seq. Used by crash reports and the console.
		[[nodiscard]] std::vector<LogEntry> ReadLast(size_t count) const;

		// The Seq the next appended entry will receive; a cursor that skips everything logged so far.
		[[nodiscard]] uint64_t GetNextSeq() const;

		[[nodiscard]] size_t GetCapacity() const;
		// The number of entries currently held (at most the capacity).
		[[nodiscard]] size_t GetSize() const;
	private:
		size_t m_Capacity = 0;
		uint64_t m_NextSeq = 1;
	};

}
