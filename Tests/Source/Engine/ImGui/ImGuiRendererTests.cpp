#include "TestsPCH.h"

#include "Engine/ImGui/ImGuiRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"

#include <imgui.h>

namespace Engine {

	namespace {

		// A Dear ImGui context of its own for one test (no platform backend: the display size and delta are set by hand),
		// destroyed after the renderer released its textures.
		class ImGuiTestContext
		{
		public:
			ImGuiTestContext()
				: m_Context(ImGui::CreateContext())
			{
				ImGuiIO& io = ImGui::GetIO();
				io.DisplaySize = ImVec2(256.0f, 128.0f);
				io.DeltaTime = 1.0f / 60.0f;
				io.IniFilename = nullptr;
			}

			~ImGuiTestContext()
			{
				ImGui::DestroyContext(m_Context);
			}

			ImGuiTestContext(const ImGuiTestContext&) = delete;
			ImGuiTestContext& operator=(const ImGuiTestContext&) = delete;
		private:
			ImGuiContext* m_Context = nullptr;
		};

	}

	// One UI frame rendered into `target`: a window with text, so the font atlas is used.
	static void RenderFrame(ImGuiRenderer& renderer, GraphicsDevice& device, FramePacer& pacer, OffscreenTarget& target,
		nvrhi::ICommandList& commandList)
	{
		pacer.BeginFrame();
		renderer.BeginFrame(pacer.GetFrameSlot());
		ImGui::NewFrame();
		ImGui::Begin("Protocol");
		ImGui::TextUnformatted("ImGui texture protocol");
		ImGui::End();
		ImGui::Render();
		commandList.open();
		target.Clear(commandList);
		const Status rendered = renderer.RenderDrawData(commandList, *target.GetFramebuffer(), *ImGui::GetDrawData());
		CHECK_MESSAGE(rendered.has_value(), (rendered.has_value() ? std::string() : rendered.error().ToString()));
		commandList.close();
		pacer.EndFrame(device.ExecuteCommandList(commandList));
		device.RunGarbageCollection();
	}

	TEST_SUITE("ImGui")
	{
		TEST_CASE("ImGuiRenderer: texture create/update/destroy protocol" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const uint64_t texturesBefore = device.GetResourceTracker().GetLiveCount(GpuResourceType::Texture);
			{
				ImGuiTestContext context;
				Result<Scope<ImGuiRenderer>> renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), { .FramesInFlight = 2 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				CHECK((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasTextures) != 0);
				CHECK((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) != 0);

				Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 256, .Height = 128 });
				REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
				FramePacer pacer(device, 2);
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());

				// WantCreate: the first frame creates the font atlas texture and hands ImGui its ImTextureID.
				RenderFrame(**renderer, device, pacer, *target, **commandList);
				REQUIRE_FALSE(ImGui::GetPlatformIO().Textures.empty());
				ImTextureData* atlas = ImGui::GetPlatformIO().Textures[0];
				CHECK(atlas->Status == ImTextureStatus_OK);
				CHECK(atlas->GetTexID() != ImTextureID_Invalid);
				CHECK((*renderer)->GetTextureCount() >= 1);

				// WantUpdates: new glyphs (a bigger font size) grow the atlas, which the renderer updates in place.
				ImGui::GetStyle().FontScaleMain = 3.0f;
				RenderFrame(**renderer, device, pacer, *target, **commandList);
				CHECK(atlas->Status == ImTextureStatus_OK);

				// User textures: a key holds its texture until the frame that used it retires.
				const ImTextureID userTexture = (*renderer)->AddTexture(*target->GetColorTexture());
				CHECK(userTexture != ImTextureID_Invalid);
				(*renderer)->RemoveTexture(userTexture);

				// WantDestroy and shutdown: every texture the renderer created is released.
				device.WaitForIdle();
				(*renderer)->DestroyTextures();
				CHECK((*renderer)->GetTextureCount() == 0);
				// The atlas keeps its pixels, so ImGui marks it for re-creation should a renderer come back.
				CHECK(atlas->GetTexID() == ImTextureID_Invalid);
			}
			device.WaitForIdle();
			device.RunGarbageCollection();
			CHECK(device.GetResourceTracker().GetLiveCount(GpuResourceType::Texture) == texturesBefore);
		}

		TEST_CASE("ImGuiRenderer: a failed texture creation never crashes and is not retried every frame"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// --gpu-inject-fault=oom-texture fails the font atlas (sampled-only); the render target keeps working (§8.14).
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomTexture });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			ImGuiTestContext context;
			Result<Scope<ImGuiRenderer>> renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), {});
			REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
			Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 256, .Height = 128 });
			REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
			FramePacer pacer(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			{
				Test::ExpectLog failure(LogLevel::Error, "cannot create texture");
				for (int frame = 0; frame < 3; ++frame)
					RenderFrame(**renderer, device, pacer, *target, **commandList);
				CHECK(failure.GetMatchCount() == 1); // logged once, not every frame
			}
			REQUIRE_FALSE(ImGui::GetPlatformIO().Textures.empty());
			CHECK(ImGui::GetPlatformIO().Textures[0]->Status == ImTextureStatus_OK);
			CHECK(ImGui::GetPlatformIO().Textures[0]->GetTexID() == ImTextureID_Invalid);
			device.WaitForIdle();
			(*renderer)->DestroyTextures();
		}

		TEST_CASE("ImGuiRenderer: the layout description matches the ImGui program" * doctest::skip(true))
		{
			const PipelineLayoutDescription description = ImGuiRenderer::GetLayoutDescription();
			CHECK(description.Program == "ImGui");
			const std::vector<std::string> entries = { "VSMain", "PSMain" };
			CHECK(description.Entries == entries);
			REQUIRE(description.BindingLayouts.size() == 1);
			CHECK(description.BindingLayouts[0].registerSpaceIsDescriptorSet);
		}
	}

}
