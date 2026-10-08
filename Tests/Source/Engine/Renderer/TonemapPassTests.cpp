#include "TestsPCH.h"

#include "Engine/Renderer/TonemapPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/BlueNoise.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/ViewConstants.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"
#include "Support/TonemapCurvesProgram.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Tonemapping and encoding (Architecture §8.9, §15.3 "tonemapper curves vs CPU at 1,024 points"; Docs/Decisions/
// 0013-m8-decisions.md decision 8). The pass tests run TonemapPass on small SceneColor rows; the curve tests run the
// TonemapCurves program (Tests/Source/Support/TonemapCurvesProgram.h), and the 1,024-point comparison checks it against
// the CPU references (Support/RenderReference.h).

namespace Engine {

	namespace {

		constexpr std::array<RenderTonemapper, 4> Tonemappers = { RenderTonemapper::AgX, RenderTonemapper::Aces, RenderTonemapper::PbrNeutral,
			RenderTonemapper::Linear };

		// The binary16 bits of `value` (round to nearest even), for RGBA16_FLOAT uploads.
		uint16_t ToHalf(double value)
		{
			return glm::packHalf1x16(static_cast<float>(value));
		}

		// RGBA binary16 texels of `colors` with alpha 1.
		std::vector<std::array<uint16_t, 4>> ToHalfTexels(std::span<const glm::dvec3> colors)
		{
			std::vector<std::array<uint16_t, 4>> texels;
			for (const glm::dvec3& color : colors)
				texels.push_back({ ToHalf(color.r), ToHalf(color.g), ToHalf(color.b), ToHalf(1.0) });
			return texels;
		}

		// A shader-readable RGBA16F texture of `texels.size()` x 1 texels.
		nvrhi::TextureHandle CreateHalfTexture(GraphicsDevice& device, std::span<const std::array<uint16_t, 4>> texels, const char* name)
		{
			nvrhi::TextureDesc desc;
			desc.width = static_cast<uint32_t>(texels.size());
			desc.height = 1;
			desc.format = SceneColorFormat;
			desc.isShaderResource = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = name;
			const std::array<TextureSubresourceData, 1> subresources = { { { .Data = std::as_bytes(texels) } } };
			Result<TextureUpload> texture = device.GetHostImageUpload().CreateTexture(desc, subresources, TextureUploadPath::Staging);
			REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());
			return texture->Texture;
		}

		// The blue-noise texture as the built-in holds it (R8_UNORM, BlueNoiseSize², GenerateBlueNoise()).
		nvrhi::TextureHandle CreateBlueNoiseTexture(GraphicsDevice& device)
		{
			const std::vector<uint8_t> noise = GenerateBlueNoise();
			nvrhi::TextureDesc desc;
			desc.width = BlueNoiseSize;
			desc.height = BlueNoiseSize;
			desc.format = nvrhi::Format::R8_UNORM;
			desc.isShaderResource = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "TonemapPassTests.BlueNoise";
			const std::array<TextureSubresourceData, 1> subresources = { { { .Data = std::as_bytes(std::span(noise)) } } };
			Result<TextureUpload> texture = device.GetHostImageUpload().CreateTexture(desc, subresources, TextureUploadPath::Staging);
			REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());
			return texture->Texture;
		}

		// The optional inputs of one record.
		struct TonemapRunInputs
		{
			nvrhi::ITexture* BlueNoise = nullptr;
			nvrhi::ITexture* Bloom = nullptr;
			float BloomIntensity = 0.0f;
		};

		// A SceneColor of `width` x 1 RGBA16F texels holding `colors` (or the binary16 `texels`), an LdrColor of the same size
		// and a ViewConstants buffer with `exposure`, on the fixture's device; Run records the pass with `settings` and reads
		// LdrColor back.
		class TonemapSetup
		{
		public:
			TonemapSetup(Test::HeadlessGpuFixture& gpu, std::span<const glm::dvec3> colors, float exposure)
				: TonemapSetup(gpu, ToHalfTexels(colors), exposure)
			{
			}

			TonemapSetup(Test::HeadlessGpuFixture& gpu, std::span<const std::array<uint16_t, 4>> texels, float exposure)
				: m_Device(gpu.GetDevice()), m_Width(static_cast<uint32_t>(texels.size()))
			{
				Result<Scope<TonemapPass>> pass = TonemapPass::Create(m_Device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				m_Pass = std::move(*pass);
				CHECK(m_Pass->GetPipelineCount() == TonemapPass::PipelineCount);
				m_SceneColor = CreateHalfTexture(m_Device, texels, "TonemapPassTests.SceneColor");

				nvrhi::TextureDesc ldrDesc = m_SceneColor->getDesc();
				ldrDesc.format = LdrColorFormat;
				ldrDesc.isUAV = true;
				ldrDesc.debugName = "TonemapPassTests.LdrColor";
				Result<nvrhi::TextureHandle> ldr = m_Device.CreateTexture(ldrDesc);
				REQUIRE(ldr.has_value());
				m_LdrColor = *ldr;

				ViewConstants view{};
				view.ViewportSize = glm::vec2(static_cast<float>(m_Width), 1.0f);
				view.InverseViewportSize = 1.0f / view.ViewportSize;
				view.Exposure = exposure;
				m_View = view;
				nvrhi::BufferDesc bufferDesc;
				bufferDesc.byteSize = sizeof(ViewConstants);
				bufferDesc.isConstantBuffer = true;
				bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				bufferDesc.keepInitialState = true;
				Result<nvrhi::BufferHandle> buffer = m_Device.CreateBuffer(bufferDesc);
				REQUIRE(buffer.has_value());
				m_ViewBuffer = *buffer;
			}

			Image Run(const TonemapSettings& settings, const TonemapRunInputs& inputs = {})
			{
				Result<nvrhi::CommandListHandle> commandList = m_Device.CreateCommandList();
				REQUIRE(commandList.has_value());
				(*commandList)->open();
				(*commandList)->writeBuffer(m_ViewBuffer, &m_View, sizeof(m_View));
				PassBindingCache bindings;
				const Status recorded = m_Pass->Record(**commandList, bindings,
					{ .ViewConstants = m_ViewBuffer, .SceneColor = m_SceneColor, .Bloom = inputs.Bloom, .BloomIntensity = inputs.BloomIntensity, .BlueNoise = inputs.BlueNoise, .LdrColor = m_LdrColor, .Settings = settings });
				(*commandList)->close();
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				m_Device.ExecuteCommandList(**commandList);
				Readback readback(m_Device);
				Result<Image> image = readback.ReadTexture(*m_LdrColor);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				return std::move(*image);
			}
		private:
			GraphicsDevice& m_Device;
			uint32_t m_Width = 0;
			Scope<TonemapPass> m_Pass;
			nvrhi::TextureHandle m_SceneColor;
			nvrhi::TextureHandle m_LdrColor;
			ViewConstants m_View{};
			nvrhi::BufferHandle m_ViewBuffer;
		};

		// Channel `channel` of pixel `pixel` of the first row of an RGBA8 image.
		int GetChannel(const Image& image, uint32_t pixel, uint32_t channel)
		{
			return std::to_integer<int>(image.GetRow(0)[static_cast<size_t>(pixel) * 4 + channel]);
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("TonemapPass: every tonemapper matches its CPU reference at 1,024 points" * doctest::test_suite(Test::GpuSuite))
		{
			// §8.9: "CPU reference implementations of every tonemapper are compared with the GPU at 1,024 sample points". The
			// TonemapCurves program evaluates the same Slang functions as the pass in 32-bit float (no 8-bit quantization); the
			// result must match the double-precision references of Support/RenderReference.h within 1e-3 (display-linear, before
			// the OETF).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				const std::vector<glm::dvec3> points = Test::MakeTonemapSamplePoints();
				REQUIRE(points.size() == 1024);
				for (const RenderTonemapper tonemapper : Tonemappers)
				{
					CAPTURE(static_cast<int>(tonemapper));
					const Result<std::vector<glm::vec3>> gpuValues = Test::RunTonemapCurves(gpu, tonemapper, points);
					REQUIRE_MESSAGE(gpuValues.has_value(), gpuValues.error().ToString());
					REQUIRE(gpuValues->size() == points.size());
					double worst = 0.0;
					for (size_t index = 0; index < points.size(); ++index)
					{
						const glm::dvec3 expected = Test::ApplyTonemapper(tonemapper, points[index]);
						const glm::dvec3 difference = glm::abs(glm::dvec3((*gpuValues)[index]) - expected);
						worst = std::max(worst, std::max({ difference.x, difference.y, difference.z }));
					}
					CHECK(worst < 1e-3);
				}
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: every tonemapper keeps black, rises with radiance and reaches white" * doctest::test_suite(Test::GpuSuite))
		{
			// Properties of the curves that need no reference: 0 maps to 0, a grey ramp never darkens and stays in [0, 1],
			// radiance far above white (2^12) reaches white (AgX's polynomial tops out at 0.9965), and the four tonemappers tell
			// apart at mid grey.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				// 0, then 2^(-12 + k / 4) * (1 + (k mod 4) / 4) for k = 1..96: from deep shadow to 2^12.
				std::vector<glm::dvec3> ramp;
				for (int step = 0; step <= 96; ++step)
					ramp.emplace_back(step == 0 ? 0.0 : std::ldexp(1.0 + (step % 4) * 0.25, -12 + step / 4));
				constexpr size_t MidGrey = 38; // 2^-3 * 1.5 = 0.1875
				REQUIRE(ramp[MidGrey].x == 0.1875);
				std::vector<float> midGreys;
				for (const RenderTonemapper tonemapper : Tonemappers)
				{
					CAPTURE(static_cast<int>(tonemapper));
					const Result<std::vector<glm::vec3>> values = Test::RunTonemapCurves(gpu, tonemapper, ramp);
					REQUIRE_MESSAGE(values.has_value(), values.error().ToString());
					REQUIRE(values->size() == ramp.size());
					CHECK(glm::all(glm::lessThan(glm::abs(values->front()), glm::vec3(1e-6f))));
					CHECK(glm::all(glm::greaterThan(values->back(), glm::vec3(0.99f))));
					for (size_t index = 1; index < values->size(); ++index)
					{
						CAPTURE(index);
						CHECK(glm::all(glm::greaterThanEqual((*values)[index] + glm::vec3(1e-6f), (*values)[index - 1])));
						CHECK(glm::all(glm::lessThanEqual((*values)[index], glm::vec3(1.0f))));
					}
					midGreys.push_back((*values)[MidGrey].g);
				}
				for (size_t first = 0; first < midGreys.size(); ++first)
				{
					for (size_t second = first + 1; second < midGreys.size(); ++second)
					{
						CAPTURE(first);
						CAPTURE(second);
						CHECK(std::abs(midGreys[first] - midGreys[second]) > 0.005f);
					}
				}
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: Linear with exposure encodes exactly through the sRGB OETF" * doctest::test_suite(Test::GpuSuite))
		{
			// The walking skeleton's tonemap, which the contract moved into the pass: 2^EV, clamp, OETF, no dither.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				const std::array<glm::dvec3, 4> colors = { glm::dvec3(0.0), glm::dvec3(0.25), glm::dvec3(0.5, 0.125, 0.0), glm::dvec3(4.0) };
				TonemapSetup setup(gpu, colors, 2.0f);
				const Image image = setup.Run({ .Tonemapper = RenderTonemapper::Linear, .Dither = false, .EncodeSrgb = true });
				CHECK(GetChannel(image, 0, 0) == 0);
				CHECK(GetChannel(image, 1, 0) == 188); // 0.25 * 2 = 0.5 -> OETF 0.7354 -> 188
				CHECK(GetChannel(image, 2, 0) == 255); // 1.0
				CHECK(GetChannel(image, 2, 1) == 137); // 0.25 -> OETF 0.5371 -> 137
				CHECK(GetChannel(image, 3, 2) == 255); // clamped
				CHECK(GetChannel(image, 0, 3) == 255); // alpha 1
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: the dither stays within one LSB and depends only on the pixel" * doctest::test_suite(Test::GpuSuite))
		{
			// §8.9: blue-noise triangular dither of +-1 LSB; no temporal variation (§8.3), so two records are identical.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				const std::vector<glm::dvec3> colors(128, glm::dvec3(0.2));
				TonemapSetup setup(gpu, colors, 1.0f);
				const nvrhi::TextureHandle blueNoise = CreateBlueNoiseTexture(device);

				const TonemapSettings settings{ .Tonemapper = RenderTonemapper::Linear, .Dither = true, .EncodeSrgb = true };
				const Image first = setup.Run(settings, { .BlueNoise = blueNoise });
				const Image second = setup.Run(settings, { .BlueNoise = blueNoise });
				CHECK(first.Pixels == second.Pixels);
				// The binary16 0.2 (0.19995) encodes to 0.48445, 123.53 levels, which quantizes to 124 without the dither.
				constexpr int Undithered = 124;
				bool varies = false;
				for (uint32_t pixel = 0; pixel < 128; ++pixel)
				{
					const int value = GetChannel(first, pixel, 0);
					CHECK(std::abs(value - Undithered) <= 1);
					varies = varies || value != Undithered;
				}
				CHECK(varies);
				// Without the blue-noise texture the dither is off.
				const Image plain = setup.Run(settings);
				for (uint32_t pixel = 0; pixel < 128; ++pixel)
					CHECK(GetChannel(plain, pixel, 0) == Undithered);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: the dither leaves exact black and white unchanged" * doctest::test_suite(Test::GpuSuite))
		{
			// Cleared and clipped pixels have no banding to hide: the dither's amplitude fades out within the first and last
			// level, so 0 and 255 stay exact, while a value a few levels inside still varies.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				const std::vector<glm::dvec3> colors(128, glm::dvec3(0.0, 4.0, 0.002));
				TonemapSetup setup(gpu, colors, 1.0f);
				const nvrhi::TextureHandle blueNoise = CreateBlueNoiseTexture(device);
				const Image image = setup.Run({ .Tonemapper = RenderTonemapper::Linear, .Dither = true, .EncodeSrgb = true }, { .BlueNoise = blueNoise });
				bool bluesVary = false;
				for (uint32_t pixel = 0; pixel < 128; ++pixel)
				{
					CAPTURE(pixel);
					CHECK(GetChannel(image, pixel, 0) == 0);
					CHECK(GetChannel(image, pixel, 1) == 255);
					// 0.002 encodes to 0.02584, 6.59 levels: dithered to 6, 7 or 8.
					const int blue = GetChannel(image, pixel, 2);
					CHECK(blue >= 6);
					CHECK(blue <= 8);
					bluesVary = bluesVary || blue != 7;
				}
				CHECK(bluesVary);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: EncodeSrgb off stores the tonemapped value directly" * doctest::test_suite(Test::GpuSuite))
		{
			// The data debug views (RenderSnapshot.h): Linear, exposure 1, no OETF, so 0.25 is stored as 64.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				const std::array<glm::dvec3, 1> colors = { glm::dvec3(0.25, 0.75, 1.0) };
				TonemapSetup setup(gpu, colors, 1.0f);
				const Image image = setup.Run({ .Tonemapper = RenderTonemapper::Linear, .Dither = false, .EncodeSrgb = false });
				CHECK(GetChannel(image, 0, 0) == 64);
				CHECK(GetChannel(image, 0, 1) == 191); // 191.25 (a value off the rounding tie of 127.5, which devices round either way)
				CHECK(GetChannel(image, 0, 2) == 255);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: the bloom composite lerps towards the bloom by BloomIntensity" * doctest::test_suite(Test::GpuSuite))
		{
			// §8.3 pass 10: lerp(scene, bloom, BloomIntensity) before the exposure, with the bloom texture's mip 0 sampled at
			// full resolution; an intensity outside [0, 1] is clamped, and without a bloom texture there is no bloom whatever
			// the intensity.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				const std::vector<glm::dvec3> colors(4, glm::dvec3(0.25, 0.0, 1.0));
				TonemapSetup setup(gpu, colors, 1.0f);
				const std::vector<glm::dvec3> bloomColors(2, glm::dvec3(1.0, 0.25, 0.0));
				const nvrhi::TextureHandle bloom = CreateHalfTexture(device, ToHalfTexels(bloomColors), "TonemapPassTests.Bloom");
				const TonemapSettings settings{ .Tonemapper = RenderTonemapper::Linear, .Dither = false, .EncodeSrgb = false };

				// 0.25 * 0.75 + 1 * 0.25 = 0.4375 (111.6 levels); 0.25 * 0.25 = 0.0625 (15.9); 1 * 0.75 = 0.75 (191.25).
				const Image quarter = setup.Run(settings, { .Bloom = bloom, .BloomIntensity = 0.25f });
				for (uint32_t pixel = 0; pixel < 4; ++pixel)
				{
					CAPTURE(pixel);
					CHECK(GetChannel(quarter, pixel, 0) == 112);
					CHECK(GetChannel(quarter, pixel, 1) == 16);
					CHECK(GetChannel(quarter, pixel, 2) == 191);
				}
				const Image clamped = setup.Run(settings, { .Bloom = bloom, .BloomIntensity = 2.0f });
				CHECK(GetChannel(clamped, 0, 0) == 255);
				CHECK(GetChannel(clamped, 0, 1) == 64);
				CHECK(GetChannel(clamped, 0, 2) == 0);
				const Image none = setup.Run(settings, { .Bloom = nullptr, .BloomIntensity = 1.0f });
				CHECK(GetChannel(none, 0, 0) == 64);
				CHECK(GetChannel(none, 0, 1) == 0);
				CHECK(GetChannel(none, 0, 2) == 255);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: non-finite radiance saturates or turns black" * doctest::test_suite(Test::GpuSuite))
		{
			// A NaN or infinite SceneColor texel (a broken shader, an overflow) must not reach the tonemappers' arithmetic: +Inf is
			// clamped to the largest radiance, NaN and -Inf count as 0. With Linear that is exactly white and black; the curves
			// mix channels, so a white channel lifts the others, but never to a NaN, which a UNORM target would store as 0.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				constexpr uint16_t PositiveInfinity = 0x7C00;
				constexpr uint16_t NegativeInfinity = 0xFC00;
				constexpr uint16_t QuietNaN = 0x7E00;
				const uint16_t one = ToHalf(1.0);
				const std::array<std::array<uint16_t, 4>, 2> texels = { {
					{ PositiveInfinity, QuietNaN, NegativeInfinity, one },
					{ QuietNaN, PositiveInfinity, QuietNaN, one },
				} };
				TonemapSetup setup(gpu, texels, 1.0f);
				for (const RenderTonemapper tonemapper : Tonemappers)
				{
					CAPTURE(static_cast<int>(tonemapper));
					const Image image = setup.Run({ .Tonemapper = tonemapper, .Dither = false, .EncodeSrgb = true });
					CHECK(GetChannel(image, 0, 0) >= 250);
					CHECK(GetChannel(image, 1, 1) >= 250);
					if (tonemapper == RenderTonemapper::Linear)
					{
						CHECK(GetChannel(image, 0, 1) == 0);
						CHECK(GetChannel(image, 0, 2) == 0);
						CHECK(GetChannel(image, 1, 0) == 0);
						CHECK(GetChannel(image, 1, 2) == 0);
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
