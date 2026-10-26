#include "TestsPCH.h"

#include "Engine/Graphics/Swapchain.h"

#include "Engine/App/EngineContext.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/Window.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"
#include "Support/TrimCounter.h"
#include "Support/WaitUntil.h"
#include "Support/WindowedChild.h"

#include <vulkan/vulkan.hpp>

#include <optional>

namespace Engine {

	namespace {

		// While alive, the dispatcher's vkCreateSwapchainKHR answers VK_ERROR_INITIALIZATION_FAILED without reaching Vulkan: a
		// swapchain creation that fails without running out of memory (M18, C18.4). Main thread, around the calls under test.
		class SwapchainCreationFails
		{
		public:
			SwapchainCreationFails()
				: m_CreateSwapchain(VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSwapchainKHR)
			{
				VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSwapchainKHR = &FailCreation;
			}

			~SwapchainCreationFails()
			{
				VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSwapchainKHR = m_CreateSwapchain;
			}

			SwapchainCreationFails(const SwapchainCreationFails&) = delete;
			SwapchainCreationFails& operator=(const SwapchainCreationFails&) = delete;
			SwapchainCreationFails(SwapchainCreationFails&&) = delete;
			SwapchainCreationFails& operator=(SwapchainCreationFails&&) = delete;
		private:
			static VKAPI_ATTR VkResult VKAPI_CALL FailCreation(VkDevice /*device*/, const VkSwapchainCreateInfoKHR* /*createInfo*/,
				const VkAllocationCallbacks* /*allocator*/, VkSwapchainKHR* swapchain)
			{
				*swapchain = VK_NULL_HANDLE;
				return VK_ERROR_INITIALIZATION_FAILED;
			}

			PFN_vkCreateSwapchainKHR m_CreateSwapchain = nullptr;
		};

	}

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
		TEST_CASE("Swapchain: acquire and present results map to the documented actions")
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
		TEST_CASE("Swapchain: a native window's swapchain survives resizing and minimizing"
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
			Scope<FramePacer> framePacer = CreateScope<FramePacer>(device, 2);
			FramePacer& pacer = *framePacer;
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			const nvrhi::Format format = (*swapchain)->GetFormat();
			CHECK((format == nvrhi::Format::BGRA8_UNORM || format == nvrhi::Format::RGBA8_UNORM));
			CHECK((*swapchain)->GetPresentMode() == VK_PRESENT_MODE_FIFO_KHR);

			// Steady state: nothing is skipped.
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 10) == 0);
			const uint32_t recreationsBefore = (*swapchain)->GetRecreationCount();

			// A resize recreates the swapchain at the new framebuffer size. The window system reports the new size with its
			// events, asynchronously on X11 and macOS, so until then the window still reports the old size, which the swapchain
			// already has: frames are presented until the window reports a new size and the swapchain has followed it.
			const uint32_t widthBefore = window.GetFramebufferWidth();
			window.SetSize(480, 300);
			const bool resized = Test::WaitUntil([&device, &swapchain, &pacer, &commandList, &window, widthBefore]()
			{
				PresentFrames(device, **swapchain, pacer, **commandList, window, 1);
				return window.GetFramebufferWidth() != widthBefore && (*swapchain)->GetWidth() == window.GetFramebufferWidth();
			});
			CHECK(resized);
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

			// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decisions 28 and 29): a device lost between a frame's acquire and
			// its submission. The submission and the present never reach Vulkan, the lost device's next frames acquire and
			// present nothing, and none of it ends the process.
			window.PollEvents();
			pacer.BeginFrame();
			const Result<SwapchainAcquireStatus> acquired = (*swapchain)->AcquireNextImage(pacer.GetFrameSlot());
			REQUIRE_MESSAGE(acquired.has_value(), acquired.error().ToString());
			REQUIRE(*acquired == SwapchainAcquireStatus::Acquired);
			(*commandList)->open();
			(*commandList)->clearTextureFloat((*swapchain)->GetCurrentTexture(), nvrhi::AllSubresources, nvrhi::Color(0.2f, 0.3f, 0.4f, 1.0f));
			(*commandList)->close();
			(*swapchain)->QueueFrameSemaphores();
			device.GetDiagnostics().SetDeviceLost(true);
			const uint64_t lostSubmission = device.ExecuteCommandList(**commandList);
			CHECK(lostSubmission == device.GetLastSubmissionID());
			(*swapchain)->Present();
			pacer.EndFrame(lostSubmission);
			device.RunGarbageCollection();
			CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 3) == 3);

			// The recovery with the window (Recover(window)): the swapchain, no registered owner here, goes before it with its
			// surface, the device and its VkInstance are created again with presentation support scored again, and a new
			// swapchain presents on them.
			commandList->Reset();
			swapchain->reset();
			framePacer.reset();
			const Status recovered = context.RecoverGraphics();
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			CHECK(device.GetRecoveryCount() == 1);
			swapchain = Swapchain::Create(device, window, { .VSync = true, .FramesInFlight = 2 });
			REQUIRE_MESSAGE(swapchain.has_value(), swapchain.error().ToString());
			framePacer = CreateScope<FramePacer>(device, 2);
			commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			CHECK(PresentFrames(device, **swapchain, *framePacer, **commandList, window, 10) == 0);

			// Zero validation messages and nothing left alive, like the GPU fixture checks (HeadlessGpuFixture.h): the counts are
			// read after the device's teardown, which reports leaks too.
			device.WaitForIdle();
			commandList->Reset();
			swapchain->reset();
			framePacer.reset();
			device.RunGarbageCollection();
			CHECK_MESSAGE(device.GetResourceTracker().GetTotalLiveCount() == 0, device.GetResourceTracker().DescribeLiveCounts());
			const GpuMessageCounts counts = context.DestroyGraphics();
			CHECK(counts.Errors == 0);
			CHECK(counts.Warnings == 0);
		}

		// Runs only in a windowed child process: M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42, 44 and 46) a
		// swapchain recreation at a resize that runs out of memory, once (oom-swapchain: retried after a trim, then presenting at
		// the new size) and every time (oom-swapchain-persistent: OutOfMemory once, no frame presented at the failed size, and the
		// next size tried again, as is the same size once the window was minimized and restored), without a GPU message and
		// without losing the device.
		TEST_CASE("Swapchain: a recreation that runs out of memory is retried after a trim or skips its size"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			for (const GpuFault fault : { GpuFault::OomSwapchain, GpuFault::OomSwapchainPersistent })
			{
				CAPTURE(GpuFaultToString(fault));
				const bool persistent = fault == GpuFault::OomSwapchainPersistent;
				Result<Scope<EngineContext>> created = EngineContext::Create({
					.WorkerCount = 0,
					.Window = WindowSpecification{ .Title = "Swapchain out of memory", .Width = 320, .Height = 240 },
					.Graphics = GraphicsSpecification{
						.Validation = true,
						.SynchronizationValidation = true,
						.MaxApiVersion = Test::GetTestOptions().VulkanApi,
						.InjectFault = fault,
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
				{
					Result<Scope<Swapchain>> swapchain = Swapchain::Create(device, window, { .VSync = true, .FramesInFlight = 2 });
					REQUIRE_MESSAGE(swapchain.has_value(), swapchain.error().ToString());
					FramePacer pacer(device, 2);
					Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
					REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
					// The startup frames, whose garbage collection arms the fault.
					CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 3) == 0);

					uint32_t outOfMemory = 0;
					uint32_t presented = 0;
					const auto frame = [&device, &swapchain, &pacer, &commandList, &window, &outOfMemory, &presented]()
					{
						window.PollEvents();
						pacer.BeginFrame();
						const Result<SwapchainAcquireStatus> status = (*swapchain)->AcquireNextImage(pacer.GetFrameSlot());
						if (!status.has_value() || *status == SwapchainAcquireStatus::Skipped)
						{
							if (!status.has_value())
							{
								CHECK(status.error().GetCode() == ErrorCode::OutOfMemory);
								++outOfMemory;
							}
							pacer.EndFrame(device.GetLastSubmissionID());
							device.RunGarbageCollection();
							return;
						}
						(*commandList)->open();
						(*commandList)->clearTextureFloat((*swapchain)->GetCurrentTexture(), nvrhi::AllSubresources, nvrhi::Color(0.2f, 0.3f, 0.4f, 1.0f));
						(*commandList)->close();
						(*swapchain)->QueueFrameSemaphores();
						const uint64_t submission = device.ExecuteCommandList(**commandList);
						(*swapchain)->Present();
						pacer.EndFrame(submission);
						device.RunGarbageCollection();
						++presented;
					};
					// Resizes the window, renders until the window reports the new size, then counts the frames presented at it.
					const auto resizeTo = [&frame, &window, &presented](uint32_t width, uint32_t height)
					{
						const uint32_t widthBefore = window.GetFramebufferWidth();
						window.SetSize(width, height);
						const bool reported = Test::WaitUntil([&frame, &window, widthBefore]()
						{
							frame();
							return window.GetFramebufferWidth() != widthBefore;
						});
						CHECK(reported);
						presented = 0;
						for (uint32_t extra = 0; extra < 6; ++extra)
							frame();
					};

					resizeTo(480, 300);
					if (persistent)
					{
						CHECK(outOfMemory == 1);
						CHECK(presented == 0);
						resizeTo(400, 260);
						CHECK(outOfMemory == 2);
						CHECK(presented == 0);
						// A minimized window forgets the size that failed, so restoring it to the same size is a new request.
						window.Minimize();
						REQUIRE(Test::WaitUntilMinimized(window, true));
						for (uint32_t minimizedFrame = 0; minimizedFrame < 3; ++minimizedFrame)
							frame();
						window.Restore();
						REQUIRE(Test::WaitUntilMinimized(window, false));
						for (uint32_t restoredFrame = 0; restoredFrame < 6; ++restoredFrame)
							frame();
						CHECK(outOfMemory == 3);
						CHECK(presented == 0);
					}
					else
					{
						CHECK(outOfMemory == 0);
						CHECK(presented > 0);
						CHECK((*swapchain)->GetWidth() == window.GetFramebufferWidth());
					}
					CHECK_FALSE(device.IsLost());
					device.WaitForIdle();
				}
				device.RunGarbageCollection();
				CHECK_MESSAGE(device.GetResourceTracker().GetTotalLiveCount() == 0, device.GetResourceTracker().DescribeLiveCounts());
				const GpuMessageCounts counts = context.DestroyGraphics();
				CHECK(counts.Errors == 0);
				CHECK(counts.Warnings == 0);
			}
		}

		TEST_CASE("Swapchain: a recreation out of memory is retried after a trim, or skips frames until the size changes"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, decisions 42 and 44): "a resize keeps the previous swapchain and targets and retries at the
			// next resize", through a real window in a child process.
			std::vector<std::string> arguments = { "--windowed-child=Swapchain: a recreation that runs out of memory is retried after a trim or skips its size" };
			const std::vector<std::string> gpuArguments = Test::GetGpuTestsChildArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> child = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stdout: ", child->StandardOutput);
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == 0);
		}

		// Runs only in a windowed child process: M18 (C18.4, decisions 42 and 44) a swapchain recreation at a resize that fails
		// without running out of memory (vkCreateSwapchainKHR answering VK_ERROR_INITIALIZATION_FAILED) returns Gpu at once,
		// without a trim, as before C18.4: only out-of-memory results make the recreation a request that fails alone.
		TEST_CASE("Swapchain: a recreation that fails without running out of memory returns Gpu without a trim"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			Result<Scope<EngineContext>> created = EngineContext::Create({
				.WorkerCount = 0,
				.Window = WindowSpecification{ .Title = "Swapchain creation failure", .Width = 320, .Height = 240 },
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
			{
				Result<Scope<Swapchain>> swapchain = Swapchain::Create(device, window, { .VSync = true, .FramesInFlight = 2 });
				REQUIRE_MESSAGE(swapchain.has_value(), swapchain.error().ToString());
				FramePacer pacer(device, 2);
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				// The startup frames, after which a creation failure is a request's.
				CHECK(PresentFrames(device, **swapchain, pacer, **commandList, window, 3) == 0);
				device.RunGarbageCollection();

				const Test::TrimCounter counter(device.GetRecreation());
				std::optional<Error> failure;
				{
					const SwapchainCreationFails creationFails;
					window.SetSize(480, 300);
					// Until the window reports its new size, the old swapchain still presents.
					const bool failed = Test::WaitUntil([&device, &swapchain, &pacer, &commandList, &window, &failure]()
					{
						window.PollEvents();
						pacer.BeginFrame();
						const Result<SwapchainAcquireStatus> status = (*swapchain)->AcquireNextImage(pacer.GetFrameSlot());
						if (!status.has_value())
						{
							failure = status.error();
							pacer.EndFrame(device.GetLastSubmissionID());
							return true;
						}
						if (*status == SwapchainAcquireStatus::Skipped)
						{
							pacer.EndFrame(device.GetLastSubmissionID());
							return false;
						}
						(*commandList)->open();
						(*commandList)->clearTextureFloat((*swapchain)->GetCurrentTexture(), nvrhi::AllSubresources, nvrhi::Color(0.2f, 0.3f, 0.4f, 1.0f));
						(*commandList)->close();
						(*swapchain)->QueueFrameSemaphores();
						const uint64_t submission = device.ExecuteCommandList(**commandList);
						(*swapchain)->Present();
						pacer.EndFrame(submission);
						return false;
					});
					CHECK(failed);
				}
				REQUIRE(failure.has_value());
				CHECK(failure->GetCode() == ErrorCode::Gpu);
				CHECK(counter.GetTrims() == 0);
				CHECK_FALSE(device.IsLost());
				device.WaitForIdle();
			}
			device.RunGarbageCollection();
			CHECK_MESSAGE(device.GetResourceTracker().GetTotalLiveCount() == 0, device.GetResourceTracker().DescribeLiveCounts());
			const GpuMessageCounts counts = context.DestroyGraphics();
			CHECK(counts.Errors == 0);
			CHECK(counts.Warnings == 0);
		}

		TEST_CASE("Swapchain: a recreation that fails otherwise still returns Gpu, without a trim" * doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, decisions 42 and 44): the swapchain's classification of its recreation's failures (GpuOutOfMemory.h's
			// RetryAfterTrim), through a real window in a child process. It passes before W18.4 too, which changes the
			// recreation, and keeps that half of the rule from regressing.
			std::vector<std::string> arguments = { "--windowed-child=Swapchain: a recreation that fails without running out of memory returns Gpu without a trim" };
			const std::vector<std::string> gpuArguments = Test::GetGpuTestsChildArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> child = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stdout: ", child->StandardOutput);
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == 0);
		}

		TEST_CASE("Swapchain: resize, minimize and out-of-date recover" * doctest::test_suite(Test::GpuSuite))
		{
			std::vector<std::string> arguments = { "--windowed-child=Swapchain: a native window's swapchain survives resizing and minimizing" };
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
