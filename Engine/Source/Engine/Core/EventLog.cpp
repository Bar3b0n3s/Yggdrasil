#include "EnginePCH.h"
#include "Engine/Core/EventLog.h"

// M1 contract stub (Roadmap rule 3): stream D implements the ring, the reads and the type names.

namespace Engine {

	std::string_view EngineEventTypeToString(EngineEventType /*type*/)
	{
		return {};
	}

	std::optional<EngineEventType> EngineEventTypeFromString(std::string_view /*text*/)
	{
		return std::nullopt;
	}

	EventLog::EventLog(size_t capacity)
		: m_Capacity(capacity)
	{
	}

	uint64_t EventLog::Append(EngineEvent /*event*/)
	{
		return 0;
	}

	EventReadResult EventLog::Read(uint64_t /*cursor*/, std::span<const EngineEventType> /*types*/, size_t /*limit*/) const
	{
		return {};
	}

	uint64_t EventLog::GetNextSeq() const
	{
		return m_NextSeq;
	}

	size_t EventLog::GetCapacity() const
	{
		return m_Capacity;
	}

	size_t EventLog::GetSize() const
	{
		return 0;
	}

}
