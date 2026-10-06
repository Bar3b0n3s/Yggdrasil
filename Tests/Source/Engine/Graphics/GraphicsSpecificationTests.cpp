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

			for (const VulkanApiVersion version : { VulkanApiVersion::Vulkan13, VulkanApiVersion::Vulkan14 })
				CHECK(VulkanApiVersionFromString(VulkanApiVersionToString(version)) == version);
			CHECK_FALSE(VulkanApiVersionFromString("1.2").has_value());
			CHECK_FALSE(VulkanApiVersionFromString("1.3.0").has_value());

			for (const GpuFault fault : { GpuFault::None, GpuFault::DeviceLost, GpuFault::OomTexture, GpuFault::Hang })
				CHECK(GpuFaultFromString(GpuFaultToString(fault)) == fault);
			CHECK(GpuFaultToString(GpuFault::DeviceLost) == "device-lost");
			CHECK(GpuFaultToString(GpuFault::OomTexture) == "oom-texture");
			CHECK_FALSE(GpuFaultFromString("Device-Lost").has_value());
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
			CHECK(GpuTestHooksEnabled);
		}
	}

}
