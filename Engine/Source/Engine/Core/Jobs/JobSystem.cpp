#include "EnginePCH.h"
#include "Engine/Core/Jobs/JobSystem.h"

// M1 contract stub (Roadmap rule 3): stream D implements the worker pool, cancellation and WaitIdle. Until then every
// job runs inline on the submitting thread, whatever the worker count.

namespace Engine {

	JobSystem::JobSystem(uint32_t workerCount, MainThreadQueue& mainThreadQueue)
		: m_MainThreadQueue(mainThreadQueue), m_WorkerCount(workerCount)
	{
	}

	JobSystem::~JobSystem() = default;

	uint32_t JobSystem::GetDefaultWorkerCount()
	{
		return 1;
	}

	void JobSystem::WaitIdle()
	{
	}

	void JobSystem::Enqueue(Scope<Detail::JobTask> task)
	{
		task->Run();
	}

	void JobSystem::WorkerMain()
	{
	}

}
