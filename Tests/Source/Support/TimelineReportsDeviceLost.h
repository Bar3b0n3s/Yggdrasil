#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>

// A really lost device's timeline for the tests of what a device loss thrown out of NVRHI unwinds (M18, C18.3;
// Docs/Decisions/0029-m18-contract.md decision 39 and the milestone review): moved here from CommandListRecordingTests.cpp,
// because the renderer's recovery tests use it too.

namespace Engine {

	namespace Test {

		// While alive, the dispatcher's vkGetSemaphoreCounterValue answers VK_ERROR_DEVICE_LOST, as a really lost device's
		// does, without reaching Vulkan: NVRHI's upload manager asks the queue's timeline what completed at the first write of
		// a recording, so its vulkan.hpp call throws vk::DeviceLostError out of the middle of the recording. First-party
		// queries of the timeline (GraphicsDevice::GetCompletedSubmissionID) go through the same entry point and report the
		// loss instead of throwing. The Vulkan device stays healthy, so the validation layer checks everything that follows.
		// Main thread, around the calls under test; not copyable or movable.
		class TimelineReportsDeviceLost
		{
		public:
			TimelineReportsDeviceLost()
				: m_GetSemaphoreCounterValue(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetSemaphoreCounterValue)
			{
				VULKAN_HPP_DEFAULT_DISPATCHER.vkGetSemaphoreCounterValue = &ReportDeviceLost;
			}

			~TimelineReportsDeviceLost()
			{
				VULKAN_HPP_DEFAULT_DISPATCHER.vkGetSemaphoreCounterValue = m_GetSemaphoreCounterValue;
			}

			TimelineReportsDeviceLost(const TimelineReportsDeviceLost&) = delete;
			TimelineReportsDeviceLost& operator=(const TimelineReportsDeviceLost&) = delete;
			TimelineReportsDeviceLost(TimelineReportsDeviceLost&&) = delete;
			TimelineReportsDeviceLost& operator=(TimelineReportsDeviceLost&&) = delete;
		private:
			static VKAPI_ATTR VkResult VKAPI_CALL ReportDeviceLost(VkDevice /*device*/, VkSemaphore /*semaphore*/, uint64_t* value)
			{
				*value = 0;
				return VK_ERROR_DEVICE_LOST;
			}

			PFN_vkGetSemaphoreCounterValue m_GetSemaphoreCounterValue = nullptr;
		};

	}

}
