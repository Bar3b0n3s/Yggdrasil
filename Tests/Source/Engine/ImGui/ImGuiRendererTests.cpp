#include "TestsPCH.h"

#include "Engine/ImGui/ImGuiRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GpuRecreation.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/Readback.h"
#include "Shared/ImGuiConstants.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"

#include <imgui.h>

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

		// The frame objects an application's frame owner holds (a pacer, an offscreen target and a command list), registered
		// with the device as that owner registers its own ("ImGuiRendererTests frame"), so a device loss releases them and the
		// recovery creates them again. Registered after the renderer under test, so it is released before it and created
		// again after it.
		class RegisteredFrame
		{
		public:
			explicit RegisteredFrame(GraphicsDevice& device)
				: m_Device(&device)
			{
				const Status created = Create();
				REQUIRE_MESSAGE(created.has_value(), (created.has_value() ? std::string() : created.error().ToString()));
				GpuRecreationHooks hooks;
				hooks.Name = "ImGuiRendererTests frame";
				hooks.Release = [this]()
				{
					Release();
				};
				hooks.Recreate = [this]()
				{
					return Create();
				};
				m_Registration = device.GetRecreation().Register(std::move(hooks));
			}

			RegisteredFrame(const RegisteredFrame&) = delete;
			RegisteredFrame& operator=(const RegisteredFrame&) = delete;

			// Whether the frame's objects exist (not between a Release and the Recreate after it).
			[[nodiscard]] bool IsCreated() const { return m_Pacer.has_value() && m_Target.has_value() && m_CommandList != nullptr; }
			// The objects; only while IsCreated.
			[[nodiscard]] FramePacer& GetPacer() { return *m_Pacer; }
			[[nodiscard]] OffscreenTarget& GetTarget() { return *m_Target; }
			[[nodiscard]] nvrhi::ICommandList& GetCommandList() { return *m_CommandList; }
		private:
			[[nodiscard]] Status Create()
			{
				m_Pacer.emplace(*m_Device, 1);
				ENGINE_TRY_ASSIGN(OffscreenTarget target, OffscreenTarget::Create(*m_Device, { .Width = 256, .Height = 128 }));
				m_Target.emplace(std::move(target));
				ENGINE_TRY_ASSIGN(m_CommandList, m_Device->CreateCommandList());
				return {};
			}

			void Release()
			{
				m_Pacer.reset();
				m_Target.reset();
				m_CommandList = nullptr;
			}
		private:
			GraphicsDevice* m_Device = nullptr; // documented back-reference: the fixture's device outlives the frame
			std::optional<FramePacer> m_Pacer{};
			std::optional<OffscreenTarget> m_Target{};
			nvrhi::CommandListHandle m_CommandList{};
			GpuRecreationRegistration m_Registration{}; // declared last, so it is dropped first
		};

		// One RGBA8 pixel of a read-back image.
		struct Pixel
		{
			uint8_t R = 0;
			uint8_t G = 0;
			uint8_t B = 0;
			uint8_t A = 0;
		};

	}

	// Starts a UI frame (the pacer's slot for the renderer) and builds a window with `text`, so the font atlas is used, and
	// an ImGui::Image of `image` unless it is ImTextureID_Invalid.
	static void BuildFrame(ImGuiRenderer& renderer, FramePacer& pacer, const char* text, ImTextureID image = ImTextureID_Invalid)
	{
		pacer.BeginFrame();
		renderer.BeginFrame(pacer.GetFrameSlot());
		ImGui::NewFrame();
		ImGui::Begin("Protocol");
		ImGui::TextUnformatted(text);
		if (image != ImTextureID_Invalid)
			ImGui::Image(ImTextureRef(image), ImVec2(16.0f, 16.0f));
		ImGui::End();
		ImGui::Render();
	}

	// Records the frame built by BuildFrame into `target`, submits it and collects garbage, as an application frame does.
	static void SubmitFrame(ImGuiRenderer& renderer, GraphicsDevice& device, FramePacer& pacer, OffscreenTarget& target,
		nvrhi::ICommandList& commandList)
	{
		commandList.open();
		target.Clear(commandList);
		const Status rendered = renderer.RenderDrawData(commandList, *target.GetFramebuffer(), *ImGui::GetDrawData());
		CHECK_MESSAGE(rendered.has_value(), (rendered.has_value() ? std::string() : rendered.error().ToString()));
		commandList.close();
		pacer.EndFrame(device.ExecuteCommandList(commandList));
		device.RunGarbageCollection();
	}

	static void RenderFrame(ImGuiRenderer& renderer, GraphicsDevice& device, FramePacer& pacer, OffscreenTarget& target,
		nvrhi::ICommandList& commandList, const char* text = "ImGui texture protocol")
	{
		BuildFrame(renderer, pacer, text);
		SubmitFrame(renderer, device, pacer, target, commandList);
	}

	// A sampled-only RGBA8 texture of one colour, as an application hands ImGui::Image (ImGuiRenderer::AddTexture); the
	// upload is recorded into `commandList`.
	static nvrhi::TextureHandle CreateSolidTexture(GraphicsDevice& device, nvrhi::ICommandList& commandList, uint32_t rgba)
	{
		nvrhi::TextureDesc desc;
		desc.width = 4;
		desc.height = 4;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.isShaderResource = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "ImGuiRendererTests.Solid";
		Result<nvrhi::TextureHandle> texture = device.CreateTexture(desc);
		REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());
		const std::array<uint32_t, 16> pixels = {
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
			rgba,
		};
		commandList.writeTexture(*texture, 0, 0, pixels.data(), 4 * sizeof(uint32_t));
		return *texture;
	}

	// The texel at (x, y) of an RGBA8_UNORM image.
	static Pixel GetPixel(const Image& image, uint32_t x, uint32_t y)
	{
		REQUIRE(image.Format == nvrhi::Format::RGBA8_UNORM);
		REQUIRE(x < image.Width);
		const std::span<const std::byte> row = image.GetRow(y);
		const size_t offset = static_cast<size_t>(x) * 4;
		return Pixel{
			.R = std::to_integer<uint8_t>(row[offset]),
			.G = std::to_integer<uint8_t>(row[offset + 1]),
			.B = std::to_integer<uint8_t>(row[offset + 2]),
			.A = std::to_integer<uint8_t>(row[offset + 3]),
		};
	}

	static std::string DescribePixel(const Pixel& pixel)
	{
		return std::format("({}, {}, {}, {})", pixel.R, pixel.G, pixel.B, pixel.A);
	}

	// Whether every channel of `pixel` is within `tolerance` of the expected one.
	static bool IsPixelNear(const Pixel& pixel, const Pixel& expected, int tolerance)
	{
		const auto isClose = [tolerance](uint8_t value, uint8_t reference)
		{
			return std::abs(int{ value } - int{ reference }) <= tolerance;
		};
		return isClose(pixel.R, expected.R) && isClose(pixel.G, expected.G) && isClose(pixel.B, expected.B) && isClose(pixel.A, expected.A);
	}

	// The ImGui textures that have an ImTextureID (created by the renderer).
	static size_t CountTexturesWithID()
	{
		return static_cast<size_t>(std::ranges::count_if(ImGui::GetPlatformIO().Textures,
			[](const ImTextureData* texture)
		{
			return texture->GetTexID() != ImTextureID_Invalid;
		}));
	}

	TEST_SUITE("ImGui")
	{
		TEST_CASE("ImGuiRenderer: texture create/update/destroy protocol" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const uint64_t texturesBefore = device.GetResourceTracker().GetLiveCount(GpuResourceType::Texture);
			{
				ImGuiTestContext context;
				Result<Scope<ImGuiRenderer>> created = ImGuiRenderer::Create(device, gpu.GetPipelines(), { .FramesInFlight = 2 });
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				ImGuiRenderer& renderer = **created;
				CHECK((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasTextures) != 0);
				CHECK((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) != 0);
				CHECK(ImGui::GetIO().BackendRendererName != nullptr);

				Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 256, .Height = 128 });
				REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
				FramePacer pacer(device, 2);
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());

				// WantCreate: the first frame asks for the font atlas, and the renderer hands ImGui its ImTextureID.
				BuildFrame(renderer, pacer, "ImGui texture protocol");
				ImTextureData* atlas = ImGui::GetIO().Fonts->TexData;
				REQUIRE(atlas != nullptr);
				CHECK(atlas->Status == ImTextureStatus_WantCreate);
				SubmitFrame(renderer, device, pacer, *target, **commandList);
				CHECK(atlas->Status == ImTextureStatus_OK);
				CHECK(atlas->GetTexID() != ImTextureID_Invalid);
				CHECK(renderer.GetTextureCount() == 1);

				// WantUpdates: new glyphs (a bigger font size) are packed into the same atlas, which the renderer updates.
				ImGui::GetStyle().FontScaleMain = 2.0f;
				BuildFrame(renderer, pacer, "ImGui texture protocol");
				REQUIRE(ImGui::GetIO().Fonts->TexData == atlas);
				CHECK(atlas->Status == ImTextureStatus_WantUpdates);
				SubmitFrame(renderer, device, pacer, *target, **commandList);
				CHECK(atlas->Status == ImTextureStatus_OK);
				CHECK(renderer.GetTextureCount() == 1);

				// WantDestroy: far more glyphs than the atlas holds make ImGui grow it into a new texture (WantCreate) and ask
				// for the old one's destruction the frame after; the old handle stays retired until its frame slot comes round.
				ImGui::GetStyle().FontScaleMain = 8.0f;
				const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789";
				RenderFrame(renderer, device, pacer, *target, **commandList, alphabet);
				ImTextureData* grown = ImGui::GetIO().Fonts->TexData;
				REQUIRE(grown != atlas);
				CHECK(grown->Status == ImTextureStatus_OK);
				CHECK(grown->GetTexID() != ImTextureID_Invalid);
				CHECK(atlas->GetTexID() != ImTextureID_Invalid); // still in use until ImGui asks for its destruction

				BuildFrame(renderer, pacer, alphabet);
				CHECK(atlas->Status == ImTextureStatus_WantDestroy);
				SubmitFrame(renderer, device, pacer, *target, **commandList);
				CHECK(atlas->Status == ImTextureStatus_Destroyed);
				CHECK(atlas->GetTexID() == ImTextureID_Invalid);
				atlas = nullptr; // ImGui frees it at its next NewFrame
				const size_t liveTextures = CountTexturesWithID();
				CHECK(liveTextures == 1);
				CHECK(renderer.GetTextureCount() > liveTextures); // the destroyed textures await retirement

				RenderFrame(renderer, device, pacer, *target, **commandList, alphabet); // the other frame slot
				CHECK(renderer.GetTextureCount() > liveTextures);
				RenderFrame(renderer, device, pacer, *target, **commandList, alphabet); // the slot of the destruction again
				CHECK(renderer.GetTextureCount() == liveTextures);

				// User textures: a key from AddTexture draws with ImGui::Image and holds its texture until the frame slot it
				// was removed in comes round again.
				(*commandList)->open();
				nvrhi::TextureHandle solid = CreateSolidTexture(device, **commandList, 0xFF00'FF00u);
				(*commandList)->close();
				device.ExecuteCommandList(**commandList);
				const ImTextureID userTexture = renderer.AddTexture(*solid);
				CHECK(userTexture != ImTextureID_Invalid);
				CHECK(renderer.GetTextureCount() == liveTextures + 1);
				BuildFrame(renderer, pacer, alphabet, userTexture);
				SubmitFrame(renderer, device, pacer, *target, **commandList);
				renderer.RemoveTexture(userTexture);
				solid = nullptr;
				CHECK(renderer.GetTextureCount() == liveTextures + 1);
				RenderFrame(renderer, device, pacer, *target, **commandList, alphabet);
				CHECK(renderer.GetTextureCount() == liveTextures + 1);
				RenderFrame(renderer, device, pacer, *target, **commandList, alphabet);
				CHECK(renderer.GetTextureCount() == liveTextures);

				// Shutdown: every texture the renderer created is released.
				const ImTextureID keptTexture = renderer.AddTexture(*target->GetColorTexture());
				CHECK(keptTexture != ImTextureID_Invalid);
				device.WaitForIdle();
				renderer.DestroyTextures();
				CHECK(renderer.GetTextureCount() == 0);
				// The atlas keeps its pixels, so ImGui marks it for re-creation should a renderer come back.
				CHECK(grown->GetTexID() == ImTextureID_Invalid);
				CHECK(grown->Status == ImTextureStatus_WantCreate);
			}
			device.WaitForIdle();
			device.RunGarbageCollection();
			CHECK(device.GetResourceTracker().GetLiveCount(GpuResourceType::Texture) == texturesBefore);
		}

		TEST_CASE("ImGuiRenderer: a failed texture creation never crashes and is not retried every frame"
			* doctest::test_suite(Test::GpuSuite))
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
			CHECK((*renderer)->GetTextureCount() == 0);
			device.WaitForIdle();
			(*renderer)->DestroyTextures();
			// The failed texture is offered again to a renderer created later.
			CHECK(ImGui::GetPlatformIO().Textures[0]->Status == ImTextureStatus_WantCreate);
		}

		TEST_CASE("ImGuiRenderer: draws with the display projection, scissor rectangles and straight-alpha blending"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			ImGuiTestContext context;
			Result<Scope<ImGuiRenderer>> created = ImGuiRenderer::Create(device, gpu.GetPipelines(), { .FramesInFlight = 1 });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			ImGuiRenderer& renderer = **created;
			Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 256, .Height = 128 });
			REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
			// A second target format needs a pipeline of its own (§8.11) and draws the same pixels.
			Result<OffscreenTarget> bgraTarget =
				OffscreenTarget::Create(device, { .Width = 256, .Height = 128, .ColorFormat = nvrhi::Format::BGRA8_UNORM });
			REQUIRE_MESSAGE(bgraTarget.has_value(), bgraTarget.error().ToString());
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());

			(*commandList)->open();
			nvrhi::TextureHandle blue = CreateSolidTexture(device, **commandList, 0xFFFF'0000u); // RGBA (0, 0, 255, 255)
			const ImTextureID blueTexture = renderer.AddTexture(*blue);

			renderer.BeginFrame(0);
			ImGui::NewFrame();
			ImDrawList* drawList = ImGui::GetForegroundDrawList();
			const ImU32 red = ImGui::GetColorU32(ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
			const ImU32 green = ImGui::GetColorU32(ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
			const ImU32 halfWhite = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 128.0f / 255.0f));
			const ImU32 yellow = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
			const ImU32 magenta = ImGui::GetColorU32(ImVec4(1.0f, 0.0f, 1.0f, 1.0f));
			// Near the top: ImGui's y axis points down, so this lands in the top rows of the target.
			drawList->AddRectFilled(ImVec2(8.0f, 8.0f), ImVec2(40.0f, 24.0f), red);
			// Clipped at x = 80 by its command's scissor rectangle.
			drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(80.0f, 128.0f));
			drawList->AddRectFilled(ImVec2(48.0f, 8.0f), ImVec2(112.0f, 24.0f), green);
			drawList->PopClipRect();
			// Straight alpha over the opaque black clear: colour 128, alpha 255.
			drawList->AddRectFilled(ImVec2(120.0f, 8.0f), ImVec2(152.0f, 24.0f), halfWhite);
			// A user texture (AddTexture) sampled through its own binding set.
			drawList->AddImage(ImTextureRef(blueTexture), ImVec2(160.0f, 8.0f), ImVec2(192.0f, 24.0f));
			// More than 65,536 vertices: 16-bit indices continue through ImDrawCmd::VtxOffset (RendererHasVtxOffset).
			for (int i = 0; i < 20000; ++i)
				drawList->AddRectFilled(ImVec2(8.0f, 40.0f), ImVec2(40.0f, 56.0f), yellow);
			drawList->AddRectFilled(ImVec2(48.0f, 40.0f), ImVec2(80.0f, 56.0f), magenta);
			ImGui::Render();
			const ImDrawData& drawData = *ImGui::GetDrawData();
			REQUIRE(drawData.TotalVtxCount > 65536);
			const bool usesVertexOffset = std::ranges::any_of(drawData.CmdLists,
				[](const ImDrawList* list)
			{
				return std::ranges::any_of(list->CmdBuffer, [](const ImDrawCmd& command)
				{
					return command.VtxOffset > 0;
				});
			});
			CHECK(usesVertexOffset);

			target->Clear(**commandList);
			const Status rendered = renderer.RenderDrawData(**commandList, *target->GetFramebuffer(), *ImGui::GetDrawData());
			REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
			bgraTarget->Clear(**commandList);
			const Status renderedAgain = renderer.RenderDrawData(**commandList, *bgraTarget->GetFramebuffer(), *ImGui::GetDrawData());
			REQUIRE_MESSAGE(renderedAgain.has_value(), renderedAgain.error().ToString());
			(*commandList)->close();
			device.ExecuteCommandList(**commandList);

			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(*target->GetColorTexture());
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			const Pixel black{ .R = 0, .G = 0, .B = 0, .A = 255 };
			const auto checkPixel = [&image](uint32_t x, uint32_t y, const Pixel& expected, int tolerance)
			{
				const Pixel pixel = GetPixel(*image, x, y);
				INFO("pixel (", x, ", ", y, ") is ", DescribePixel(pixel), ", expected ", DescribePixel(expected));
				CHECK(IsPixelNear(pixel, expected, tolerance));
			};
			checkPixel(16, 16, { .R = 255, .G = 0, .B = 0, .A = 255 }, 0);
			checkPixel(16, 112, black, 0); // the same rectangle mirrored: a wrong y orientation would put red here
			checkPixel(60, 16, { .R = 0, .G = 255, .B = 0, .A = 255 }, 0);
			checkPixel(100, 16, black, 0); // scissored away
			checkPixel(136, 16, { .R = 128, .G = 128, .B = 128, .A = 255 }, 1);
			checkPixel(176, 16, { .R = 0, .G = 0, .B = 255, .A = 255 }, 0);
			checkPixel(24, 48, { .R = 255, .G = 255, .B = 0, .A = 255 }, 0);
			checkPixel(64, 48, { .R = 255, .G = 0, .B = 255, .A = 255 }, 0); // drawn through a command with VtxOffset
			checkPixel(200, 100, black, 0);

			const Result<Image> bgraImage = readback.ReadTexture(*bgraTarget->GetColorTexture());
			REQUIRE_MESSAGE(bgraImage.has_value(), bgraImage.error().ToString());
			const Result<Image> converted = ConvertToRgba8(*bgraImage);
			REQUIRE_MESSAGE(converted.has_value(), converted.error().ToString());
			CHECK(converted->Pixels == image->Pixels);

			renderer.RemoveTexture(blueTexture);
			device.WaitForIdle();
			renderer.DestroyTextures();
		}

		TEST_CASE("ImGuiRenderer: the layout description matches the ImGui program")
		{
			const PipelineLayoutDescription description = ImGuiRenderer::GetLayoutDescription();
			CHECK(description.Name == "ImGui");
			CHECK(description.Program == "ImGui");
			const std::vector<std::string> entries = { "VSMain", "PSMain" };
			CHECK(description.Entries == entries);
			CHECK(description.Permutation.empty());
			CHECK(description.StorageImages.empty());
			REQUIRE(description.BindingLayouts.size() == 1);
			const nvrhi::BindingLayoutDesc& layout = description.BindingLayouts[0];
			CHECK(layout.registerSpace == 0);
			CHECK(layout.registerSpaceIsDescriptorSet);
			// t0 Texture, s0 Sampler and the ImGuiConstants push constants (Passes/ImGui.slang).
			REQUIRE(layout.bindings.size() == 3);
			CHECK(layout.bindings[0].type == nvrhi::ResourceType::Texture_SRV);
			CHECK(layout.bindings[0].slot == 0);
			CHECK(layout.bindings[1].type == nvrhi::ResourceType::Sampler);
			CHECK(layout.bindings[1].slot == 0);
			CHECK(layout.bindings[2].type == nvrhi::ResourceType::PushConstants);
			CHECK(layout.bindings[2].size == sizeof(ImGuiConstants));
		}

		TEST_CASE("ImGuiRenderer: a device loss uploads ImGui's textures again and draws the same image"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, Docs/Decisions/0029-m18-contract.md decisions 32 and 33): the renderer registers itself
			// ("ImGuiRenderer"); its Release drops its pipelines, buffers, sampler, binding sets and texture handles and marks
			// ImGui's textures for creation, so the font atlas is uploaded again from ImGui's own pixels and the same frame draws
			// the same image. The frame's own objects stand for an application's frame owner, which registers them.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			ImGuiTestContext context;
			Result<Scope<ImGuiRenderer>> created = ImGuiRenderer::Create(device, gpu.GetPipelines(), { .FramesInFlight = 1 });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			ImGuiRenderer& renderer = **created;
			RegisteredFrame frame(device);
			// Dear ImGui fits a new window to its contents over its first two frames (hiding it on the first), so the frames the
			// loss test compares come after two that settled the window.
			for (int settling = 0; settling < 2; ++settling)
				RenderFrame(renderer, device, frame.GetPacer(), frame.GetTarget(), frame.GetCommandList(), "ImGui after a device loss");

			Test::DeviceLossTest test;
			test.Owners = { "ImGuiRenderer" };
			test.Use = [&device, &renderer, &frame]() -> Result<std::vector<std::byte>>
			{
				if (!frame.IsCreated())
					return MakeError(ErrorCode::InvalidState, "the frame's objects were released");
				BuildFrame(renderer, frame.GetPacer(), "ImGui after a device loss");
				nvrhi::ICommandList& commandList = frame.GetCommandList();
				commandList.open();
				frame.GetTarget().Clear(commandList);
				const Status rendered = renderer.RenderDrawData(commandList, *frame.GetTarget().GetFramebuffer(), *ImGui::GetDrawData());
				commandList.close();
				frame.GetPacer().EndFrame(device.ExecuteCommandList(commandList));
				ENGINE_TRY(rendered);
				Readback readback(device);
				ENGINE_TRY_ASSIGN(const Image image, readback.ReadTexture(*frame.GetTarget().GetColorTexture()));
				return image.Pixels;
			};
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			device.WaitForIdle();
			renderer.DestroyTextures();
		}

		TEST_CASE("ImGuiRenderer: a texture it cannot create on a lost device is created after the recovery, without an error"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, decision 28): "a Gpu error on a lost device is the loss, never out-of-memory". The font atlas that the
			// first frame asks for cannot be created on the lost device: it stays WantCreate without an Error line (which would
			// fail the test: Support/ExpectLog.h) instead of being logged and given up as a failed creation, and the first frame
			// after the recovery creates it.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			ImGuiTestContext context;
			Result<Scope<ImGuiRenderer>> created = ImGuiRenderer::Create(device, gpu.GetPipelines(), { .FramesInFlight = 1 });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			ImGuiRenderer& renderer = **created;
			RegisteredFrame frame(device);

			device.GetDiagnostics().SetDeviceLost(true);
			device.ReportDeviceLost("ImGuiRendererTests");
			BuildFrame(renderer, frame.GetPacer(), "ImGui on a lost device");
			ImTextureData* atlas = ImGui::GetIO().Fonts->TexData;
			REQUIRE(atlas != nullptr);
			CHECK(atlas->Status == ImTextureStatus_WantCreate);
			nvrhi::ICommandList& commandList = frame.GetCommandList();
			commandList.open();
			frame.GetTarget().Clear(commandList);
			// The frame's draws fail too (the pipeline for the target cannot be created), which a lost frame may.
			static_cast<void>(renderer.RenderDrawData(commandList, *frame.GetTarget().GetFramebuffer(), *ImGui::GetDrawData()));
			commandList.close();
			frame.GetPacer().EndFrame(device.ExecuteCommandList(commandList));
			CHECK(atlas->Status == ImTextureStatus_WantCreate);
			CHECK(atlas->GetTexID() == ImTextureID_Invalid);
			CHECK(renderer.GetTextureCount() == 0);

			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			REQUIRE(frame.IsCreated());
			RenderFrame(renderer, device, frame.GetPacer(), frame.GetTarget(), frame.GetCommandList(), "ImGui on a lost device");
			CHECK(atlas->Status == ImTextureStatus_OK);
			CHECK(atlas->GetTexID() != ImTextureID_Invalid);
			CHECK(renderer.GetTextureCount() == 1);
			device.WaitForIdle();
			renderer.DestroyTextures();
		}
	}

}
