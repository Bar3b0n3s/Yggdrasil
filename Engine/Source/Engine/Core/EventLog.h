#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Engine {

	// Engine-level notifications (Architecture §4.9). Values are stable: append new types at the end.
	enum class EngineEventType : uint8_t
	{
		EntityCreated,
		EntityDestroyed,
		ComponentChanged,
		SceneOpened,
		SceneChangedOnDisk,
		PlayStateChanged,
		AssetReloaded,
		AssetImportFailed,
		ScriptErrorRaised,
		DiagnosticsChanged,
		AutomationClientDisconnected
	};

	// The enumerator name ("EntityCreated").
	[[nodiscard]] std::string_view EngineEventTypeToString(EngineEventType type);
	// Case-insensitive inverse of EngineEventTypeToString (automation events.read {types}); nullopt otherwise.
	[[nodiscard]] std::optional<EngineEventType> EngineEventTypeFromString(std::string_view text);

	// One notification. The fields each type uses (unused fields stay empty or invalid):
	//   EntityCreated                 Id = entity, Name = entity name
	//   EntityDestroyed               Id = entity
	//   ComponentChanged              Id = entity, Name = component registry name
	//   SceneOpened                   Id = scene asset handle (invalid for an unsaved scene), Path = scene path
	//   SceneChangedOnDisk            Path = scene or prefab path, Dirty = whether the open scene has unsaved changes
	//   PlayStateChanged              Name = new state ("Edit", "Play", "Simulate", "Paused")
	//   AssetReloaded                 Id = asset handle, Path = asset path
	//   AssetImportFailed             Id = asset handle, Path = asset path, Message = the error
	//   ScriptErrorRaised             Id = entity (invalid for module-level errors), Path = script path, Message = error
	//   DiagnosticsChanged            Message = summary ("2 errors, 1 warning")
	//   AutomationClientDisconnected  Name = client name
	struct EngineEvent
	{
		uint64_t Seq = 0;             // assigned by EventLog::Append: 1, 2, 3 ... never reused
		std::optional<uint64_t> Tick; // simulation tick when raised during play
		EngineEventType Type = EngineEventType::EntityCreated;
		UUID Id;
		std::string Path;
		std::string Name;
		std::string Message;
		bool Dirty = false;
	};

	struct EventReadResult
	{
		std::vector<EngineEvent> Events; // ascending Seq
		// One past the last scanned event (events skipped by the type filter are not scanned again); one past the last
		// returned event when the limit stopped the scan.
		uint64_t NextCursor = 0;
		// Events with Seq >= the requested cursor that were overwritten before this read.
		uint64_t DroppedCount = 0;
	};

	// A cursor-based ring of engine events (Architecture §4.9), read by the editor panels, automation events.read and
	// the _meta delta. There is no pub/sub: systems append, readers poll by cursor. Main thread only (asserted in
	// Debug and Release builds against the constructing thread).
	class EventLog
	{
	public:
		static constexpr size_t DefaultCapacity = 4096;

		// `capacity` must be at least 1 (asserted). The constructing thread becomes the log's main thread.
		explicit EventLog(size_t capacity = DefaultCapacity);

		EventLog(const EventLog&) = delete;
		EventLog& operator=(const EventLog&) = delete;

		// Stores `event`, overwriting its Seq with the next sequence number (the oldest event is dropped when full), and
		// returns that number.
		uint64_t Append(EngineEvent event);

		// Events with Seq >= `cursor` (0 = the oldest held) whose type is in `types` (empty = every type), at most
		// `limit` of them, in Seq order. Cursor 0 reports DroppedCount 0; a cursor past GetNextSeq() returns nothing with
		// NextCursor = GetNextSeq().
		[[nodiscard]] EventReadResult Read(uint64_t cursor, std::span<const EngineEventType> types = {}, size_t limit = 100) const;

		// The Seq the next event will receive.
		[[nodiscard]] uint64_t GetNextSeq() const;
		[[nodiscard]] size_t GetCapacity() const;
		[[nodiscard]] size_t GetSize() const;
	private:
		// The event with sequence number `seq`, which must be held.
		[[nodiscard]] const EngineEvent& GetHeld(uint64_t seq) const;
		// The Seq of the oldest held event (GetNextSeq() when the log is empty).
		[[nodiscard]] uint64_t GetOldestSeq() const;
	private:
		size_t m_Capacity = 0;
		uint64_t m_NextSeq = 1;
		std::vector<EngineEvent> m_Events; // the ring: Seq s lives at index (s - 1) % m_Capacity
		std::thread::id m_MainThread;
	};

}
