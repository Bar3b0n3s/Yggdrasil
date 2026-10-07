#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/RingBufferSink.h"

#include <cstdint>
#include <string>
#include <vector>

// The observe domain's M4 subset (Architecture §13.5, §13.7): log.read, events.read and docs.get. stats.get (M9),
// physics.bodyInfo (M11) and audio.stats (M12) follow. Conventions as in MethodRegistry.h.

namespace Engine {

	class EditorMethodContext;
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

	// events.read {cursor?, types?, limit?}: the engine event log (§4.9) by cursor (EventLog::Read).
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

	// docs.get {topic?} (§13.10): without a topic, the list of topics; with one, its Markdown. Topics are
	// "skills/<name>" (.claude/skills/<name>/SKILL.md) and "reference/<name>" (Docs/Reference/<name>.md, generated from
	// M14), read from the repository root the server was given (AutomationServerSpecification::DocsRoot).
	struct DocsGetParams
	{
		std::string Topic{};
	};

	// Registry struct "DocsTopic".
	struct DocsTopic
	{
		std::string Name{};  // "skills/game-building"
		std::string Title{}; // the skill's description, or the document's first heading
	};

	struct DocsGetResult
	{
		std::vector<DocsTopic> Topics{}; // without a topic
		std::string Topic{};             // with a topic: its name, its Markdown and its repository-relative path
		std::string Content{};
		std::string Path{};
	};

	namespace Automation {

		[[nodiscard]] Result<LogReadMethodResult> LogRead(EditorMethodContext& context, const LogReadParams& params);
		[[nodiscard]] Result<EventsReadResult> EventsRead(EditorMethodContext& context, const EventsReadParams& params);
		// docs.get. Errors: NotFound for an unknown topic (with suggestions); Unsupported when the server has no DocsRoot.
		[[nodiscard]] Result<DocsGetResult> DocsGet(EditorMethodContext& context, const DocsGetParams& params);

	}

	void RegisterObserveMethodTypes(TypeRegistry& registry);

	// Registers log.read, events.read and docs.get: read-only and AllowedInBatch; log.read and events.read are available in
	// the Runtime (M7; their declarations move to Engine/Automation/Methods then, ADR 0008 decision 26); docs.get is
	// available in the launcher state (§12.1); log.read and docs.get are tools (§13.8).
	void RegisterObserveMethods(MethodRegistry& methods);

}
