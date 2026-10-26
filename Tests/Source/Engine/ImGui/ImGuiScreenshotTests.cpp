#include "TestsPCH.h"

#include "Engine/ImGui/ImGuiScreenshot.h"

#include "Engine/App/EngineContext.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"
#include "Support/TrimCounter.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	namespace {

		// A headless context with a null-platform window and a device, as the headless editor has.
		Result<Scope<EngineContext>> CreateUiContext(GpuFault fault)
		{
			return EngineContext::Create({
				.WorkerCount = 0,
				.Window = WindowSpecification{ .Title = "ImGuiScreenshot", .Width = 320, .Height = 200 },
				.Graphics = GraphicsSpecification{
					.Validation = true,
					.SynchronizationValidation = true,
					.MaxApiVersion = Test::GetTestOptions().VulkanApi,
					.InjectFault = fault,
				},
			});
		}

		// Two UI frames of ImGui's demo window into an offscreen target of the window's size and `format` (RGBA8 by default,
		// the capture's format; BGRA8 as a swapchain's), each paced and followed by the device's garbage collection, as an
		// application's frames are (the first collection arms C18.4's injected faults).
		void RenderDemoFrames(GraphicsDevice& device, ImGuiLayer& layer, nvrhi::Format format = nvrhi::Format::RGBA8_UNORM)
		{
			FramePacer pacer(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 320, .Height = 200, .ColorFormat = format });
			REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
			for (int frame = 0; frame < 2; ++frame)
			{
				pacer.BeginFrame();
				layer.BeginFrame(1.0 / 60.0, pacer.GetFrameSlot());
				ImGui::ShowDemoWindow();
				layer.EndFrame();
				(*commandList)->open();
				target->Clear(**commandList);
				const Status rendered = layer.Render(**commandList, *target->GetFramebuffer());
				CHECK_MESSAGE(rendered.has_value(), (rendered.has_value() ? std::string() : rendered.error().ToString()));
				(*commandList)->close();
				pacer.EndFrame(device.ExecuteCommandList(**commandList));
				device.RunGarbageCollection();
			}
			device.WaitForIdle();
		}

	}

	TEST_SUITE("ImGui")
	{
		TEST_CASE("ImGuiScreenshot: captures the last UI frame at the UI's framebuffer size"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Result<Scope<EngineContext>> created = CreateUiContext(GpuFault::None);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			EngineContext& context = **created;
			GraphicsDevice& device = *context.GetGraphicsDevice();
			Result<Scope<ImGuiLayer>> layer = ImGuiLayer::Create(*context.GetWindow(), device, *context.GetPipelineFactory(), {});
			REQUIRE_MESSAGE(layer.has_value(), layer.error().ToString());

			// Before the first UI frame there is nothing to capture.
			const Result<Image> early = CaptureImGuiScreenshot(device, **layer, {});
			REQUIRE_FALSE(early.has_value());
			CHECK(early.error().GetCode() == ErrorCode::InvalidState);

			RenderDemoFrames(device, **layer);

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

		TEST_CASE("ImGuiScreenshot: a capture that runs out of memory is retried after a trim, or fails alone with OutOfMemory"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42, 44 and 45): editor.screenshot's capture is one
			// request. Its target is a render target created after the first frames, so under oom-rendertarget its first
			// creation fails, the capture trims and runs once more, and the image equals a capture without the fault; under
			// oom-rendertarget-persistent the retry fails too, and the capture fails alone with OutOfMemory after one trim,
			// while the device stays usable.
			if (!Test::ProbeGpuForProcess())
				return;
			std::vector<std::vector<std::byte>> images;
			for (const GpuFault fault : { GpuFault::None, GpuFault::OomRenderTarget, GpuFault::OomRenderTargetPersistent })
			{
				CAPTURE(GpuFaultToString(fault));
				Result<Scope<EngineContext>> created = CreateUiContext(fault);
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				EngineContext& context = **created;
				GraphicsDevice& device = *context.GetGraphicsDevice();
				Result<Scope<ImGuiLayer>> layer = ImGuiLayer::Create(*context.GetWindow(), device, *context.GetPipelineFactory(), {});
				REQUIRE_MESSAGE(layer.has_value(), layer.error().ToString());
				RenderDemoFrames(device, **layer);
				{
					const Test::TrimCounter counter(device.GetRecreation());
					const Result<Image> image = CaptureImGuiScreenshot(device, **layer, {});
					CHECK(counter.GetTrims() == (fault == GpuFault::None ? 0u : 1u));
					if (fault == GpuFault::OomRenderTargetPersistent)
					{
						REQUIRE_FALSE(image.has_value());
						CHECK(image.error().GetCode() == ErrorCode::OutOfMemory);
					}
					else
					{
						REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
						images.push_back(image->Pixels);
					}
				}
				CHECK_FALSE(device.IsLost());
				device.WaitForIdle();
				layer->reset();
				CHECK(device.GetDiagnostics().GetErrorCount() == 0);
			}
			REQUIRE(images.size() == 2);
			CHECK(images[1] == images[0]);
		}

		TEST_CASE("ImGuiScreenshot: a capture retried after a trim reuses the target its first attempt created"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42 and 43): the editor's UI frames render into the
			// swapchain's format, so a capture's first attempt creates its RGBA8 target before ImGui's pipeline for that format,
			// which oom-pipeline fails. The retry after the trim draws into the same target, so it never needs memory for a
			// second one while the first is held: the capture creates as many textures as one without the fault, with the same
			// image.
			if (!Test::ProbeGpuForProcess())
				return;
			std::vector<std::vector<std::byte>> images;
			std::vector<uint64_t> createdTextures;
			for (const GpuFault fault : { GpuFault::None, GpuFault::OomPipeline })
			{
				CAPTURE(GpuFaultToString(fault));
				Result<Scope<EngineContext>> created = CreateUiContext(fault);
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				EngineContext& context = **created;
				GraphicsDevice& device = *context.GetGraphicsDevice();
				Result<Scope<ImGuiLayer>> layer = ImGuiLayer::Create(*context.GetWindow(), device, *context.GetPipelineFactory(), {});
				REQUIRE_MESSAGE(layer.has_value(), layer.error().ToString());
				RenderDemoFrames(device, **layer, nvrhi::Format::BGRA8_UNORM);
				{
					const Test::TrimCounter counter(device.GetRecreation());
					const uint64_t before = device.GetResourceTracker().GetCounts(GpuResourceType::Texture).Created;
					const Result<Image> image = CaptureImGuiScreenshot(device, **layer, {});
					REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
					CHECK(counter.GetTrims() == (fault == GpuFault::None ? 0u : 1u));
					createdTextures.push_back(device.GetResourceTracker().GetCounts(GpuResourceType::Texture).Created - before);
					images.push_back(image->Pixels);
				}
				CHECK_FALSE(device.IsLost());
				device.WaitForIdle();
				layer->reset();
				CHECK(device.GetDiagnostics().GetErrorCount() == 0);
			}
			REQUIRE(images.size() == 2);
			CHECK(images[1] == images[0]);
			if (GpuResourceTracker::IsEnabled())
			{
				CHECK(createdTextures[0] > 0);
				CHECK(createdTextures[1] == createdTextures[0]);
			}
		}
	}

}
