#include "EnginePCH.h"
#include "Engine/Scripting/TrackingAllocator.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace Engine {

	struct TrackingAllocator::State
	{
		ScriptMemoryState Memory{};
	};

	TrackingAllocator::TrackingAllocator(ConstructionKey /*key*/, uint64_t softLimitBytes)
		: m_State(CreateScope<State>())
	{
		m_State->Memory.SoftLimitBytes = softLimitBytes;
		m_State->Memory.HardLimitBytes = softLimitBytes + HeadroomBytes;
	}

	TrackingAllocator::~TrackingAllocator()
	{
		ENGINE_CORE_ASSERT(m_State->Memory.UsedBytes == 0, "script allocator destroyed with live allocations");
	}

	Result<Scope<TrackingAllocator>> TrackingAllocator::Create(uint32_t limitMB)
	{
		const uint64_t soft = static_cast<uint64_t>(limitMB) * 1024 * 1024;
		if (limitMB == 0 || soft > std::numeric_limits<size_t>::max() - HeadroomBytes)
			return MakeError(ErrorCode::InvalidArgument, "script memory limit must fit the address space with 16 MiB headroom");
		return CreateScope<TrackingAllocator>(ConstructionKey{}, soft);
	}

	void* TrackingAllocator::Reallocate(void* memory, size_t oldSize, size_t newSize) noexcept
	{
		auto& state = m_State->Memory;
		const uint64_t oldBytes = memory ? oldSize : 0;
		ENGINE_CORE_VERIFY(oldBytes <= state.UsedBytes, "invalid script allocation size");
		if (newSize == 0)
		{
			std::free(memory);
			state.UsedBytes -= oldBytes;
			return nullptr;
		}
		const uint64_t retained = state.UsedBytes - oldBytes;
		const bool grows = newSize > oldBytes;
		if ((grows && state.MustStop) || newSize > std::numeric_limits<uint64_t>::max() - retained || (grows && retained + newSize >= state.HardLimitBytes))
		{
			state.MustStop = true;
			return nullptr;
		}
		void* result = std::realloc(memory, newSize);
		if (!result)
		{
			state.MustStop = true;
			return nullptr;
		}
		state.UsedBytes = retained + newSize;
		state.PeakBytes = std::max(state.PeakBytes, state.UsedBytes);
		if (state.UsedBytes > state.SoftLimitBytes && !state.NeedsRecovery && !state.MustStop)
		{
			++state.SoftBreachCount;
			state.NeedsRecovery = true;
			state.MustStop = state.SoftBreachCount >= 2;
		}
		return result;
	}

	ScriptMemoryAction TrackingAllocator::CheckInterrupt(int gc) const noexcept
	{
		if (gc >= 0)
			return ScriptMemoryAction::Continue;
		if (m_State->Memory.MustStop)
			return ScriptMemoryAction::StopSession;
		return m_State->Memory.NeedsRecovery ? ScriptMemoryAction::RecoverInstance : ScriptMemoryAction::Continue;
	}

	Status TrackingAllocator::FinishRecovery()
	{
		auto& state = m_State->Memory;
		if (!state.NeedsRecovery)
			return MakeError(ErrorCode::InvalidState, "script allocator has no pending recovery");
		if (state.MustStop || state.UsedBytes > state.SoftLimitBytes)
		{
			state.MustStop = true;
			return MakeError(ErrorCode::Script, "script memory remains over its limit after collection");
		}
		state.NeedsRecovery = false;
		return {};
	}

	ScriptMemoryState TrackingAllocator::GetState() const noexcept
	{
		return m_State->Memory;
	}

}
