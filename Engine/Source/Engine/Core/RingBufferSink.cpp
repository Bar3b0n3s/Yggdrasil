#include "EnginePCH.h"
#include "Engine/Core/RingBufferSink.h"

// M1 contract stub (Roadmap rule 3): stream A implements the ring, the queries and the name conversions.

namespace Engine {

	std::string_view LogLevelToString(LogLevel /*level*/)
	{
		return {};
	}

	std::optional<LogLevel> LogLevelFromString(std::string_view /*text*/)
	{
		return std::nullopt;
	}

	std::string_view LogChannelToString(LogChannel /*channel*/)
	{
		return {};
	}

	std::optional<LogChannel> LogChannelFromString(std::string_view /*text*/)
	{
		return std::nullopt;
	}

	RingBufferSink::RingBufferSink(size_t capacity)
		: m_Capacity(capacity)
	{
	}

	uint64_t RingBufferSink::Append(LogEntry /*entry*/)
	{
		return 0;
	}

	LogReadResult RingBufferSink::Read(const LogQuery& /*query*/) const
	{
		return {};
	}

	std::vector<LogEntry> RingBufferSink::ReadLast(size_t /*count*/) const
	{
		return {};
	}

	uint64_t RingBufferSink::GetNextSeq() const
	{
		return m_NextSeq;
	}

	size_t RingBufferSink::GetCapacity() const
	{
		return m_Capacity;
	}

	size_t RingBufferSink::GetSize() const
	{
		return 0;
	}

}
