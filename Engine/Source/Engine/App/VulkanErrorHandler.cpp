#include "EnginePCH.h"
#include "Engine/App/VulkanErrorHandler.h"

#include "Engine/App/EngineContext.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <vulkan/vulkan.hpp>

#include <format>
#include <optional>
#include <string>

namespace Engine {

	SystemErrorHandler MakeVulkanSystemErrorHandler(EngineContext& context)
	{
		EngineContext* owner = &context;
		return [owner](const std::system_error& error, std::string_view method) -> std::optional<Error>
		{
			if (error.code().category() != vk::errorCategory())
				return std::nullopt;
			const VkResult result = static_cast<VkResult>(error.code().value());
			const std::string message = std::format("Vulkan error in automation method '{}': {}", method, error.what());
			// A device loss, and any Vulkan error on a lost device (NVRHI's calls fail with other codes there too, the loss's
			// consequences), is recovered at the next frame boundary: the request fails, and the server keeps serving
			// (decision 28). Every other Vulkan error still ends the process.
			GraphicsDevice* device = owner->GetGraphicsDevice();
			if (device != nullptr && (result == VK_ERROR_DEVICE_LOST || device->IsLost()))
			{
				device->ReportDeviceLost(message);
				return Error(ErrorCode::Gpu, std::format("the GPU device was lost during '{}'; it is recovered at the next frame boundary", method));
			}
			RaiseVulkanError(device, result, message);
		};
	}

}
