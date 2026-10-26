#include "EnginePCH.h"
#include "Engine/Renderer/RenderStats.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Profiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace Engine {

	namespace Utils {

		// The profiler zone of each stage: "Stage:" and its RenderStageToString name, in enumerator order. String literals,
		// since a CPU zone's name must have static storage duration (Profiler::RecordZone); "RenderRecordingContext: every
		// stage's zones are named after the stage" checks them against RenderStageToString.
		constexpr std::array<const char*, RenderStageCount> RenderStageZoneNames = {
			"Stage:Prepare",
			"Stage:Deform",
			"Stage:Shadows",
			"Stage:DepthNormal",
			"Stage:Ambient",
			"Stage:Forward",
			"Stage:Sprites",
			"Stage:HdrPost",
			"Stage:Tonemap",
			"Stage:LdrPost",
			"Stage:SceneOverlay",
			"Stage:Output",
			"Stage:EditorOverlay",
		};

		// The CPU zones compile to nothing in Dist, like ENGINE_PROFILE_SCOPE (Core/Profiler.h), and so does the GPU merge:
		// Dist has no automation, so nothing reads the profiler's timeline there.
#if defined(ENGINE_DIST)
		constexpr bool RecordsProfilerZones = false;
#else
		constexpr bool RecordsProfilerZones = true;
#endif

		// A GPU time in milliseconds as whole nanoseconds; GpuProfiler gives finite, non-negative values (asserted by the
		// caller).
		[[nodiscard]] static uint64_t MillisecondsToNanoseconds(double milliseconds)
		{
			return static_cast<uint64_t>(std::llround(milliseconds * 1.0e6));
		}

		// Merges an available frame's samples into the process profiler's timeline, each at the frame's CPU anchor plus its
		// start, with its depth (RenderStatsHistory::PublishGpu).
		static void SubmitGpuFrameToProfiler(const GpuTimingFrame& frame)
		{
			if (!Profiler::IsInitialized() || frame.Samples.empty())
				return;
			std::vector<ProfileZone> zones;
			zones.reserve(frame.Samples.size());
			for (const GpuTimingSample& sample : frame.Samples)
			{
				ENGINE_CORE_ASSERT(std::isfinite(sample.StartMilliseconds) && sample.StartMilliseconds >= 0.0 && std::isfinite(sample.Milliseconds)
						&& sample.Milliseconds >= 0.0,
					"Invalid GPU timing for '{}'", sample.Name);
				const uint64_t begin = frame.CpuBeginNs + MillisecondsToNanoseconds(sample.StartMilliseconds);
				zones.push_back({
					.Name = sample.Name,
					.BeginNs = begin,
					.EndNs = begin + MillisecondsToNanoseconds(sample.Milliseconds),
					.ThreadIndex = Profiler::GpuThreadIndex,
					.Depth = sample.Depth,
				});
			}
			Profiler::SubmitGpuZones(zones);
		}

	}

	void RenderStatsHistory::PublishCpu(RenderStats stats)
	{
		stats.GpuAvailable = m_Latest.GpuAvailable;
		stats.GpuFrameIndex = m_Latest.GpuFrameIndex;
		for (RenderPassStats& pass : stats.Passes)
		{
			pass.GpuAvailable = false;
			pass.GpuMilliseconds = 0.0;
			const auto previous = std::find_if(m_Latest.Passes.begin(), m_Latest.Passes.end(), [&pass](const RenderPassStats& candidate)
			{
				return candidate.Name == pass.Name;
			});
			if (previous != m_Latest.Passes.end())
			{
				pass.GpuAvailable = previous->GpuAvailable;
				pass.GpuMilliseconds = previous->GpuMilliseconds;
			}
		}
		m_Latest = std::move(stats);
	}

	void RenderStatsHistory::PublishGpu(const GpuTimingFrame& frame)
	{
		if (frame.Available && m_HasGpuFrame && frame.FrameIndex < m_LastGpuFrameIndex)
			return;
		// A frame reaches the profiler's timeline once, however often it is published (RenderStats.h).
		const bool isNewFrame = frame.Available && (!m_HasGpuFrame || frame.FrameIndex > m_LastGpuFrameIndex);

		m_Latest.GpuAvailable = frame.Available;
		m_Latest.GpuFrameIndex = frame.Available ? frame.FrameIndex : 0;
		for (RenderPassStats& pass : m_Latest.Passes)
		{
			pass.GpuAvailable = false;
			pass.GpuMilliseconds = 0.0;
			if (!frame.Available)
				continue;
			for (const GpuTimingSample& sample : frame.Samples)
			{
				if (sample.Name != pass.Name)
					continue;
				ENGINE_CORE_ASSERT(std::isfinite(sample.Milliseconds) && sample.Milliseconds >= 0.0, "Invalid GPU timing for '{}'", sample.Name);
				pass.GpuMilliseconds += sample.Milliseconds;
				pass.GpuAvailable = true;
			}
		}
		if (frame.Available)
		{
			m_LastGpuFrameIndex = frame.FrameIndex;
			m_HasGpuFrame = true;
		}
		if constexpr (Utils::RecordsProfilerZones)
		{
			if (isNewFrame)
				Utils::SubmitGpuFrameToProfiler(frame);
		}
	}

	const RenderStats& RenderStatsHistory::GetLatest() const
	{
		return m_Latest;
	}

	void RenderStatsHistory::Reset()
	{
		m_Latest = {};
		m_LastGpuFrameIndex = 0;
		m_HasGpuFrame = false;
	}

	struct RenderRecordingContext::State
	{
		RenderStats* Stats = nullptr; // back-reference for this stack-owned recording context
		std::function<double()> NowSeconds{};
		GpuProfiler* Profiler = nullptr;            // same lifetime as Stats; optional for CPU-only collection
		nvrhi::ICommandList* CommandList = nullptr; // borrowed only between BeginPass and EndPass
		size_t PassIndex = 0;
		double StartSeconds = 0.0;
		double LastSeconds = 0.0;
		bool HasClockSample = false;
		bool IsOpen = false;
		// The open stage (BeginStage to EndStage): its command list (borrowed, null for a CPU-only stage) and its CPU zone in
		// the process profiler.
		nvrhi::ICommandList* StageCommandList = nullptr;
		std::optional<ProfileScope> StageZone{};
		RenderStage Stage = RenderStage::Prepare;
		bool IsStageOpen = false;
		// std::uncaught_exceptions() when the context was made: more at its destruction means an exception unwinds the recording.
		int UncaughtExceptions = 0;
	};

	RenderRecordingContext::RenderRecordingContext(RenderStats& stats, std::function<double()> nowSeconds, GpuProfiler* profiler)
		: m_State(CreateScope<State>())
	{
		ENGINE_CORE_VERIFY(static_cast<bool>(nowSeconds), "RenderRecordingContext needs a diagnostic clock");
		m_State->Stats = &stats;
		m_State->NowSeconds = std::move(nowSeconds);
		m_State->Profiler = profiler;
		m_State->UncaughtExceptions = std::uncaught_exceptions();
	}

	RenderRecordingContext::~RenderRecordingContext()
	{
		State& state = *m_State;
		// M18 (the milestone review; Docs/Decisions/0029-m18-contract.md decision 39): a vk::SystemError thrown out of NVRHI in
		// the middle of a pass or a stage (a device loss that an upload's first write meets) unwinds the recording on its way to
		// the frame boundary or an automation method's handler, which recover the device. The open stage's CPU zone ends here;
		// its GPU scopes stay open on the profiler, which the device-loss release replaces (SceneRenderer::ReleaseGpuObjects),
		// and on the list, which is retired without being submitted (CommandListRecording). Only then may a pass or a stage
		// still be open.
		if (std::uncaught_exceptions() > state.UncaughtExceptions)
		{
			state.StageZone.reset();
			return;
		}
		ENGINE_CORE_VERIFY(!state.IsOpen, "RenderRecordingContext destroyed with an open pass");
		ENGINE_CORE_VERIFY(!state.IsStageOpen, "RenderRecordingContext destroyed with stage {} open", RenderStageToString(state.Stage));
	}

	void RenderRecordingContext::BeginPass(std::string_view name, nvrhi::ICommandList* commandList)
	{
		State& state = *m_State;
		ENGINE_CORE_VERIFY(!state.IsOpen, "RenderRecordingContext does not allow nested passes");
		ENGINE_CORE_ASSERT(!name.empty(), "RenderRecordingContext needs a pass name");
		const double nowSeconds = state.NowSeconds();
		ENGINE_CORE_VERIFY(std::isfinite(nowSeconds) && (!state.HasClockSample || nowSeconds >= state.LastSeconds),
			"RenderRecordingContext needs a finite monotonic clock");
		std::vector<RenderPassStats>& passes = state.Stats->Passes;
		const auto found = std::find_if(passes.begin(), passes.end(), [name](const RenderPassStats& pass)
		{
			return pass.Name == name;
		});
		state.PassIndex = static_cast<size_t>(found - passes.begin());
		if (found == passes.end())
			passes.push_back({ .Name = std::string(name) });
		state.StartSeconds = nowSeconds;
		state.LastSeconds = nowSeconds;
		state.HasClockSample = true;
		state.CommandList = commandList;
		state.IsOpen = true;
		if (commandList != nullptr)
		{
			if (state.Profiler != nullptr)
				state.Profiler->BeginScope(*commandList, passes[state.PassIndex].Name);
			else
				commandList->beginMarker(passes[state.PassIndex].Name.c_str());
		}
	}

	void RenderRecordingContext::EndPass(const RenderPassCounters& counters)
	{
		State& state = *m_State;
		ENGINE_CORE_VERIFY(state.IsOpen, "RenderRecordingContext::EndPass without an open pass");
		if (state.CommandList != nullptr)
		{
			if (state.Profiler != nullptr)
				state.Profiler->EndScope(*state.CommandList);
			else
				state.CommandList->endMarker();
		}
		const double nowSeconds = state.NowSeconds();
		ENGINE_CORE_VERIFY(std::isfinite(nowSeconds) && nowSeconds >= state.LastSeconds,
			"RenderRecordingContext needs a finite monotonic clock");
		const double elapsedMilliseconds = (nowSeconds - state.StartSeconds) * 1000.0;
		ENGINE_CORE_VERIFY(std::isfinite(elapsedMilliseconds), "RenderRecordingContext elapsed time overflowed");
		RenderPassStats& pass = state.Stats->Passes[state.PassIndex];
		pass.CpuMilliseconds += elapsedMilliseconds;
		const auto addSaturated = [](uint32_t a, uint32_t b)
		{
			return static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(a) + b, std::numeric_limits<uint32_t>::max()));
		};
		pass.DrawCalls = addSaturated(pass.DrawCalls, counters.DrawCalls);
		pass.Dispatches = addSaturated(pass.Dispatches, counters.Dispatches);
		pass.Triangles = addSaturated(pass.Triangles, counters.Triangles);
		state.LastSeconds = nowSeconds;
		state.CommandList = nullptr;
		state.IsOpen = false;
	}

	void RenderRecordingContext::BeginStage(RenderStage stage, nvrhi::ICommandList* commandList)
	{
		State& state = *m_State;
		const auto index = static_cast<size_t>(stage);
		ENGINE_CORE_VERIFY(index < Utils::RenderStageZoneNames.size(), "RenderRecordingContext::BeginStage of an unknown stage {}", index);
		ENGINE_CORE_VERIFY(!state.IsStageOpen, "RenderRecordingContext::BeginStage of {} while stage {} is open", RenderStageToString(stage),
			RenderStageToString(state.Stage));
		ENGINE_CORE_VERIFY(!state.IsOpen, "RenderRecordingContext::BeginStage of {} while a pass is open", RenderStageToString(stage));
		const char* name = Utils::RenderStageZoneNames[index];
		state.Stage = stage;
		state.StageCommandList = commandList;
		state.IsStageOpen = true;
		if constexpr (Utils::RecordsProfilerZones)
			state.StageZone.emplace(name);
		if (commandList != nullptr)
		{
			// The stage's pass scopes nest in its GPU zone (depth 1); the marker names the stage in captures and device-fault
			// reports whether or not this frame is timed.
			if (state.Profiler != nullptr)
				state.Profiler->BeginScope(*commandList, name);
			else
				commandList->beginMarker(name);
		}
	}

	void RenderRecordingContext::EndStage()
	{
		State& state = *m_State;
		ENGINE_CORE_VERIFY(state.IsStageOpen, "RenderRecordingContext::EndStage without an open stage");
		ENGINE_CORE_VERIFY(!state.IsOpen, "RenderRecordingContext::EndStage of {} while a pass is open", RenderStageToString(state.Stage));
		if (state.StageCommandList != nullptr)
		{
			if (state.Profiler != nullptr)
				state.Profiler->EndScope(*state.StageCommandList);
			else
				state.StageCommandList->endMarker();
		}
		state.StageZone.reset();
		state.StageCommandList = nullptr;
		state.IsStageOpen = false;
	}

}
