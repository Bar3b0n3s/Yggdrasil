#include "EnginePCH.h"
#include "Engine/Automation/Methods/ObserveMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Log.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <optional>
#include <string>

namespace Engine {

	namespace Utils {

		// A tick for the reflected results: -1 outside a session step, saturated at INT32_MAX.
		static int32_t ToAutomationTick(const std::optional<uint64_t>& tick)
		{
			if (!tick.has_value())
				return -1;
			return *tick > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ? std::numeric_limits<int32_t>::max() : static_cast<int32_t>(*tick);
		}

	}

	namespace Automation {

		Result<LogReadMethodResult> LogRead(AutomationMethodContext& /*context*/, const LogReadParams& params)
		{
			const RingBufferSink& log = Log::GetRingBuffer();
			ENGINE_TRY_ASSIGN(const std::optional<uint64_t> cursor, Utils::ParseSequenceCursor(params.Cursor));

			LogReadMethodResult result;
			if (!cursor.has_value())
			{
				result.NextCursor = std::to_string(log.GetNextSeq());
				return result;
			}

			LogQuery query;
			query.Cursor = *cursor;
			query.MinimumLevel = params.MinLevel;
			query.Channels = params.Loggers;
			query.Contains = params.Contains;
			query.Limit = params.Limit;
			const LogReadResult read = log.Read(query);
			for (const LogEntry& entry : read.Entries)
			{
				LogEntrySummary summary;
				summary.Seq = ToAutomationCounter(entry.Seq);
				summary.Level = entry.Level;
				summary.Logger = entry.Logger;
				summary.Message = entry.Message;
				summary.File = entry.File;
				summary.Line = entry.Line;
				summary.Tick = Utils::ToAutomationTick(entry.Tick);
				summary.Entity = Utils::FormatOptionalUUID(entry.EntityId);
				summary.ScriptFile = entry.ScriptFile;
				summary.ScriptLine = entry.ScriptLine;
				result.Entries.push_back(std::move(summary));
			}
			result.NextCursor = std::to_string(read.NextCursor);
			result.Dropped = ToAutomationCounter(read.DroppedCount);
			return result;
		}

		Result<EventsReadResult> EventsRead(AutomationMethodContext& context, const EventsReadParams& params)
		{
			const EventLog& events = context.GetEventLog();
			ENGINE_TRY_ASSIGN(const std::optional<uint64_t> cursor, Utils::ParseSequenceCursor(params.Cursor));

			EventsReadResult result;
			if (!cursor.has_value())
			{
				result.NextCursor = std::to_string(events.GetNextSeq());
				return result;
			}

			const EventReadResult read = events.Read(*cursor, params.Types, params.Limit);
			for (const EngineEvent& event : read.Events)
			{
				EngineEventSummary summary;
				summary.Seq = ToAutomationCounter(event.Seq);
				summary.Type = event.Type;
				summary.Tick = Utils::ToAutomationTick(event.Tick);
				summary.Id = Utils::FormatOptionalUUID(event.Id);
				summary.Path = event.Path;
				summary.Name = event.Name;
				summary.Message = event.Message;
				summary.Dirty = event.Dirty;
				result.Events.push_back(std::move(summary));
			}
			result.NextCursor = std::to_string(read.NextCursor);
			result.Dropped = ToAutomationCounter(read.DroppedCount);
			return result;
		}

	}

	void RegisterObserveMethodTypes(TypeRegistry& registry)
	{
		const FieldMeta limitMeta{ .Min = 1.0, .Max = 1000.0 };

		registry.Struct<LogReadParams>("LogReadParams", "The params of log.read: entries of the log by cursor, filtered.")
			.Field("cursor", &LogReadParams::Cursor,
				"Where to start: \"\" for the oldest held entry, \"end\" for nothing but the cursor of the next entry, or a previous "
				"nextCursor.")
			.Field("minLevel", &LogReadParams::MinLevel, "Skip entries below this level.")
			.Field("loggers", &LogReadParams::Loggers, "Only these loggers; empty for every logger.")
			.Field("contains", &LogReadParams::Contains, "Only entries whose message contains this text (case-sensitive).")
			.Field("limit", &LogReadParams::Limit, "The most entries returned.", limitMeta);

		registry.Struct<LogEntrySummary>("LogEntrySummary", "One log entry.")
			.Field("seq", &LogEntrySummary::Seq, "The entry's sequence number.")
			.Field("level", &LogEntrySummary::Level, "The entry's level.")
			.Field("logger", &LogEntrySummary::Logger, "The logger it came from.")
			.Field("message", &LogEntrySummary::Message, "The message.")
			.Field("file", &LogEntrySummary::File, "The source file of the log statement; empty when unknown.")
			.Field("line", &LogEntrySummary::Line, "The source line of the log statement; 0 when unknown.")
			.Field("tick", &LogEntrySummary::Tick, "The simulation tick; -1 outside a session step.")
			.Field("entity", &LogEntrySummary::Entity, "The id of the entity it is about; empty when none.")
			.Field("scriptFile", &LogEntrySummary::ScriptFile, "The script it came from; empty when none.")
			.Field("scriptLine", &LogEntrySummary::ScriptLine, "The script line; 0 when none.");

		registry.Struct<LogReadMethodResult>("LogReadMethodResult", "Log entries read by cursor.")
			.Field("entries", &LogReadMethodResult::Entries, "The matching entries, oldest first.")
			.Field("nextCursor", &LogReadMethodResult::NextCursor, "Where the next read continues.")
			.Field("dropped", &LogReadMethodResult::Dropped, "Entries overwritten before this read could see them.");

		registry.Struct<EventsReadParams>("EventsReadParams", "The params of events.read: engine events by cursor.")
			.Field("cursor", &EventsReadParams::Cursor,
				"Where to start: \"\" for the oldest held event, \"end\" for nothing but the cursor of the next event, or a previous "
				"nextCursor.")
			.Field("types", &EventsReadParams::Types, "Only these event types; empty for every type.")
			.Field("limit", &EventsReadParams::Limit, "The most events returned.", limitMeta);

		registry.Struct<EngineEventSummary>("EngineEventSummary", "One engine event.")
			.Field("seq", &EngineEventSummary::Seq, "The event's sequence number.")
			.Field("type", &EngineEventSummary::Type, "The event's type.")
			.Field("tick", &EngineEventSummary::Tick, "The simulation tick; -1 outside play.")
			.Field("id", &EngineEventSummary::Id, "The entity or asset it is about; empty when none.")
			.Field("path", &EngineEventSummary::Path, "The scene, prefab, asset or script path it is about; empty when none.")
			.Field("name", &EngineEventSummary::Name, "The entity, component, client or play state name it is about; empty when none.")
			.Field("message", &EngineEventSummary::Message, "The error or summary it carries; empty when none.")
			.Field("dirty", &EngineEventSummary::Dirty, "For SceneChangedOnDisk: whether the open scene had unsaved changes.");

		registry.Struct<EventsReadResult>("EventsReadResult", "Engine events read by cursor.")
			.Field("events", &EventsReadResult::Events, "The matching events, oldest first.")
			.Field("nextCursor", &EventsReadResult::NextCursor, "Where the next read continues.")
			.Field("dropped", &EventsReadResult::Dropped, "Events overwritten before this read could see them.");
	}

	void RegisterObserveMethods(MethodRegistry& methods)
	{
		Json logExample = Json::object();
		logExample["cursor"] = "";
		logExample["minLevel"] = "Warn";
		logExample["limit"] = 50;
		methods.Add(
			{
				.Name = "log.read",
				.Description = "Reads the host's log (the editor's or the exported game's) by cursor so nothing is lost between polls, filtered "
							   "by level, logger and text. Pass nextCursor to the next read; \"end\" returns only the cursor of the next entry.",
				.ExposeAsTool = true,
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the warnings and errors held in the log.", .Params = logExample } },
			},
			&Automation::LogRead);

		Json eventsExample = Json::object();
		eventsExample["cursor"] = "end";
		methods.Add(
			{
				.Name = "events.read",
				.Description = "Reads engine events (entities, scenes, assets, scripts, clients) by cursor, filtered by type. Pass nextCursor to "
							   "the next read; \"end\" returns only the cursor of the next event.",
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Take the cursor of the next event before an action.", .Params = eventsExample } },
			},
			&Automation::EventsRead);
	}

}
