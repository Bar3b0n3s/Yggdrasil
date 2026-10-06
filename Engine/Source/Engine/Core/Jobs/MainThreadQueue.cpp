#include "EnginePCH.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"

// M1 contract stub (Roadmap rule 3): stream D implements the FIFO and Drain. Until then posted tasks are dropped.

namespace Engine {

	MainThreadQueue::MainThreadQueue()
		: m_MainThread(std::this_thread::get_id())
	{
	}

	MainThreadQueue::~MainThreadQueue() = default;

	void MainThreadQueue::Post(Task /*task*/)
	{
	}

	uint32_t MainThreadQueue::Drain()
	{
		return 0;
	}

	size_t MainThreadQueue::GetPendingCount() const
	{
		return 0;
	}

	bool MainThreadQueue::IsMainThread() const
	{
		return std::this_thread::get_id() == m_MainThread;
	}

}
