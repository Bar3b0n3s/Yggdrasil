#include "EnginePCH.h"
#include "Engine/Automation/Protocol/MetaBuilder.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/RingBufferSink.h"

#include <map>

namespace Engine {

	struct MetaBuilder::State
	{
		const RingBufferSink* Log = nullptr;    // documented back-reference: outlives the builder
		std::map<ClientId, uint64_t> Cursors{}; // each client's next unseen log sequence number
	};

	namespace Utils {

		// One diagnostics.firstNew entry: {seq, level, logger, message, file?, line?, entity?}. A script entry is located in
		// its script, any other entry at its log statement.
		[[nodiscard]] static Json DescribeLogEntry(const LogEntry& entry)
		{
			Json json = Json::object();
			json["seq"] = entry.Seq;
			json["level"] = std::string(LogLevelToString(entry.Level));
			json["logger"] = std::string(LogChannelToString(entry.Logger));
			json["message"] = entry.Message;
			const bool hasScriptLocation = !entry.ScriptFile.empty();
			const std::string& file = hasScriptLocation ? entry.ScriptFile : entry.File;
			const uint32_t line = hasScriptLocation ? entry.ScriptLine : entry.Line;
			if (!file.empty())
				json["file"] = file;
			if (line != 0)
				json["line"] = line;
			if (entry.EntityId.IsValid())
				json["entity"] = entry.EntityId.ToString();
			return json;
		}

	}

	MetaBuilder::MetaBuilder(const RingBufferSink& log)
		: m_State(CreateScope<State>())
	{
		m_State->Log = &log;
	}

	MetaBuilder::~MetaBuilder() = default;

	void MetaBuilder::AddClient(ClientId client)
	{
		const bool inserted = m_State->Cursors.emplace(client, m_State->Log->GetNextSeq()).second;
		ENGINE_CORE_ASSERT(inserted, "MetaBuilder::AddClient: client {} is already known", client);
		static_cast<void>(inserted);
	}

	void MetaBuilder::RemoveClient(ClientId client)
	{
		m_State->Cursors.erase(client);
	}

	Json MetaBuilder::Build(ClientId client, const MetaState& state)
	{
		const auto cursor = m_State->Cursors.find(client);
		ENGINE_CORE_ASSERT(cursor != m_State->Cursors.end(), "MetaBuilder::Build: unknown client {}", client);

		uint64_t newErrors = 0;
		uint64_t newWarnings = 0;
		uint64_t newScriptErrors = 0;
		uint64_t dropped = 0;
		Json firstNew = Json::array();
		uint64_t nextCursor = m_State->Log->GetNextSeq();
		if (cursor != m_State->Cursors.end())
		{
			// Every held entry at Warn and above since the client's cursor: the ring holds at most its capacity, so this limit
			// never stops the scan early.
			const LogReadResult read = m_State->Log->Read(LogQuery{
				.Cursor = cursor->second,
				.MinimumLevel = LogLevel::Warn,
				.Channels = {},
				.Contains = {},
				.Limit = m_State->Log->GetCapacity(),
			});
			for (const LogEntry& entry : read.Entries)
			{
				const bool isError = entry.Level == LogLevel::Error || entry.Level == LogLevel::Critical;
				if (isError && entry.Logger == LogChannel::Script)
					++newScriptErrors;
				else if (isError)
					++newErrors;
				else if (entry.Level == LogLevel::Warn)
					++newWarnings; // every logger's, the Script logger's included
				if (firstNew.size() < MaxFirstNewDiagnostics)
					firstNew.push_back(Utils::DescribeLogEntry(entry));
			}
			dropped = read.DroppedCount;
			nextCursor = read.NextCursor;
			cursor->second = nextCursor;
		}

		Json diagnostics = Json::object();
		diagnostics["newErrors"] = newErrors;
		diagnostics["newWarnings"] = newWarnings;
		diagnostics["newScriptErrors"] = newScriptErrors;
		diagnostics["logCursor"] = std::to_string(nextCursor);
		diagnostics["firstNew"] = std::move(firstNew);
		if (dropped != 0)
			diagnostics["dropped"] = dropped;

		Json meta = Json::object();
		meta["revision"] = state.Revision;
		meta["dirty"] = state.Dirty;
		if (!state.UndoLabel.empty())
			meta["undoLabel"] = state.UndoLabel;
		if (state.Tick.has_value())
			meta["tick"] = *state.Tick;
		meta["playState"] = state.PlayState;
		if (state.SceneChangedOnDisk)
			meta["sceneChangedOnDisk"] = true;
		meta["diagnostics"] = std::move(diagnostics);
		return meta;
	}

}
