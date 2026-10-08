#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/RingBufferSink.h"

#include <cstdint>
#include <string>
#include <vector>

// The observe domain's reads the Editor and the Runtime share (Architecture §13.5, §13.7): log.read and events.read
// (moved here from EditorCore with M7, unchanged in name and wire format, Docs/Decisions/0012-m7-decisions.md decision 12).
// stats.get (M9), physics.bodyInfo (M11) and audio.stats (M12) join them; docs.get is the editor's
// (EditorCore/Automation/ObserveMethods.h). Conventions as in MethodRegistry.h.

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Log and event cursors (convention 9 of MethodRegistry.h): the decimal 64-bit sequence number of the next entry to read,
	// as a string, so they never saturate. "" (the default) reads from the oldest held entry; "end" reads nothing and
	// returns the cursor of the next entry to be appended (the "now" cursor a caller takes before an action, then reads
	// what the action logged); "_meta".diagnostics.logCursor is a log cursor too. Anything else is InvalidArgument at
	// "/cursor".

	// log.read {cursor?, minLevel?, loggers?, contains?, limit?}: the log's ring buffer (§4.4) by cursor, so nothing is lost
	// between polls (RingBufferSink::Read).
	struct LogReadParams
	{
		std::string Cursor{};                // "", "end" or the nextCursor of the previous read
		LogLevel MinLevel = LogLevel::Trace; // registry enum "LogLevel"
		std::vector<LogChannel> Loggers{};   // registry enum "LogChannel"; empty: every logger
		std::string Contains{};              // case-sensitive substring of the message
		uint32_t Limit = 100;                // 1 to 1000
	};

	// Registry struct "LogEntrySummary".
	struct LogEntrySummary
	{
		uint32_t Seq = 0;
		LogLevel Level = LogLevel::Info;
		LogChannel Logger = LogChannel::Engine;
		std::string Message{};
		std::string File{};
		uint32_t Line = 0;
		int32_t Tick = -1;    // the simulation tick; -1 outside a session step
		std::string Entity{}; // 16 hex digits of the entity context; empty when none
		std::string ScriptFile{};
		uint32_t ScriptLine = 0;
	};

	// Named LogReadMethodResult, not LogReadResult (convention 1 of MethodRegistry.h): Core's RingBufferSink.h owns that name.
	struct LogReadMethodResult
	{
		std::vector<LogEntrySummary> Entries{};
		std::string NextCursor{}; // always a sequence number: where the next read continues
		uint32_t Dropped = 0;     // entries overwritten before this read
	};

	// events.read {cursor?, types?, limit?}: the host's engine event log (§4.9, AutomationMethodContext::GetEventLog) by
	// cursor (EventLog::Read).
	struct EventsReadParams
	{
		std::string Cursor{};                 // "", "end" or the nextCursor of the previous read
		std::vector<EngineEventType> Types{}; // registry enum "EngineEventType"; empty: every type
		uint32_t Limit = 100;                 // 1 to 1000
	};

	// Registry struct "EngineEventSummary" (the fields per type are those of EngineEvent).
	struct EngineEventSummary
	{
		uint32_t Seq = 0;
		EngineEventType Type = EngineEventType::EntityCreated;
		int32_t Tick = -1;
		std::string Id{}; // 16 hex digits; empty for the invalid UUID
		std::string Path{};
		std::string Name{};
		std::string Message{};
		bool Dirty = false;
	};

	struct EventsReadResult
	{
		std::vector<EngineEventSummary> Events{};
		std::string NextCursor{}; // always a sequence number: where the next read continues
		uint32_t Dropped = 0;
	};

	namespace Automation {

		// log.read. Errors: InvalidArgument at /cursor for a malformed cursor.
		[[nodiscard]] Result<LogReadMethodResult> LogRead(AutomationMethodContext& context, const LogReadParams& params);
		// events.read. Errors: InvalidArgument at /cursor for a malformed cursor.
		[[nodiscard]] Result<EventsReadResult> EventsRead(AutomationMethodContext& context, const EventsReadParams& params);

	}

	// Registers the structs above.
	void RegisterObserveMethodTypes(TypeRegistry& registry);

	// Registers log.read and events.read: read-only, AllowedInBatch and available in the Runtime; log.read is a tool (§13.8).
	void RegisterObserveMethods(MethodRegistry& methods);

}
