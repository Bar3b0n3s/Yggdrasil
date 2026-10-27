#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsSpecification.h"

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GraphicsSpecification: command-line spellings round-trip and reject anything else")
		{
			CHECK(RendererModeFromCommandLine("vulkan") == RendererMode::Vulkan);
			CHECK(RendererModeFromCommandLine("none") == RendererMode::None);
			CHECK_FALSE(RendererModeFromCommandLine("Vulkan").has_value());
			CHECK_FALSE(RendererModeFromCommandLine("").has_value());
			CHECK(RendererModeToString(RendererMode::Vulkan) == "Vulkan");
			CHECK(RendererModeToString(RendererMode::None) == "None");
			for (const RendererMode mode : { RendererMode::None, RendererMode::Vulkan })
				CHECK(RendererModeFromCommandLine(RendererModeToCommandLine(mode)) == mode);
			CHECK(RendererModeToCommandLine(RendererMode::Vulkan) == "vulkan");

			for (const VulkanApiVersion version : { VulkanApiVersion::Vulkan13, VulkanApiVersion::Vulkan14 })
				CHECK(VulkanApiVersionFromString(VulkanApiVersionToString(version)) == version);
			CHECK_FALSE(VulkanApiVersionFromString("1.2").has_value());
			CHECK_FALSE(VulkanApiVersionFromString("1.3.0").has_value());

			for (const GpuFault fault : { GpuFault::None, GpuFault::DeviceLost, GpuFault::OomTexture, GpuFault::Hang, GpuFault::OomSpecialization,
					 GpuFault::DeviceLostUnrecoverable, GpuFault::HangReset, GpuFault::HangResetPersistent, GpuFault::OomRenderTarget,
					 GpuFault::OomRenderTargetPersistent, GpuFault::OomPipeline, GpuFault::OomPipelinePersistent, GpuFault::OomSwapchain,
					 GpuFault::OomSwapchainPersistent })
				CHECK(GpuFaultFromString(GpuFaultToString(fault)) == fault);
			CHECK(GpuFaultToString(GpuFault::DeviceLost) == "device-lost");
			CHECK(GpuFaultToString(GpuFault::DeviceLostUnrecoverable) == "device-lost-unrecoverable");
			CHECK(GpuFaultToString(GpuFault::OomTexture) == "oom-texture");
			CHECK(GpuFaultToString(GpuFault::OomSpecialization) == "oom-specialization");
			// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 46).
			CHECK(GpuFaultToString(GpuFault::HangReset) == "hang-reset");
			CHECK(GpuFaultToString(GpuFault::HangResetPersistent) == "hang-reset-persistent");
			CHECK(GpuFaultToString(GpuFault::OomRenderTarget) == "oom-rendertarget");
			CHECK(GpuFaultToString(GpuFault::OomRenderTargetPersistent) == "oom-rendertarget-persistent");
			CHECK(GpuFaultToString(GpuFault::OomPipeline) == "oom-pipeline");
			CHECK(GpuFaultToString(GpuFault::OomPipelinePersistent) == "oom-pipeline-persistent");
			CHECK(GpuFaultToString(GpuFault::OomSwapchain) == "oom-swapchain");
			CHECK(GpuFaultToString(GpuFault::OomSwapchainPersistent) == "oom-swapchain-persistent");
			CHECK(GpuFaultToString(static_cast<GpuFault>(std::to_underlying(GpuFault::OomSwapchainPersistent) + 1)) == "unknown");
			CHECK_FALSE(GpuFaultFromString("Device-Lost").has_value());
			CHECK_FALSE(GpuFaultFromString("oom-rendertarget-Persistent").has_value());
			CHECK_FALSE(GpuFaultFromString("hang-persistent").has_value());
		}

		TEST_CASE("GraphicsSpecification: defaults follow the configuration table")
		{
			const GraphicsSpecification defaults;
			// §2.2: validation on by default in Debug, off in Release (Tests never build Dist).
#if defined(ENGINE_DEBUG)
			CHECK(defaults.Validation);
#else
			CHECK_FALSE(defaults.Validation);
#endif
			CHECK_FALSE(defaults.SynchronizationValidation);
			CHECK(defaults.MaxApiVersion == VulkanApiVersion::Vulkan14);
			CHECK(defaults.GpuOverride.empty());
			CHECK(defaults.FramesInFlight == 2);
			CHECK(defaults.VSync);
			CHECK(defaults.InjectFault == GpuFault::None);
			CHECK(defaults.ResetWaitMilliseconds == DefaultGpuResetWaitMilliseconds);
			// M18 (C18.4, decision 41): the default reset wait fits a Windows TDR and a Linux GPU reset, within the bound.
			CHECK(DefaultGpuResetWaitMilliseconds >= 10'000);
			CHECK(DefaultGpuResetWaitMilliseconds <= MaxGpuResetWaitMilliseconds);
			CHECK(GpuTestHooksEnabled);
		}
	}

}
