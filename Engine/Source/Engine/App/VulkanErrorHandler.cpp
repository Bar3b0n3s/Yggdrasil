#include "EnginePCH.h"
#include "Engine/App/VulkanErrorHandler.h"

#include "Engine/App/EngineContext.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <vulkan/vulkan.hpp>

#include <format>

namespace Engine {

	SystemErrorHandler MakeVulkanSystemErrorHandler(EngineContext& context)
	{
		EngineContext* owner = &context;
		return [owner](const std::system_error& error, std::string_view method)
		{
			if (error.code().category() != vk::errorCategory())
				return;
			RaiseVulkanError(owner->GetGraphicsDevice(), static_cast<VkResult>(error.code().value()),
				std::format("Vulkan error in automation method '{}': {}", method, error.what()));
		};
	}

}
