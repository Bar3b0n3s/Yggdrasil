#include "TestsPCH.h"

#include "Engine/Renderer/TextRenderer.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/GraphicsSpecification.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/ViewConstants.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

// The text pass (Architecture §8.10, §8.3 pass 14; Docs/Decisions/0013-m8-decisions.md decision 11) with the Default font
// (Inter, M6's FontImporter through the engine resources), and M22's glyph runs with a font of their own.

namespace Engine {

	namespace {

		constexpr uint32_t Width = 128;
		constexpr uint32_t Height = 64;

		// An overlay framebuffer (LDR + SceneDepth) of Width x Height and a ViewConstants buffer of a camera at the origin
		// looking down -Z; Render clears SceneDepth to `depth` (0, the far plane, by default), draws `inputs` (Framebuffer and
		// ViewConstants filled in) and reads the LDR target back.
		class TextSetup
		{
		public:
			explicit TextSetup(Test::HeadlessGpuFixture& gpu)
				: m_Device(gpu.GetDevice())
			{
				Result<Scope<TextRenderer>> renderer = TextRenderer::Create(m_Device, gpu.GetPipelines());
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				m_Renderer = std::move(*renderer);
				nvrhi::TextureDesc colorDesc;
				colorDesc.width = Width;
				colorDesc.height = Height;
				colorDesc.format = LdrColorFormat;
				colorDesc.isRenderTarget = true;
				colorDesc.isShaderResource = true;
				colorDesc.initialState = nvrhi::ResourceStates::RenderTarget;
				colorDesc.keepInitialState = true;
				colorDesc.debugName = "TextRendererTests.LdrColor";
				nvrhi::TextureDesc depthDesc = colorDesc;
				depthDesc.format = SceneDepthFormat;
				depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
				depthDesc.debugName = "TextRendererTests.SceneDepth";
				Result<nvrhi::TextureHandle> color = m_Device.CreateTexture(colorDesc);
				Result<nvrhi::TextureHandle> depth = m_Device.CreateTexture(depthDesc);
				REQUIRE(color.has_value());
				REQUIRE(depth.has_value());
				m_Color = *color;
				m_Depth = *depth;
				Result<nvrhi::FramebufferHandle> framebuffer =
					m_Device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_Color).setDepthAttachment(m_Depth));
				REQUIRE(framebuffer.has_value());
				m_Framebuffer = *framebuffer;
				nvrhi::BufferDesc bufferDesc;
				bufferDesc.byteSize = sizeof(ViewConstants);
				bufferDesc.isConstantBuffer = true;
				bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				bufferDesc.keepInitialState = true;
				Result<nvrhi::BufferHandle> buffer = m_Device.CreateBuffer(bufferDesc);
				REQUIRE(buffer.has_value());
				m_ViewBuffer = *buffer;
				m_View.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 100.0f, Width, Height);
				m_View.View = glm::mat4(1.0f);
				m_View.ViewProjection = m_View.Projection;
				m_View.InverseProjection = glm::inverse(m_View.Projection);
				m_View.InverseViewProjection = m_View.InverseProjection;
				m_View.ViewportSize = glm::vec2(static_cast<float>(Width), static_cast<float>(Height));
				m_View.InverseViewportSize = 1.0f / m_View.ViewportSize;
			}

			[[nodiscard]] TextRenderer& GetRenderer() { return *m_Renderer; }

			Image Render(TextRenderInputs inputs, uint32_t& drawn, float depth = 0.0f)
			{
				inputs.Framebuffer = m_Framebuffer;
				inputs.ViewConstants = m_ViewBuffer;
				return Record(drawn, depth, [this, &inputs](nvrhi::ICommandList& commandList, PassBindingCache& bindings)
				{
					return m_Renderer->Record(commandList, bindings, inputs);
				});
			}

			// M22: draws glyph runs (TextRenderer::RecordGlyphRuns) as Render draws texts, adding to `counters`.
			Image RenderGlyphRuns(TextGlyphRenderInputs inputs, uint32_t& drawn, RenderPassCounters& counters)
			{
				inputs.Framebuffer = m_Framebuffer;
				inputs.ViewConstants = m_ViewBuffer;
				return Record(drawn, 0.0f, [this, &inputs, &counters](nvrhi::ICommandList& commandList, PassBindingCache& bindings)
				{
					return m_Renderer->RecordGlyphRuns(commandList, bindings, inputs, counters);
				});
			}
		private:
			// Clears the targets, calls `record` with the command list and a binding cache, executes and reads the LDR target.
			template<typename RecordFunction>
			Image Record(uint32_t& drawn, float depth, const RecordFunction& record)
			{
				Result<nvrhi::CommandListHandle> commandList = m_Device.CreateCommandList();
				REQUIRE(commandList.has_value());
				(*commandList)->open();
				(*commandList)->writeBuffer(m_ViewBuffer, &m_View, sizeof(m_View));
				(*commandList)->clearTextureFloat(m_Color, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
				(*commandList)->clearDepthStencilTexture(m_Depth, nvrhi::AllSubresources, true, depth, false, 0);
				PassBindingCache bindings;
				const Result<uint32_t> recorded = record(**commandList, bindings);
				(*commandList)->close();
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				drawn = *recorded;
				m_Device.ExecuteCommandList(**commandList);
				Readback readback(m_Device);
				Result<Image> image = readback.ReadTexture(*m_Color);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				return std::move(*image);
			}
		private:
			GraphicsDevice& m_Device;
			Scope<TextRenderer> m_Renderer;
			nvrhi::TextureHandle m_Color;
			nvrhi::TextureHandle m_Depth;
			nvrhi::FramebufferHandle m_Framebuffer;
			nvrhi::BufferHandle m_ViewBuffer;
			ViewConstants m_View{};
		};

		// M22: a font whose 4 x 4 atlas is inside every glyph everywhere (each texel 255, beyond the outline's 128), so a glyph
		// quad covers its pixels fully and draws exactly its colour; its glyph 'A' spans the whole atlas.
		Ref<FontData> MakeSolidFont()
		{
			auto font = CreateRef<FontData>();
			font->Ascent = 0.8f;
			font->Descent = -0.2f;
			font->AtlasWidth = 4;
			font->AtlasHeight = 4;
			font->AtlasPixels.assign(16, std::byte{ 255 });
			font->Glyphs = { FontGlyph{ .Codepoint = 'A', .Advance = 1.0f, .PlaneMax = glm::vec2(1.0f), .AtlasMin = glm::vec2(0.0f), .AtlasMax = glm::vec2(1.0f) } };
			REQUIRE(ValidateFontData(*font).has_value());
			return font;
		}

		// A glyph of the solid font over [min, max) in pixels.
		TextPixelGlyph MakeSolidGlyph(glm::vec2 min, glm::vec2 max)
		{
			return TextPixelGlyph{ .Min = min, .Max = max, .AtlasMin = glm::vec2(0.0f), .AtlasMax = glm::vec2(1.0f) };
		}

		// The red, green and blue bytes of pixel (x, y).
		glm::ivec3 GetPixel(const Image& image, uint32_t x, uint32_t y)
		{
			const std::span<const std::byte> texel = image.GetRow(y).subspan(static_cast<size_t>(x) * 4, 4);
			return glm::ivec3(std::to_integer<int>(texel[0]), std::to_integer<int>(texel[1]), std::to_integer<int>(texel[2]));
		}

		// The number of pixels whose red channel exceeds 128 inside the rectangle [x0, x1) x [y0, y1).
		uint32_t CountLit(const Image& image, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
		{
			uint32_t count = 0;
			for (uint32_t y = y0; y < y1; ++y)
			{
				for (uint32_t x = x0; x < x1; ++x)
					count += std::to_integer<int>(image.GetRow(y)[static_cast<size_t>(x) * 4]) > 128 ? 1U : 0U;
			}
			return count;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("TextRenderer: screen text appears at its anchor in its colour" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets({ .EngineResources = true });
			assets.OpenProject(false);
			{
				TextSetup setup(gpu);
				CHECK(setup.GetRenderer().GetPipelineCount() == TextRenderer::PipelineCount);
				// A white "IIII" at the top-left corner, 600 px per em at the 1080p reference: 600 * 64 / 1080, about 36 px per em
				// on this 64 px tall target.
				const std::vector<TextItem> texts = { TextItem{ .Text = "IIII", .Size = 600.0f, .Color = glm::vec4(1.0f), .Anchor = glm::vec2(0.0f, 0.0f), .Pivot = glm::vec2(0.0f, 0.0f) } };
				uint32_t drawn = 0;
				const Image image = setup.Render({ .Texts = texts, .DebugDraw = nullptr, .Assets = &assets.GetManager() }, drawn);
				CHECK(drawn == 1);
				CHECK(CountLit(image, 0, 0, Width / 2, Height / 2) > 0);
				CHECK(CountLit(image, Width / 2, Height / 2, Width, Height) == 0);
				CHECK(setup.GetRenderer().GetFontAtlasCount() == 1);
				// In its colour: a pure blue text lights the blue channel only.
				const std::vector<TextItem> blue = { TextItem{ .Text = "IIII", .Size = 600.0f, .Color = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), .Anchor = glm::vec2(0.0f, 0.0f), .Pivot = glm::vec2(0.0f, 0.0f) } };
				const Image blueImage = setup.Render({ .Texts = blue, .DebugDraw = nullptr, .Assets = &assets.GetManager() }, drawn);
				CHECK(drawn == 1);
				CHECK(CountLit(blueImage, 0, 0, Width, Height) == 0);
				uint32_t blueTexels = 0;
				for (uint32_t y = 0; y < Height; ++y)
				{
					for (uint32_t x = 0; x < Width; ++x)
					{
						const std::span<const std::byte> texel = blueImage.GetRow(y).subspan(static_cast<size_t>(x) * 4, 4);
						blueTexels += std::to_integer<int>(texel[2]) > 128 && std::to_integer<int>(texel[1]) == 0 ? 1U : 0U;
					}
				}
				CHECK(blueTexels > 0);
				// The renderer reuses the atlas it mirrored.
				CHECK(setup.GetRenderer().GetFontAtlasCount() == 1);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TextRenderer: world text is hidden behind nearer geometry and billboard text faces the camera"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets({ .EngineResources = true });
			assets.OpenProject(false);
			{
				TextSetup setup(gpu);
				// A world text 5 m in front of the camera, 1 m per em (Size 100 at WorldTextPixelsPerMetre 100), facing +Z.
				TextItem text{ .Text = "HH", .Size = 100.0f, .Color = glm::vec4(1.0f), .Space = RenderTextSpace::World };
				text.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f));
				const std::vector<TextItem> texts = { text };
				uint32_t drawn = 0;
				const Image visible = setup.Render({ .Texts = texts, .DebugDraw = nullptr, .Assets = &assets.GetManager() }, drawn);
				CHECK(drawn == 1);
				CHECK(CountLit(visible, 0, 0, Width, Height) > 0);
				// Behind a nearer wall (SceneDepth 0.5 everywhere: z = -0.2 for the near plane 0.1) it is hidden.
				const Image hidden = setup.Render({ .Texts = texts, .DebugDraw = nullptr, .Assets = &assets.GetManager() }, drawn, 0.5f);
				CHECK(drawn == 1);
				CHECK(CountLit(hidden, 0, 0, Width, Height) == 0);
				// Turned 180 degrees about Y it shows its back, still drawn (text is double-sided); a billboard turned away
				// still faces the camera.
				text.World = glm::rotate(text.World, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
				const std::vector<TextItem> back = { text };
				const Image backImage = setup.Render({ .Texts = back, .DebugDraw = nullptr, .Assets = &assets.GetManager() }, drawn);
				CHECK(drawn == 1);
				CHECK(CountLit(backImage, 0, 0, Width, Height) > 0);
				text.Billboard = true;
				const std::vector<TextItem> billboard = { text };
				const Image facing = setup.Render({ .Texts = billboard, .DebugDraw = nullptr, .Assets = &assets.GetManager() }, drawn);
				CHECK(CountLit(facing, 0, 0, Width, Height) == CountLit(visible, 0, 0, Width, Height));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TextRenderer: debug labels draw at their projected position, and a missing font falls back" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets({ .EngineResources = true });
			assets.OpenProject(false);
			{
				TextSetup setup(gpu);
				DebugDrawList list;
				list.AddText(glm::vec3(0.0f, 0.0f, -5.0f), "X", 600.0f, glm::vec4(1.0f), 0.0f, DebugDepthMode::OnTop);
				list.AddText(glm::vec3(0.0f, 0.0f, 5.0f), "Behind", 600.0f, glm::vec4(1.0f)); // behind the camera: not drawn
				const std::vector<TextItem> texts = { TextItem{ .Text = "A", .Font = AssetHandle(0x7777), .Size = 300.0f } };
				uint32_t drawn = 0;
				Test::ExpectLog missing(LogLevel::Error, "ASSET_MISSING");
				// With the camera's view matrix (the fixture's camera is the identity at the origin, looking down -Z), the label
				// behind the camera is neither recorded nor counted.
				const Image image =
					setup.Render({ .Texts = texts, .DebugDraw = &list, .Assets = &assets.GetManager(), .CameraView = glm::mat4(1.0f) }, drawn);
				CHECK(drawn == 2);
				CHECK(CountLit(image, Width / 2 - 16, Height / 2 - 16, Width / 2 + 16, Height / 2 + 16) > 0);
				// Without it both labels are recorded and counted, and the vertex shader drops the one behind the camera: the
				// image is the same.
				const Image withoutView = setup.Render({ .Texts = texts, .DebugDraw = &list, .Assets = &assets.GetManager() }, drawn);
				CHECK(drawn == 3);
				CHECK(withoutView.Pixels == image.Pixels);
				// The missing font and the labels share the Default font's atlas.
				CHECK(setup.GetRenderer().GetFontAtlasCount() == 1);
				// Atlases of unused fonts are released on request.
				setup.GetRenderer().CollectStale(assets.GetManager(), true);
				setup.GetRenderer().CollectStale(assets.GetManager(), true);
				CHECK(setup.GetRenderer().GetFontAtlasCount() == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TextRenderer: without a camera only screen texts are drawn" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets({ .EngineResources = true });
			assets.OpenProject(false);
			{
				TextSetup setup(gpu);
				TextItem world{ .Text = "HH", .Size = 100.0f, .Space = RenderTextSpace::World };
				world.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f));
				const std::vector<TextItem> texts = {
					world,
					TextItem{ .Text = "IIII", .Size = 600.0f, .Anchor = glm::vec2(0.0f, 0.0f), .Pivot = glm::vec2(0.0f, 0.0f) },
					TextItem{ .Text = "", .Size = 600.0f },    // nothing to draw
					TextItem{ .Text = "Zero", .Size = 0.0f },  // no size
					TextItem{ .Text = "   ", .Size = 600.0f }, // spaces only: no glyph quad
				};
				DebugDrawList list;
				list.AddText(glm::vec3(0.0f, 0.0f, -5.0f), "X", 600.0f, glm::vec4(1.0f), 0.0f, DebugDepthMode::OnTop);
				uint32_t drawn = 0;
				const Image image = setup.Render({ .Texts = texts, .DebugDraw = &list, .Assets = &assets.GetManager(), .HasCamera = false }, drawn);
				CHECK(drawn == 1);
				CHECK(CountLit(image, 0, 0, Width / 2, Height / 2) > 0);
				CHECK(CountLit(image, Width / 2, Height / 4, Width, Height) == 0);
				// With the camera the world text and the label are drawn too.
				setup.Render({ .Texts = texts, .DebugDraw = &list, .Assets = &assets.GetManager(), .HasCamera = true }, drawn);
				CHECK(drawn == 3);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TextRenderer: a text placed beyond float's range is skipped and not counted" * doctest::test_suite(Test::GpuSuite))
		{
			// Every value is finite, but a 3e38 Size places the glyphs of a 256-glyph line beyond float's range, for a screen
			// text, a world text and a label alike: none of them is drawn or counted (no non-finite vertex reaches the GPU), and
			// a sound text beside them is.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets({ .EngineResources = true });
			assets.OpenProject(false);
			{
				TextSetup setup(gpu);
				constexpr float Huge = 3e38f;
				const std::string line(256, 'H');
				TextItem world{ .Text = line, .Size = Huge, .Color = glm::vec4(1.0f), .Space = RenderTextSpace::World, .Pivot = glm::vec2(0.0f) };
				world.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f));
				const std::vector<TextItem> texts = {
					TextItem{ .Text = line, .Size = Huge, .Color = glm::vec4(1.0f) },
					world,
					TextItem{ .Text = "IIII", .Size = 600.0f, .Color = glm::vec4(1.0f), .Anchor = glm::vec2(0.0f, 0.0f), .Pivot = glm::vec2(0.0f, 0.0f) },
				};
				DebugDrawList list;
				list.AddText(glm::vec3(0.0f, 0.0f, -5.0f), line, Huge, glm::vec4(1.0f), 0.0f, DebugDepthMode::OnTop);
				uint32_t drawn = 0;
				const Image image =
					setup.Render({ .Texts = texts, .DebugDraw = &list, .Assets = &assets.GetManager(), .CameraView = glm::mat4(1.0f) }, drawn);
				CHECK(drawn == 1);
				// Only the sound text's corner is lit.
				CHECK(CountLit(image, 0, 0, Width / 2, Height / 2) > 0);
				CHECK(CountLit(image, Width / 2, Height / 2, Width, Height) == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TextRenderer: texts beyond the vertex budget are left out whole and warned about once" * doctest::test_suite(Test::GpuSuite))
		{
			// 2^20 vertices per record, six per glyph quad (TextRenderer.h): a text of more glyphs than that is left out and not
			// counted, a later, smaller one still fits, and the warning is logged once per renderer.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets({ .EngineResources = true });
			assets.OpenProject(false);
			{
				TextSetup setup(gpu);
				constexpr size_t BudgetGlyphs = (size_t{ 1 } << 20) / 6;
				const std::vector<TextItem> texts = {
					TextItem{ .Text = std::string(BudgetGlyphs + 1, 'I'), .Size = 600.0f, .Color = glm::vec4(1.0f) },
					TextItem{ .Text = "IIII", .Size = 600.0f, .Color = glm::vec4(1.0f), .Anchor = glm::vec2(0.0f, 0.0f), .Pivot = glm::vec2(0.0f, 0.0f) },
				};
				Test::ExpectLog warning(LogLevel::Warn, "budget");
				uint32_t drawn = 0;
				const Image first = setup.Render({ .Texts = texts, .DebugDraw = nullptr, .Assets = &assets.GetManager() }, drawn);
				CHECK(drawn == 1);
				CHECK(CountLit(first, 0, 0, Width / 2, Height / 2) > 0);
				setup.Render({ .Texts = texts, .DebugDraw = nullptr, .Assets = &assets.GetManager() }, drawn);
				CHECK(drawn == 1);
				CHECK(warning.GetMatchCount() == 1);
				// A text of exactly the budget fits.
				const std::vector<TextItem> fitting = { TextItem{ .Text = std::string(BudgetGlyphs, 'I'), .Size = 600.0f, .Color = glm::vec4(1.0f) } };
				setup.Render({ .Texts = fitting, .DebugDraw = nullptr, .Assets = &assets.GetManager() }, drawn);
				CHECK(drawn == 1);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TextRenderer: a font atlas that cannot be uploaded skips its texts and is reported once" * doctest::test_suite(Test::GpuSuite))
		{
			// --gpu-inject-fault=oom-texture fails every sampled-only texture creation, the atlas among them; the render targets
			// keep working.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomTexture });
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets({ .EngineResources = true });
			assets.OpenProject(false);
			{
				TextSetup setup(gpu);
				const std::vector<TextItem> texts = { TextItem{ .Text = "IIII", .Size = 600.0f, .Anchor = glm::vec2(0.0f, 0.0f), .Pivot = glm::vec2(0.0f, 0.0f) } };
				Test::ExpectLog failed(LogLevel::Error, "ASSET_UPLOAD_FAILED");
				uint32_t drawn = 1;
				const Image first = setup.Render({ .Texts = texts, .DebugDraw = nullptr, .Assets = &assets.GetManager() }, drawn);
				CHECK(drawn == 0);
				CHECK(CountLit(first, 0, 0, Width, Height) == 0);
				CHECK(setup.GetRenderer().GetFontAtlasCount() == 0);
				// The failure is remembered for this font version: no second upload attempt or diagnostic.
				setup.Render({ .Texts = texts, .DebugDraw = nullptr, .Assets = &assets.GetManager() }, drawn);
				CHECK(drawn == 0);
				CHECK(failed.GetMatchCount() == 1);
				const auto uploadFailures = std::ranges::count_if(assets.GetManager().GetDiagnostics(), [](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Code == AssetUploadFailedCode && diagnostic.Asset == BuiltinAssetHandles::DefaultFont;
				});
				CHECK(uploadFailures == 1);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TextRenderer: glyph runs draw their glyphs in their colours, each clipped to its scissor" * doctest::test_suite(Test::GpuSuite))
		{
			// M22 (C22.1, Docs/Decisions/0033-m22-contract.md decision 7): pre-laid-out glyphs in the framebuffer's pixels, a run
			// per font, colour and scissor; a scissor partly outside the framebuffer is cut to it, one wholly outside or empty
			// draws nothing and is not counted.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			constexpr AssetHandle SolidFont{ 0xF047 };
			Test::InMemoryAssetManager assets;
			assets.Publish(SolidFont, MakeSolidFont());
			{
				TextSetup setup(gpu);
				const std::vector<TextPixelGlyph> glyphs = {
					MakeSolidGlyph(glm::vec2(8.0f), glm::vec2(24.0f)),
					MakeSolidGlyph(glm::vec2(40.0f, 8.0f), glm::vec2(56.0f, 24.0f)),
					MakeSolidGlyph(glm::vec2(72.0f, 8.0f), glm::vec2(88.0f, 24.0f)),
					MakeSolidGlyph(glm::vec2(104.0f, 8.0f), glm::vec2(120.0f, 24.0f)),
				};
				const std::vector<TextGlyphRun> runs = {
					TextGlyphRun{ .Font = SolidFont, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), .FirstGlyph = 0, .GlyphCount = 1 },
					TextGlyphRun{ .Font = SolidFont, .Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), .FirstGlyph = 1, .GlyphCount = 1, .Scissor = nvrhi::Rect(44, 52, -8, 16) },
					TextGlyphRun{ .Font = SolidFont, .Color = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), .FirstGlyph = 2, .GlyphCount = 1, .Scissor = nvrhi::Rect(200, 300, 0, 64) },
					TextGlyphRun{ .Font = SolidFont, .Color = glm::vec4(1.0f), .FirstGlyph = 3, .GlyphCount = 1, .Scissor = nvrhi::Rect(104, 104, 8, 24) },
				};
				uint32_t drawn = 0;
				RenderPassCounters counters;
				const Image image = setup.RenderGlyphRuns({ .Glyphs = glyphs, .Runs = runs, .Assets = &assets }, drawn, counters);
				CHECK(drawn == 2);
				CHECK(counters.DrawCalls == 2);
				CHECK(counters.Triangles == 4);
				for (uint32_t y = 0; y < 32; ++y)
				{
					for (uint32_t x = 0; x < Width; ++x)
					{
						CAPTURE(x);
						CAPTURE(y);
						glm::ivec3 expected(0);
						if (x >= 8 && x < 24 && y >= 8 && y < 24)
							expected = glm::ivec3(255, 0, 0);
						else if (x >= 44 && x < 52 && y >= 8 && y < 16)
							expected = glm::ivec3(0, 255, 0);
						CHECK(GetPixel(image, x, y) == expected);
					}
				}
				CHECK(setup.GetRenderer().GetFontAtlasCount() == 1);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TextRenderer: glyph runs of one atlas and scissor draw together, and runs placed beyond float's range are left out"
			* doctest::test_suite(Test::GpuSuite))
		{
			// Colour is per vertex, so two runs of one font and one scissor are one draw; a run with a non-finite glyph corner or
			// colour, or without glyphs, is left out and not counted, and the others still draw.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			constexpr AssetHandle SolidFont{ 0xF047 };
			Test::InMemoryAssetManager assets;
			assets.Publish(SolidFont, MakeSolidFont());
			{
				TextSetup setup(gpu);
				constexpr float Infinity = std::numeric_limits<float>::infinity();
				const std::vector<TextPixelGlyph> glyphs = {
					MakeSolidGlyph(glm::vec2(8.0f), glm::vec2(24.0f)),
					MakeSolidGlyph(glm::vec2(40.0f, 8.0f), glm::vec2(56.0f, 24.0f)),
					MakeSolidGlyph(glm::vec2(72.0f, 8.0f), glm::vec2(Infinity, 24.0f)),
				};
				const std::vector<TextGlyphRun> runs = {
					TextGlyphRun{ .Font = SolidFont, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), .FirstGlyph = 0, .GlyphCount = 1 },
					TextGlyphRun{ .Font = SolidFont, .Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), .FirstGlyph = 1, .GlyphCount = 1 },
					TextGlyphRun{ .Font = SolidFont, .Color = glm::vec4(1.0f), .FirstGlyph = 2, .GlyphCount = 1 },
					TextGlyphRun{ .Font = SolidFont, .Color = glm::vec4(Infinity), .FirstGlyph = 0, .GlyphCount = 1 },
					TextGlyphRun{ .Font = SolidFont, .Color = glm::vec4(1.0f), .FirstGlyph = 1, .GlyphCount = 0 },
				};
				uint32_t drawn = 0;
				RenderPassCounters counters;
				const Image image = setup.RenderGlyphRuns({ .Glyphs = glyphs, .Runs = runs, .Assets = &assets }, drawn, counters);
				CHECK(drawn == 2);
				CHECK(counters.DrawCalls == 1);
				CHECK(GetPixel(image, 16, 16) == glm::ivec3(255, 0, 0));
				CHECK(GetPixel(image, 48, 16) == glm::ivec3(0, 255, 0));
				CHECK(GetPixel(image, 80, 16) == glm::ivec3(0));
				CHECK(CountLit(image, 64, 0, Width, Height) == 0);

				// No run to draw: nothing is recorded.
				RenderPassCounters none;
				const std::vector<TextGlyphRun> empty = { runs[2] };
				setup.RenderGlyphRuns({ .Glyphs = glyphs, .Runs = empty, .Assets = &assets }, drawn, none);
				CHECK(drawn == 0);
				CHECK(none.DrawCalls == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
