#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>

// Jolt's process-level state (Architecture §9.1, §4.1 level 1, §3 rule 5): its allocator hooks, its Trace and
// AssertFailed hooks, Factory::sInstance with the registered types, and one JobSystemThreadPool every PhysicsWorld steps
// on (PhysicsWorld::Step reaches the pool through Physics/Private/). ProcessContext initializes it once per process (its
// Physics step, after the crash handler and before the Vulkan loader) and shuts it down after every PhysicsWorld is gone.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 1).

namespace Engine {

	struct PhysicsEngineSpecification
	{
		// The fewest and the most worker threads Initialize starts by default, whatever the machine (§4.11).
		static constexpr uint32_t MinDefaultWorkerThreads = 1;
		static constexpr uint32_t MaxDefaultWorkerThreads = 4;
		// The most worker threads SetWorkerThreadCount and WorkerThreads accept.
		static constexpr uint32_t MaxWorkerThreads = 64;

		// The worker threads of the job system pool; nullopt: clamp(hardware concurrency - 2, MinDefaultWorkerThreads,
		// MaxDefaultWorkerThreads) (Architecture §4.11; an unknown hardware concurrency counts as 0). 0 is allowed here and
		// in SetWorkerThreadCount: Jolt's JobSystemThreadPool then runs every job on the stepping thread. The count never
		// changes results (§9.1, §9.7 "state hash identical with 0, 1 and 8 Jolt threads"). At most MaxWorkerThreads.
		std::optional<uint32_t> WorkerThreads{};
	};

	// Static access to Jolt's process-level state. Every member is main-thread-only (asserted in Debug and Release against
	// the thread that called Initialize), except IsInitialized and the live counters, which any thread may read.
	class PhysicsEngine
	{
	public:
		// The JobSystemThreadPool's capacities (§9.1: JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, threads)).
		static constexpr uint32_t MaxPhysicsJobs = 2048;
		static constexpr uint32_t MaxPhysicsBarriers = 8;

		PhysicsEngine() = delete;

		// In order (§9.1): JPH::RegisterDefaultAllocator(); JPH::Trace routed to the Engine logger (Info) and, where Jolt
		// has asserts (JPH_ENABLE_ASSERTS, Debug and Release), JPH::AssertFailed routed to the engine's assert handler (a
		// failed Jolt assert is a programmer error, reported like ENGINE_CORE_ASSERT); JPH::VerifyJoltVersionID(), whose
		// failure is fatal (FatalError, a consumer built with other Jolt defines would corrupt memory); Factory::sInstance;
		// JPH::RegisterTypes(); the JobSystemThreadPool with the specification's worker threads. Initializing twice without
		// Shutdown in between is a programmer error (asserted). Errors: InvalidArgument for WorkerThreads above
		// MaxWorkerThreads (nothing initialized).
		[[nodiscard]] static Status Initialize(const PhysicsEngineSpecification& specification);

		// Reverses Initialize: the pool, the types (JPH::UnregisterTypes), the factory, the hooks. Every PhysicsWorld must be
		// destroyed first (asserted with the live world and body counts: §4.1 "teardown asserts that every live-object counter
		// is zero"). Idempotent.
		static void Shutdown();

		[[nodiscard]] static bool IsInitialized();

		// The pool's worker thread count.
		[[nodiscard]] static uint32_t GetWorkerThreadCount();
		// Changes the pool's worker thread count (JobSystemThreadPool::SetNumThreads) between steps; results do not change
		// (the determinism tests step the same scene with 0, 1 and 8). Errors: InvalidState when not initialized;
		// InvalidArgument above MaxWorkerThreads.
		[[nodiscard]] static Status SetWorkerThreadCount(uint32_t count);

		// The PhysicsWorld objects alive in the process, and the bodies in them (character inner bodies included).
		[[nodiscard]] static uint32_t GetLiveWorldCount();
		[[nodiscard]] static uint32_t GetLiveBodyCount();
	};

}
