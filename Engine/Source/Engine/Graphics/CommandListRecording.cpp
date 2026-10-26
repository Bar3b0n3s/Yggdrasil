#include "EnginePCH.h"
#include "Engine/Graphics/CommandListRecording.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <exception>
#include <utility>

namespace Engine {

	CommandListRecording::CommandListRecording(GraphicsDevice& device, nvrhi::ICommandList& commandList)
		: m_Device(&device), m_CommandList(&commandList), m_UncaughtExceptions(std::uncaught_exceptions())
	{
		m_CommandList->open();
		m_IsOpen = true;
	}

	CommandListRecording::~CommandListRecording()
	{
		if (m_IsExecuted)
			return;
		// Every caller executes its recording, also after a failure of what it recorded; only an exception thrown out of an
		// NVRHI call during the recording gets here, on its way to the frame boundary or an automation method's handler.
		ENGINE_CORE_ASSERT(std::uncaught_exceptions() > m_UncaughtExceptions, "a command list recording ended without Execute");
		// The close records nothing that can fail on a lost device (vkEndCommandBuffer reports only out-of-memory), and the
		// retirement never reaches Vulkan's queue, so nothing here throws while an exception unwinds.
		if (std::exchange(m_IsOpen, false))
			m_CommandList->close();
		m_Device->RetireCommandList(*m_CommandList);
	}

	uint64_t CommandListRecording::Execute()
	{
		ENGINE_CORE_ASSERT(m_IsOpen && !m_IsExecuted, "CommandListRecording::Execute runs once per recording");
		// Closed from here on, and executed once the close returned, also when the close or the submission throws (out of
		// memory, which ends the process): the destructor neither closes the list again nor retires a submitted one.
		m_IsOpen = false;
		m_CommandList->close();
		m_IsExecuted = true;
		return m_Device->ExecuteCommandList(*m_CommandList);
	}

}
