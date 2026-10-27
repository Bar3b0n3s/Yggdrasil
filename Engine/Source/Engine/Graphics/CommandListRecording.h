#pragma once

#include "Engine/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

// One recording of a command list, from its open to its execution (M18, C18.3; Docs/Decisions/0029-m18-contract.md
// decision 39; Architecture §8.14 item 10). NVRHI's open makes the list's command buffer reference the list itself, so a
// list that is opened and never executed keeps itself, and everything it recorded, alive: the device's recovery then
// refuses to go on (GraphicsDevice::Recover's live check), and the device's destruction reports a leak. NVRHI's calls
// throw a vk::SystemError out of the middle of a recording on a really lost device (an upload's first write asks the
// queue's timeline what completed), so every first-party list that records outside the application frame's, whose own
// device-loss handling closes and executes it (Application::OnFrameDeviceLost), records through one:
//
//     CommandListRecording recording(device, *commandList); // opens the list
//     commandList->writeBuffer(buffer, data, size);
//     const uint64_t submission = recording.Execute();      // closes and executes it
//
// A recording ends with Execute, also when what it recorded failed (the caller returns that error after Execute). One
// that ends without it, because a vk::SystemError (a device loss among them) unwound it, is closed and retired by the
// destructor without being submitted (GraphicsDevice::RetireCommandList): nothing it recorded runs, and NVRHI drops the
// list with the queue's next completed submission, on a lost device at the recovery. Main thread only; not copyable or
// movable.

namespace Engine {

	class GraphicsDevice;

	class CommandListRecording
	{
	public:
		// Opens `commandList`, which is not open. `device` is a documented back-reference and outlives the recording; the
		// recording holds a reference to the list.
		CommandListRecording(GraphicsDevice& device, nvrhi::ICommandList& commandList);
		// Retires the list unless Execute ran (see the file comment). Only a vk::SystemError unwinding the recording may end
		// it without Execute (asserted).
		~CommandListRecording();

		CommandListRecording(const CommandListRecording&) = delete;
		CommandListRecording& operator=(const CommandListRecording&) = delete;
		CommandListRecording(CommandListRecording&&) = delete;
		CommandListRecording& operator=(CommandListRecording&&) = delete;

		// Closes the list and executes it (GraphicsDevice::ExecuteCommandList, which submits nothing on a lost device), and
		// returns that submission's ID. Once per recording (asserted).
		uint64_t Execute();
	private:
		GraphicsDevice* m_Device = nullptr; // documented back-reference
		nvrhi::CommandListHandle m_CommandList{};
		int m_UncaughtExceptions = 0; // std::uncaught_exceptions() when the recording opened
		bool m_IsOpen = false;
		bool m_IsExecuted = false;
	};

}
