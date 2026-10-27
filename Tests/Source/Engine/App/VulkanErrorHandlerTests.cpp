#include "TestsPCH.h"

#include "Engine/App/VulkanErrorHandler.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/DeathTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"

#include <vulkan/vulkan.hpp>

#include <optional>
#include <system_error>

namespace Engine {

	// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 28): the child of "VulkanErrorHandler: any other Vulkan error
	// in an automation method on a healthy device ends the process": the handler gets the out-of-memory error vulkan.hpp
	// throws out of NVRHI while the device is not lost.
	ENGINE_DEATH_TEST("App/VulkanErrorHandlerOutOfMemoryOnHealthyDevice")
	{
		const ProcessContext* process = ProcessContext::GetCurrent();
		if (process == nullptr || !process->IsVulkanLoaderAvailable())
		{
			ENGINE_CORE_ERROR("No Vulkan loader in the handler's child");
			return;
		}
		GraphicsSpecification graphics;
		graphics.Validation = true;
		graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
		Result<Scope<EngineContext>> created = EngineContext::Create({ .WorkerCount = 0, .Graphics = graphics });
		if (!created.has_value())
		{
			ENGINE_CORE_ERROR("The handler's child cannot create its engine context: {}", created.error());
			return;
		}
		const SystemErrorHandler handler = MakeVulkanSystemErrorHandler(**created);
		const std::optional<Error> answered = handler(vk::OutOfDeviceMemoryError("a buffer of the healthy device"), "viewport.screenshot");
		ENGINE_CORE_ERROR("The handler answered a Vulkan error on a healthy device instead of ending the process: {}",
			answered.has_value() ? answered->ToString() : std::string("handed on"));
	}

	TEST_SUITE("App")
	{
		TEST_CASE("VulkanErrorHandler: a device loss in an automation method fails the request with Gpu, and the device recovers"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 28): the loss is reported and recovered at the frame
			// boundary, and the server keeps serving; the errors are thrown here as vulkan.hpp throws them out of NVRHI.
			if (!Test::ProbeGpuForProcess())
				return;
			GraphicsSpecification graphics;
			graphics.Validation = true;
			graphics.SynchronizationValidation = true;
			graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
			Result<Scope<EngineContext>> created = EngineContext::Create({ .WorkerCount = 0, .Graphics = graphics });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			EngineContext& context = **created;
			GraphicsDevice& device = *context.GetGraphicsDevice();
			const SystemErrorHandler handler = MakeVulkanSystemErrorHandler(context);

			// A system error that is not Vulkan's is handed on to the Dispatcher.
			CHECK_FALSE(handler(std::system_error(std::make_error_code(std::errc::io_error)), "viewport.screenshot").has_value());
			CHECK_FALSE(device.IsLost());

			const std::optional<Error> lost = handler(vk::DeviceLostError("a wait of NVRHI's on the lost device"), "viewport.screenshot");
			REQUIRE(lost.has_value());
			CHECK(lost->GetCode() == ErrorCode::Gpu);
			CHECK(lost->ToString().contains("viewport.screenshot"));
			CHECK(device.IsLost());
			REQUIRE(device.GetLossReport().has_value());
			CHECK(device.GetLossReport()->Context.contains("Vulkan error in automation method 'viewport.screenshot'"));
			CHECK_FALSE(device.GetLossReport()->Injected);

			// Any Vulkan error on the lost device is the loss's consequence: answered the same way, the first report kept.
			const std::optional<Error> consequence = handler(vk::OutOfDeviceMemoryError("a descriptor pool of the lost device"), "editor.screenshot");
			REQUIRE(consequence.has_value());
			CHECK(consequence->GetCode() == ErrorCode::Gpu);
			CHECK(consequence->ToString().contains("editor.screenshot"));
			CHECK(device.GetLossReport()->Context.contains("viewport.screenshot"));

			const Status recovered = context.RecoverGraphics();
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			CHECK_FALSE(device.IsLost());
			CHECK(device.GetRecoveryCount() == 1);
			const GpuMessageCounts counts = context.DestroyGraphics();
			CHECK(counts.Errors == 0);
			CHECK(counts.Warnings == 0);
		}

		TEST_CASE("VulkanErrorHandler: any other Vulkan error in an automation method on a healthy device ends the process"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, decision 28's vk::SystemError rule): only a device loss, or an error on a device already lost, is
			// recovered; an out-of-memory error on a healthy device still ends the process through RaiseVulkanError:
			// FatalError(OutOfMemory) and exit code 4.
			if (!Test::ProbeGpuForProcess())
				return;
			ENGINE_CHECK_DEATH("App/VulkanErrorHandlerOutOfMemoryOnHealthyDevice", "Fatal error (OutOfMemory)");
		}
	}

}
