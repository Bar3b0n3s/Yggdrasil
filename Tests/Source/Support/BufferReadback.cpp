#include "TestsPCH.h"
#include "Support/BufferReadback.h"

#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <cstring>
#include <format>
#include <string>

// The GPU buffer readback of the tests (BufferReadback.h): a copy into a CPU-readable staging buffer on a command list of its
// own, whose submission is waited for (bounded, as Graphics/Readback waits for a texture copy) before the staging buffer is
// mapped.

namespace Engine {

	namespace Test {

		Result<std::vector<std::byte>> ReadBufferBytes(GraphicsDevice& device, nvrhi::IBuffer& buffer, uint64_t byteCount)
		{
			const nvrhi::BufferDesc& desc = buffer.getDesc();
			if (byteCount == 0 || byteCount > desc.byteSize)
			{
				return MakeError(ErrorCode::InvalidArgument, "cannot read {} bytes back from buffer '{}' of {} bytes", byteCount, desc.debugName,
					desc.byteSize);
			}
			if (device.IsLost())
				return MakeError(ErrorCode::Gpu, "cannot read back buffer '{}': the device is lost", desc.debugName);

			nvrhi::BufferDesc stagingDesc;
			stagingDesc.byteSize = byteCount;
			stagingDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
			stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
			stagingDesc.keepInitialState = true;
			stagingDesc.debugName = desc.debugName + ".Readback";
			ENGINE_TRY_ASSIGN(const nvrhi::BufferHandle staging, device.CreateBuffer(stagingDesc));
			ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
				device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
			uint64_t submission = 0;
			{
				// NVRHI takes the buffer's state from its kept initial state, and orders the copy after the prior submissions.
				CommandListRecording recording(device, *commandList);
				commandList->copyBuffer(staging, 0, &buffer, 0, byteCount);
				submission = recording.Execute();
			}
			WaitForSubmission(device, submission, std::format("the readback of buffer '{}'", desc.debugName));
			if (device.IsLost())
				return MakeError(ErrorCode::Gpu, "cannot read back buffer '{}': the device was lost during the copy", desc.debugName);

			nvrhi::IDevice* nvrhiDevice = device.GetNvrhiDevice();
			const void* mapped = nvrhiDevice->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
			if (mapped == nullptr)
				return MakeError(ErrorCode::Gpu, "cannot map the readback staging buffer of buffer '{}'", desc.debugName);
			std::vector<std::byte> bytes(static_cast<size_t>(byteCount));
			std::memcpy(bytes.data(), mapped, bytes.size());
			nvrhiDevice->unmapBuffer(staging);
			return bytes;
		}

	}

}
