#pragma once

#include "Engine/Core/Base.h"

#include <cstddef>

// Detour's process-level state (M30, Docs/Decisions/0045-m30-navigation.md decision 2; Architecture §3 rule 5): its
// allocator and assert hooks, which the ProcessContext's Navigation step sets once, before any navigation mesh exists,
// and puts back at its teardown.
//   - Allocator: dtAlloc and dtFree go to the C runtime's heap, as Detour's defaults do, through a counting hook, so the
//     step's teardown verifies that no Detour allocation outlived every navigation mesh and query (a leak check the
//     program's unhardened gates keep without AddressSanitizer, ADR 0042's answer 9).
//   - Assert: a failed dtAssert is a programmer error (the payload Detour reads is validated first-party, NavMeshData.h),
//     reported to the engine's assert handler like ENGINE_CORE_ASSERT, naming Detour's file and line. Dist builds with
//     NDEBUG, where dtAssert compiles to nothing and Detour has no assert hook to set.
// Recast, the editor's bake, has hooks of the same kind (EditorCore/Navigation/NavMeshBuilder.h), which the editor and the
// Tests main hand to the same step (ProcessContextSpecification::NavMeshBuildHooks), so the Runtime never links Recast.

namespace Engine {

	// Sets Detour's allocator and assert hooks. A second call without ShutdownNavigationRuntime is a programmer error
	// (asserted). Main thread, before any Detour object exists.
	void InitializeNavigationRuntime();

	// Puts Detour's default hooks back. Verifies (ENGINE_CORE_VERIFY, active in Dist) that no Detour allocation is still live:
	// a navigation mesh or query that outlived its owner would later free its memory through a hook that no longer counts it.
	// Does nothing when not initialized. Main thread.
	void ShutdownNavigationRuntime();

	// Whether InitializeNavigationRuntime ran without a ShutdownNavigationRuntime since.
	[[nodiscard]] bool IsNavigationRuntimeInitialized();

	// The number of Detour allocations not yet freed (dtAlloc minus dtFree of non-null blocks). Thread-safe.
	[[nodiscard]] size_t GetLiveNavigationAllocationCount();

}
