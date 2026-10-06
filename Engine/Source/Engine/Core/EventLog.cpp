#include "EnginePCH.h"
#include "Engine/Core/EventLog.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Private/AsciiText.h"

#include <algorithm>
#include <limits>

namespace Engine {

	namespace Utils {

		// The enumerator name, or an empty view for a value that is not an enumerator. A switch rather than a table, so
		// an enumerator added without a name is a -Wswitch error on GCC and Clang.
		static std::string_view TryGetEventTypeName(EngineEventType type)
		{
			switch (type)
			{
				case EngineEventType::EntityCreated:                return "EntityCreated";
				case EngineEventType::EntityDestroyed:              return "EntityDestroyed";
				case EngineEventType::ComponentChanged:             return "ComponentChanged";
				case EngineEventType::SceneOpened:                  return "SceneOpened";
				case EngineEventType::SceneChangedOnDisk:           return "SceneChangedOnDisk";
				case EngineEventType::PlayStateChanged:             return "PlayStateChanged";
				case EngineEventType::AssetReloaded:                return "AssetReloaded";
				case EngineEventType::AssetImportFailed:            return "AssetImportFailed";
				case EngineEventType::ScriptErrorRaised:            return "ScriptErrorRaised";
				case EngineEventType::DiagnosticsChanged:           return "DiagnosticsChanged";
				case EngineEventType::AutomationClientDisconnected: return "AutomationClientDisconnected";
			}
			return {};
		}

	}

	std::string_view EngineEventTypeToString(EngineEventType type)
	{
		const std::string_view name = Utils::TryGetEventTypeName(type);
		if (!name.empty())
			return name;

		ENGINE_CORE_ASSERT(false, "Unknown EngineEventType {}", std::to_underlying(type));
		return "Unknown";
	}

	std::optional<EngineEventType> EngineEventTypeFromString(std::string_view text)
	{
		constexpr uint32_t MaxValue = std::numeric_limits<std::underlying_type_t<EngineEventType>>::max();
		for (uint32_t value = 0; value <= MaxValue; ++value)
		{
			const EngineEventType type = static_cast<EngineEventType>(value);
			const std::string_view name = Utils::TryGetEventTypeName(type);
			if (!name.empty() && Utils::EqualsIgnoreAsciiCase(name, text))
				return type;
		}
		return std::nullopt;
	}

	EventLog::EventLog(size_t capacity)
		: m_Capacity(std::max<size_t>(capacity, 1)), m_MainThread(std::this_thread::get_id())
	{
		// The clamp above keeps a zero capacity from dividing by zero in Dist, where the assert is compiled out.
		ENGINE_CORE_ASSERT(capacity >= 1, "EventLog capacity must be at least 1");
		m_Events.reserve(m_Capacity);
	}

	uint64_t EventLog::Append(EngineEvent event)
	{
		ENGINE_CORE_ASSERT(std::this_thread::get_id() == m_MainThread, "EventLog::Append called off the main thread");

		const uint64_t seq = m_NextSeq++;
		event.Seq = seq;
		if (m_Events.size() < m_Capacity)
			m_Events.push_back(std::move(event)); // still filling: index seq - 1 is the end
		else
			m_Events[static_cast<size_t>((seq - 1) % m_Capacity)] = std::move(event);
		return seq;
	}

	EventReadResult EventLog::Read(uint64_t cursor, std::span<const EngineEventType> types, size_t limit) const
	{
		ENGINE_CORE_ASSERT(std::this_thread::get_id() == m_MainThread, "EventLog::Read called off the main thread");

		EventReadResult result;
		const uint64_t oldest = GetOldestSeq();
		// Cursor 0 asks for the oldest held event, so nothing counts as dropped for it. A cursor past GetNextSeq()
		// (from another log, for example a previous editor session) cannot be honoured; the read restarts at the next
		// event instead of waiting for that number and silently skipping everything before it.
		if (cursor != 0 && cursor < oldest)
			result.DroppedCount = oldest - cursor;
		const uint64_t start = cursor == 0 ? oldest : std::clamp(cursor, oldest, m_NextSeq);

		std::array<bool, std::numeric_limits<std::underlying_type_t<EngineEventType>>::max() + 1> isWanted{};
		isWanted.fill(types.empty());
		for (const EngineEventType type : types)
			isWanted[std::to_underlying(type)] = true;

		result.Events.reserve(static_cast<size_t>(std::min<uint64_t>(limit, m_NextSeq - start)));
		uint64_t seq = start;
		for (; seq < m_NextSeq && result.Events.size() < limit; ++seq)
		{
			const EngineEvent& event = GetHeld(seq);
			if (isWanted[std::to_underlying(event.Type)])
				result.Events.push_back(event);
		}
		result.NextCursor = seq;
		return result;
	}

	uint64_t EventLog::GetNextSeq() const
	{
		ENGINE_CORE_ASSERT(std::this_thread::get_id() == m_MainThread, "EventLog::GetNextSeq called off the main thread");
		return m_NextSeq;
	}

	size_t EventLog::GetCapacity() const
	{
		return m_Capacity;
	}

	size_t EventLog::GetSize() const
	{
		ENGINE_CORE_ASSERT(std::this_thread::get_id() == m_MainThread, "EventLog::GetSize called off the main thread");
		return m_Events.size();
	}

	const EngineEvent& EventLog::GetHeld(uint64_t seq) const
	{
		ENGINE_CORE_ASSERT(seq >= GetOldestSeq() && seq < m_NextSeq, "Event {} is not held", seq);
		return m_Events[static_cast<size_t>((seq - 1) % m_Capacity)];
	}

	uint64_t EventLog::GetOldestSeq() const
	{
		return m_NextSeq - m_Events.size();
	}

}
