#include "TestsPCH.h"
#include "Engine/Navigation/NavigationRuntime.h"

#include "Engine/App/ProcessContext.h"
#include "Support/DeathTest.h"

#include <DetourAlloc.h>
#include <DetourAssert.h>

#include <doctest/doctest.h>

#include <algorithm>

// Detour's process-level hooks (Navigation/NavigationRuntime.h; Docs/Decisions/0045-m30-navigation.md decision 2).

namespace Engine {

	ENGINE_DEATH_TEST("Navigation/DetourAssertReachesTheEngineHandler")
	{
		// A condition the compiler cannot fold: no Detour allocation is live in a fresh child process.
		const size_t live = GetLiveNavigationAllocationCount();
		dtAssert(live == 1);
	}

	ENGINE_DEATH_TEST("Navigation/DetourShutDownUnderLiveAllocation")
	{
		void* block = dtAlloc(16, DT_ALLOC_PERM);
		if (block != nullptr)
			ShutdownNavigationRuntime();
	}

	TEST_SUITE("Navigation")
	{
		TEST_CASE("NavigationRuntime: the process context's Navigation step sets Detour's hooks")
		{
			const ProcessContext* context = ProcessContext::GetCurrent();
			REQUIRE(context != nullptr);
			const auto steps = context->GetSteps();
			CHECK(std::find(steps.begin(), steps.end(), ProcessContextStep::Navigation) != steps.end());
			CHECK(IsNavigationRuntimeInitialized());
			// No navigation mesh or query outlives its test.
			CHECK(GetLiveNavigationAllocationCount() == 0);
		}

		TEST_CASE("NavigationRuntime: the allocator hook counts every live Detour allocation")
		{
			void* first = dtAlloc(64, DT_ALLOC_TEMP);
			REQUIRE(first != nullptr);
			void* second = dtAlloc(32, DT_ALLOC_PERM);
			REQUIRE(second != nullptr);
			CHECK(GetLiveNavigationAllocationCount() == 2);
			dtFree(first);
			dtFree(second);
			CHECK(GetLiveNavigationAllocationCount() == 0);
			// Freeing null frees nothing and counts nothing.
			dtFree(nullptr);
			CHECK(GetLiveNavigationAllocationCount() == 0);
		}

		TEST_CASE("NavigationRuntime: a failed Detour assert reaches the engine's assert handler")
		{
			ENGINE_CHECK_DEATH("Navigation/DetourAssertReachesTheEngineHandler", "Detour: assertion failed");
		}

		TEST_CASE("NavigationRuntime: the teardown refuses a live Detour allocation")
		{
			ENGINE_CHECK_DEATH("Navigation/DetourShutDownUnderLiveAllocation", "live Detour allocation");
		}
	}

}
