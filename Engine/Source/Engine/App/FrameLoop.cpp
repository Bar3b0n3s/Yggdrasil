#include "EnginePCH.h"
#include "Engine/App/FrameLoop.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"

// M2 contract stub (Roadmap rule 3): stream D (application and frame loop) implements the frame sequence, exit requests,
// the minimized wait and the headless throttle. Until then no frame runs and Run returns ExitCode::Failed at once.

namespace Engine {

	FrameLoop::FrameLoop(EngineContext& /*context*/, IFrameLoopClient& /*client*/, Scope<Clock> /*clock*/,
		const FrameLoopSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	FrameLoop::~FrameLoop() = default;

	void FrameLoop::RunFrame()
	{
		ENGINE_CONTRACT_STUB();
	}

	int FrameLoop::Run()
	{
		ENGINE_CONTRACT_STUB();
		return ExitCode::Failed;
	}

	void FrameLoop::RequestExit(int /*exitCode*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool FrameLoop::IsExitRequested() const
	{
		ENGINE_CONTRACT_STUB();
		return true;
	}

	int FrameLoop::GetExitCode() const
	{
		ENGINE_CONTRACT_STUB();
		return ExitCode::Failed;
	}

	uint64_t FrameLoop::GetFrameCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	const FixedStepScheduler& FrameLoop::GetScheduler() const
	{
		ENGINE_CONTRACT_STUB();
		static const FixedStepScheduler StubScheduler{ FrameLoopConfig() };
		return StubScheduler;
	}

	const Clock& FrameLoop::GetClock() const
	{
		ENGINE_CONTRACT_STUB();
		static const SystemClock StubClock;
		return StubClock;
	}

	const FrameTime& FrameLoop::GetLastFrameTime() const
	{
		ENGINE_CONTRACT_STUB();
		static const FrameTime StubFrameTime;
		return StubFrameTime;
	}

}
