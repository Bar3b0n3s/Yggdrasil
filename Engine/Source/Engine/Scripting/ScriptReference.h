#pragma once

#include "Engine/Core/UUID.h"

#include <compare>
#include <cstdint>

namespace Engine {

	// An opaque retained function, coroutine or task in one VM. Neither field is a Lua registry index or pointer.
	// Generation 0 is null; a VM never reuses an Index, including after release. Every consumer checks both fields
	// and the reference's internal kind before use, so released, forged and previous-scene values cannot alias live work.
	struct ScriptReference
	{
		uint64_t VMGeneration = 0;
		uint64_t Index = 0;

		[[nodiscard]] constexpr bool IsNull() const { return VMGeneration == 0 || Index == 0; }
		constexpr auto operator<=>(const ScriptReference&) const = default;
	};

	// Tasks inherit the calling behaviour and case. Null Entity means suite/eval-owned work; null Case means ordinary
	// play. Cancel an owner's work when its entity is destroyed/disabled or its case ends, before releasing the owner.
	struct ScriptTaskOwner
	{
		UUID Entity{};
		ScriptReference Case{};
	};

}
