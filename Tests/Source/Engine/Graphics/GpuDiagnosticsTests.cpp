#include "TestsPCH.h"

#include "Engine/Graphics/GpuDiagnostics.h"

#include "Engine/Core/Log.h"
#include "Support/ExpectLog.h"
#include "Support/WaitUntil.h"

#include <thread>

// GpuDiagnostics is plain CPU state, so its message routing, device-loss detection and fault rules are tested without a
// device; the paths that end the process are tested through the editor (Editor/EditorFaultInjectionTests.cpp).

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GpuDiagnostics: errors and warnings are logged with their source and counted")
		{
			GpuDiagnostics diagnostics;
			{
				Test::ExpectLog error(LogLevel::Error, "Vulkan validation: VUID-vkCmdDraw-None-08600");
				diagnostics.ReportMessage(GpuMessageSeverity::Error, "Vulkan validation", "VUID-vkCmdDraw-None-08600");
			}
			diagnostics.ReportMessage(GpuMessageSeverity::Warning, "Vulkan validation", "a performance warning");
			diagnostics.ReportMessage(GpuMessageSeverity::Info, "Vulkan validation", "an info message");
			CHECK(diagnostics.GetErrorCount() == 1);
			CHECK(diagnostics.GetWarningCount() == 1);
			diagnostics.ResetCounts();
			CHECK(diagnostics.GetErrorCount() == 0);
			CHECK(diagnostics.GetWarningCount() == 0);
		}

		TEST_CASE("GpuDiagnostics: NVRHI's Device Removed! message sets the device-lost flag")
		{
			// NVRHI catches vk::DeviceLostError inside Queue::submit and reports it through its message callback (§8.1).
			GpuDiagnostics diagnostics;
			nvrhi::IMessageCallback& callback = diagnostics;
			CHECK_FALSE(diagnostics.IsDeviceLost());
			{
				Test::ExpectLog error(LogLevel::Error, "NVRHI: Device Removed!");
				callback.message(nvrhi::MessageSeverity::Error, "Device Removed!");
			}
			CHECK(diagnostics.IsDeviceLost());
			CHECK(diagnostics.GetErrorCount() == 1);
			// Other errors count without setting the flag.
			GpuDiagnostics other;
			{
				Test::ExpectLog error(LogLevel::Error, "NVRHI: binding set mismatch");
				static_cast<nvrhi::IMessageCallback&>(other).message(nvrhi::MessageSeverity::Error, "binding set mismatch");
			}
			CHECK_FALSE(other.IsDeviceLost());
			CHECK(other.GetErrorCount() == 1);
		}

		TEST_CASE("GpuDiagnostics: NVRHI severities map to log levels and counters, and a null text is an empty message")
		{
			GpuDiagnostics diagnostics;
			nvrhi::IMessageCallback& callback = diagnostics;
			// Info is logged at Trace and not counted.
			callback.message(nvrhi::MessageSeverity::Info, "an info message");
			{
				Test::ExpectLog warning(LogLevel::Warn, "NVRHI: ");
				callback.message(nvrhi::MessageSeverity::Warning, nullptr);
			}
			CHECK(diagnostics.GetErrorCount() == 0);
			CHECK(diagnostics.GetWarningCount() == 1);
			{
				Test::ExpectLog fatal(LogLevel::Critical, "NVRHI: a fatal message");
				callback.message(nvrhi::MessageSeverity::Fatal, "a fatal message");
			}
			CHECK(diagnostics.GetErrorCount() == 1);
			CHECK_FALSE(diagnostics.IsDeviceLost());

			// Only an error reports device loss; the same text as a warning is no device loss.
			callback.message(nvrhi::MessageSeverity::Warning, "Device Removed!");
			CHECK_FALSE(diagnostics.IsDeviceLost());
			CHECK(diagnostics.GetWarningCount() == 2);
		}

		TEST_CASE("GpuDiagnostics: the device-lost flag is set by SetDeviceLost and cleared only by ClearDeviceLost")
		{
			GpuDiagnostics diagnostics;
			CHECK_FALSE(diagnostics.IsDeviceLost());
			CHECK_FALSE(diagnostics.IsDeviceLossInjected());
			diagnostics.SetDeviceLost();
			CHECK(diagnostics.IsDeviceLost());
			CHECK_FALSE(diagnostics.IsDeviceLossInjected());
			diagnostics.ResetCounts();
			CHECK(diagnostics.IsDeviceLost());
			CHECK(diagnostics.GetInjectedFault() == GpuFault::None);
			CHECK(GpuMessageSeverityToString(GpuMessageSeverity::Fatal) == "Fatal");
			// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 29): the recovery clears the flag and the injected marker
			// once the device exists again.
			diagnostics.SetDeviceLost(true);
			diagnostics.ClearDeviceLost();
			CHECK_FALSE(diagnostics.IsDeviceLost());
			CHECK_FALSE(diagnostics.IsDeviceLossInjected());
		}

		TEST_CASE("GpuDiagnostics: an unrecoverable loss fails every device recreation, also after the flag is cleared")
		{
			// M18 (C18.3, decision 30): the test hooks' unrecoverable loss and --gpu-inject-fault=device-lost-unrecoverable.
			GpuDiagnostics hook;
			CHECK_FALSE(hook.ShouldFailDeviceRecreation());
			hook.InjectUnrecoverableDeviceLoss();
			CHECK(hook.IsDeviceLost());
			CHECK(hook.IsDeviceLossInjected());
			CHECK(hook.ShouldFailDeviceRecreation());
			hook.ClearDeviceLost();
			CHECK_FALSE(hook.IsDeviceLost());
			CHECK(hook.ShouldFailDeviceRecreation());

			GpuDiagnostics fault(GpuFault::DeviceLostUnrecoverable);
			CHECK(fault.ShouldFailDeviceRecreation());
			CHECK_FALSE(fault.ShouldInjectHang());
			GpuDiagnostics recoverable(GpuFault::DeviceLost);
			CHECK_FALSE(recoverable.ShouldFailDeviceRecreation());
			recoverable.SetDeviceLost(true);
			CHECK_FALSE(recoverable.ShouldFailDeviceRecreation());
		}

		TEST_CASE("GpuDiagnostics: injected device loss stays marked after native reports and resetting counts")
		{
			GpuDiagnostics diagnostics;
			diagnostics.SetDeviceLost(true);
			CHECK(diagnostics.IsDeviceLost());
			CHECK(diagnostics.IsDeviceLossInjected());
			CHECK(diagnostics.GetInjectedFault() == GpuFault::None);
			diagnostics.SetDeviceLost(); // RaiseDeviceLost reports the loss again through this defaulted call.
			diagnostics.ResetCounts();
			diagnostics.OnSubmitted();
			CHECK(diagnostics.IsDeviceLost());
			CHECK(diagnostics.IsDeviceLossInjected());
			{
				Test::ExpectLog error(LogLevel::Error, "NVRHI: Device Removed!");
				diagnostics.ReportNvrhiMessage(nvrhi::MessageSeverity::Error, "Device Removed!");
			}
			CHECK(diagnostics.IsDeviceLossInjected());
			GpuDiagnostics nativeThenInjected;
			nativeThenInjected.SetDeviceLost();
			CHECK_FALSE(nativeThenInjected.IsDeviceLossInjected());
			nativeThenInjected.SetDeviceLost(true);
			CHECK(nativeThenInjected.IsDeviceLossInjected());
		}

		TEST_CASE("GpuDiagnostics: observing injected device loss across threads also observes its marker")
		{
			GpuDiagnostics diagnostics;
			std::jthread publisher([&diagnostics]()
			{
				diagnostics.SetDeviceLost(true);
			});
			REQUIRE(Test::WaitUntil([&diagnostics]()
			{
				return diagnostics.IsDeviceLost();
			}));
			CHECK(diagnostics.IsDeviceLossInjected());
		}

		TEST_CASE("GpuDiagnostics: an injected device loss strikes once, at the first submission after a garbage collection")
		{
			// M18 acceptance (C18.3, decision 30): the startup submissions and the first frame are unaffected, the loss strikes
			// the second frame, and the recovered device is not lost again.
			for (const GpuFault fault : { GpuFault::DeviceLost, GpuFault::DeviceLostUnrecoverable })
			{
				CAPTURE(GpuFaultToString(fault));
				GpuDiagnostics diagnostics(fault);
				diagnostics.OnSubmitted();
				CHECK_FALSE(diagnostics.IsDeviceLost());
				diagnostics.OnGarbageCollected();
				{
					Test::ExpectLog injected(LogLevel::Warn, "Injected GPU fault");
					diagnostics.OnSubmitted();
				}
				CHECK(diagnostics.IsDeviceLost());
				CHECK(diagnostics.IsDeviceLossInjected());
				diagnostics.ClearDeviceLost();
				diagnostics.OnGarbageCollected();
				diagnostics.OnSubmitted();
				CHECK_FALSE(diagnostics.IsDeviceLost());
			}
		}

		TEST_CASE("GpuDiagnostics: injected faults follow --gpu-inject-fault")
		{
			// device-lost: the flag after the first submission once a garbage collection armed it (decision 30).
			GpuDiagnostics deviceLost(GpuFault::DeviceLost);
			CHECK_FALSE(deviceLost.IsDeviceLost());
			CHECK_FALSE(deviceLost.IsDeviceLossInjected());
			deviceLost.OnGarbageCollected();
			deviceLost.OnSubmitted();
			CHECK(deviceLost.IsDeviceLost());
			CHECK(deviceLost.IsDeviceLossInjected());
			deviceLost.SetDeviceLost();
			CHECK(deviceLost.IsDeviceLossInjected());
			CHECK_FALSE(deviceLost.ShouldInjectHang());

			// oom-texture: sampled-only textures fail, render targets and storage images do not.
			GpuDiagnostics oom(GpuFault::OomTexture);
			nvrhi::TextureDesc sampled;
			sampled.isShaderResource = true;
			CHECK(oom.ShouldFailTextureCreation(sampled));
			nvrhi::TextureDesc target = sampled;
			target.isRenderTarget = true;
			CHECK_FALSE(oom.ShouldFailTextureCreation(target));
			nvrhi::TextureDesc storage = sampled;
			storage.isUAV = true;
			CHECK_FALSE(oom.ShouldFailTextureCreation(storage));
			oom.OnSubmitted();
			CHECK_FALSE(oom.IsDeviceLost());

			// hang: every bounded wait slice times out, from the start (M5), its reset wait's too (M18, C18.4).
			GpuDiagnostics hang(GpuFault::Hang);
			CHECK(hang.ShouldInjectHang());
			CHECK(hang.HasBoundedWaitFault());
			CHECK(hang.TakeBoundedWaitFault() == GpuInjectedHang::Hang);
			CHECK(hang.TakeBoundedWaitFault() == GpuInjectedHang::Hang);
			CHECK_FALSE(hang.ShouldFailTextureCreation(sampled));
			CHECK_FALSE(hang.ShouldFailShaderSpecialization());
			CHECK_FALSE(hang.TakeRenderTargetCreationFault());
			CHECK_FALSE(hang.TakePipelineCreationFault());
			CHECK_FALSE(deviceLost.HasBoundedWaitFault());
			CHECK(deviceLost.TakeBoundedWaitFault() == GpuInjectedHang::None);
			CHECK_FALSE(deviceLost.TakeRenderTargetCreationFault());
			CHECK_FALSE(deviceLost.TakePipelineCreationFault());

			// oom-specialization: only shader specializations (the lazily created debug-view pipelines) fail.
			GpuDiagnostics specialization(GpuFault::OomSpecialization);
			CHECK(specialization.ShouldFailShaderSpecialization());
			CHECK_FALSE(specialization.ShouldFailTextureCreation(sampled));
			CHECK_FALSE(specialization.ShouldInjectHang());
			CHECK_FALSE(oom.ShouldFailShaderSpecialization());

			// No fault: nothing is forced.
			GpuDiagnostics none;
			none.OnSubmitted();
			CHECK_FALSE(none.IsDeviceLost());
			CHECK_FALSE(none.ShouldFailTextureCreation(sampled));
			CHECK_FALSE(none.ShouldInjectHang());
			CHECK_FALSE(none.ShouldFailShaderSpecialization());
			none.OnGarbageCollected();
			CHECK_FALSE(none.HasBoundedWaitFault());
			CHECK(none.TakeBoundedWaitFault() == GpuInjectedHang::None);
			CHECK_FALSE(none.TakeRenderTargetCreationFault());
			CHECK_FALSE(none.TakePipelineCreationFault());
		}

		TEST_CASE("GpuDiagnostics: hang resets and out-of-memory faults strike after the first garbage collection, once or every time")
		{
			// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 46): an application's startup and first frame run
			// without them; the once spellings strike the first wait or creation of their kind after the device's first garbage
			// collection, so a retried request or a recovered device is not struck again, and the -persistent spellings strike
			// every one, the retry's included.
			struct Row
			{
				GpuFault Fault = GpuFault::None;
				GpuInjectedHang Wait = GpuInjectedHang::None;
				bool Target = false;
				bool Pipeline = false;
				bool Swapchain = false;
				bool Persistent = false;
			};
			const std::array<Row, 8> rows = { {
				{ .Fault = GpuFault::HangReset, .Wait = GpuInjectedHang::HangReset },
				{ .Fault = GpuFault::HangResetPersistent, .Wait = GpuInjectedHang::HangReset, .Persistent = true },
				{ .Fault = GpuFault::OomRenderTarget, .Target = true },
				{ .Fault = GpuFault::OomRenderTargetPersistent, .Target = true, .Persistent = true },
				{ .Fault = GpuFault::OomPipeline, .Pipeline = true },
				{ .Fault = GpuFault::OomPipelinePersistent, .Pipeline = true, .Persistent = true },
				{ .Fault = GpuFault::OomSwapchain, .Swapchain = true },
				{ .Fault = GpuFault::OomSwapchainPersistent, .Swapchain = true, .Persistent = true },
			} };
			nvrhi::TextureDesc sampled;
			sampled.isShaderResource = true;
			for (const Row& row : rows)
			{
				CAPTURE(GpuFaultToString(row.Fault));
				GpuDiagnostics diagnostics(row.Fault);
				CHECK_FALSE(diagnostics.HasBoundedWaitFault());
				CHECK(diagnostics.TakeBoundedWaitFault() == GpuInjectedHang::None);
				CHECK_FALSE(diagnostics.TakeRenderTargetCreationFault());
				CHECK_FALSE(diagnostics.TakePipelineCreationFault());
				CHECK_FALSE(diagnostics.TakeSwapchainCreationFault());

				diagnostics.OnGarbageCollected();
				CHECK(diagnostics.HasBoundedWaitFault() == (row.Wait != GpuInjectedHang::None));
				CHECK(diagnostics.TakeBoundedWaitFault() == row.Wait);
				CHECK(diagnostics.TakeRenderTargetCreationFault() == row.Target);
				CHECK(diagnostics.TakePipelineCreationFault() == row.Pipeline);
				CHECK(diagnostics.TakeSwapchainCreationFault() == row.Swapchain);

				// After a recovery and the next frames: only the persistent spellings strike again.
				diagnostics.ClearDeviceLost();
				diagnostics.OnGarbageCollected();
				CHECK(diagnostics.HasBoundedWaitFault() == (row.Persistent && row.Wait != GpuInjectedHang::None));
				CHECK(diagnostics.TakeBoundedWaitFault() == (row.Persistent ? row.Wait : GpuInjectedHang::None));
				CHECK(diagnostics.TakeRenderTargetCreationFault() == (row.Persistent && row.Target));
				CHECK(diagnostics.TakePipelineCreationFault() == (row.Persistent && row.Pipeline));
				CHECK(diagnostics.TakeSwapchainCreationFault() == (row.Persistent && row.Swapchain));

				// None of them loses the device by itself (a hang reset's wait reports the loss), or fails what the older
				// faults fail.
				diagnostics.OnSubmitted();
				CHECK_FALSE(diagnostics.IsDeviceLost());
				CHECK_FALSE(diagnostics.ShouldInjectHang());
				CHECK_FALSE(diagnostics.ShouldFailTextureCreation(sampled));
				CHECK_FALSE(diagnostics.ShouldFailShaderSpecialization());
				CHECK_FALSE(diagnostics.ShouldFailDeviceRecreation());
			}

			// ENGINE_GPU_FAULT_AT_STARTUP's arming (GraphicsDevice::Create): the same faults strike before any garbage
			// collection, an application's startup, and so do the device-lost faults; the unarmed faults are unchanged.
			GpuDiagnostics startup(GpuFault::OomRenderTargetPersistent);
			startup.ArmInjectedFault();
			CHECK(startup.TakeRenderTargetCreationFault());
			GpuDiagnostics startupLoss(GpuFault::DeviceLost);
			startupLoss.ArmInjectedFault();
			startupLoss.OnSubmitted();
			CHECK(startupLoss.IsDeviceLost());
			GpuDiagnostics hang(GpuFault::Hang);
			hang.ArmInjectedFault();
			CHECK(hang.TakeBoundedWaitFault() == GpuInjectedHang::Hang);
			CHECK_FALSE(hang.TakeRenderTargetCreationFault());
		}

		TEST_CASE("GpuDiagnostics: Vulkan results map to fatal-error kinds")
		{
			CHECK(GetFatalErrorKind(VK_ERROR_DEVICE_LOST) == FatalErrorKind::DeviceLost);
			CHECK(GetFatalErrorKind(VK_ERROR_OUT_OF_HOST_MEMORY) == FatalErrorKind::OutOfMemory);
			CHECK(GetFatalErrorKind(VK_ERROR_OUT_OF_DEVICE_MEMORY) == FatalErrorKind::OutOfMemory);
			CHECK(GetFatalErrorKind(VK_ERROR_INITIALIZATION_FAILED) == FatalErrorKind::Gpu);
			CHECK(GetFatalErrorKind(VK_ERROR_SURFACE_LOST_KHR) == FatalErrorKind::Gpu);
		}

		TEST_CASE("GpuDiagnostics: a Vulkan result is named by vulkan.hpp's name and its value")
		{
			CHECK(VkResultToString(VK_SUCCESS) == "Success (0)");
			CHECK(VkResultToString(VK_ERROR_DEVICE_LOST) == "ErrorDeviceLost (-4)");
			CHECK(VkResultToString(VK_ERROR_OUT_OF_DATE_KHR) == "ErrorOutOfDateKHR (-1000001004)");
		}

		TEST_CASE("GpuDiagnostics: the loader's installation notes are logged without counting and everything else counts")
		{
			constexpr VkDebugUtilsMessageTypeFlagsEXT General = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
			constexpr VkDebugUtilsMessageTypeFlagsEXT Validation = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
			constexpr VkDebugUtilsMessageTypeFlagsEXT Performance = VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			CHECK(IsLoaderInstallationMessage(General, "Loader Message"));
			CHECK_FALSE(IsLoaderInstallationMessage(General | Validation, "Loader Message"));
			CHECK_FALSE(IsLoaderInstallationMessage(General | Performance, "Loader Message"));
			CHECK_FALSE(IsLoaderInstallationMessage(General, "VUID-vkCreateDevice-ppEnabledExtensionNames-01387"));
			CHECK_FALSE(IsLoaderInstallationMessage(General, ""));

			GpuDiagnostics diagnostics;
			// A general "Loader Message" about the installation, even at error severity, is a warning that does not count.
			{
				Test::ExpectLog note(LogLevel::Warn, "Vulkan loader (Error): a layer manifest is unreadable");
				diagnostics.ReportDebugUtilsMessage(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, General, "Loader Message",
					"a layer manifest is unreadable");
			}
			diagnostics.ReportDebugUtilsMessage(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, General, "Loader Message",
				"an implicit layer was built for an older API version");
			CHECK(diagnostics.GetErrorCount() == 0);
			CHECK(diagnostics.GetWarningCount() == 0);

			// The loader's validation of an API call carries the validation type and counts, with its ID in the text.
			{
				Test::ExpectLog error(LogLevel::Error, "Vulkan validation: [Loader Message] vkCreateDevice: an invalid extension");
				diagnostics.ReportDebugUtilsMessage(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, General | Validation, "Loader Message",
					"vkCreateDevice: an invalid extension");
			}
			// So do performance messages, and every other ID, by the highest severity bit.
			diagnostics.ReportDebugUtilsMessage(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, Performance, "Loader Message", "slow");
			{
				Test::ExpectLog hazard(LogLevel::Error, "Vulkan validation: [SYNC-HAZARD-WRITE-AFTER-WRITE] a hazard");
				diagnostics.ReportDebugUtilsMessage(
					static_cast<VkDebugUtilsMessageSeverityFlagBitsEXT>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
						| VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT),
					Validation, "SYNC-HAZARD-WRITE-AFTER-WRITE", "a hazard");
			}
			// An ID already in the text is not repeated; info messages are not counted.
			{
				Test::ExpectLog warning(LogLevel::Warn, "Vulkan validation: VUID-x-01 is broken");
				diagnostics.ReportDebugUtilsMessage(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, Validation, "VUID-x-01", "VUID-x-01 is broken");
			}
			diagnostics.ReportDebugUtilsMessage(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, Validation, "VUID-x-02", "informational");
			CHECK(diagnostics.GetErrorCount() == 2);
			CHECK(diagnostics.GetWarningCount() == 2);
			const GpuMessageCounts counts = diagnostics.GetCounts();
			CHECK(counts.Errors == 2);
			CHECK(counts.Warnings == 2);
		}
	}

}
