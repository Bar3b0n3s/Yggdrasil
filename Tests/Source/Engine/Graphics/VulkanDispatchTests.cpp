#include "TestsPCH.h"

#include "Engine/Graphics/VulkanDispatch.h"

#include "Engine/App/ExitCode.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Process.h"
#include "Support/DeathTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"

namespace Engine {

	// Reports whether this child's ProcessContext found a Vulkan loader: the child of the ENGINE_VULKAN_LOADER test.
	ENGINE_DEATH_TEST("Graphics/ReportsVulkanLoader")
	{
		const ProcessContext* context = ProcessContext::GetCurrent();
		if (context == nullptr)
		{
			ENGINE_CORE_ERROR("The child has no process context");
			return;
		}
		ENGINE_CORE_WARN("Vulkan loader available: [{}]", context->IsVulkanLoaderAvailable() ? "yes" : "no");
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("VulkanDispatch: the process loader is initialized once and handed to GLFW"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// The Tests main asks for the loader if available; on a machine with a GPU it is there.
			const ProcessContext* context = ProcessContext::GetCurrent();
			REQUIRE(context != nullptr);
			if (!context->IsVulkanLoaderAvailable())
			{
				Test::ReportGpuUnavailable("no Vulkan loader in this process");
				return;
			}
			CHECK(VulkanDispatch::IsInitialized());
			CHECK(VulkanDispatch::GetInstanceProcAddr() != nullptr);
			// At least Vulkan 1.3 from the loader on a machine that runs the GPU suite (VK_API_VERSION_MAJOR/MINOR).
			const uint32_t version = VulkanDispatch::GetLoaderApiVersion();
			CHECK(((version >> 22) & 0x7Fu) == 1);
			CHECK(((version >> 12) & 0x3FFu) >= 3);
		}

		TEST_CASE("VulkanDispatch: ENGINE_VULKAN_LOADER=missing makes the loader unavailable" * doctest::skip(true))
		{
			// The Tests main loads the loader if available, so a missing one is a warning there, never a failure; an
			// application, which requires it, exits with code 3 (EditorAppTests.cpp).
			ProcessSpecification specification = Test::MakeTestsChildSpecification({ "--death-test=Graphics/ReportsVulkanLoader" });
			specification.Environment.emplace_back(std::string(VulkanLoaderEnvironmentVariable), "missing");
			const Result<ProcessResult> child = Process::Run(specification, std::chrono::seconds(60));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == ExitCode::Failed); // the body returns without dying
			CHECK(child->StandardError.contains("Vulkan loader available: [no]"));
			CHECK(child->StandardError.contains(NoVulkanLoaderMessage));
		}
	}

}
