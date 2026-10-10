#include "EnginePCH.h"
#include "Engine/Scripting/TrackingAllocator.h"

namespace Engine {

	struct TrackingAllocator::State
	{
	};

	TrackingAllocator::TrackingAllocator(ConstructionKey key, uint64_t softLimitBytes)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(key);
		static_cast<void>(softLimitBytes);
	}

	TrackingAllocator::~TrackingAllocator()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Scope<TrackingAllocator>> TrackingAllocator::Create(uint32_t limitMB)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(limitMB);
		return MakeError(ErrorCode::Unsupported, "TrackingAllocator is an M13 contract stub");
	}

	void* TrackingAllocator::Reallocate(void* memory, size_t oldSize, size_t newSize) noexcept
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(memory);
		static_cast<void>(oldSize);
		static_cast<void>(newSize);
		return nullptr;
	}

	ScriptMemoryAction TrackingAllocator::CheckInterrupt(int gc) const noexcept
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(gc);
		return ScriptMemoryAction::Continue;
	}

	Status TrackingAllocator::FinishRecovery()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "TrackingAllocator is an M13 contract stub");
	}

	ScriptMemoryState TrackingAllocator::GetState() const noexcept
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
