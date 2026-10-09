#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Graphics/GpuProfiler.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Value snapshots for §8.2 and §13.7. Timings are diagnostic, never simulation input or part of a state hash.
namespace Engine {

	struct GpuTimingFrame; // Graphics-owned frame-aware result; parent integration adds its definition.

	struct RenderPassStats
	{
		std::string Name{}; // fixed pass-list order, stable names in ADR 0017
		double CpuMilliseconds = 0.0;
		double GpuMilliseconds = 0.0;
		bool GpuAvailable = false; // false means no completed sample, not zero GPU cost
		uint32_t DrawCalls = 0;
		uint32_t Dispatches = 0;
		uint32_t Triangles = 0;
	};

	struct RenderStats
	{
		bool Available = false;     // no completed render/renderer none -> false and empty passes
		uint64_t FrameIndex = 0;    // CPU counters' frame
		uint64_t GpuFrameIndex = 0; // completed GPU timing frame; never relabel old samples as current
		bool GpuAvailable = false;
		uint32_t Width = 0;
		uint32_t Height = 0;
		double CpuMilliseconds = 0.0;
		std::vector<RenderPassStats> Passes{};
		uint32_t VisibleMeshes = 0;
		uint32_t CulledSubmeshes = 0;
		uint32_t ShadowDraws = 0;
		uint32_t ShadowedSpotLights = 0;
		uint32_t DroppedSpotShadows = 0;
		uint32_t MemoryAllocationCount = 0; // tracked Vulkan device allocations, not NVRHI object count
		uint32_t MaxMemoryAllocationCount = 0;
	};

	struct RenderPassCounters
	{
		uint32_t DrawCalls = 0;
		uint32_t Dispatches = 0;
		uint32_t Triangles = 0;
	};

	// Borrowed per-view recording boundary, owned on the stack by SceneRenderer::Render. stats/profiler outlive it;
	// passes retain neither this object nor its command list. Main thread. No Graphics -> Renderer dependency.
	// nowSeconds is an owned callback to a monotonic CPU diagnostic clock, never a simulation clock. CPU-only tests
	// inject a manual clock and null profiler/list. Production passes supply the view's profiler and open list.
	class RenderRecordingContext
	{
	public:
		RenderRecordingContext(RenderStats& stats, std::function<double()> nowSeconds, GpuProfiler* profiler = nullptr);
		~RenderRecordingContext();
		RenderRecordingContext(const RenderRecordingContext&) = delete;
		RenderRecordingContext& operator=(const RenderRecordingContext&) = delete;
		// Flat, nonnested scopes; names/order are ADR0017's pass list. Copies name. A repeated name accumulates CPU time
		// and counters into the same row; GPU samples of that name sum. Null list means CPU-only (Prepare).
		// A non-null list always gets debug markers, even with a null profiler (busy timing slot); it outlives this scope.
		// Begin/End pairing, finite monotonic clock and no open scope at destruction are asserted programmer invariants.
		void BeginPass(std::string_view name, nvrhi::ICommandList* commandList = nullptr);
		// Ends GPU scope/marker and adds measured CPU duration plus saturating uint32 counters to the current CPU frame.
		// Every success/error path closes a begun scope; counts describe work actually recorded, not candidate draws.
		void EndPass(const RenderPassCounters& counters);
	private:
		struct State;
		Scope<State> m_State{};
	};

	// A CPU copy of one view's samples. No global or shared "last viewport": scene, game and captures own separate
	// histories. The per-view renderer owns a GpuProfiler (reuse Graphics' query pool) for command recording; this value
	// collector merges completed samples without polling/waiting itself. Main thread, never retains caller-owned sample storage.
	class RenderStatsHistory
	{
	public:
		RenderStatsHistory() = default;
		void PublishCpu(RenderStats stats);
		// Consumes only GpuProfiler::GetLastFrameResult(), never an inferred slot/frame plus GetLastFrameSamples().
		// Copies values, retains no reference. Unavailable clears GPU availability/timings (FrameIndex is then ignored);
		// available older frames are ignored. Match by pass name, summing repeated scopes; absent names stay unavailable.
		// PublishCpu retains source-tagged GPU values only for matching names, never invents timing for newly enabled passes.
		void PublishGpu(const GpuTimingFrame& frame);
		// Owns this value; reference valid until the next Publish/Reset. Reset on scene replacement, not screenshot capture.
		[[nodiscard]] const RenderStats& GetLatest() const;
		void Reset();
	private:
		RenderStats m_Latest{};
	};

}
