#include "TestsPCH.h"

#include "Engine/Graphics/Swapchain.h"

#include "Engine/App/EngineContext.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/Window.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"
#include "Support/WindowedChild.h"

namespace Engine {

	// Renders `frames` cleared frames into the swapchain; returns the number of frames the swapchain skipped.
	static uint32_t PresentFrames(GraphicsDevice& device, Swapchain& swapchain, FramePacer& pacer, nvrhi::ICommandList& commandList,
		Window& window, uint32_t frames)
	{
		uint32_t skipped = 0;
		for (uint32_t frame = 0; frame < frames; ++frame)
		{
			window.PollEvents();
			pacer.BeginFrame();
			const Result<SwapchainAcquireStatus> status = swapchain.AcquireNextImage(pacer.GetFrameSlot());
			REQUIRE_MESSAGE(status.has_value(), status.error().ToString());
			if (*status == SwapchainAcquireStatus::Skipped)
			{
				++skipped;
				pacer.EndFrame(device.GetLastSubmissionID());
				continue;
			}
			commandList.open();
			commandList.clearTextureFloat(swapchain.GetCurrentTexture(), nvrhi::AllSubresources, nvrhi::Color(0.2f, 0.3f, 0.4f, 1.0f));
			commandList.close();
			swapchain.QueueFrameSemaphores();
			const uint64_t submission = device.ExecuteCommandList(commandList);
			swapchain.Present();
			pacer.EndFrame(submission);
			device.RunGarbageCollection();
		}
		return skipped;
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("Swapchain: acquire and present results map to the documented actions" * doctest::skip(true))
		{
			CHECK(ClassifyAcquireResult(VK_SUCCESS) == SwapchainAction::Continue);
			CHECK(ClassifyAcquireResult(VK_SUBOPTIMAL_KHR) == SwapchainAction::ContinueThenRecreate);
			CHECK(ClassifyAcquireResult(VK_ERROR_OUT_OF_DATE_KHR) == SwapchainAction::Recreate);
			CHECK(ClassifyAcquireResult(VK_ERROR_SURFACE_LOST_KHR) == SwapchainAction::RecreateSurface);
			CHECK(ClassifyAcquireResult(VK_TIMEOUT) == SwapchainAction::Retry);
			CHECK(ClassifyAcquireResult(VK_NOT_READY) == SwapchainAction::Retry);
			CHECK(ClassifyAcquireResult(VK_ERROR_DEVICE_LOST) == SwapchainAction::DeviceLost);
			CHECK(ClassifyAcquireResult(VK_ERROR_OUT_OF_DEVICE_MEMORY) == SwapchainAction::OutOfMemory);
			CHECK(ClassifyAcquireResult(VK_ERROR_OUT_OF_HOST_MEMORY) == SwapchainAction::OutOfMemory);
			CHECK(ClassifyAcquireResult(VK_ERROR_INITIALIZATION_FAILED) == SwapchainAction::Fail);

			CHECK(ClassifyPresentResult(VK_SUCCESS) == SwapchainAction::Continue);
			CHECK(ClassifyPresentResult(VK_SUBOPTIMAL_KHR) == SwapchainAction::Recreate);
			CHECK(ClassifyPresentResult(VK_ERROR_OUT_OF_DATE_KHR) == SwapchainAction::Recreate);
			CHECK(ClassifyPresentResult(VK_ERROR_SURFACE_LOST_KHR) == SwapchainAction::RecreateSurface);
			CHECK(ClassifyPresentResult(VK_ERROR_DEVICE_LOST) == SwapchainAction::DeviceLost);
			CHECK(ClassifyPresentResult(VK_ERROR_OUT_OF_HOST_MEMORY) == SwapchainAction::OutOfMemory);
			CHECK(ClassifyPresentResult(VK_TIMEOUT) == SwapchainAction::Fail);

			CHECK(SwapchainActionToString(SwapchainAction::ContinueThenRecreate) == "ContinueThenRecreate");
			CHECK(SwapchainActionToString(SwapchainAction::Fail) == "Fail");
		}

		// Runs only in a windowed child process (Support/WindowedChild.h): a real window and its swapchain through a resize, a
		// minimize and a restore, presenting all along.
		TEST_CASE("Swapchain: a native window's swapchain survives resize, minimize and restore"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			Result<Scope<EngineContext>> created = EngineContext::Create({
				.WorkerCount = 0,
				.Window = WindowSpecification{ .Title = "Swapchain", .Width = 320, .Height = 240 },
				.Graphics = GraphicsSpecification{
					.Validation = true,
					.SynchronizationValidation = true,
					.MaxApiVersion = Test::GetTestOptions().VulkanApi,
				},
			});
			if (!created.has_value())
			{
				Test::ReportGpuUnavailable(created.error().ToString());
				return;
			}
			EngineContext& context = **created;
			GraphicsDevice& device = *context.GetGraphicsDevice();
			Window& window = *context.GetWindow();
			Result<Scope<Swapchain>> swapchain = Swapchain::Create(device, window, { .VSync = true, .FramesInFlight = 2 });
			REQUIRE_MESSAGE(swapchain.has_value(), swapchain.error().ToString());
			FramePacer pacer(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			const nvrhi::Format format = (*swapchain)->GetFormat();
			CHECK((format == nvrhi::Format::BGRA8_UNORM || format == nvrhi::Format::RGBA8_UNORM));
			CHECK((*swapchain)->GetPresentMode() == VK_PRESENT_MODE_FIFO_KHR);

			// Steady state: nothing is skipped.
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 10) == 0);
			const uint32_t recreationsBefore = (*swapchain)->GetRecreationCount();

			// A resize recreates the swapchain at the new framebuffer size (asynchronous on X11 and macOS).
			window.SetSize(480, 300);
			for (int attempt = 0; attempt < 200 && (*swapchain)->GetWidth() != window.GetFramebufferWidth(); ++attempt)
				PresentFrames(device, **swapchain, pacer, **commandList, window, 1);
			CHECK((*swapchain)->GetWidth() == window.GetFramebufferWidth());
			CHECK((*swapchain)->GetHeight() == window.GetFramebufferHeight());
			CHECK((*swapchain)->GetRecreationCount() > recreationsBefore);

			// Minimized: frames are skipped, nothing is presented and nothing fails.
			window.Minimize();
			REQUIRE(Test::WaitUntilMinimized(window, true));
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 5) == 5);
			CHECK((*swapchain)->IsMinimized());

			// Restored: presenting resumes after at most one skipped frame of recreation.
			window.Restore();
			REQUIRE(Test::WaitUntilMinimized(window, false));
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 10) <= 1);
			CHECK_FALSE((*swapchain)->IsMinimized());

			// A recreation request: one skipped frame.
			uint32_t recreations = (*swapchain)->GetRecreationCount();
			(*swapchain)->RequestRecreate();
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 5) == 1);
			CHECK((*swapchain)->GetRecreationCount() == recreations + 1);

			// The VkResults of §8.1 through the real acquire and present paths, each an ordinary state (the C entry points
			// never throw vk::OutOfDateKHRError): an out-of-date acquire recreates and skips its frame.
			recreations = (*swapchain)->GetRecreationCount();
			(*swapchain)->InjectAcquireResultForTesting(VK_ERROR_OUT_OF_DATE_KHR);
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 5) == 1);
			CHECK((*swapchain)->GetRecreationCount() == recreations + 1);

			// A suboptimal acquire still renders and presents its image, and the next frame recreates.
			recreations = (*swapchain)->GetRecreationCount();
			(*swapchain)->InjectAcquireResultForTesting(VK_SUBOPTIMAL_KHR);
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 1) == 0);
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 5) == 1);
			CHECK((*swapchain)->GetRecreationCount() == recreations + 1);

			// A suboptimal or out-of-date present marks a recreation for the next acquire.
			for (const VkResult presentResult : { VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR })
			{
				CAPTURE(static_cast<int>(presentResult));
				recreations = (*swapchain)->GetRecreationCount();
				(*swapchain)->InjectPresentResultForTesting(presentResult);
				CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 1) == 0);
				CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 5) == 1);
				CHECK((*swapchain)->GetRecreationCount() == recreations + 1);
			}

			// A lost surface, from acquire or present, recreates the surface and the swapchain.
			recreations = (*swapchain)->GetRecreationCount();
			(*swapchain)->InjectAcquireResultForTesting(VK_ERROR_SURFACE_LOST_KHR);
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 5) == 1);
			CHECK((*swapchain)->GetRecreationCount() == recreations + 1);
			recreations = (*swapchain)->GetRecreationCount();
			(*swapchain)->InjectPresentResultForTesting(VK_ERROR_SURFACE_LOST_KHR);
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 1) == 0);
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 5) == 1);
			CHECK((*swapchain)->GetRecreationCount() == recreations + 1);

			device.WaitForIdle();
			commandList->Reset();
			swapchain->reset();
			CHECK(device.GetDiagnostics().GetErrorCount() == 0);
		}

		TEST_CASE("Swapchain: resize, minimize and out-of-date recover" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			std::vector<std::string> arguments = { "--windowed-child=Swapchain: a native window's swapchain survives resize, minimize and restore" };
			const std::vector<std::string> gpuArguments = Test::GetGpuTestsChildArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> child = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stdout: ", child->StandardOutput);
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == 0);
		}
	}

}
