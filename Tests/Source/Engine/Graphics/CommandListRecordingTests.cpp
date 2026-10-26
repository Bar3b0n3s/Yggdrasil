#include "TestsPCH.h"

#include "Engine/Graphics/CommandListRecording.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/FrameLoop.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Support/DeathTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"

#include <vulkan/vulkan.hpp>

// A command list's recording (M18, C18.3; Docs/Decisions/0029-m18-contract.md decision 39): executed by its owner, or
// retired without a submission when a vk::SystemError unwinds it, so no opened list outlives its owner and keeps itself and
// what it recorded alive past a device loss.

namespace Engine {

	namespace {

		// While alive, the dispatcher's vkGetSemaphoreCounterValue answers VK_ERROR_DEVICE_LOST, as a really lost device's
		// does, without reaching Vulkan: NVRHI's upload manager asks the queue's timeline what completed at the first write
		// of a recording, so its vulkan.hpp call throws vk::DeviceLostError out of the middle of the recording. The Vulkan
		// device stays healthy, so the validation layer checks everything that follows. Main thread, around the calls under
		// test.
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
		private:
			static VKAPI_ATTR VkResult VKAPI_CALL ReportDeviceLost(VkDevice /*device*/, VkSemaphore /*semaphore*/, uint64_t* value)
			{
				*value = 0;
				return VK_ERROR_DEVICE_LOST;
			}

			PFN_vkGetSemaphoreCounterValue m_GetSemaphoreCounterValue = nullptr;
		};

		// In the update of frame 0, uploads a texture through HostImageUpload's staged path while the queue's timeline reports
		// the device lost, so its recording is unwound by the vk::DeviceLostError that the frame-boundary catch reports; then
		// recovers the device when the loop asks, as Application does.
		class UploadingClient final : public IFrameLoopClient
		{
		public:
			explicit UploadingClient(EngineContext& context)
				: m_Context(&context)
			{
			}

			void OnFrameEvent(Event& /*event*/) override {}

			void OnFrameFixedStep(const SimStep& /*step*/) override {}

			void OnFrameUpdate(const FrameTime& frame) override
			{
				if (frame.FrameIndex != 0)
					return;
				nvrhi::TextureDesc desc;
				desc.width = 4;
				desc.height = 4;
				desc.format = nvrhi::Format::RGBA8_UNORM;
				desc.debugName = "UnwoundUpload";
				const std::array<std::byte, 4 * 4 * 4> pixels{};
				const std::array<TextureSubresourceData, 1> data{ { { .Data = pixels } } };
				const TimelineReportsDeviceLost lost;
				const Result<TextureUpload> upload = m_Context->GetGraphicsDevice()->GetHostImageUpload().CreateTexture(desc, data,
					TextureUploadPath::Staging);
				UploadReturned = upload.has_value() ? "the texture" : upload.error().ToString();
			}

			void OnFrameRender(const FrameTime& frame) override
			{
				Rendered.push_back(frame.FrameIndex);
			}

			void OnFrameDeviceLost() override
			{
				++Recoveries;
				Recovery = m_Context->RecoverGraphics();
			}
		public:
			std::string UploadReturned{}; // what the upload returned, which it never does: it throws
			std::vector<uint64_t> Rendered{};
			uint32_t Recoveries = 0;
			Status Recovery{};
		private:
			EngineContext* m_Context = nullptr; // documented back-reference
		};

	}

	// The child of "CommandListRecording: a recording that ends without Execute and without an exception is a programmer
	// error".
	ENGINE_DEATH_TEST("Graphics/CommandListRecordingEndsWithoutExecute")
	{
		Test::HeadlessGpuFixture gpu;
		if (!gpu.IsAvailable())
		{
			ENGINE_CORE_ERROR("The death test needs a GPU");
			return;
		}
		Result<nvrhi::CommandListHandle> list = gpu.GetDevice().CreateCommandList();
		if (!list.has_value())
		{
			ENGINE_CORE_ERROR("Cannot create the command list: {}", list.error());
			return;
		}
		{
			const CommandListRecording recording(gpu.GetDevice(), **list);
		}
		ENGINE_CORE_ERROR("A recording ended without Execute and without an assert");
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("CommandListRecording: Execute closes the list and executes it as the device's last submission"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			nvrhi::BufferDesc desc;
			desc.byteSize = 256;
			desc.initialState = nvrhi::ResourceStates::Common;
			desc.keepInitialState = true;
			desc.debugName = "RecordedBuffer";
			Result<nvrhi::BufferHandle> buffer = device.CreateBuffer(desc);
			REQUIRE_MESSAGE(buffer.has_value(), buffer.error().ToString());
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
			const uint64_t before = device.GetLastSubmissionID();
			CommandListRecording recording(device, **list);
			(*list)->clearBufferUInt(*buffer, 7);
			const uint64_t submission = recording.Execute();
			CHECK(submission > before);
			CHECK(submission == device.GetLastSubmissionID());
			device.WaitForIdle();
			device.RunGarbageCollection();
		}

		TEST_CASE("CommandListRecording: a recording that a device loss thrown out of NVRHI unwinds is retired, and the device recovers"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, decisions 25 and 39): a real loss that no first-party call has seen yet surfaces as a vk::DeviceLostError
			// out of an upload's first write. The recording is closed and retired while the error unwinds it, so the
			// frame-boundary catch reports the loss and the recovery finds no GPU object alive; opened and dropped, the list
			// kept itself, the texture and its upload buffer alive, and the recovery failed (an exit 4 in an application).
			if (!Test::ProbeGpuForProcess())
				return;
			GraphicsSpecification graphics;
			graphics.Validation = true;
			graphics.SynchronizationValidation = true;
			graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
			Result<Scope<EngineContext>> created = EngineContext::Create({ .WorkerCount = 0, .Graphics = graphics });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			EngineContext& context = **created;
			UploadingClient client(context);
			FrameLoop loop(context, client, CreateScope<ManualClock>(1.0 / 60.0), {});
			loop.RunFrame();
			loop.RunFrame();
			CHECK(client.UploadReturned.empty());
			CHECK(client.Recoveries == 1);
			CHECK_MESSAGE(client.Recovery.has_value(), (client.Recovery.has_value() ? std::string() : client.Recovery.error().ToString()));
			CHECK_FALSE(context.GetGraphicsDevice()->IsLost());
			CHECK(context.GetGraphicsDevice()->GetRecoveryCount() == 1);
			CHECK(client.Rendered == std::vector<uint64_t>{ 1 });
			const GpuMessageCounts counts = context.DestroyGraphics();
			CHECK(counts.Errors == 0);
			CHECK(counts.Warnings == 0);
		}

		TEST_CASE("CommandListRecording: a recording that ends without Execute and without an exception is a programmer error"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			ENGINE_CHECK_DEATH("Graphics/CommandListRecordingEndsWithoutExecute", "a command list recording ended without Execute");
		}
	}

}
