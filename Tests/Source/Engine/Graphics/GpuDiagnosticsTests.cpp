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

		TEST_CASE("GpuDiagnostics: the device-lost flag is set by SetDeviceLost and never cleared")
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

		TEST_CASE("GpuDiagnostics: injected faults follow --gpu-inject-fault")
		{
			// device-lost: the flag after the first submission.
			GpuDiagnostics deviceLost(GpuFault::DeviceLost);
			CHECK_FALSE(deviceLost.IsDeviceLost());
			CHECK_FALSE(deviceLost.IsDeviceLossInjected());
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

			// hang: every bounded wait slice times out.
			GpuDiagnostics hang(GpuFault::Hang);
			CHECK(hang.ShouldInjectHang());
			CHECK_FALSE(hang.ShouldFailTextureCreation(sampled));

			// No fault: nothing is forced.
			GpuDiagnostics none;
			none.OnSubmitted();
			CHECK_FALSE(none.IsDeviceLost());
			CHECK_FALSE(none.ShouldFailTextureCreation(sampled));
			CHECK_FALSE(none.ShouldInjectHang());
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
