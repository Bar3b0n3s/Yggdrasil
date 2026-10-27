#include "TestsPCH.h"

#include "Engine/Renderer/QuadBatcher.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// The QuadBatcher and its 9-slice expansion (M18, C18.1, R-2D-BATCH; Docs/Decisions/0029-m18-contract.md decision 10).

namespace Engine {

	namespace {

		// The texel (x, y) of the source textures: distinct bytes per texel, so a read-back pixel names its texel.
		std::array<uint8_t, 4> MakeTexel(uint32_t x, uint32_t y)
		{
			return { static_cast<uint8_t>(16 + 29 * x), static_cast<uint8_t>(16 + 23 * y), static_cast<uint8_t>(200 - 7 * (x + y)), 255 };
		}

		glm::u8vec4 GetPixel(const Image& image, uint32_t x, uint32_t y)
		{
			const std::span<const std::byte> texel = image.GetRow(y).subspan(static_cast<size_t>(x) * 4, 4);
			return { std::to_integer<uint8_t>(texel[0]), std::to_integer<uint8_t>(texel[1]), std::to_integer<uint8_t>(texel[2]),
				std::to_integer<uint8_t>(texel[3]) };
		}

		glm::u8vec4 ToPixel(const std::array<uint8_t, 4>& texel)
		{
			return { texel[0], texel[1], texel[2], texel[3] };
		}

		// A batcher, a `width` x `height` LDR target with SceneDepth (QuadBatchTarget::Ldr) and an sRGB source texture of
		// `textureSize` texels filled with MakeTexel.
		class QuadSetup
		{
		public:
			QuadSetup(Test::HeadlessGpuFixture& gpu, uint32_t width, uint32_t height, glm::uvec2 textureSize)
				: m_Device(&gpu.GetDevice()), m_Width(width), m_Height(height)
			{
				Result<Scope<QuadBatcher>> batcher = QuadBatcher::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(batcher.has_value(), batcher.error().ToString());
				m_Batcher = std::move(*batcher);

				nvrhi::TextureDesc colorDesc;
				colorDesc.width = width;
				colorDesc.height = height;
				colorDesc.format = LdrColorFormat;
				colorDesc.isRenderTarget = true;
				colorDesc.isShaderResource = true;
				colorDesc.initialState = nvrhi::ResourceStates::RenderTarget;
				colorDesc.keepInitialState = true;
				colorDesc.debugName = "QuadBatcherTests.Color";
				nvrhi::TextureDesc depthDesc = colorDesc;
				depthDesc.format = SceneDepthFormat;
				depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
				depthDesc.debugName = "QuadBatcherTests.Depth";
				Result<nvrhi::TextureHandle> color = m_Device->CreateTexture(colorDesc);
				Result<nvrhi::TextureHandle> depth = m_Device->CreateTexture(depthDesc);
				REQUIRE(color.has_value());
				REQUIRE(depth.has_value());
				m_Color = *color;
				m_Depth = *depth;
				Result<nvrhi::FramebufferHandle> framebuffer =
					m_Device->CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_Color).setDepthAttachment(m_Depth));
				REQUIRE(framebuffer.has_value());
				m_Framebuffer = *framebuffer;

				nvrhi::TextureDesc sourceDesc;
				sourceDesc.width = textureSize.x;
				sourceDesc.height = textureSize.y;
				sourceDesc.format = nvrhi::Format::SRGBA8_UNORM;
				sourceDesc.isShaderResource = true;
				sourceDesc.initialState = nvrhi::ResourceStates::ShaderResource;
				sourceDesc.keepInitialState = true;
				sourceDesc.debugName = "QuadBatcherTests.Source";
				Result<nvrhi::TextureHandle> source = m_Device->CreateTexture(sourceDesc);
				REQUIRE(source.has_value());
				m_Source = *source;
				std::vector<uint8_t> texels;
				for (uint32_t y = 0; y < textureSize.y; ++y)
				{
					for (uint32_t x = 0; x < textureSize.x; ++x)
					{
						const std::array<uint8_t, 4> texel = MakeTexel(x, y);
						texels.insert(texels.end(), texel.begin(), texel.end());
					}
				}
				Result<nvrhi::CommandListHandle> list = m_Device->CreateCommandList();
				REQUIRE(list.has_value());
				(*list)->open();
				(*list)->writeTexture(m_Source, 0, 0, texels.data(), static_cast<size_t>(textureSize.x) * 4);
				(*list)->close();
				m_Device->ExecuteCommandList(**list);
			}

			~QuadSetup() { m_Device->WaitForIdle(); }

			QuadSetup(const QuadSetup&) = delete;
			QuadSetup& operator=(const QuadSetup&) = delete;

			[[nodiscard]] nvrhi::ITexture* GetSource() const { return m_Source.Get(); }
			[[nodiscard]] QuadBatcher& GetBatcher() { return *m_Batcher; }

			// Clears the target to black, records `quads` in scene pixels, clipped to `scissor`, and reads the target back;
			// `drawn` gets the count and `recordedCounters`, when given, the record's counters.
			[[nodiscard]] Image Render(std::span<const QuadDraw> quads, uint32_t& drawn, std::optional<nvrhi::Rect> scissor = std::nullopt,
				RenderPassCounters* recordedCounters = nullptr)
			{
				Result<nvrhi::CommandListHandle> list = m_Device->CreateCommandList();
				REQUIRE(list.has_value());
				(*list)->open();
				(*list)->clearTextureFloat(m_Color, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
				(*list)->clearDepthStencilTexture(m_Depth, nvrhi::AllSubresources, true, 0.0f, false, 0);
				RenderPassCounters counters;
				const Result<uint32_t> recorded = m_Batcher->Record(**list, m_Bindings,
					{ .Quads = quads,
						.Target = QuadBatchTarget::Ldr,
						.Framebuffer = m_Framebuffer,
						.Scissor = scissor,
						.Transform = MakeQuadPixelTransform(m_Width, m_Height) },
					counters);
				(*list)->close();
				m_Device->ExecuteCommandList(**list);
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				drawn = *recorded;
				CHECK(counters.DrawCalls >= 1);
				if (recordedCounters != nullptr)
					*recordedCounters = counters;
				Readback readback(*m_Device);
				Result<Image> image = readback.ReadTexture(*m_Color);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				return std::move(*image);
			}

			// Records `quads` in scene pixels over the target, executes the command list and returns the record's result,
			// failed or not.
			[[nodiscard]] Result<uint32_t> Record(std::span<const QuadDraw> quads)
			{
				Result<nvrhi::CommandListHandle> list = m_Device->CreateCommandList();
				REQUIRE(list.has_value());
				(*list)->open();
				RenderPassCounters counters;
				Result<uint32_t> recorded = m_Batcher->Record(**list, m_Bindings,
					{ .Quads = quads, .Target = QuadBatchTarget::Ldr, .Framebuffer = m_Framebuffer, .Transform = MakeQuadPixelTransform(m_Width, m_Height) },
					counters);
				(*list)->close();
				m_Device->ExecuteCommandList(**list);
				return recorded;
			}
		private:
			GraphicsDevice* m_Device = nullptr; // fixture back-reference; the device outlives every owned GPU object
			uint32_t m_Width = 1;
			uint32_t m_Height = 1;
			Scope<QuadBatcher> m_Batcher{};
			PassBindingCache m_Bindings{};
			nvrhi::TextureHandle m_Color{};
			nvrhi::TextureHandle m_Depth{};
			nvrhi::FramebufferHandle m_Framebuffer{};
			nvrhi::TextureHandle m_Source{};
		};

		// A quad over the pixel rectangle [min, max) at depth 0 showing the normalized texture rectangle [uvMin, uvMax].
		QuadDraw MakePixelQuad(glm::vec2 min, glm::vec2 max, glm::vec2 uvMin, glm::vec2 uvMax, nvrhi::ITexture* texture)
		{
			return QuadDraw{ .Corners = { glm::vec3(min.x, min.y, 0.0f), glm::vec3(max.x, min.y, 0.0f), glm::vec3(max.x, max.y, 0.0f),
								 glm::vec3(min.x, max.y, 0.0f) },
				.TexCoordMin = uvMin,
				.TexCoordMax = uvMax,
				.Texture = texture,
				.Sampler = QuadSampler::Point };
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("NineSlice: the corners keep their size and the edges and centre stretch")
		{
			const NineSliceSource whole{ .TextureSize = glm::uvec2(16), .RectMin = glm::uvec2(0), .RectSize = glm::uvec2(16), .Borders = glm::uvec4(4) };
			const NineSliceExpansion expansion = ExpandNineSlice(whole, glm::vec2(40.0f, 30.0f));
			REQUIRE(expansion.Count == 9);
			CHECK(expansion.Patches[0] == NineSlicePatch{ .Min = { 0.0f, 0.0f }, .Max = { 4.0f, 4.0f }, .TexCoordMin = { 0.0f, 0.0f }, .TexCoordMax = { 0.25f, 0.25f } });
			CHECK(expansion.Patches[1] == NineSlicePatch{ .Min = { 4.0f, 0.0f }, .Max = { 36.0f, 4.0f }, .TexCoordMin = { 0.25f, 0.0f }, .TexCoordMax = { 0.75f, 0.25f } });
			CHECK(expansion.Patches[4] == NineSlicePatch{ .Min = { 4.0f, 4.0f }, .Max = { 36.0f, 26.0f }, .TexCoordMin = { 0.25f, 0.25f }, .TexCoordMax = { 0.75f, 0.75f } });
			CHECK(expansion.Patches[8] == NineSlicePatch{ .Min = { 36.0f, 26.0f }, .Max = { 40.0f, 30.0f }, .TexCoordMin = { 0.75f, 0.75f }, .TexCoordMax = { 1.0f, 1.0f } });
			// The border scale multiplies the corners' size, not their texels.
			const NineSliceExpansion scaled = ExpandNineSlice(whole, glm::vec2(40.0f, 30.0f), 3.0f);
			REQUIRE(scaled.Count == 9);
			CHECK(scaled.Patches[4] == NineSlicePatch{ .Min = { 12.0f, 12.0f }, .Max = { 28.0f, 18.0f }, .TexCoordMin = { 0.25f, 0.25f }, .TexCoordMax = { 0.75f, 0.75f } });
			// A frame inside a sheet: texel edges over the texture size, without a half-texel inset.
			const NineSliceSource frame{ .TextureSize = glm::uvec2(64, 32), .RectMin = glm::uvec2(16, 8), .RectSize = glm::uvec2(16), .Borders = glm::uvec4(2, 3, 4, 5) };
			const NineSliceExpansion framed = ExpandNineSlice(frame, glm::vec2(16.0f));
			REQUIRE(framed.Count == 9);
			CHECK(framed.Patches[0].TexCoordMin == glm::vec2(16.0f / 64.0f, 8.0f / 32.0f));
			CHECK(framed.Patches[0].TexCoordMax == glm::vec2(18.0f / 64.0f, 11.0f / 32.0f));
			CHECK(framed.Patches[8].TexCoordMin == glm::vec2(28.0f / 64.0f, 19.0f / 32.0f));
			CHECK(framed.Patches[8].TexCoordMax == glm::vec2(32.0f / 64.0f, 24.0f / 32.0f));
			// Without borders the frame is one stretched patch.
			const NineSliceExpansion plain = ExpandNineSlice({ .TextureSize = glm::uvec2(16), .RectSize = glm::uvec2(16) }, glm::vec2(40.0f, 30.0f));
			REQUIRE(plain.Count == 1);
			CHECK(plain.Patches[0] == NineSlicePatch{ .Min = { 0.0f, 0.0f }, .Max = { 40.0f, 30.0f }, .TexCoordMin = { 0.0f, 0.0f }, .TexCoordMax = { 1.0f, 1.0f } });
		}

		TEST_CASE("NineSlice: borders wider than the destination shrink in proportion")
		{
			// 4 + 4 texels of border in 6 units: 3 each, and the middle column vanishes (two columns of three rows).
			const NineSliceSource whole{ .TextureSize = glm::uvec2(16), .RectSize = glm::uvec2(16), .Borders = glm::uvec4(4) };
			const NineSliceExpansion narrow = ExpandNineSlice(whole, glm::vec2(6.0f, 30.0f));
			REQUIRE(narrow.Count == 6);
			CHECK(narrow.Patches[0] == NineSlicePatch{ .Min = { 0.0f, 0.0f }, .Max = { 3.0f, 4.0f }, .TexCoordMin = { 0.0f, 0.0f }, .TexCoordMax = { 0.25f, 0.25f } });
			CHECK(narrow.Patches[1] == NineSlicePatch{ .Min = { 3.0f, 0.0f }, .Max = { 6.0f, 4.0f }, .TexCoordMin = { 0.75f, 0.0f }, .TexCoordMax = { 1.0f, 0.25f } });
			// Unequal borders keep their proportion: 2 and 6 texels in 4 units are 1 and 3.
			const NineSliceSource unequal{ .TextureSize = glm::uvec2(16), .RectSize = glm::uvec2(16), .Borders = glm::uvec4(2, 0, 6, 0) };
			const NineSliceExpansion squeezed = ExpandNineSlice(unequal, glm::vec2(4.0f, 10.0f));
			REQUIRE(squeezed.Count == 2);
			CHECK(squeezed.Patches[0].Max == glm::vec2(1.0f, 10.0f));
			CHECK(squeezed.Patches[1].Min == glm::vec2(1.0f, 0.0f));
			CHECK(squeezed.Patches[1].Max == glm::vec2(4.0f, 10.0f));
		}

		TEST_CASE("QuadBatcher: the pixel transform maps pixel corners to clip space")
		{
			const glm::mat4 transform = MakeQuadPixelTransform(200, 100);
			const auto check = [&transform](glm::vec3 pixel, glm::vec4 expected)
			{
				const glm::vec4 clip = transform * glm::vec4(pixel, 1.0f);
				CAPTURE(pixel.x);
				CAPTURE(pixel.y);
				for (glm::length_t component = 0; component < 4; ++component)
					CHECK(clip[component] == doctest::Approx(expected[component]).epsilon(1e-6));
			};
			check(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec4(-1.0f, 1.0f, 0.0f, 1.0f));
			check(glm::vec3(200.0f, 100.0f, 0.0f), glm::vec4(1.0f, -1.0f, 0.0f, 1.0f));
			check(glm::vec3(100.0f, 50.0f, 0.5f), glm::vec4(0.0f, 0.0f, 0.5f, 1.0f));
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("QuadBatcher: an integer-scale draw reads back its source texels")
		{
			// M18 acceptance (C18.1), under both API caps (the gpu suite runs every case under 1.4 and 1.3): a 4 x 4 texture
			// drawn at scale 3 with the point sampler into the LDR target gives each texel a 3 x 3 block of its exact bytes.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			QuadSetup setup(gpu, 24, 24, glm::uvec2(4));
			const std::array<QuadDraw, 1> quads = { MakePixelQuad(glm::vec2(6.0f), glm::vec2(18.0f), glm::vec2(0.0f), glm::vec2(1.0f), setup.GetSource()) };
			uint32_t drawn = 0;
			const Image image = setup.Render(quads, drawn);
			CHECK(drawn == 1);
			for (uint32_t y = 0; y < 24; ++y)
			{
				for (uint32_t x = 0; x < 24; ++x)
				{
					const bool inside = x >= 6 && x < 18 && y >= 6 && y < 18;
					const glm::u8vec4 expected = inside ? ToPixel(MakeTexel((x - 6) / 3, (y - 6) / 3)) : glm::u8vec4(0, 0, 0, 255);
					CHECK(GetPixel(image, x, y) == expected);
				}
			}
		}

		TEST_CASE("QuadBatcher: a clipped draw leaves the pixels outside its scissor untouched")
		{
			// M22's scroll views clip with QuadBatchInputs::Scissor: the 4 x 4 texture drawn at scale 3 over [6, 18) x [6, 18),
			// clipped to [9, 30) x [-4, 12), a rectangle partly outside the 24 x 24 target, shows only [9, 18) x [6, 12).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			QuadSetup setup(gpu, 24, 24, glm::uvec2(4));
			const std::array<QuadDraw, 1> quads = { MakePixelQuad(glm::vec2(6.0f), glm::vec2(18.0f), glm::vec2(0.0f), glm::vec2(1.0f), setup.GetSource()) };
			uint32_t drawn = 0;
			const Image image = setup.Render(quads, drawn, nvrhi::Rect(9, 30, -4, 12));
			CHECK(drawn == 1);
			for (uint32_t y = 0; y < 24; ++y)
			{
				for (uint32_t x = 0; x < 24; ++x)
				{
					const bool inside = x >= 9 && x < 18 && y >= 6 && y < 12;
					const glm::u8vec4 expected = inside ? ToPixel(MakeTexel((x - 6) / 3, (y - 6) / 3)) : glm::u8vec4(0, 0, 0, 255);
					CHECK(GetPixel(image, x, y) == expected);
				}
			}
		}

		TEST_CASE("QuadBatcher: a 9-slice draw matches its CPU reference")
		{
			// An 8 x 8 frame with 2-texel borders drawn over a 20 x 16 rectangle at pixel (2, 2): the corners at 1 pixel per
			// texel, the 4 middle texels stretched over 16 pixels across (4 each) and 12 down (3 each), with the point sampler.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			QuadSetup setup(gpu, 24, 20, glm::uvec2(8));
			const NineSliceExpansion expansion =
				ExpandNineSlice({ .TextureSize = glm::uvec2(8), .RectSize = glm::uvec2(8), .Borders = glm::uvec4(2) }, glm::vec2(20.0f, 16.0f));
			REQUIRE(expansion.Count == 9);
			std::vector<QuadDraw> quads;
			for (uint32_t index = 0; index < expansion.Count; ++index)
			{
				const NineSlicePatch& patch = expansion.Patches[index];
				quads.push_back(MakePixelQuad(patch.Min + glm::vec2(2.0f), patch.Max + glm::vec2(2.0f), patch.TexCoordMin, patch.TexCoordMax, setup.GetSource()));
			}
			uint32_t drawn = 0;
			const Image image = setup.Render(quads, drawn);
			CHECK(drawn == 9);
			// The CPU reference of the 9-slice rule, independent of ExpandNineSlice: a destination pixel's texel per axis.
			const auto texelOf = [](uint32_t pixel, uint32_t size, uint32_t pixelsPerMiddleTexel) -> uint32_t
			{
				if (pixel < 2)
					return pixel;
				if (pixel >= size - 2)
					return 6 + (pixel - (size - 2));
				return 2 + (pixel - 2) / pixelsPerMiddleTexel;
			};
			for (uint32_t y = 0; y < 20; ++y)
			{
				for (uint32_t x = 0; x < 24; ++x)
				{
					const bool inside = x >= 2 && x < 22 && y >= 2 && y < 18;
					const glm::u8vec4 expected = inside ? ToPixel(MakeTexel(texelOf(x - 2, 20, 4), texelOf(y - 2, 16, 3))) : glm::u8vec4(0, 0, 0, 255);
					CHECK(GetPixel(image, x, y) == expected);
				}
			}
		}

		TEST_CASE("QuadBatcher: a record splits into draws only where the texture or the sampler changes")
		{
			// Five quads in a row, 4 x 4 pixels each: the source twice with Point, the white texture (no texture) with Point,
			// then with Linear, then the source again: four draws. A quad without a texture shows its colour (linear, encoded
			// with the sRGB OETF into the LDR target).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			QuadSetup setup(gpu, 20, 4, glm::uvec2(4));
			const auto quad = [](float x, nvrhi::ITexture* texture, QuadSampler sampler, glm::vec4 color)
			{
				QuadDraw draw = MakePixelQuad(glm::vec2(x, 0.0f), glm::vec2(x + 4.0f, 4.0f), glm::vec2(0.0f), glm::vec2(1.0f), texture);
				draw.Sampler = sampler;
				draw.Color = color;
				return draw;
			};
			const glm::vec4 white(1.0f);
			const std::array<QuadDraw, 5> quads = {
				quad(0.0f, setup.GetSource(), QuadSampler::Point, white),
				quad(4.0f, setup.GetSource(), QuadSampler::Point, white),
				quad(8.0f, nullptr, QuadSampler::Point, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)),
				quad(12.0f, nullptr, QuadSampler::Linear, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)),
				quad(16.0f, setup.GetSource(), QuadSampler::Point, white),
			};
			uint32_t drawn = 0;
			RenderPassCounters counters;
			const Image image = setup.Render(quads, drawn, std::nullopt, &counters);
			CHECK(drawn == 5);
			CHECK(counters.DrawCalls == 4);
			CHECK(counters.Triangles == 10);
			CHECK(GetPixel(image, 0, 0) == ToPixel(MakeTexel(0, 0)));
			CHECK(GetPixel(image, 7, 3) == ToPixel(MakeTexel(3, 3)));
			CHECK(GetPixel(image, 9, 1) == glm::u8vec4(255, 0, 0, 255));
			CHECK(GetPixel(image, 13, 2) == glm::u8vec4(0, 255, 0, 255));
			CHECK(GetPixel(image, 19, 0) == ToPixel(MakeTexel(3, 0)));
			QuadBatcher& batcher = setup.GetBatcher();
			CHECK(batcher.GetSampler(QuadSampler::Point) != nullptr);
			CHECK(batcher.GetSampler(QuadSampler::Linear) != nullptr);
			CHECK(batcher.GetSampler(QuadSampler::Point) != batcher.GetSampler(QuadSampler::Linear));
			CHECK(batcher.GetPipelineCount() == QuadBatcher::PipelineCount);
		}

		TEST_CASE("QuadBatcher: a non-finite quad is skipped and the others draw")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			QuadSetup setup(gpu, 8, 8, glm::uvec2(4));
			QuadDraw brokenCorner = MakePixelQuad(glm::vec2(0.0f), glm::vec2(8.0f), glm::vec2(0.0f), glm::vec2(1.0f), nullptr);
			brokenCorner.Corners[2].x = std::numeric_limits<float>::quiet_NaN();
			QuadDraw brokenColor = MakePixelQuad(glm::vec2(0.0f), glm::vec2(8.0f), glm::vec2(0.0f), glm::vec2(1.0f), nullptr);
			brokenColor.Color.g = std::numeric_limits<float>::infinity();
			QuadDraw red = MakePixelQuad(glm::vec2(4.0f), glm::vec2(8.0f), glm::vec2(0.0f), glm::vec2(1.0f), nullptr);
			red.Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
			const std::array<QuadDraw, 3> quads = { brokenCorner, brokenColor, red };
			uint32_t drawn = 0;
			RenderPassCounters counters;
			const Image image = setup.Render(quads, drawn, std::nullopt, &counters);
			CHECK(drawn == 1);
			CHECK(counters.DrawCalls == 1);
			CHECK(GetPixel(image, 1, 1) == glm::u8vec4(0, 0, 0, 255));
			CHECK(GetPixel(image, 6, 6) == glm::u8vec4(255, 0, 0, 255));
		}

		TEST_CASE("QuadBatcher: an empty clip rectangle records nothing")
		{
			// A scissor wholly outside the framebuffer, and an empty one inside it: the record draws nothing, creates no binding
			// set and returns 0.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<Scope<QuadBatcher>> batcher = QuadBatcher::Create(device, gpu.GetPipelines());
			REQUIRE_MESSAGE(batcher.has_value(), batcher.error().ToString());
			nvrhi::TextureDesc colorDesc;
			colorDesc.width = 8;
			colorDesc.height = 8;
			colorDesc.format = LdrColorFormat;
			colorDesc.isRenderTarget = true;
			colorDesc.initialState = nvrhi::ResourceStates::RenderTarget;
			colorDesc.keepInitialState = true;
			colorDesc.debugName = "QuadBatcherTests.EmptyClipColor";
			nvrhi::TextureDesc depthDesc = colorDesc;
			depthDesc.format = SceneDepthFormat;
			depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
			depthDesc.debugName = "QuadBatcherTests.EmptyClipDepth";
			Result<nvrhi::TextureHandle> color = device.CreateTexture(colorDesc);
			Result<nvrhi::TextureHandle> depth = device.CreateTexture(depthDesc);
			REQUIRE(color.has_value());
			REQUIRE(depth.has_value());
			Result<nvrhi::FramebufferHandle> framebuffer = device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(*color).setDepthAttachment(*depth));
			REQUIRE(framebuffer.has_value());
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			const std::array<QuadDraw, 1> quads = { MakePixelQuad(glm::vec2(0.0f), glm::vec2(8.0f), glm::vec2(0.0f), glm::vec2(1.0f), nullptr) };
			PassBindingCache bindings;
			for (const nvrhi::Rect scissor : { nvrhi::Rect(10, 20, 0, 8), nvrhi::Rect(2, 2, 0, 8) })
			{
				(*list)->open();
				RenderPassCounters counters;
				const Result<uint32_t> recorded = (*batcher)->Record(**list, bindings,
					{ .Quads = quads, .Target = QuadBatchTarget::Ldr, .Framebuffer = *framebuffer, .Scissor = scissor, .Transform = MakeQuadPixelTransform(8, 8) },
					counters);
				(*list)->close();
				device.ExecuteCommandList(**list);
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				CHECK(*recorded == 0);
				CHECK(counters.DrawCalls == 0);
			}
			CHECK(bindings.GetSize() == 0);
			device.WaitForIdle();
		}

		TEST_CASE("QuadBatcher: quads beyond the per-record budget are skipped with one warning")
		{
			// MaxQuadsPerRecord quads draw; the one after them is skipped, and the warning is logged once per batcher.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			QuadSetup setup(gpu, 4, 4, glm::uvec2(4));
			std::vector<QuadDraw> quads(QuadBatcher::MaxQuadsPerRecord + 1,
				MakePixelQuad(glm::vec2(0.0f), glm::vec2(2.0f), glm::vec2(0.0f), glm::vec2(1.0f), nullptr));
			// The quad past the budget would light the bottom-right pixels.
			quads.back() = MakePixelQuad(glm::vec2(2.0f), glm::vec2(4.0f), glm::vec2(0.0f), glm::vec2(1.0f), nullptr);
			Test::ExpectLog warning(LogLevel::Warn, "budget");
			uint32_t drawn = 0;
			const Image first = setup.Render(quads, drawn);
			CHECK(drawn == QuadBatcher::MaxQuadsPerRecord);
			CHECK(GetPixel(first, 0, 0) == glm::u8vec4(255, 255, 255, 255));
			CHECK(GetPixel(first, 3, 3) == glm::u8vec4(0, 0, 0, 255));
			const Image second = setup.Render(quads, drawn);
			CHECK(drawn == QuadBatcher::MaxQuadsPerRecord);
			CHECK(GetPixel(second, 3, 3) == glm::u8vec4(0, 0, 0, 255));
			CHECK(warning.GetMatchCount() == 1);
		}

		TEST_CASE("QuadBatcher: a record whose binding set fails keeps no unwritten index buffer, and the next record draws")
		{
			// A record of 1,500 quads grows the buffers past their first 1,024 quads; its first texture's binding set fails
			// (NVRHI's validation refuses a texture handle whose description has no mip level: its subresources do not
			// intersect the binding's). The failed record must leave no index buffer whose indices it never wrote, which every
			// later record would draw with: the same quads with a texture it can bind then draw, the last one included.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			QuadSetup setup(gpu, 8, 8, glm::uvec2(4));
			nvrhi::TextureDesc unbindableDesc = setup.GetSource()->getDesc();
			unbindableDesc.mipLevels = 0;
			unbindableDesc.debugName = "QuadBatcherTests.Unbindable";
			nvrhi::TextureHandle unbindable = device.GetNvrhiDevice()->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image,
				setup.GetSource()->getNativeObject(nvrhi::ObjectTypes::VK_Image), unbindableDesc);
			REQUIRE(unbindable != nullptr);
			std::vector<QuadDraw> quads(1500, MakePixelQuad(glm::vec2(0.0f), glm::vec2(4.0f), glm::vec2(0.0f), glm::vec2(1.0f), nullptr));
			quads.front().Texture = unbindable.Get();
			{
				Test::ExpectLog refused(LogLevel::Error, "does not intersect");
				const Result<uint32_t> failed = setup.Record(quads);
				REQUIRE_FALSE(failed.has_value());
				CHECK(failed.error().GetCode() == ErrorCode::Gpu);
			}
			device.GetDiagnostics().ResetCounts();
			unbindable = nullptr;

			quads.front().Texture = nullptr;
			quads.back() = MakePixelQuad(glm::vec2(4.0f), glm::vec2(8.0f), glm::vec2(0.0f), glm::vec2(1.0f), nullptr);
			quads.back().Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
			uint32_t drawn = 0;
			const Image image = setup.Render(quads, drawn);
			CHECK(drawn == 1500);
			CHECK(GetPixel(image, 1, 1) == glm::u8vec4(255, 255, 255, 255));
			CHECK(GetPixel(image, 6, 6) == glm::u8vec4(255, 0, 0, 255));
			CHECK(GetPixel(image, 6, 1) == glm::u8vec4(0, 0, 0, 255));
		}

		TEST_CASE("QuadBatcher: an Hdr record draws linear radiance tested against SceneDepth")
		{
			// The Hdr pipeline writes the product unencoded into SceneColor (RGBA16_FLOAT, values above 1 kept) and tests
			// against SceneDepth (reverse-Z, GreaterOrEqual): with the depth cleared to 0.5, the quad at depth 0.25 (farther)
			// is hidden and the one at 0.75 (nearer) draws.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<Scope<QuadBatcher>> batcher = QuadBatcher::Create(device, gpu.GetPipelines());
			REQUIRE_MESSAGE(batcher.has_value(), batcher.error().ToString());
			nvrhi::TextureDesc colorDesc;
			colorDesc.width = 8;
			colorDesc.height = 4;
			colorDesc.format = SceneColorFormat;
			colorDesc.isRenderTarget = true;
			colorDesc.isShaderResource = true;
			colorDesc.initialState = nvrhi::ResourceStates::RenderTarget;
			colorDesc.keepInitialState = true;
			colorDesc.debugName = "QuadBatcherTests.SceneColor";
			nvrhi::TextureDesc depthDesc = colorDesc;
			depthDesc.format = SceneDepthFormat;
			depthDesc.isShaderResource = false;
			depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
			depthDesc.debugName = "QuadBatcherTests.SceneDepth";
			Result<nvrhi::TextureHandle> color = device.CreateTexture(colorDesc);
			Result<nvrhi::TextureHandle> depth = device.CreateTexture(depthDesc);
			REQUIRE(color.has_value());
			REQUIRE(depth.has_value());
			Result<nvrhi::FramebufferHandle> framebuffer = device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(*color).setDepthAttachment(*depth));
			REQUIRE(framebuffer.has_value());
			const auto atDepth = [](float minX, float depthValue, glm::vec4 emitted)
			{
				QuadDraw draw = MakePixelQuad(glm::vec2(minX, 0.0f), glm::vec2(minX + 4.0f, 4.0f), glm::vec2(0.0f), glm::vec2(1.0f), nullptr);
				for (glm::vec3& corner : draw.Corners)
					corner.z = depthValue;
				draw.Color = emitted;
				return draw;
			};
			const std::array<QuadDraw, 2> quads = { atDepth(0.0f, 0.25f, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)),
				atDepth(4.0f, 0.75f, glm::vec4(2.0f, 0.5f, 0.25f, 1.0f)) };
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			PassBindingCache bindings;
			(*list)->open();
			(*list)->clearTextureFloat(*color, nvrhi::AllSubresources, nvrhi::Color(0.0f));
			(*list)->clearDepthStencilTexture(*depth, nvrhi::AllSubresources, true, 0.5f, false, 0);
			RenderPassCounters counters;
			const Result<uint32_t> recorded = (*batcher)->Record(**list, bindings,
				{ .Quads = quads, .Target = QuadBatchTarget::Hdr, .Framebuffer = *framebuffer, .Transform = MakeQuadPixelTransform(8, 4) }, counters);
			(*list)->close();
			device.ExecuteCommandList(**list);
			REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
			CHECK(*recorded == 2);
			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(**color);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			const auto radiance = [&image](uint32_t x, uint32_t y)
			{
				const std::span<const std::byte> texel = image->GetRow(y).subspan(static_cast<size_t>(x) * 8, 8);
				std::array<uint16_t, 4> halves{};
				std::memcpy(halves.data(), texel.data(), sizeof(halves));
				return glm::vec4(glm::unpackHalf1x16(halves[0]), glm::unpackHalf1x16(halves[1]), glm::unpackHalf1x16(halves[2]), glm::unpackHalf1x16(halves[3]));
			};
			CHECK(radiance(1, 1) == glm::vec4(0.0f));
			CHECK(radiance(6, 2) == glm::vec4(2.0f, 0.5f, 0.25f, 1.0f));
			device.WaitForIdle();
		}
	}

}
