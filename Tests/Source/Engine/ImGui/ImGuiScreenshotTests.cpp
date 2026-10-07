#include "TestsPCH.h"

#include "Engine/ImGui/ImGuiScreenshot.h"

#include "Engine/App/EngineContext.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"

#include <imgui.h>

namespace Engine {

	TEST_SUITE("ImGui")
	{
		TEST_CASE("ImGuiScreenshot: captures the last UI frame at the UI's framebuffer size"
			* doctest::test_suite(Test::GpuSuite))
		{
			// A headless context with a null-platform window and a device, as the headless editor has.
			Result<Scope<EngineContext>> created = EngineContext::Create({
				.WorkerCount = 0,
				.Window = WindowSpecification{ .Title = "ImGuiScreenshot", .Width = 320, .Height = 200 },
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
			Result<Scope<ImGuiLayer>> layer = ImGuiLayer::Create(*context.GetWindow(), device, *context.GetPipelineFactory(), {});
			REQUIRE_MESSAGE(layer.has_value(), layer.error().ToString());

			// Before the first UI frame there is nothing to capture.
			const Result<Image> early = CaptureImGuiScreenshot(device, **layer, {});
			REQUIRE_FALSE(early.has_value());
			CHECK(early.error().GetCode() == ErrorCode::InvalidState);

			FramePacer pacer(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 320, .Height = 200 });
			REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
			for (int frame = 0; frame < 2; ++frame)
			{
				pacer.BeginFrame();
				(*layer)->BeginFrame(1.0 / 60.0, pacer.GetFrameSlot());
				ImGui::ShowDemoWindow();
				(*layer)->EndFrame();
				(*commandList)->open();
				target->Clear(**commandList);
				const Status rendered = (*layer)->Render(**commandList, *target->GetFramebuffer());
				CHECK_MESSAGE(rendered.has_value(), (rendered.has_value() ? std::string() : rendered.error().ToString()));
				(*commandList)->close();
				pacer.EndFrame(device.ExecuteCommandList(**commandList));
				device.RunGarbageCollection();
			}

			// The capture re-renders the last frame's draw data: the window's framebuffer size, with the demo window drawn over
			// the clear colour, and the same image on a second capture.
			const Result<Image> image = CaptureImGuiScreenshot(device, **layer, {});
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			CHECK(image->Width == context.GetWindow()->GetFramebufferWidth());
			CHECK(image->Height == context.GetWindow()->GetFramebufferHeight());
			CHECK(image->Format == nvrhi::Format::RGBA8_UNORM);
			REQUIRE(image->IsValid());
			const bool allClear = std::ranges::all_of(image->Pixels, [](std::byte value)
			{
				return value == std::byte{ 0 } || value == std::byte{ 255 };
			});
			CHECK_FALSE(allClear);
			const Result<Image> again = CaptureImGuiScreenshot(device, **layer, {});
			REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
			CHECK(again->Pixels == image->Pixels);

			// MaxDimension keeps the aspect ratio.
			const Result<Image> small = CaptureImGuiScreenshot(device, **layer, { .MaxDimension = 160 });
			REQUIRE_MESSAGE(small.has_value(), small.error().ToString());
			CHECK(small->Width == 160);
			CHECK(small->Height == 100);

			device.WaitForIdle();
			layer->reset();
			CHECK(device.GetDiagnostics().GetErrorCount() == 0);
		}
	}

}
