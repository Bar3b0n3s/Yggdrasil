#include "EnginePCH.h"
#include "Engine/Core/RingBufferSink.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Private/AsciiText.h"

namespace Engine {

	namespace Utils {

		static bool MatchesQuery(const LogEntry& entry, const LogQuery& query)
		{
			if (entry.Level < query.MinimumLevel)
				return false;
			if (!query.Channels.empty() && std::ranges::find(query.Channels, entry.Logger) == query.Channels.end())
				return false;
			return query.Contains.empty() || entry.Message.contains(query.Contains);
		}

	}

	std::string_view LogLevelToString(LogLevel level)
	{
		switch (level)
		{
			case LogLevel::Trace:    return "Trace";
			case LogLevel::Info:     return "Info";
			case LogLevel::Warn:     return "Warn";
			case LogLevel::Error:    return "Error";
			case LogLevel::Critical: return "Critical";
		}

		ENGINE_CORE_ASSERT(false, "Unknown LogLevel {}", std::to_underlying(level));
		return "Unknown";
	}

	std::optional<LogLevel> LogLevelFromString(std::string_view text)
	{
		for (const LogLevel level : { LogLevel::Trace, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Critical })
		{
			if (Utils::EqualsIgnoreAsciiCase(text, LogLevelToString(level)))
				return level;
		}
		return std::nullopt;
	}

	std::string_view LogChannelToString(LogChannel channel)
	{
		switch (channel)
		{
			case LogChannel::Engine: return "Engine";
			case LogChannel::App:    return "App";
			case LogChannel::Script: return "Script";
		}

		ENGINE_CORE_ASSERT(false, "Unknown LogChannel {}", std::to_underlying(channel));
		return "Unknown";
	}

	std::optional<LogChannel> LogChannelFromString(std::string_view text)
	{
		for (const LogChannel channel : { LogChannel::Engine, LogChannel::App, LogChannel::Script })
		{
			if (Utils::EqualsIgnoreAsciiCase(text, LogChannelToString(channel)))
				return channel;
		}
		return std::nullopt;
	}

	RingBufferSink::RingBufferSink(size_t capacity)
		: m_Capacity(std::max<size_t>(capacity, 1))
	{
		ENGINE_CORE_ASSERT(capacity >= 1, "A RingBufferSink needs a capacity of at least 1");
	}

	uint64_t RingBufferSink::Append(LogEntry entry)
	{
		std::scoped_lock lock(m_Mutex);
		const uint64_t seq = m_NextSeq++;
		entry.Seq = seq;
		if (m_Entries.size() < m_Capacity)
			m_Entries.push_back(std::move(entry)); // still filling: the next index is the current size
		else
			m_Entries[static_cast<size_t>((seq - 1) % m_Capacity)] = std::move(entry);
		return seq;
	}

	LogReadResult RingBufferSink::Read(const LogQuery& query) const
	{
		std::scoped_lock lock(m_Mutex);
		const uint64_t oldestSeq = m_NextSeq - m_Entries.size();

		LogReadResult result;
		uint64_t seq = oldestSeq;
		if (query.Cursor != 0)
		{
			if (query.Cursor < oldestSeq)
				result.DroppedCount = oldestSeq - query.Cursor;
			// A cursor past the end can only come from an earlier ring buffer (the log was initialized again): it resumes
			// at the next entry instead of waiting for sequence numbers this buffer has not reached.
			seq = std::clamp(query.Cursor, oldestSeq, m_NextSeq);
		}

		for (; seq < m_NextSeq; ++seq)
		{
			if (result.Entries.size() >= query.Limit)
				break;
			const LogEntry& entry = m_Entries[static_cast<size_t>((seq - 1) % m_Capacity)];
			if (Utils::MatchesQuery(entry, query))
				result.Entries.push_back(entry);
		}
		result.NextCursor = seq;
		return result;
	}

	std::vector<LogEntry> RingBufferSink::ReadLast(size_t count) const
	{
		std::scoped_lock lock(m_Mutex);
		const size_t returned = std::min(count, m_Entries.size());

		std::vector<LogEntry> entries;
		entries.reserve(returned);
		for (uint64_t seq = m_NextSeq - returned; seq < m_NextSeq; ++seq)
			entries.push_back(m_Entries[static_cast<size_t>((seq - 1) % m_Capacity)]);
		return entries;
	}

	uint64_t RingBufferSink::GetNextSeq() const
	{
		std::scoped_lock lock(m_Mutex);
		return m_NextSeq;
	}

	size_t RingBufferSink::GetCapacity() const
	{
		return m_Capacity;
	}

	size_t RingBufferSink::GetSize() const
	{
		std::scoped_lock lock(m_Mutex);
		return m_Entries.size();
	}

}
