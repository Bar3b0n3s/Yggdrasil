#include "EnginePCH.h"
#include "Engine/Renderer/RenderStats.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Engine {

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
	};

	RenderRecordingContext::RenderRecordingContext(RenderStats& stats, std::function<double()> nowSeconds, GpuProfiler* profiler)
		: m_State(CreateScope<State>())
	{
		ENGINE_CORE_VERIFY(static_cast<bool>(nowSeconds), "RenderRecordingContext needs a diagnostic clock");
		m_State->Stats = &stats;
		m_State->NowSeconds = std::move(nowSeconds);
		m_State->Profiler = profiler;
	}

	RenderRecordingContext::~RenderRecordingContext()
	{
		ENGINE_CORE_VERIFY(!m_State->IsOpen, "RenderRecordingContext destroyed with an open pass");
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

}
