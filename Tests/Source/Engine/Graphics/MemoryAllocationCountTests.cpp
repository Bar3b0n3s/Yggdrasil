#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsDevice.h"

#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/VulkanDispatch.h"
#include "Support/HeadlessGpuFixture.h"

#include <vulkan/vulkan.hpp>

#include <array>

namespace Engine {

	namespace {

		// An injected native boundary, never installed into the driver dispatcher. It proves exact forwarding and
		// failure accounting without invalid Vulkan inputs, failed driver callbacks or real memory exhaustion.
		struct AllocationReturnProbe
		{
			VkResult Result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
			uint32_t Allocations = 0;
			uint32_t Frees = 0;
			VkDeviceMemory Memory = VK_NULL_HANDLE;
			const VkMemoryAllocateInfo* Info = nullptr;
			const VkAllocationCallbacks* Allocator = nullptr;
		};

		VKAPI_ATTR VkResult VKAPI_CALL ReturnAllocationResult(VkDevice device, const VkMemoryAllocateInfo* info,
			const VkAllocationCallbacks* allocator, VkDeviceMemory* memory)
		{
			auto& probe = *static_cast<AllocationReturnProbe*>(allocator->pUserData);
			CHECK(device == VK_NULL_HANDLE);
			++probe.Allocations;
			probe.Info = info;
			probe.Allocator = allocator;
			*memory = probe.Result == VK_SUCCESS ? probe.Memory : VK_NULL_HANDLE;
			return probe.Result;
		}

		VKAPI_ATTR void VKAPI_CALL ObserveFree(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator)
		{
			auto& probe = *static_cast<AllocationReturnProbe*>(allocator->pUserData);
			CHECK(device == VK_NULL_HANDLE);
			CHECK((memory == VK_NULL_HANDLE || memory == probe.Memory));
			++probe.Frees;
		}

		nvrhi::TextureDesc AllocationTextureDescription()
		{
			nvrhi::TextureDesc desc;
			desc.width = 8;
			desc.height = 8;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.debugName = "AllocationCounter";
			return desc;
		}

		// Owns a real, small allocation made through the engine's hooked C dispatcher. The fixture/device outlives it.
		class NativeMemoryProbe
		{
		public:
			explicit NativeMemoryProbe(GraphicsDevice& device)
				: m_Device(device.GetVulkanDevice())
			{
				VkPhysicalDeviceMemoryProperties properties{};
				VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties(device.GetVulkanPhysicalDevice(), &properties);
				uint32_t memoryType = 0;
				while (memoryType < properties.memoryTypeCount && (properties.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
					++memoryType;
				REQUIRE(memoryType < properties.memoryTypeCount);
				VkMemoryAllocateInfo info{};
				info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
				info.allocationSize = 4096;
				info.memoryTypeIndex = memoryType;
				const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateMemory(m_Device, &info, nullptr, &m_Memory);
				REQUIRE(result == VK_SUCCESS);
			}
			~NativeMemoryProbe()
			{
				VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory(m_Device, m_Memory, nullptr);
			}
			NativeMemoryProbe(const NativeMemoryProbe&) = delete;
			NativeMemoryProbe& operator=(const NativeMemoryProbe&) = delete;
		private:
			VkDevice m_Device = VK_NULL_HANDLE;
			VkDeviceMemory m_Memory = VK_NULL_HANDLE;
		};

	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("VulkanMemoryAllocationTracker: native failure results and null frees preserve the count")
		{
			AllocationReturnProbe probe;
			VkAllocationCallbacks allocator{};
			allocator.pUserData = &probe;
			VkMemoryAllocateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			info.allocationSize = 4096;
			// Opaque token used only by these injected entry points, never by a Vulkan driver.
			probe.Memory = reinterpret_cast<VkDeviceMemory>(&probe);
			VulkanMemoryAllocationTracker tracker(VK_NULL_HANDLE, &ReturnAllocationResult, &ObserveFree);
			probe.Result = VK_SUCCESS;
			VkDeviceMemory live = VK_NULL_HANDLE;
			REQUIRE(tracker.AllocateMemory(VK_NULL_HANDLE, &info, &allocator, &live) == VK_SUCCESS);
			CHECK(live == probe.Memory);
			CHECK(tracker.GetCount() == 1);
			for (VkResult result : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_TOO_MANY_OBJECTS })
			{
				probe.Result = result;
				VkDeviceMemory memory = VK_NULL_HANDLE;
				CHECK(tracker.AllocateMemory(VK_NULL_HANDLE, &info, &allocator, &memory) == result);
				CHECK(tracker.GetCount() == 1); // failure must not erase an earlier live allocation
				CHECK(probe.Info == &info);
				CHECK(probe.Allocator == &allocator);
			}
			tracker.FreeMemory(VK_NULL_HANDLE, VK_NULL_HANDLE, &allocator);
			CHECK(tracker.GetCount() == 1);
			tracker.FreeMemory(VK_NULL_HANDLE, live, &allocator);
			CHECK(tracker.GetCount() == 0);
			CHECK(probe.Allocations == 4);
			CHECK(probe.Frees == 2);
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("GraphicsDevice: allocation count follows actual NVRHI and host-image memory")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const uint32_t before = device.GetMemoryAllocationCount();
			{
				NativeMemoryProbe native(device);
				CHECK(device.GetMemoryAllocationCount() == before + 1);
				VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory(device.GetVulkanDevice(), VK_NULL_HANDLE, nullptr);
				CHECK(device.GetMemoryAllocationCount() == before + 1);
			}
			CHECK(device.GetMemoryAllocationCount() == before);
			{
				auto sampler = device.CreateSampler({});
				REQUIRE(sampler);
				CHECK(device.GetMemoryAllocationCount() == before);
				nvrhi::BufferDesc desc;
				desc.byteSize = 256;
				auto buffer = device.CreateBuffer(desc);
				REQUIRE(buffer);
				CHECK(device.GetMemoryAllocationCount() == before + 1);
				auto texture = device.CreateTexture(AllocationTextureDescription());
				REQUIRE(texture);
				CHECK(device.GetMemoryAllocationCount() == before + 2);
				auto staging = device.CreateStagingTexture(AllocationTextureDescription(), nvrhi::CpuAccessMode::Read);
				REQUIRE(staging);
				CHECK(device.GetMemoryAllocationCount() == before + 3);
			}
			device.RunGarbageCollection();
			CHECK(device.GetMemoryAllocationCount() == before);
			for (const auto preferred : { TextureUploadPath::Staging, TextureUploadPath::HostImageCopy })
			{
				const std::array<std::byte, 8 * 8 * 4> pixels{};
				const std::array<TextureSubresourceData, 1> data{ { { .Data = pixels } } };
				{
					auto first = device.GetHostImageUpload().CreateTexture(AllocationTextureDescription(), data, preferred);
					REQUIRE(first);
					const uint32_t one = device.GetMemoryAllocationCount();
					CHECK(one > before);
					auto second = device.GetHostImageUpload().CreateTexture(AllocationTextureDescription(), data, preferred);
					REQUIRE(second);
					const uint32_t two = device.GetMemoryAllocationCount();
					if (first->Path == TextureUploadPath::HostImageCopy)
					{
						CHECK(device.GetHostImageUpload().GetHostImageCount() == 2);
						CHECK(two >= one);
						CHECK(two <= one + 1); // shared block, or a required dedicated allocation
						MESSAGE("Native host-image allocations for two images: ", two - before);
					}
					else
					{
						CHECK(second->Path == TextureUploadPath::Staging);
						CHECK(two > one);
					}
				}
				device.WaitForIdle();
				device.RunGarbageCollection();
				device.RunGarbageCollection();
				CHECK(device.GetMemoryAllocationCount() == before);
			}
		}

		TEST_CASE("GraphicsDevice: native memory remains counted until a held submission retires")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const uint32_t before = device.GetMemoryAllocationCount();
			auto gate = Test::GpuSubmissionGate::Create(device);
			REQUIRE(gate);
			{
				nvrhi::BufferDesc desc;
				desc.byteSize = 256;
				desc.initialState = nvrhi::ResourceStates::Common;
				desc.keepInitialState = true;
				auto buffer = device.CreateBuffer(desc);
				REQUIRE(buffer);
				auto commands = device.CreateCommandList();
				REQUIRE(commands);
				(*commands)->open();
				(*commands)->clearBufferUInt(*buffer, 0);
				(*commands)->close();
				(*gate)->HoldNextSubmission();
				device.ExecuteCommandList(**commands);
			}
			device.RunGarbageCollection();
			CHECK(device.GetMemoryAllocationCount() == before + 1);
			(*gate)->Open();
			device.WaitForIdle();
			device.RunGarbageCollection();
			CHECK(device.GetMemoryAllocationCount() == before);
		}

		TEST_CASE("GraphicsDevice: an NVRHI heap counts once for multiple virtual buffers")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const uint32_t before = device.GetMemoryAllocationCount();
			{
				// Direct NVRHI public API exercises the heap allocator; the engine has no heap creation wrapper.
				nvrhi::HeapDesc heapDesc;
				heapDesc.capacity = 1024 * 1024;
				heapDesc.type = nvrhi::HeapType::DeviceLocal;
				nvrhi::HeapHandle heap = device.GetNvrhiDevice()->createHeap(heapDesc);
				REQUIRE(heap);
				CHECK(device.GetMemoryAllocationCount() == before + 1);
				{
					nvrhi::BufferDesc desc;
					desc.byteSize = 256;
					desc.isVirtual = true;
					auto first = device.CreateBuffer(desc);
					auto second = device.CreateBuffer(desc);
					REQUIRE(first);
					REQUIRE(second);
					const auto requirements = device.GetNvrhiDevice()->getBufferMemoryRequirements(*first);
					const uint64_t offset = ((requirements.size + requirements.alignment - 1) / requirements.alignment) * requirements.alignment;
					REQUIRE(device.GetNvrhiDevice()->bindBufferMemory(*first, heap, 0));
					REQUIRE(device.GetNvrhiDevice()->bindBufferMemory(*second, heap, offset));
					CHECK(device.GetMemoryAllocationCount() == before + 1);
				}
				device.RunGarbageCollection();
				CHECK(device.GetMemoryAllocationCount() == before + 1);
			}
			CHECK(device.GetMemoryAllocationCount() == before);
		}

		TEST_CASE("GraphicsDevice: failed allocations and a replacement device do not leak allocation counts")
		{
			for (const auto fault : { GpuFault::OomTexture, GpuFault::None })
			{
				Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
				ENGINE_REQUIRE_GPU(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				const uint32_t before = device.GetMemoryAllocationCount();
				CHECK(before == 0);
				CHECK(device.GetInfo().MaxMemoryAllocationCount > 0);
				{
					NativeMemoryProbe native(device);
					CHECK(device.GetMemoryAllocationCount() == before + 1);
					auto texture = device.CreateTexture(AllocationTextureDescription());
					if (fault == GpuFault::OomTexture)
					{
						REQUIRE_FALSE(texture);
						CHECK(texture.error().GetCode() == ErrorCode::Gpu);
						CHECK(device.GetMemoryAllocationCount() == before + 1);
					}
					else
					{
						REQUIRE(texture);
						CHECK(device.GetMemoryAllocationCount() == before + 2);
					}
				}
				device.RunGarbageCollection();
				CHECK(device.GetMemoryAllocationCount() == before);
				gpu.Reset();
				CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateMemory == nullptr);
				CHECK(VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory == nullptr);
			}
		}
	}

}
