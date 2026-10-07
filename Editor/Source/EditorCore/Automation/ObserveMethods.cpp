#include "EditorPCH.h"
#include "EditorCore/Automation/ObserveMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <limits>

namespace Engine {

	namespace Utils {

		constexpr std::string_view SkillsTopicPrefix = "skills/";
		constexpr std::string_view ReferenceTopicPrefix = "reference/";
		constexpr std::string_view SkillsDirectory = ".claude/skills";
		constexpr std::string_view SkillFileName = "SKILL.md";
		constexpr std::string_view ReferenceDirectory = "Docs/Reference";

		// A tick for the reflected results: -1 outside a session step, saturated at INT32_MAX.
		static int32_t ToAutomationTick(const std::optional<uint64_t>& tick)
		{
			if (!tick.has_value())
				return -1;
			return *tick > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ? std::numeric_limits<int32_t>::max() : static_cast<int32_t>(*tick);
		}

		// A topic name segment: letters, digits, '-' and '_' only, so a topic can never name a path outside its directory.
		static bool IsTopicSegment(std::string_view name)
		{
			return !name.empty() && std::all_of(name.begin(), name.end(), [](char character)
			{
				return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
					|| character == '-' || character == '_';
			});
		}

		// The value of the "description:" line of a SKILL.md front matter; empty when there is none.
		static std::string ReadSkillDescription(std::string_view text)
		{
			if (!text.starts_with("---"))
				return {};
			size_t position = text.find('\n');
			while (position != std::string_view::npos && position + 1 < text.size())
			{
				const size_t start = position + 1;
				const size_t end = text.find('\n', start);
				std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
				if (line.ends_with('\r'))
					line.remove_suffix(1);
				if (line == "---")
					return {};
				constexpr std::string_view Key = "description:";
				if (line.starts_with(Key))
				{
					std::string_view value = line.substr(Key.size());
					while (!value.empty() && value.front() == ' ')
						value.remove_prefix(1);
					return std::string(value);
				}
				position = end;
			}
			return {};
		}

		// The text of a Markdown document's first "# " heading; empty when there is none.
		static std::string ReadFirstHeading(std::string_view text)
		{
			size_t start = 0;
			while (start < text.size())
			{
				const size_t end = text.find('\n', start);
				std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
				if (line.ends_with('\r'))
					line.remove_suffix(1);
				if (line.starts_with("# "))
					return std::string(line.substr(2));
				if (end == std::string_view::npos)
					break;
				start = end + 1;
			}
			return {};
		}

		// The file of a topic, repository-relative ("skills/build-and-test" -> ".claude/skills/build-and-test/SKILL.md"); empty
		// for a name that is not a topic.
		static std::string GetTopicPath(std::string_view topic)
		{
			if (topic.starts_with(SkillsTopicPrefix))
			{
				const std::string_view name = topic.substr(SkillsTopicPrefix.size());
				return IsTopicSegment(name) ? std::format("{}/{}/{}", SkillsDirectory, name, SkillFileName) : std::string();
			}
			if (topic.starts_with(ReferenceTopicPrefix))
			{
				const std::string_view name = topic.substr(ReferenceTopicPrefix.size());
				return IsTopicSegment(name) ? std::format("{}/{}.md", ReferenceDirectory, name) : std::string();
			}
			return {};
		}

		// Every topic under `docsRoot`, sorted by name. Missing directories give no topics.
		static Result<std::vector<DocsTopic>> ListTopics(const std::filesystem::path& docsRoot)
		{
			std::vector<DocsTopic> topics;
			const Result<std::vector<std::filesystem::path>> skills = FileSystem::ListDirectory(docsRoot / FileSystem::PathFromUtf8(SkillsDirectory));
			if (skills.has_value())
			{
				for (const std::filesystem::path& directory : *skills)
				{
					const std::string name = FileSystem::PathToUtf8(directory.filename());
					const Result<std::string> text = FileSystem::ReadText(directory / FileSystem::PathFromUtf8(SkillFileName));
					if (!IsTopicSegment(name) || !text)
						continue;
					topics.push_back(DocsTopic{ std::format("{}{}", SkillsTopicPrefix, name), ReadSkillDescription(*text) });
				}
			}
			else if (skills.error().GetCode() != ErrorCode::NotFound)
			{
				return std::unexpected(ToEditorFileError(skills.error()));
			}

			const Result<std::vector<std::filesystem::path>> references = FileSystem::ListDirectory(docsRoot / FileSystem::PathFromUtf8(ReferenceDirectory));
			if (references.has_value())
			{
				for (const std::filesystem::path& file : *references)
				{
					const std::filesystem::path extension = file.extension();
					const std::string stem = FileSystem::PathToUtf8(file.stem());
					if (extension != std::filesystem::path(".md") || !IsTopicSegment(stem))
						continue;
					const Result<std::string> text = FileSystem::ReadText(file);
					if (!text)
						continue;
					topics.push_back(DocsTopic{ std::format("{}{}", ReferenceTopicPrefix, stem), ReadFirstHeading(*text) });
				}
			}
			else if (references.error().GetCode() != ErrorCode::NotFound)
			{
				return std::unexpected(ToEditorFileError(references.error()));
			}

			std::sort(topics.begin(), topics.end(), [](const DocsTopic& left, const DocsTopic& right)
			{
				return left.Name < right.Name;
			});
			return topics;
		}

	}

	namespace Automation {

		Result<LogReadMethodResult> LogRead(EditorMethodContext& /*context*/, const LogReadParams& params)
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

		Result<EventsReadResult> EventsRead(EditorMethodContext& context, const EventsReadParams& params)
		{
			const EventLog& events = context.GetEditor().GetEngine().GetEventLog();
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

		Result<DocsGetResult> DocsGet(EditorMethodContext& context, const DocsGetParams& params)
		{
			const std::filesystem::path& docsRoot = context.GetServer().GetSpecification().DocsRoot;
			if (docsRoot.empty())
				return MakeError(ErrorCode::Unsupported, "docs.get needs the repository root, which this editor was not given");

			DocsGetResult result;
			if (params.Topic.empty())
			{
				ENGINE_TRY_ASSIGN(result.Topics, Utils::ListTopics(docsRoot));
				return result;
			}

			const std::string path = Utils::GetTopicPath(params.Topic);
			if (!path.empty())
			{
				Result<std::string> text = FileSystem::ReadText(docsRoot / FileSystem::PathFromUtf8(path));
				// The same spelling on every host: a topic that matches a file only ignoring case is not that topic.
				if (text.has_value() && FileSystem::VerifyCase(docsRoot, path).has_value())
				{
					result.Topic = params.Topic;
					result.Content = std::move(*text);
					result.Path = path;
					return result;
				}
				if (!text && text.error().GetCode() != ErrorCode::NotFound)
					return std::unexpected(Utils::ToEditorFileError(text.error()));
			}

			ENGINE_TRY_ASSIGN(const std::vector<DocsTopic> topics, Utils::ListTopics(docsRoot));
			std::vector<std::string> names;
			for (const DocsTopic& topic : topics)
				names.push_back(topic.Name);
			const std::vector<std::string> suggestions = FuzzySuggest(params.Topic, std::span<const std::string>(names));
			return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/topic", std::format("no topic '{}'", params.Topic),
				suggestions.empty() ? std::string("docs.get {} lists the topics") : MakeDidYouMeanHint(suggestions)));
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

		registry.Struct<DocsGetParams>("DocsGetParams", "The params of docs.get.")
			.Field("topic", &DocsGetParams::Topic, "The topic, such as \"skills/game-building\"; empty for the list of topics.");

		registry.Struct<DocsTopic>("DocsTopic", "One documentation topic.")
			.Field("name", &DocsTopic::Name, "The topic's name, such as \"skills/game-building\" or \"reference/Validation\".")
			.Field("title", &DocsTopic::Title, "A skill's description or a reference document's first heading.");

		registry.Struct<DocsGetResult>("DocsGetResult", "The documentation topics, or one topic's Markdown.")
			.Field("topics", &DocsGetResult::Topics, "Every topic, when no topic was asked for.")
			.Field("topic", &DocsGetResult::Topic, "The topic asked for.")
			.Field("content", &DocsGetResult::Content, "The topic's Markdown.")
			.Field("path", &DocsGetResult::Path, "The topic's file, relative to the repository root.");
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
				.Description = "Reads the editor's log by cursor so nothing is lost between polls, filtered by level, logger and text. Pass "
							   "nextCursor to the next read; \"end\" returns only the cursor of the next entry.",
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

		Json docsExample = Json::object();
		docsExample["topic"] = "skills/add-automation-method";
		methods.Add(
			{
				.Name = "docs.get",
				.Description = "Serves the agent documentation: without a topic the list of skills and reference documents, with one its "
							   "Markdown.",
				.ExposeAsTool = true,
				.AvailableInLauncher = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the add-automation-method skill.", .Params = docsExample } },
			},
			&Automation::DocsGet);
	}

}
