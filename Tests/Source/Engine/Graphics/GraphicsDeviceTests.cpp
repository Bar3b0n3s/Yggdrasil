#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsDevice.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/VulkanDispatch.h"
#include "Engine/Platform/Process.h"
#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"

#include <vulkan/vulkan.hpp>

// The device (Architecture §8.1, §8.14). Every case runs in the gpu stage under both API caps: Test.py runs the GPU suite
// once as it is and once with --vulkan-api=1.3, which the fixture applies.

namespace Engine {

	// Creates a device the way an application does, without --gpu, so the ENGINE_GPU variable the parent set decides the
	// override, and reports the outcome: the child of the ENGINE_GPU test.
	ENGINE_DEATH_TEST("Graphics/CreatesDeviceFromEnvironment")
	{
		if (!VulkanDispatch::IsInitialized())
		{
			ENGINE_CORE_WARN("Device creation: [no loader]");
			return;
		}
		GraphicsDeviceSpecification specification;
		specification.Graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
		const Result<Scope<GraphicsDevice>> device = GraphicsDevice::Create(specification);
		if (device.has_value())
			ENGINE_CORE_WARN("Device creation: [created '{}']", (*device)->GetInfo().DeviceName);
		else
			ENGINE_CORE_WARN("Device creation: [{}] {}", ErrorCodeToString(device.error().GetCode()), device.error());
	}

	// Runs the ENGINE_GPU child with `value` and returns its standard error.
	static std::string RunDeviceFromEnvironmentChild(std::string value)
	{
		std::vector<std::string> arguments = { "--death-test=Graphics/CreatesDeviceFromEnvironment" };
		for (std::string& argument : Test::GetGpuTestsChildArguments())
			arguments.push_back(std::move(argument));
		ProcessSpecification specification = Test::MakeTestsChildSpecification(std::move(arguments));
		specification.Environment.emplace_back("ENGINE_GPU", std::move(value));
		const Result<ProcessResult> child = Process::Run(specification, std::chrono::seconds(120));
		REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
		INFO("child stderr: ", child->StandardError);
		CHECK(child->ExitCode == ExitCode::Failed); // the body returns without dying
		return child->StandardError;
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GraphicsDevice: create and destroy with zero validation messages"
			* doctest::test_suite(Test::GpuSuite))
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
			CHECK(info.DeviceClass == GetDeviceClass(info.VendorID, info.DriverVersion, info.DriverID));
			CHECK(info.MaxMemoryAllocationCount > 0);
			CHECK(device.GetNvrhiDevice() != nullptr);
			CHECK(device.GetVulkanInstance() != VK_NULL_HANDLE);
			CHECK(device.GetVulkanPhysicalDevice() != VK_NULL_HANDLE);
			CHECK(device.GetVulkanDevice() != VK_NULL_HANDLE);
			CHECK(device.GetGraphicsQueue() != VK_NULL_HANDLE);
			CHECK(device.GetGraphicsQueueTimelineSemaphore() != VK_NULL_HANDLE);
			CHECK(device.GetFramesInFlight() == 2);
			CHECK(device.GetHostImageUpload().IsHostImageCopyAvailable() == info.HostImageCopy);

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

		TEST_CASE("GraphicsDevice: validation and synchronization-validation messages reach GpuDiagnostics and count"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The wiring every GPU test's "zero validation messages" relies on (§15.3): the layer with synchronization
			// validation on, the debug messenger, and the device's GpuDiagnostics as its sink.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			GpuDiagnostics& diagnostics = device.GetDiagnostics();
			REQUIRE(diagnostics.GetErrorCount() == 0);

			// A real synchronization hazard: two clears of one texture with no barrier between them.
			{
				nvrhi::TextureDesc desc;
				desc.width = 16;
				desc.height = 16;
				desc.format = nvrhi::Format::RGBA8_UNORM;
				desc.isRenderTarget = true;
				desc.debugName = "HazardTarget";
				desc.enableAutomaticStateTracking(nvrhi::ResourceStates::RenderTarget);
				Result<nvrhi::TextureHandle> texture = device.CreateTexture(desc);
				REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());

				Test::ExpectLog hazard(LogLevel::Error, "SYNC-HAZARD-WRITE-AFTER-WRITE");
				nvrhi::ICommandList& list = **commandList;
				list.open();
				list.clearTextureFloat(*texture, nvrhi::AllSubresources, nvrhi::Color(1.0f, 0.0f, 0.0f, 1.0f));
				list.setEnableAutomaticBarriers(false);
				list.clearTextureFloat(*texture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 1.0f, 0.0f, 1.0f));
				list.setEnableAutomaticBarriers(true);
				list.close();
				device.ExecuteCommandList(list);
				device.WaitForIdle();
			}
			CHECK(diagnostics.GetErrorCount() > 0);
			diagnostics.ResetCounts();

			// An ordinary validation error through a dispatcher C entry point: a binary semaphore with a non-zero initial
			// value, which the driver ignores.
			{
				VkSemaphoreTypeCreateInfo typeInfo{};
				typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
				typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_BINARY;
				typeInfo.initialValue = 1;
				VkSemaphoreCreateInfo createInfo{};
				createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
				createInfo.pNext = &typeInfo;
				VkSemaphore semaphore = VK_NULL_HANDLE;
				Test::ExpectLog invalid(LogLevel::Error, "VUID-VkSemaphoreTypeCreateInfo-semaphoreType-03279");
				const VkResult created = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore(device.GetVulkanDevice(), &createInfo, nullptr, &semaphore);
				if (created == VK_SUCCESS)
					VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySemaphore(device.GetVulkanDevice(), semaphore, nullptr);
			}
			CHECK(diagnostics.GetErrorCount() > 0);
			diagnostics.ResetCounts();
			CHECK(diagnostics.GetErrorCount() == 0);
		}

		TEST_CASE("GraphicsDevice: Destroy returns the counts of the device's whole life, its teardown included"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			GraphicsDeviceSpecification specification;
			specification.Graphics.Validation = true;
			specification.Graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
			specification.ApplicationName = "Tests";
			Result<Scope<GraphicsDevice>> device = GraphicsDevice::Create(specification);
			REQUIRE_MESSAGE(device.has_value(), device.error().ToString());

			// Two leaks only the teardown reports: a host image the tracker still counts, and a semaphore the validation
			// layer finds alive at vkDestroyDevice.
			(*device)->GetResourceTracker().RecordCreated(GpuResourceType::HostImage);
			VkSemaphoreCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			VkSemaphore leaked = VK_NULL_HANDLE;
			REQUIRE(VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore((*device)->GetVulkanDevice(), &createInfo, nullptr, &leaked) == VK_SUCCESS);
			CHECK((*device)->GetDiagnostics().GetErrorCount() == 0);

			Test::ExpectLog trackerLeak(LogLevel::Error, "GPU resource tracker: GPU objects are still alive when the device is destroyed: HostImage: 1");
			Test::ExpectLog layerLeak(LogLevel::Error, "VUID-vkDestroyDevice-device-05137");
			const GpuMessageCounts counts = GraphicsDevice::Destroy(std::move(*device));
			CHECK(counts.Errors >= 2);
			CHECK(counts.Warnings == 0);

			const GpuMessageCounts none = GraphicsDevice::Destroy(nullptr);
			CHECK(none.Errors == 0);
			CHECK(none.Warnings == 0);
		}

		TEST_CASE("GraphicsDevice: a null CreateTexture never crashes and is a Gpu error naming the texture"
			* doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			GpuResourceTracker& tracker = device.GetResourceTracker();
			const uint64_t buffersBefore = tracker.GetLiveCount(GpuResourceType::GpuBuffer);
			{
				nvrhi::BufferDesc desc;
				desc.byteSize = 256;
				desc.debugName = "TrackedBuffer";
				Result<nvrhi::BufferHandle> buffer = device.CreateBuffer(desc);
				REQUIRE_MESSAGE(buffer.has_value(), buffer.error().ToString());
				CHECK(tracker.GetLiveCount(GpuResourceType::GpuBuffer) == buffersBefore + 1);
			}
			device.RunGarbageCollection();
			CHECK(tracker.GetLiveCount(GpuResourceType::GpuBuffer) == buffersBefore);
		}

		TEST_CASE("GraphicsDevice: a second device while one exists is InvalidState"
			* doctest::test_suite(Test::GpuSuite))
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

		TEST_CASE("GraphicsDevice: a GPU override that matches no device is NotFound and leaves nothing behind"
			* doctest::test_suite(Test::GpuSuite))
		{
			// NotFound needs devices to match against: a loader without a driver fails at vkCreateInstance with a Gpu error,
			// so the probe decides whether this machine can run the test.
			if (!Test::ProbeGpuForProcess())
				return;
			GraphicsDeviceSpecification specification;
			specification.Graphics.Validation = true;
			specification.Graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
			specification.Graphics.GpuOverride = "no such device";
			const Result<Scope<GraphicsDevice>> device = GraphicsDevice::Create(specification);
			REQUIRE_FALSE(device.has_value());
			CHECK(device.error().GetCode() == ErrorCode::NotFound);
			CHECK(device.error().ToString().contains("no such device"));

			// The failed creation released its instance and its claim on the process's one device: the probe created a
			// device, so one must be created again now.
			Test::HeadlessGpuFixture gpu;
			REQUIRE_MESSAGE(gpu.IsAvailable(), gpu.GetUnavailableReason());
		}

		TEST_CASE("GraphicsDevice: ENGINE_GPU overrides the selection when --gpu is not given" * doctest::test_suite(Test::GpuSuite))
		{
			// Both outcomes need a device: without a driver the children fail at vkCreateInstance whatever ENGINE_GPU says.
			if (!Test::ProbeGpuForProcess())
				return;
			const std::string unknown = RunDeviceFromEnvironmentChild("no such device");
			CHECK(unknown.contains("Device creation: [NotFound]"));
			CHECK(unknown.contains("no such device"));

			// Index 0 exists: the probe created a device.
			const std::string first = RunDeviceFromEnvironmentChild("0");
			CHECK(first.contains("Device creation: [created '"));
		}

		TEST_CASE("GraphicsDevice: the device-fault description of a healthy device reports no fault" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const std::string description = device.DescribeDeviceFault();
			if (device.GetInfo().DeviceFault)
				CHECK(description == "no fault information");
			else
				CHECK(description == "VK_EXT_device_fault is not available");
		}

		TEST_CASE("GraphicsDevice: FramesInFlight 0 is InvalidArgument")
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
