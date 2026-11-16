#include "EnginePCH.h"
#include "Engine/Navigation/NavigationRuntime.h"

#include "Engine/Core/Assert.h"

#include <DetourAlloc.h>
#include <DetourAssert.h>

#include <atomic>
#include <cstdlib>
#include <format>
#include <source_location>
#include <string_view>

// Detour's process-level hooks (NavigationRuntime.h; Docs/Decisions/0045-m30-navigation.md decision 2).

namespace Engine {

	// Process-level state (Architecture §3 rule 5): set and cleared by the ProcessContext's Navigation step on the main
	// thread; the counter is changed by every thread that allocates through Detour.
	static std::atomic<bool> s_IsNavigationRuntimeInitialized{ false };
	static std::atomic<size_t> s_LiveNavigationAllocations{ 0 };

	namespace Utils {

		// dtAllocFunc: the C runtime's heap, as Detour's default, counted.
		static void* AllocateForDetour(size_t size, dtAllocHint /*hint*/)
		{
			void* memory = std::malloc(size);
			if (memory != nullptr)
				s_LiveNavigationAllocations.fetch_add(1, std::memory_order_relaxed);
			return memory;
		}

		// dtFreeFunc: frees what AllocateForDetour returned (Detour never frees null, but a null pointer is ignored).
		static void FreeForDetour(void* memory)
		{
			if (memory == nullptr)
				return;
			s_LiveNavigationAllocations.fetch_sub(1, std::memory_order_relaxed);
			std::free(memory);
		}

#if !defined(ENGINE_DIST)
		// dtAssertFailFunc: a failed Detour assert is a programmer error, reported to the engine's assert handler (which ends
		// the process) with Detour's file and line.
		static void OnDetourAssertFailed(const char* expression, const char* file, int line)
		{
			Detail::ReportAssertFailure(AssertKind::Assert, false, expression != nullptr ? expression : "",
				std::format("Detour: assertion failed ({}:{})", file != nullptr ? file : "(unknown file)", line), std::source_location::current());
		}
#endif

	}

	void InitializeNavigationRuntime()
	{
		ENGINE_CORE_ASSERT(!s_IsNavigationRuntimeInitialized.load(), "InitializeNavigationRuntime called twice without ShutdownNavigationRuntime");
		dtAllocSetCustom(&Utils::AllocateForDetour, &Utils::FreeForDetour);
#if !defined(ENGINE_DIST)
		// Dist defines NDEBUG, where Detour declares no assert hook (DetourAssert.h).
		dtAssertFailSetCustom(&Utils::OnDetourAssertFailed);
#endif
		s_IsNavigationRuntimeInitialized.store(true);
	}

	void ShutdownNavigationRuntime()
	{
		if (!s_IsNavigationRuntimeInitialized.load())
			return;
		const size_t live = s_LiveNavigationAllocations.load();
		ENGINE_CORE_VERIFY(live == 0,
			"ShutdownNavigationRuntime with {} live Detour allocation(s): destroy every navigation mesh and query before the process context", live);
#if !defined(ENGINE_DIST)
		dtAssertFailSetCustom(nullptr);
#endif
		// Null restores Detour's default allocator (DetourAlloc.cpp).
		dtAllocSetCustom(nullptr, nullptr);
		s_IsNavigationRuntimeInitialized.store(false);
	}

	bool IsNavigationRuntimeInitialized()
	{
		return s_IsNavigationRuntimeInitialized.load();
	}

	size_t GetLiveNavigationAllocationCount()
	{
		return s_LiveNavigationAllocations.load();
	}

}
