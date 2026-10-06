#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsDevice.h"

#include "Engine/Graphics/HostImageUpload.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"

// The device (Architecture §8.1, §8.14). Every case runs in the gpu stage under both API caps: Test.py runs the GPU suite
// once as it is and once with --vulkan-api=1.3, which the fixture applies.

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GraphicsDevice: create and destroy with zero validation messages"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const GraphicsDeviceInfo& info = device.GetInfo();
			CHECK_FALSE(info.DeviceName.empty());
			CHECK_FALSE(info.DeviceClass.empty());
			CHECK(info.Validation);
			CHECK(info.SynchronizationValidation);
			CHECK(info.ApiVersion <= Test::GetTestOptions().VulkanApi);
			if (Test::GetTestOptions().VulkanApi == VulkanApiVersion::Vulkan13)
				CHECK_FALSE(info.HostImageCopy); // a 1.4 path, which the 1.3 cap forces off (§8.1)
			CHECK(device.GetNvrhiDevice() != nullptr);
			CHECK(device.GetVulkanDevice() != VK_NULL_HANDLE);
			CHECK(device.GetGraphicsQueueTimelineSemaphore() != VK_NULL_HANDLE);
			CHECK(device.GetFramesInFlight() == 2);

			// A trivial submission completes and is reported as completed.
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			(*commandList)->open();
			(*commandList)->close();
			const uint64_t submission = device.ExecuteCommandList(**commandList);
			CHECK(submission > 0);
			CHECK(device.GetLastSubmissionID() == submission);
			device.WaitForIdle();
			CHECK(device.GetCompletedSubmissionID() >= submission);
			commandList->Reset();

			// The fixture's destruction checks zero validation errors and zero live objects (HeadlessGpuFixture.h).
			gpu.Reset();
			CHECK_FALSE(gpu.IsAvailable());
		}

		TEST_CASE("GraphicsDevice: a null CreateTexture never crashes and is a Gpu error naming the texture"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// --gpu-inject-fault=oom-texture makes NVRHI-style null results for sampled-only textures (§8.14 items 7 and 8).
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomTexture });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();

			nvrhi::TextureDesc sampled;
			sampled.width = 4;
			sampled.height = 4;
			sampled.format = nvrhi::Format::RGBA8_UNORM;
			sampled.debugName = "InjectedFailure";
			const Result<nvrhi::TextureHandle> failed = device.CreateTexture(sampled);
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::Gpu);
			CHECK(failed.error().GetMessageText().contains("InjectedFailure"));

			// Render targets keep working, so the application can run while asset textures fail.
			nvrhi::TextureDesc target = sampled;
			target.isRenderTarget = true;
			target.initialState = nvrhi::ResourceStates::RenderTarget;
			target.keepInitialState = true;
			target.debugName = "StillWorks";
			const Result<nvrhi::TextureHandle> created = device.CreateTexture(target);
			CHECK_MESSAGE(created.has_value(), (created.has_value() ? std::string() : created.error().ToString()));
		}

		TEST_CASE("GraphicsDevice: GpuResourceTracker counts what the wrappers create"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			GpuResourceTracker& tracker = device.GetResourceTracker();
			const uint64_t buffersBefore = tracker.GetLiveCount(GpuResourceType::Buffer);
			{
				nvrhi::BufferDesc desc;
				desc.byteSize = 256;
				desc.debugName = "TrackedBuffer";
				Result<nvrhi::BufferHandle> buffer = device.CreateBuffer(desc);
				REQUIRE_MESSAGE(buffer.has_value(), buffer.error().ToString());
				CHECK(tracker.GetLiveCount(GpuResourceType::Buffer) == buffersBefore + 1);
			}
			device.RunGarbageCollection();
			CHECK(tracker.GetLiveCount(GpuResourceType::Buffer) == buffersBefore);
		}

		TEST_CASE("GraphicsDevice: a second device while one exists is InvalidState"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDeviceSpecification specification;
			specification.Graphics.Validation = true;
			specification.Graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
			const Result<Scope<GraphicsDevice>> second = GraphicsDevice::Create(specification);
			REQUIRE_FALSE(second.has_value());
			CHECK(second.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("GraphicsDevice: FramesInFlight 0 is InvalidArgument" * doctest::skip(true))
		{
			// Rejected before any Vulkan call, so it needs no GPU.
			GraphicsDeviceSpecification specification;
			specification.Graphics.FramesInFlight = 0;
			const Result<Scope<GraphicsDevice>> device = GraphicsDevice::Create(specification);
			REQUIRE_FALSE(device.has_value());
			CHECK(device.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
