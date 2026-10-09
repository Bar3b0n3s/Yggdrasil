#include "EnginePCH.h"
#include "Engine/Graphics/GpuProfiler.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <utility>

namespace Engine {

	GpuProfiler::GpuProfiler(GraphicsDevice& device, uint32_t framesInFlight)
		: m_Device(&device)
	{
		// Frame slots index this vector.
		ENGINE_CORE_VERIFY(framesInFlight >= 1, "GpuProfiler needs at least one frame in flight");
		m_Frames.resize(framesInFlight);
	}

	GpuProfiler::~GpuProfiler() = default;

	void GpuProfiler::BeginFrame(uint32_t, uint64_t)
	{
		ENGINE_CONTRACT_STUB();
	}

	GpuTimingFrame GpuProfiler::GetLastFrameResult() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void GpuProfiler::BeginFrame(uint32_t frameSlot)
	{
		ENGINE_CORE_VERIFY(frameSlot < m_Frames.size(), "GpuProfiler::BeginFrame slot {} is out of range ({} frames in flight)", frameSlot,
			m_Frames.size());
		ENGINE_CORE_ASSERT(m_OpenScopes.empty(), "GpuProfiler::BeginFrame with {} scope(s) still open", m_OpenScopes.size());
		m_OpenScopes.clear();
		m_OpenScopeCommandList = nullptr;

		// The slot's previous frame has completed (FramePacer::BeginFrame waited for it), so its queries are readable and,
		// once read or dropped, free to record again.
		FrameRecord& frame = m_Frames[frameSlot];
		if (frame.IsRecorded)
			CollectFrame(frame);
		for (TimedScope& scope : frame.Scopes)
		{
			if (scope.Query != nullptr)
				m_FreeQueries.push_back(std::move(scope.Query));
		}
		frame.Scopes.clear();
		frame.TimedScopeCount = 0;
		frame.IsRecorded = true;
		m_FrameSlot = frameSlot;
		m_HasFrame = true;
	}

	void GpuProfiler::BeginScope(nvrhi::ICommandList& commandList, std::string_view name)
	{
		ENGINE_CORE_ASSERT(m_HasFrame, "GpuProfiler::BeginScope before the first BeginFrame");
		ENGINE_CORE_ASSERT(m_OpenScopes.empty() || m_OpenScopeCommandList == &commandList,
			"GpuProfiler scope '{}' opened on another command list than its enclosing scope", name);

		TimedScope scope = {
			.Name = std::string(name),
			.Depth = static_cast<uint32_t>(m_OpenScopes.size()),
		};
		commandList.beginMarker(scope.Name.c_str());

		FrameRecord& frame = m_Frames[m_FrameSlot];
		if (frame.TimedScopeCount < MaxScopesPerFrame)
		{
			scope.Query = AcquireQuery();
		}
		else if (!m_HasReportedScopeLimit)
		{
			m_HasReportedScopeLimit = true;
			ENGINE_CORE_WARN("GPU profiler: a frame opened more than {} scopes; '{}' and later scopes of such frames are not timed",
				MaxScopesPerFrame, name);
		}
		if (scope.Query != nullptr)
		{
			commandList.beginTimerQuery(scope.Query);
			++frame.TimedScopeCount;
		}

		m_OpenScopes.push_back(static_cast<uint32_t>(frame.Scopes.size()));
		m_OpenScopeCommandList = &commandList;
		frame.Scopes.push_back(std::move(scope));
	}

	void GpuProfiler::EndScope(nvrhi::ICommandList& commandList)
	{
		ENGINE_CORE_VERIFY(!m_OpenScopes.empty(), "GpuProfiler::EndScope without an open scope");
		ENGINE_CORE_ASSERT(m_OpenScopeCommandList == &commandList, "GpuProfiler::EndScope on another command list than BeginScope");

		const TimedScope& scope = m_Frames[m_FrameSlot].Scopes[m_OpenScopes.back()];
		m_OpenScopes.pop_back();
		if (m_OpenScopes.empty())
			m_OpenScopeCommandList = nullptr;
		if (scope.Query != nullptr)
			commandList.endTimerQuery(scope.Query);
		commandList.endMarker();
	}

	std::span<const GpuTimingSample> GpuProfiler::GetLastFrameSamples() const
	{
		return m_LastFrameSamples;
	}

	nvrhi::TimerQueryHandle GpuProfiler::AcquireQuery()
	{
		if (!m_FreeQueries.empty())
		{
			nvrhi::TimerQueryHandle query = std::move(m_FreeQueries.back());
			m_FreeQueries.pop_back();
			return query;
		}

		Result<nvrhi::TimerQueryHandle> created = m_Device->CreateTimerQuery();
		if (created.has_value())
			return std::move(*created);
		// Timing is diagnostics: a missing query costs that scope its timing, never the frame (GpuProfiler.h).
		if (!m_HasReportedQueryFailure)
		{
			m_HasReportedQueryFailure = true;
			ENGINE_CORE_WARN("GPU profiler: cannot create a timer query, so some scopes are not timed: {}", created.error());
		}
		return nullptr;
	}

	void GpuProfiler::CollectFrame(FrameRecord& frame)
	{
		nvrhi::IDevice* device = m_Device->GetNvrhiDevice();
		std::vector<double> durations(frame.Scopes.size(), 0.0);
		bool isComplete = true;
		for (size_t index = 0; index < frame.Scopes.size(); ++index)
		{
			nvrhi::ITimerQuery* query = frame.Scopes[index].Query;
			if (query == nullptr)
				continue;
			if (device->pollTimerQuery(query))
			{
				durations[index] = static_cast<double>(device->getTimerQueryTime(query)) * 1000.0;
			}
			else
			{
				// Not finished although its frame completed: dropped, never waited for (GpuProfiler.h).
				device->resetTimerQuery(query);
				isComplete = false;
			}
		}
		if (!isComplete)
			return;

		// NVRHI's timer queries measure durations, so the scopes are laid out back to back: each starts where its previous
		// sibling ended, or where its parent started (GpuTimingSample::StartMilliseconds). nextStart[d] is where the next
		// scope of depth d starts.
		m_LastFrameSamples.clear();
		std::vector<double> nextStart(1, 0.0);
		for (size_t index = 0; index < frame.Scopes.size(); ++index)
		{
			const TimedScope& scope = frame.Scopes[index];
			nextStart.resize(scope.Depth + 1, nextStart.back());
			const double start = nextStart[scope.Depth];
			nextStart[scope.Depth] = start + durations[index];
			nextStart.push_back(start);
			if (scope.Query != nullptr)
			{
				m_LastFrameSamples.push_back({
					.Name = scope.Name,
					.StartMilliseconds = start,
					.Milliseconds = durations[index],
					.Depth = scope.Depth,
				});
			}
		}
	}

}
