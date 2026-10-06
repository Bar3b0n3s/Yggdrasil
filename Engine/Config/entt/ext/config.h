#pragma once

#include "Engine/Core/Assert.h"

// EnTT's user configuration header (Architecture §4.5). entt/config/config.h includes <entt/ext/config.h> when
// __has_include finds it; Engine/Config is on the include path of every first-party project (ApplyFirstPartySettings
// in Dependencies.lua), so every EnTT assertion goes through the engine's assert handler instead of <cassert>: compiled
// out in Dist like ENGINE_CORE_ASSERT, a death-testable assert everywhere else. This is the only file allowed to define
// ENTT_* macros (Scripts/ModuleRules.json, Banned.AbiMacroFiles).
#define ENTT_ASSERT(condition, msg) ENGINE_CORE_ASSERT(condition, "{}", msg)
