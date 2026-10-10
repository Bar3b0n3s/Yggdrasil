#pragma once

#include "Engine/Asset/ReplayData.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Session/PlayInput.h"

#include <cstdint>
#include <string_view>

namespace Engine {

	struct StampedPlayInputEvent
	{
		uint64_t Tick = 0; // Absolute for replay; an offset for automation until its caller adds the base tick.
		PlayInputEvent Event{};
	};

	// Pure common parser for camelCase automation input and the Test.Inject* host bridge. No request context or
	// higher-layer include. Unknown/inappropriate members, nonfinite values, malformed controls and actions are located
	// InvalidArgument errors with hints. allowTick=false rejects a tick member (Test injection targets GetNextTick).
	// Enum/control spellings are case-insensitive, action names exact. State defaults to Down as in input.inject.
	// Existing Automation::MakePlayInputEvent becomes a thin caller; it keeps its reflected params/schema unchanged.
	[[nodiscard]] Result<StampedPlayInputEvent> ParsePlayInputEvent(const Json& value, const InputActionMap& actions,
		std::string_view pointer = {}, bool allowTick = true);

	// Replay event names are already canonical; validates against the same project action map and event validator.
	[[nodiscard]] Result<StampedPlayInputEvent> DecodeReplayEvent(const ReplayEvent& event, const InputActionMap& actions,
		std::string_view pointer = {});
	// Copies an applied event to Asset vocabulary. Does not expand Tap or invent a release beyond the applied stream.
	[[nodiscard]] Result<ReplayEvent> EncodeReplayEvent(uint64_t tick, const PlayInputEvent& event);

}
