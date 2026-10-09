#pragma once

#include "Engine/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// GPU pass timings (Architecture §8.2): each pass is wrapped in a debug marker (beginMarker) and a timer query from a
// pool per frame slot. Results are read back without waiting, FramesInFlight frames later, when the slot comes round
// again (pollTimerQuery); a query that has not finished by then is dropped for that frame instead of stalling.
//
// The samples feed the observability surfaces of §4.13 and §13.7 (stats.get's per-pass GPU timings, the profiler
// timeline and its Chrome trace export), which arrive with their milestones and read GetLastFrameSamples.

namespace Engine {

	class GraphicsDevice;

	// One timed scope of a completed frame.
	struct GpuTimingSample
	{
		std::string Name{};
		// Where the scope starts on the frame's GPU timeline, relative to the start of the frame's first scope. NVRHI's
		// timer queries measure durations, not timestamps, so the profiler lays the scopes out back to back: a scope starts
		// where its previous sibling ended, or where its parent started; the gaps between scopes are not measured
		// (Docs/Decisions/0009-m5-decisions.md). Enough to place the frame's passes on the profiler timeline in order.
		double StartMilliseconds = 0.0;
		double Milliseconds = 0.0;
		uint32_t Depth = 0; // nesting depth: 0 for a top-level scope
	};

	// Owned observation of the last BeginFrame collection attempt. Unavailable means no retired frame or an unfinished
	// query; Samples is then empty, never a previous frame's vector labelled with a new FrameIndex.
	struct GpuTimingFrame
	{
		bool Available = false;
		uint64_t FrameIndex = 0; // the collected frame's supplied identity; meaningful only when Available
		std::vector<GpuTimingSample> Samples{};
	};

	// Not copyable or movable; main thread only (§4.11). Application owns one for its frames (RenderContext::Profiler).
	class GpuProfiler
	{
	public:
		// The most scopes one frame may time; further scopes get their marker but no timer query (logged once).
		static constexpr uint32_t MaxScopesPerFrame = 128;

		// `device` is a documented back-reference and must outlive the profiler. `framesInFlight` >= 1 (asserted). Timer
		// queries are created on demand through GraphicsDevice::CreateTimerQuery; a failed creation disables timing for
		// that scope (logged once), never the frame.
		GpuProfiler(GraphicsDevice& device, uint32_t framesInFlight);
		~GpuProfiler();

		GpuProfiler(const GpuProfiler&) = delete;
		GpuProfiler& operator=(const GpuProfiler&) = delete;

		// Starts the frame of `frameSlot` (FramePacer::GetFrameSlot, after FramePacer::BeginFrame, so the slot's previous
		// frame has completed): collects that frame's finished queries into GetLastFrameSamples and recycles them.
		void BeginFrame(uint32_t frameSlot);
		// M9 per-view form: caller supplies the host frame identity and has already established that this slot's prior
		// submission retired. Collect without waiting, tag samples with the OLD recorded frame's identity, then start this
		// frame. The one-argument form uses a monotonically increasing local sequence; do not mix forms in one profiler.
		void BeginFrame(uint32_t frameSlot, uint64_t frameIndex);
		// Copy of the most recent collection attempt, including availability and source identity. Failure/no prior frame
		// produces unavailable/empty, not a stale successful result. Missing query scopes remain absent, not zero timings.
		[[nodiscard]] GpuTimingFrame GetLastFrameResult() const;

		// Opens a scope on `commandList`: beginMarker(name) and beginTimerQuery. Scopes nest and must be closed in reverse
		// order on the same command list (asserted).
		void BeginScope(nvrhi::ICommandList& commandList, std::string_view name);
		// Closes the innermost open scope: endTimerQuery and endMarker.
		void EndScope(nvrhi::ICommandList& commandList);

		// The samples of the most recently collected frame, in the order their scopes opened.
		[[nodiscard]] std::span<const GpuTimingSample> GetLastFrameSamples() const;
	private:
		// One scope a frame opened; Query is null when the scope is not timed (over MaxScopesPerFrame or no query).
		struct TimedScope
		{
			std::string Name{};
			uint32_t Depth = 0;
			nvrhi::TimerQueryHandle Query{};
		};

		// The scopes of the frame recorded in one slot, collected when the slot comes round again.
		struct FrameRecord
		{
			std::vector<TimedScope> Scopes{};
			uint32_t TimedScopeCount = 0;
			bool IsRecorded = false; // a frame used the slot since the last collection
		};
	private:
		// A recycled timer query, or a new one; null when creation fails (logged once).
		[[nodiscard]] nvrhi::TimerQueryHandle AcquireQuery();
		// Reads the frame's queries into the last samples, or drops the frame when one has not finished.
		void CollectFrame(FrameRecord& frame);
	private:
		GraphicsDevice* m_Device = nullptr; // documented back-reference: outlives the profiler
		std::vector<FrameRecord> m_Frames;  // one per frame slot
		std::vector<nvrhi::TimerQueryHandle> m_FreeQueries;
		std::vector<GpuTimingSample> m_LastFrameSamples;
		std::vector<uint32_t> m_OpenScopes;                    // indices into the current frame's scopes, innermost last
		nvrhi::ICommandList* m_OpenScopeCommandList = nullptr; // the command list of the open scopes, for the assertions
		uint32_t m_FrameSlot = 0;
		bool m_HasFrame = false;
		bool m_HasReportedScopeLimit = false;
		bool m_HasReportedQueryFailure = false;
	};

	// Opens a GpuProfiler scope for the lifetime of the object (RAII).
	class GpuProfileScope
	{
	public:
		// `profiler` and `commandList` are documented back-references for the scope's lifetime.
		GpuProfileScope(GpuProfiler& profiler, nvrhi::ICommandList& commandList, std::string_view name)
			: m_Profiler(&profiler), m_CommandList(&commandList)
		{
			m_Profiler->BeginScope(*m_CommandList, name);
		}

		~GpuProfileScope()
		{
			m_Profiler->EndScope(*m_CommandList);
		}

		GpuProfileScope(const GpuProfileScope&) = delete;
		GpuProfileScope& operator=(const GpuProfileScope&) = delete;
	private:
		GpuProfiler* m_Profiler = nullptr;
		nvrhi::ICommandList* m_CommandList = nullptr;
	};

}
