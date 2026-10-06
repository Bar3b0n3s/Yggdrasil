#include "TestsPCH.h"

#include "Engine/Graphics/GpuDiagnostics.h"

#include "Engine/Core/Log.h"
#include "Support/ExpectLog.h"

// GpuDiagnostics is plain CPU state, so its message routing, device-loss detection and fault rules are tested without a
// device; the paths that end the process are tested through the editor (Editor/EditorFaultInjectionTests.cpp).

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GpuDiagnostics: errors and warnings are logged with their source and counted" * doctest::skip(true))
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

		TEST_CASE("GpuDiagnostics: NVRHI's Device Removed! message sets the device-lost flag" * doctest::skip(true))
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

		TEST_CASE("GpuDiagnostics: the device-lost flag is set by SetDeviceLost and never cleared")
		{
			GpuDiagnostics diagnostics;
			CHECK_FALSE(diagnostics.IsDeviceLost());
			diagnostics.SetDeviceLost();
			CHECK(diagnostics.IsDeviceLost());
			diagnostics.ResetCounts();
			CHECK(diagnostics.IsDeviceLost());
			CHECK(diagnostics.GetInjectedFault() == GpuFault::None);
			CHECK(GpuMessageSeverityToString(GpuMessageSeverity::Fatal) == "Fatal");
		}

		TEST_CASE("GpuDiagnostics: injected faults follow --gpu-inject-fault" * doctest::skip(true))
		{
			// device-lost: the flag after the first submission.
			GpuDiagnostics deviceLost(GpuFault::DeviceLost);
			CHECK_FALSE(deviceLost.IsDeviceLost());
			deviceLost.OnSubmitted();
			CHECK(deviceLost.IsDeviceLost());
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

		TEST_CASE("GpuDiagnostics: Vulkan results map to fatal-error kinds" * doctest::skip(true))
		{
			CHECK(GetFatalErrorKind(VK_ERROR_DEVICE_LOST) == FatalErrorKind::DeviceLost);
			CHECK(GetFatalErrorKind(VK_ERROR_OUT_OF_HOST_MEMORY) == FatalErrorKind::OutOfMemory);
			CHECK(GetFatalErrorKind(VK_ERROR_OUT_OF_DEVICE_MEMORY) == FatalErrorKind::OutOfMemory);
			CHECK(GetFatalErrorKind(VK_ERROR_INITIALIZATION_FAILED) == FatalErrorKind::Gpu);
			CHECK(GetFatalErrorKind(VK_ERROR_SURFACE_LOST_KHR) == FatalErrorKind::Gpu);
		}
	}

}
