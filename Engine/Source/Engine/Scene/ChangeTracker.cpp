#include "EnginePCH.h"
#include "Engine/Scene/ChangeTracker.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream B (Scene core) implements change recording.

namespace Engine {

	void ChangeTracker::Begin()
	{
		ENGINE_CONTRACT_STUB();
	}

	std::vector<EntityChange> ChangeTracker::End()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	bool ChangeTracker::NeedsSnapshot(UUID /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool ChangeTracker::NeedsChildOrder(UUID /*parent*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void ChangeTracker::RecordChildOrder(UUID /*parent*/, std::span<const UUID> /*children*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ChangeTracker::RecordSnapshot(UUID /*entity*/, Ref<const Json> /*before*/, UUID /*parent*/, uint32_t /*siblingIndex*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ChangeTracker::RecordCreated(UUID /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ChangeTracker::RecordDestroyed(UUID /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ChangeTracker::RecordComponent(UUID /*entity*/, std::string_view /*component*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
