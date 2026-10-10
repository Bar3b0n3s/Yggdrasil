#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>

namespace Engine {

	// The protected-call owner disables the faulting instance before collection, or stops after unwinding the VM.
	enum class ScriptMemoryAction : uint8_t
	{
		Continue,
		RecoverInstance,
		StopSession
	};

	struct ScriptMemoryState
	{
		uint64_t UsedBytes = 0;
		uint64_t PeakBytes = 0;
		uint64_t SoftLimitBytes = 0;
		uint64_t HardLimitBytes = 0;
		uint32_t SoftBreachCount = 0;
		bool NeedsRecovery = false;
		bool MustStop = false;
	};

	// One VM's allocation accounting (§11.1), used by its owning thread only. The private VM adapter translates the
	// vendor allocation callback to Reallocate; neither this header nor callers need a vendor state or allocation type.
	// This object outlives the VM, which frees every block before the allocator is destroyed. It is not movable.
	class TrackingAllocator
	{
	public:
		static constexpr uint64_t HeadroomBytes = 16ull * 1024 * 1024;

		// Restricts construction to Create while allowing CreateScope to call the public constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class TrackingAllocator;
		};

		explicit TrackingAllocator(ConstructionKey key, uint64_t softLimitBytes); // use Create
		~TrackingAllocator();
		TrackingAllocator(const TrackingAllocator&) = delete;
		TrackingAllocator& operator=(const TrackingAllocator&) = delete;

		// limitMB >= 1; one MB is 1024 * 1024 bytes. InvalidArgument for zero or limits not representable by size_t,
		// including headroom. Accounting starts empty; headroom is an allowance, not an eagerly allocated buffer.
		[[nodiscard]] static Result<Scope<TrackingAllocator>> Create(uint32_t limitMB = 256);

		// Allocates/reallocates the VM-owned block, or frees it when newSize == 0 (returns null). memory == nullptr
		// ignores oldSize (the vendor can pass a type tag there); otherwise oldSize is the exact last successful size.
		// A failed growth leaves the old block and its accounting intact. No logging, allocating diagnostics or throwing.
		// Successful usage > soft latches a breach. A request reaching/exceeding hard, arithmetic overflow or host
		// allocation failure refuses growth and latches MustStop; shrinking/freeing remains possible after stop.
		[[nodiscard]] void* Reallocate(void* memory, size_t oldSize, size_t newSize) noexcept;

		// gc >= 0 ALWAYS returns Continue without consuming a pending fault. A non-GC interrupt returns the latched
		// action: first breach RecoverInstance, second/hard breach StopSession. Repeated polls/allocations before
		// recovery do not count as new breaches. The VM adapter raises a Memory ScriptError only outside a GC step.
		[[nodiscard]] ScriptMemoryAction CheckInterrupt(int gc) const noexcept;

		// Called after the first error has unwound, the instance is disabled and a full GC has completed. Usage must be
		// <= soft to rearm; otherwise latches stop and returns Script. Does not clear breach count or a terminal stop.
		// InvalidState without pending recovery. A later crossing is the second breach even after many healthy calls.
		[[nodiscard]] Status FinishRecovery();
		[[nodiscard]] ScriptMemoryState GetState() const noexcept;
	private:
		struct State;
	private:
		Scope<State> m_State{};
	};

}
