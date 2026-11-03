#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <functional>
#include <utility>

// Out-of-memory buffer creations for the tests of a buffer owner's failure path: moved here from GpuResourceCacheTests.cpp's
// index-buffer case, because the scene renderer's instance and light-cluster buffers use it too (M23,
// Docs/Decisions/0034-m23-contract.md decision 20).

namespace Engine {

	namespace Test {

		// While alive, the device's buffer creations whose Vulkan create info `fails` selects fail as out of device memory
		// without reaching Vulkan (NVRHI's createBuffer then returns null without a message, so GraphicsDevice::CreateBuffer
		// returns a Gpu error, out-of-memory on a device that is not lost), and every other buffer is created: the
		// dispatcher's vkCreateBuffer, which NVRHI calls through, is replaced. At most one at a time; main thread, around the
		// calls under test; not copyable or movable.
		class BufferCreationFails
		{
		public:
			using Selector = std::function<bool(const VkBufferCreateInfo& info)>;

			explicit BufferCreationFails(Selector fails)
			{
				s_Fails = std::move(fails);
				s_FailedCount = 0;
				s_CreateBuffer = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateBuffer;
				VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateBuffer = &CreateBuffer;
			}

			~BufferCreationFails()
			{
				VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateBuffer = s_CreateBuffer;
				s_CreateBuffer = nullptr;
				s_Fails = nullptr;
			}

			BufferCreationFails(const BufferCreationFails&) = delete;
			BufferCreationFails& operator=(const BufferCreationFails&) = delete;
			BufferCreationFails(BufferCreationFails&&) = delete;
			BufferCreationFails& operator=(BufferCreationFails&&) = delete;

			// The creations it failed so far.
			[[nodiscard]] uint32_t GetFailedCount() const { return s_FailedCount; }
		private:
			static VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* allocator,
				VkBuffer* buffer)
			{
				if (s_Fails(*info))
				{
					++s_FailedCount;
					return VK_ERROR_OUT_OF_DEVICE_MEMORY;
				}
				return s_CreateBuffer(device, info, allocator, buffer);
			}

			// The state while one is alive, which CreateBuffer, a C function pointer, cannot capture.
			static inline PFN_vkCreateBuffer s_CreateBuffer = nullptr;
			static inline Selector s_Fails{};
			static inline uint32_t s_FailedCount = 0;
		};

	}

}
