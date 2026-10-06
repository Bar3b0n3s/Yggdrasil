#pragma once

#include "Engine/Core/Base.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// CPU profiling (Architecture §4.13): ENGINE_PROFILE_SCOPE("Name") records a zone into the calling thread's ring buffer.
// GPU timer queries (GpuProfiler, M8) are merged into the same timeline. The timeline is readable for stats.get and
// exportable as Chrome trace JSON. Timing uses the steady clock: profiling data is diagnostic and never feeds simulated
// state. Scopes compile to nothing in Dist, which has no automation and no stats.get.

namespace Engine {

	// One completed zone.
	struct ProfileZone
	{
		std::string_view Name; // the string literal given to ENGINE_PROFILE_SCOPE, or a GPU pass name owned by the profiler
		uint64_t BeginNs = 0;  // steady-clock nanoseconds since Profiler::Initialize
		uint64_t EndNs = 0;
		uint32_t ThreadIndex = 0; // small stable index per thread in registration order; Profiler::GpuThreadIndex for GPU
		uint32_t Depth = 0;       // nesting depth on its thread, 0 for outermost
	};

	struct ProfilerSpecification
	{
		size_t ZonesPerThread = 65536; // capacity of each thread's ring buffer; the oldest zones are overwritten
	};

	// The profiler's buffers are process-level state, like the logger registry: ENGINE_PROFILE_SCOPE takes no context
	// argument, so each thread records into its own thread-local ring, registered with the process-wide profiler on
	// first use. ProcessContext (or the Tests main) initializes it.
	// Thread safety, with the same ordering rule as Log: Initialize and Shutdown create and free every ring, so they run
	// only while no other thread records or calls the profiler (at process start and end, before the first and after
	// the last worker thread; a test that cycles them does so on its only recording thread). Every other member is
	// thread-safe; recording is lock-free on the recording thread except for the first zone of a thread in each
	// initialization. Each Initialize starts a new generation: a thread whose cached ring belongs to an earlier
	// generation registers a fresh ring on its next zone instead of writing into freed memory.
	class Profiler
	{
	public:
		// The ThreadIndex of zones submitted by SubmitGpuZones.
		static constexpr uint32_t GpuThreadIndex = 0xffffffffu;

		Profiler() = delete;

		// Starts recording with the given buffer sizes. Calling it while initialized is a programmer error (asserted).
		static void Initialize(const ProfilerSpecification& specification = {});
		// Stops recording and frees every buffer. Idempotent. Zones recorded before Initialize or after Shutdown are
		// discarded.
		static void Shutdown();
		[[nodiscard]] static bool IsInitialized();

		// Names the calling thread in exports ("Main", "Job 3"). The name is copied.
		static void SetThreadName(std::string_view name);

		// Records a completed CPU zone on the calling thread; ProfileScope calls it. `name` must have static storage
		// duration (a string literal) and must not be null, and beginNs <= endNs (both asserted). A thread's ring
		// outlives the thread: the zones of the 16 most recently exited threads stay collectable until Shutdown, and
		// older ones are freed.
		static void RecordZone(const char* name, uint64_t beginNs, uint64_t endNs, uint32_t depth);

		// Merges GPU zones into the timeline (their names are copied and their ThreadIndex set to GpuThreadIndex). GPU
		// timestamps are driver data, so a zone that ends before it begins is dropped, not asserted.
		static void SubmitGpuZones(std::span<const ProfileZone> zones);

		// Every zone that ended at or after `sinceNs` and is still held, sorted by BeginNs, then ThreadIndex, then Depth.
		// GPU zone names stay valid until Shutdown.
		[[nodiscard]] static std::vector<ProfileZone> CollectZones(uint64_t sinceNs = 0);

		// The zones of CollectZones(sinceNs) as Chrome trace JSON ({"traceEvents": [...]} with "X" complete events in
		// microseconds, plus "M" thread-name metadata), loadable in chrome://tracing and Perfetto.
		[[nodiscard]] static std::string ExportChromeTrace(uint64_t sinceNs = 0);

		// Steady-clock nanoseconds since Initialize (0 when not initialized).
		[[nodiscard]] static uint64_t GetTimeNs();
	};

	// Records the zone from construction to destruction on the calling thread. Use ENGINE_PROFILE_SCOPE.
	class ProfileScope
	{
	public:
		// `name` must have static storage duration (a string literal).
		explicit ProfileScope(const char* name);
		~ProfileScope();

		ProfileScope(const ProfileScope&) = delete;
		ProfileScope& operator=(const ProfileScope&) = delete;
	private:
		const char* m_Name = nullptr;
		// The profiler initialization the zone began in (0: none). Declared, and so initialized, before m_BeginNs: the
		// generation is read before the begin time is sampled.
		uint64_t m_Generation = 0;
		uint64_t m_BeginNs = 0;
		uint32_t m_Depth = 0;
	};

}

#if defined(ENGINE_DIST)
	// Type-checks the name without evaluating it, like a compiled-out assert: a name held in a variable stays referenced,
	// so it never becomes an unused variable in Dist only.
	#define ENGINE_PROFILE_SCOPE(name) \
		do \
		{ \
			if constexpr (false) \
				static_cast<void>(static_cast<const char*>(name)); \
		} while (false)
#else
	#define ENGINE_PROFILE_SCOPE(name) const ::Engine::ProfileScope ENGINE_CONCAT(engineProfileScope, __LINE__)(name)
#endif
