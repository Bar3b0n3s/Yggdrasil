#include "EnginePCH.h"
#include "Engine/Graphics/VulkanDispatch.h"

#include <vulkan/vulkan.hpp>

// M5 contract stub (Roadmap rule 3): stream A (loader, device, selection, creation wrappers, host image upload) implements
// the process-level loader: ENGINE_VULKAN_LOADER, vk::detail::DynamicLoader (whose std::runtime_error this file catches,
// §4.6) and VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr). Until then no loader is ever initialized, so
// ProcessContext reports the Vulkan loader as unavailable.

namespace Engine {

	Status VulkanDispatch::Initialize()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "VulkanDispatch::Initialize is not implemented yet");
	}

	void VulkanDispatch::Shutdown()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool VulkanDispatch::IsInitialized()
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	VulkanGetInstanceProcAddrFunction VulkanDispatch::GetInstanceProcAddr()
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	uint32_t VulkanDispatch::GetLoaderApiVersion()
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}

// The one translation unit with the storage of vulkan.hpp's default dispatcher (Architecture §8.1; Vendor/NVRHI/VENDOR.md
// "Dynamic dispatcher"): NVRHI and every engine Vulkan call dispatch through it. The macro defines
// vk::detail::defaultDispatchLoaderDynamic at namespace scope.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
