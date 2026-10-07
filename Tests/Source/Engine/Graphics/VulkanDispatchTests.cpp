#include "TestsPCH.h"

#include "Engine/Graphics/VulkanDispatch.h"

#include "Engine/App/ExitCode.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Platform/Process.h"
#include "Support/DeathTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"

namespace Engine {

	// Reports whether this child's ProcessContext found a Vulkan loader, what VulkanDispatch says without one, and the error
	// a device creation gets: the child of the ENGINE_VULKAN_LOADER tests.
	ENGINE_DEATH_TEST("Graphics/ReportsVulkanLoader")
	{
		const ProcessContext* context = ProcessContext::GetCurrent();
		if (context == nullptr)
		{
			ENGINE_CORE_ERROR("The child has no process context");
			return;
		}
		ENGINE_CORE_WARN("Vulkan loader available: [{}]", context->IsVulkanLoaderAvailable() ? "yes" : "no");
		ENGINE_CORE_WARN("Dispatch initialized: [{}], vkGetInstanceProcAddr: [{}], loader version: [{}]",
			VulkanDispatch::IsInitialized() ? "yes" : "no", VulkanDispatch::GetInstanceProcAddr() != nullptr ? "set" : "null",
			VulkanDispatch::GetLoaderApiVersion());
		const Result<Scope<GraphicsDevice>> device = GraphicsDevice::Create({});
		ENGINE_CORE_WARN("Device creation: [{}]", device.has_value() ? std::string("created") : std::string(ErrorCodeToString(device.error().GetCode())));
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("VulkanDispatch: the process loader is initialized once and handed to GLFW"
			* doctest::test_suite(Test::GpuSuite))
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

		TEST_CASE("VulkanDispatch: ENGINE_VULKAN_LOADER=missing makes the loader unavailable")
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
			// Nothing stays loaded, and a device needs the loader (GraphicsDevice::Create: InvalidState).
			CHECK(child->StandardError.contains("Dispatch initialized: [no], vkGetInstanceProcAddr: [null], loader version: [0]"));
			CHECK(child->StandardError.contains("Device creation: [InvalidState]"));
		}

		TEST_CASE("VulkanDispatch: an ENGINE_VULKAN_LOADER value other than missing is InvalidArgument naming the variable")
		{
			// A misspelled hook never passes silently: under the Tests main's IfAvailable policy the error is the warning.
			ProcessSpecification specification = Test::MakeTestsChildSpecification({ "--death-test=Graphics/ReportsVulkanLoader" });
			specification.Environment.emplace_back(std::string(VulkanLoaderEnvironmentVariable), "Missing");
			const Result<ProcessResult> child = Process::Run(specification, std::chrono::seconds(60));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == ExitCode::Failed); // the body returns without dying
			CHECK(child->StandardError.contains("Vulkan loader available: [no]"));
			CHECK(child->StandardError.contains("InvalidArgument: 'Missing' is not a valid value of ENGINE_VULKAN_LOADER"));
		}
	}

}
