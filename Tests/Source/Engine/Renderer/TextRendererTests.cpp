#include "TestsPCH.h"

#include "Engine/Renderer/TextRenderer.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/GraphicsSpecification.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/TextLayout.h"
#include "Engine/Renderer/UiPass.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/PixelPerfectCameraComponent.h"
#include "Engine/Scene/Components/TextComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Shared/ViewConstants.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The text pass (Architecture §8.10, §8.3 pass 14; Docs/Decisions/0013-m8-decisions.md decision 11) with the Default font
// (Inter, M6's FontImporter through the engine resources), and M22's glyph runs with a font of their own. M25's friction F01
// (Docs/Decisions/0036-m25-tetris-pixel-art.md): small UI text and screen text under a pixel-perfect camera keep every stroke.

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

		// M25's F01: Tetris's HUD geometry, a 360 x 450 reference under a PixelPerfectCamera at scale 2 in a 720 x 900 view.
		constexpr uint32_t PixelArtViewWidth = 720;
		constexpr uint32_t PixelArtViewHeight = 900;
		constexpr uint32_t PixelArtSceneWidth = 360;
		constexpr uint32_t PixelArtSceneHeight = 450;
		constexpr uint32_t PixelArtScale = 2;
		// The shortest stretch of a stroke along a row or a column of pixel centres that the stroke check asks coverage of, in
		// scene pixels, and the least coverage such a stroke keeps: a quarter, 64 of 255 for white text on black.
		constexpr float MinCheckedStroke = 0.5f;
		constexpr int MinStrokeCoverage = 64;
		// The atlas value of an outline (FontData: 128 is the outline, larger values inside).
		constexpr float AtlasOutline = 128.0f;

		// The atlas value of `font` at normalized coordinates `uv`, 0 to 255, interpolated bilinearly between texel centres and
		// clamped at the atlas's edges, as the text sampler reads it.
		float SampleAtlas(const FontData& font, glm::vec2 uv)
		{
			const glm::vec2 texel = uv * glm::vec2(static_cast<float>(font.AtlasWidth), static_cast<float>(font.AtlasHeight)) - 0.5f;
			const glm::vec2 base = glm::floor(texel);
			const glm::vec2 fraction = texel - base;
			const auto at = [&font](float x, float y)
			{
				const auto column = static_cast<size_t>(std::clamp(x, 0.0f, static_cast<float>(font.AtlasWidth - 1)));
				const auto row = static_cast<size_t>(std::clamp(y, 0.0f, static_cast<float>(font.AtlasHeight - 1)));
				return static_cast<float>(std::to_integer<int>(font.AtlasPixels[row * font.AtlasWidth + column]));
			};
			const float top = glm::mix(at(base.x, base.y), at(base.x + 1.0f, base.y), fraction.x);
			const float bottom = glm::mix(at(base.x, base.y + 1.0f), at(base.x + 1.0f, base.y + 1.0f), fraction.x);
			return glm::mix(top, bottom, fraction.y);
		}

		// The red byte of scene pixel (x, y) in `image`, the view image at PixelArtScale view pixels per scene pixel.
		int GetScenePixel(const Image& image, int x, int y)
		{
			return std::to_integer<int>(image.GetRow(static_cast<uint32_t>(y) * PixelArtScale)[static_cast<size_t>(x) * PixelArtScale * 4]);
		}

		// The strokes the stroke check found, and those it found without coverage.
		struct StrokeCheck
		{
			uint32_t Strokes = 0;
			std::vector<std::string> Misses{}; // one line per stroke without coverage
		};

		// Checks every stroke of `glyph` (in scene pixels, its atlas rectangle in `font`) that a row (axis 0) or a column
		// (axis 1) of pixel centres crosses, against `image` (white text on black): each stretch of the line inside the outline
		// (SampleAtlas at least AtlasOutline, found in 1/32-pixel steps) at least MinCheckedStroke long is a stroke, and one of
		// the scene pixels it passes through must keep MinStrokeCoverage. A stretch of a pixel or more holds a pixel centre, so
		// it is covered whatever the shading; a shorter one is a stem or a bar under a pixel wide, which lies between two pixel
		// centres when its pixels are dropped.
		void CheckStrokes(const Image& image, const FontData& font, const TextPixelGlyph& glyph, std::string_view label, StrokeCheck& check)
		{
			constexpr float Step = 1.0f / 32.0f;
			const glm::vec2 size = glyph.Max - glyph.Min;
			const auto toAtlas = [&glyph, size](glm::vec2 point)
			{
				return glyph.AtlasMin + (point - glyph.Min) / size * (glyph.AtlasMax - glyph.AtlasMin);
			};
			for (glm::length_t along = 0; along < 2; ++along)
			{
				const glm::length_t across = 1 - along;
				const int firstLine = static_cast<int>(std::ceil(glyph.Min[across] - 0.5f));
				const int lastLine = static_cast<int>(std::floor(glyph.Max[across] - 0.5f));
				const int steps = static_cast<int>(std::floor(size[along] / Step));
				for (int line = firstLine; line <= lastLine; ++line)
				{
					glm::vec2 point(0.0f);
					point[across] = static_cast<float>(line) + 0.5f;
					std::optional<float> start;
					for (int step = 0; step <= steps; ++step)
					{
						point[along] = glyph.Min[along] + (static_cast<float>(step) + 0.5f) * Step;
						const bool inside = step < steps && SampleAtlas(font, toAtlas(point)) >= AtlasOutline;
						if (inside && !start.has_value())
							start = point[along] - 0.5f * Step;
						if (inside || !start.has_value())
							continue;
						const float end = point[along] - 0.5f * Step;
						if (end - *start >= MinCheckedStroke)
						{
							++check.Strokes;
							int coverage = 0;
							for (int pixel = static_cast<int>(std::floor(*start)); pixel < static_cast<int>(std::ceil(end)); ++pixel)
							{
								const int x = along == 0 ? pixel : line;
								const int y = along == 0 ? line : pixel;
								if (x >= 0 && y >= 0 && x < static_cast<int>(PixelArtSceneWidth) && y < static_cast<int>(PixelArtSceneHeight))
									coverage = std::max(coverage, GetScenePixel(image, x, y));
							}
							if (coverage < MinStrokeCoverage)
							{
								check.Misses.push_back(std::format("{}: {} {} from {:.2f} to {:.2f}: at most {} of 255", label, along == 0 ? "row" : "column", line,
									*start, end, coverage));
							}
						}
						start.reset();
					}
				}
			}
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

		TEST_CASE("TextRenderer: UI text and screen text of 9 and 10 pixels keep every stroke under a pixel-perfect camera at scale 2"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M25's F01 (Docs/Decisions/0036-m25-tetris-pixel-art.md): Tetris's HUD labels in the Default font at 9 and 10 scene
			// pixels per em, as UiText in a Screen canvas and as Screen Text, drawn at the 360 x 450 reference and upscaled
			// twice. Their stems and bars are under a pixel wide, and every one of them keeps coverage (CheckStrokes) wherever
			// it falls between the pixel centres; "HOLD" read "IIOLD" when coverage followed the field's own screen-space
			// derivative, which vanishes across a stroke that two pixels of one 2 x 2 quad straddle.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets({ .EngineResources = true });
			assets.OpenProject(false);
			const Result<AssetRef<Asset>> loaded = assets.GetManager().Load(BuiltinAssetHandles::DefaultFont);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			const AssetRef<FontData> font = AssetCast<FontData>(*loaded);
			REQUIRE(font != nullptr);
			{
				Test::SceneRendererIntegrationFixture setup(gpu, PixelArtViewWidth, PixelArtViewHeight);
				setup.Assets().Publish(BuiltinAssetHandles::DefaultFont, *loaded);
				Test::SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				Entity camera = scene.CreateEntity("Camera");
				camera.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(0.0f, 0.0f, 10.0f);
				});
				camera.AddComponent<CameraComponent>(
					CameraComponent{ .Projection = ProjectionType::Orthographic, .Primary = true, .Clear = ClearMode::Color, .ClearColor = glm::vec3(0.0f) });
				camera.AddComponent<PixelPerfectCameraComponent>(
					PixelPerfectCameraComponent{ .ReferenceWidth = PixelArtSceneWidth, .ReferenceHeight = PixelArtSceneHeight, .PixelsPerUnit = 16.0f });
				Entity canvas = scene.CreateEntity("HUD");
				canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});

				// Each label at 9 and at 10 pixels per em, as UiText in rows from y 20 and as Screen Text in rows from y 120 (a
				// Text's Size and Offset are at the 1080p reference: 2.4 of them per scene pixel of the 450-pixel scene).
				constexpr std::array<std::string_view, 2> Labels = { "HOLD LEVEL LINES PRESS RESTART", "SCORE NEXT ROTATE BACK" };
				constexpr std::array<float, 2> Sizes = { 9.0f, 10.0f };
				constexpr float TextReferencePerScenePixel = TextReferenceHeight / static_cast<float>(PixelArtSceneHeight);
				float row = 20.0f;
				for (const std::string_view label : Labels)
				{
					for (const float size : Sizes)
					{
						Entity element = scene.CreateEntity(std::format("UiText{}", row), canvas);
						element.AddComponent<UiRectComponent>(UiRectComponent{
							.AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Position = glm::vec2(8.0f, row), .Size = glm::vec2(344.0f, 16.0f) });
						element.AddComponent<UiTextComponent>(UiTextComponent{ .Text = std::string(label), .Size = size, .Wrap = false, .RaycastTarget = false });
						Entity text = scene.CreateEntity(std::format("Text{}", row));
						text.AddComponent<TextComponent>(TextComponent{ .Text = std::string(label),
							.Font = {},
							.Size = size * TextReferencePerScenePixel,
							.Anchor = glm::vec2(0.0f),
							.Pivot = glm::vec2(0.0f),
							.Offset = glm::vec2(8.0f, row + 100.0f) * TextReferencePerScenePixel,
							.Alignment = TextAlignment::Left });
						row += 20.0f;
					}
				}
				TransformSystem::Update(scene);

				Result<RenderSnapshot> extracted = ExtractRenderSnapshot(scene, { .Width = PixelArtViewWidth, .Height = PixelArtViewHeight });
				REQUIRE_MESSAGE(extracted.has_value(), extracted.error().ToString());
				const RenderSnapshot snapshot = std::move(*extracted);
				REQUIRE(snapshot.Ui.Items.size() == 4);
				REQUIRE(snapshot.Texts.size() == 4);
				const Image image = setup.Render(snapshot);
				const RenderOutputMapping mapping = setup.Renderer().GetOutputMapping();
				REQUIRE(mapping.Scale == PixelArtScale);
				REQUIRE(mapping.OffsetX == 0);
				REQUIRE(mapping.OffsetY == 0);

				// The UI's glyphs where the Ui pass places them, and the screen texts' where the Text pass does (Size and Offset
				// scaled by the scene's height over TextReferenceHeight, the block placed by PlaceScreenText).
				StrokeCheck check;
				const UiPassPlan plan = PlanUiPass(snapshot.Ui, setup.Assets());
				for (size_t index = 0; index < plan.Glyphs.size(); ++index)
					CheckStrokes(image, *font, plan.Glyphs[index], std::format("UiText glyph {}", index), check);
				const float pixelScale = static_cast<float>(PixelArtSceneHeight) / TextReferenceHeight;
				size_t textGlyphs = 0;
				for (const TextItem& item : snapshot.Texts)
				{
					const TextLayoutResult layout = LayoutText(*font, item.Text, item.Alignment);
					const float pixelsPerEm = item.Size * pixelScale;
					const glm::vec2 topLeft = PlaceScreenText(item, layout.Size * pixelsPerEm, PixelArtSceneWidth, PixelArtSceneHeight);
					for (const TextGlyphQuad& quad : layout.Quads)
					{
						const TextPixelGlyph glyph{ .Min = topLeft + quad.PositionMin * pixelsPerEm,
							.Max = topLeft + quad.PositionMax * pixelsPerEm,
							.AtlasMin = quad.AtlasMin,
							.AtlasMax = quad.AtlasMax };
						CheckStrokes(image, *font, glyph, std::format("Text \"{}\" at {} glyph {}", item.Text, pixelsPerEm, textGlyphs++), check);
					}
				}
				// 90 glyphs of each kind (the labels' 45 letters at both sizes), each crossed by several strokes.
				CHECK(plan.Glyphs.size() == 90);
				CHECK(textGlyphs == 90);
				CHECK(check.Strokes > 2 * (plan.Glyphs.size() + textGlyphs));
				std::string misses;
				for (size_t index = 0; index < std::min<size_t>(check.Misses.size(), 20); ++index)
					misses += std::format("\n  {}", check.Misses[index]);
				const std::string summary = std::format("{} of {} strokes keep no coverage, the first {}:{}", check.Misses.size(), check.Strokes,
					std::min<size_t>(check.Misses.size(), 20), misses);
				CHECK_MESSAGE(check.Misses.empty(), summary);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
