#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/ShaderLibrary.h"
#include "Support/TestOptions.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("HeadlessGpuFixture: creates a validated headless device with the run's API cap and the shaders mounted"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			CHECK(gpu.GetUnavailableReason().empty());
			const GraphicsDeviceInfo& info = gpu.GetDevice().GetInfo();
			CHECK(info.Validation);
			CHECK(info.SynchronizationValidation);
			CHECK(info.ApiVersion <= Test::GetTestOptions().VulkanApi);
			CHECK(gpu.GetVfs().IsMounted(ShaderScheme));
			const Result<const ShaderReflection*> reflection = gpu.GetShaders().GetReflection("Triangle", "VSMain");
			CHECK_MESSAGE(reflection.has_value(), (reflection.has_value() ? std::string() : reflection.error().ToString()));
			gpu.Reset();
			CHECK_FALSE(gpu.IsAvailable());
		}

		TEST_CASE("HeadlessGpuFixture: GPU process arguments follow the run's options" * doctest::skip(true))
		{
			const std::vector<std::string> application = Test::GetGpuApplicationArguments();
			CHECK(std::ranges::find(application, std::string("--gpu-validation")) != application.end());
			const bool capped = Test::GetTestOptions().VulkanApi == VulkanApiVersion::Vulkan13;
			CHECK((std::ranges::find(application, std::string("--vulkan-api")) != application.end()) == capped);

			const std::vector<std::string> child = Test::GetGpuTestsChildArguments();
			const std::string api = std::format("--vulkan-api={}", VulkanApiVersionToString(Test::GetTestOptions().VulkanApi));
			CHECK(std::ranges::find(child, api) != child.end());
			CHECK((std::ranges::find(child, std::string("--require-gpu")) != child.end()) == Test::GetTestOptions().RequireGpu);
		}
	}

}
