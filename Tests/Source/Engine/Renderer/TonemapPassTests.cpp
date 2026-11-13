#include "TestsPCH.h"

#include "Engine/Renderer/TonemapPass.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/LutData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/BlueNoise.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/ViewConstants.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/RenderReference.h"
#include "Support/TonemapCurvesProgram.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

// Tonemapping and encoding (Architecture §8.9, §15.3 "tonemapper curves vs CPU at 1,024 points"; Docs/Decisions/
// 0013-m8-decisions.md decision 8). The pass tests run TonemapPass on small SceneColor rows; the curve tests run the
// TonemapCurves program (Tests/Source/Support/TonemapCurvesProgram.h), and the 1,024-point comparison checks it against
// the CPU references (Support/RenderReference.h). M26 (Docs/Decisions/0037-m26-contract.md decision 15, stream X-B): the
// grading LUT's cases compare the pass with the LUT step's CPU statement (GradeReference below) and run it in a
// SceneRenderer, its loss test among them.

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
			// M26: the grading LUT's mirror (GpuResourceCache::GetLut) and its share; null grades nothing.
			const GpuLut* Lut = nullptr;
			float LutIntensity = 1.0f;
		};

		// The LUT step's CPU statement (TonemapPass.h step 3, Shared/TonemapConstants.h) in double precision: `encoded`, a
		// display-encoded value, mapped from the table's domain to its grid (clamped), interpolated trilinearly between the
		// eight entries around it.
		glm::dvec3 SampleLutReference(const LutData& lut, const glm::dvec3& encoded)
		{
			const int last = static_cast<int>(lut.Size) - 1;
			glm::ivec3 base(0);
			glm::dvec3 fraction(0.0);
			for (glm::length_t channel = 0; channel < 3; ++channel)
			{
				const double low = lut.DomainMin[channel];
				const double high = lut.DomainMax[channel];
				const double position = std::clamp((encoded[channel] - low) / (high - low), 0.0, 1.0) * last;
				base[channel] = std::min(static_cast<int>(std::floor(position)), last - 1);
				fraction[channel] = position - base[channel];
			}
			glm::dvec3 sum(0.0);
			for (int corner = 0; corner < 8; ++corner)
			{
				const glm::ivec3 offset((corner & 1) != 0 ? 1 : 0, (corner & 2) != 0 ? 1 : 0, (corner & 4) != 0 ? 1 : 0);
				double weight = 1.0;
				for (glm::length_t channel = 0; channel < 3; ++channel)
					weight *= offset[channel] != 0 ? fraction[channel] : 1.0 - fraction[channel];
				const glm::ivec3 entry = base + offset;
				const size_t index = static_cast<size_t>(entry.x) + lut.Size * (static_cast<size_t>(entry.y) + lut.Size * static_cast<size_t>(entry.z));
				sum += weight * glm::dvec3(lut.Values[index]);
			}
			return sum;
		}

		// The pass's encoded output level (0 to 255) of a display-linear value graded by `lut` with `intensity`: the OETF,
		// the table, the EOTF of its clamped output, the blend, and the OETF of the result (no dither).
		glm::ivec3 GradeReference(const LutData& lut, const glm::dvec3& display, double intensity)
		{
			glm::dvec3 encoded(0.0);
			for (glm::length_t channel = 0; channel < 3; ++channel)
				encoded[channel] = Test::LinearToSrgb(display[channel]);
			const glm::dvec3 graded = SampleLutReference(lut, encoded);
			glm::ivec3 levels(0);
			for (glm::length_t channel = 0; channel < 3; ++channel)
			{
				const double decoded = Test::SrgbToLinear(std::clamp(graded[channel], 0.0, 1.0));
				const double blended = display[channel] + (decoded - display[channel]) * intensity;
				levels[channel] = static_cast<int>(std::lround(Test::LinearToSrgb(std::clamp(blended, 0.0, 1.0)) * 255.0));
			}
			return levels;
		}

		// A table of `size` whose entry at the grid coordinates (x, y, z) in [0, 1] is `entry(x, y, z)`, over `domainMin` to
		// `domainMax`.
		LutData MakeLut(uint32_t size, const std::function<glm::vec3(double, double, double)>& entry, glm::vec3 domainMin = glm::vec3(0.0f),
			glm::vec3 domainMax = glm::vec3(1.0f))
		{
			LutData lut;
			lut.Size = size;
			lut.DomainMin = domainMin;
			lut.DomainMax = domainMax;
			const double last = size - 1;
			for (uint32_t blue = 0; blue < size; ++blue)
			{
				for (uint32_t green = 0; green < size; ++green)
				{
					for (uint32_t red = 0; red < size; ++red)
						lut.Values.push_back(entry(red / last, green / last, blue / last));
				}
			}
			REQUIRE(ValidateLutData(lut).has_value());
			return lut;
		}

		// Swaps red and blue: a grade no rounding can mistake for the identity.
		LutData MakeSwapLut()
		{
			return MakeLut(17, [](double x, double y, double z)
			{
				return glm::vec3(glm::dvec3(z, y, x));
			});
		}

		// The SceneColor texels of `count` colours spread over [0, 1.25] in each channel (each channel a different stride),
		// as binary16 values, and the display-linear values the Linear tonemapper makes of them at exposure 1.
		struct LutColors
		{
			std::vector<glm::dvec3> Colors{};
			std::vector<glm::dvec3> Display{};
		};

		LutColors MakeLutColors(uint32_t count)
		{
			LutColors colors;
			for (uint32_t index = 0; index < count; ++index)
			{
				const glm::dvec3 color((index % 16) / 12.0, ((index * 7) % 23) / 18.0, ((index * 5) % 31) / 24.0);
				colors.Colors.push_back(color);
				glm::dvec3 display(0.0);
				for (glm::length_t channel = 0; channel < 3; ++channel)
					display[channel] = std::clamp(static_cast<double>(glm::unpackHalf1x16(ToHalf(color[channel]))), 0.0, 1.0);
				colors.Display.push_back(display);
			}
			return colors;
		}

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
				TonemapPassInputs passInputs{ .ViewConstants = m_ViewBuffer, .SceneColor = m_SceneColor, .Bloom = inputs.Bloom, .BloomIntensity = inputs.BloomIntensity, .BlueNoise = inputs.BlueNoise, .LdrColor = m_LdrColor, .Settings = settings };
				if (inputs.Lut != nullptr)
				{
					passInputs.Lut = inputs.Lut->Texture;
					passInputs.LutDomainMin = inputs.Lut->DomainMin;
					passInputs.LutDomainMax = inputs.Lut->DomainMax;
					passInputs.LutIntensity = inputs.LutIntensity;
				}
				const Status recorded = m_Pass->Record(**commandList, bindings, passInputs);
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

		// A SceneRenderer snapshot of the fixture's view without meshes, cleared to linear (0.25, 0, 1), which the Linear
		// tonemapper at exposure 1 encodes as (137, 0, 255), graded by `lut` when it is set.
		RenderSnapshot MakeClearSnapshot(const Test::SceneRendererIntegrationFixture& setup, AssetHandle lut)
		{
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes.clear();
			snapshot.PickTable.clear();
			snapshot.Camera.ClearColor = glm::vec3(0.25f, 0.0f, 1.0f);
			snapshot.Post.Lut = lut;
			snapshot.Post.LutIntensity = 1.0f;
			return snapshot;
		}

		// The view's centre pixel.
		glm::ivec3 GetCentre(const Image& image)
		{
			return { Test::SceneRendererIntegrationFixture::Channel(image, 32, 32, 0), Test::SceneRendererIntegrationFixture::Channel(image, 32, 32, 1),
				Test::SceneRendererIntegrationFixture::Channel(image, 32, 32, 2) };
		}

		// Whether every channel of `actual` is within one level of `expected`.
		bool IsWithinOneLevel(const glm::ivec3& actual, const glm::ivec3& expected)
		{
			return glm::all(glm::lessThanEqual(glm::abs(actual - expected), glm::ivec3(1)));
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

		TEST_CASE("TonemapPass: a LUT grades the tonemapped value in its display-encoded domain, LutIntensity its share"
			* doctest::test_suite(Test::GpuSuite))
		{
			// Docs/Decisions/0037-m26-contract.md decision 15: after the curve, the display-linear value is encoded with the
			// OETF, mapped from the table's domain to its grid (clamped outside it), sampled trilinearly, decoded with the EOTF
			// and blended with the ungraded value by LutIntensity, then encoded. Each pixel equals the CPU statement within one
			// level (the trilinear weights' and binary16 entries' rounding). Tables: a red-blue swap over [0, 1], and a curved
			// table over a domain that is not [0, 1] in any channel, so values outside it clamp.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				const LutColors colors = MakeLutColors(96);
				TonemapSetup setup(gpu, colors.Colors, 1.0f);
				Test::InMemoryAssetManager assets;
				GpuResourceCache cache(device, assets);
				const AssetHandle swapHandle(0x1D71);
				const AssetHandle curvedHandle(0x1D72);
				const LutData swap = MakeSwapLut();
				const LutData curved = MakeLut(9, [](double x, double y, double z)
				{
					return glm::vec3(glm::dvec3(x * x, (y + z) * 0.5, 1.0 - x));
				}, glm::vec3(0.0f, 0.1f, 0.2f), glm::vec3(0.5f, 0.9f, 1.0f));
				assets.Publish(swapHandle, CreateRef<LutData>(swap));
				assets.Publish(curvedHandle, CreateRef<LutData>(curved));
				// Both mirrors uploaded first: the pointers GetLut returns below then stay valid (no later call uploads).
				REQUIRE(cache.GetLut(swapHandle) != nullptr);
				REQUIRE(cache.GetLut(curvedHandle) != nullptr);
				const TonemapSettings settings{ .Tonemapper = RenderTonemapper::Linear, .Dither = false, .EncodeSrgb = true };
				const Image ungraded = setup.Run(settings);
				struct Grade
				{
					const LutData* Lut = nullptr;
					const GpuLut* Mirror = nullptr;
					float Intensity = 1.0f;
				};
				const std::array<Grade, 4> grades = { {
					{ .Lut = &swap, .Mirror = cache.GetLut(swapHandle), .Intensity = 1.0f },
					{ .Lut = &swap, .Mirror = cache.GetLut(swapHandle), .Intensity = 0.5f },
					{ .Lut = &curved, .Mirror = cache.GetLut(curvedHandle), .Intensity = 1.0f },
					{ .Lut = &curved, .Mirror = cache.GetLut(curvedHandle), .Intensity = 0.25f },
				} };
				for (const Grade& grade : grades)
				{
					CAPTURE(grade.Lut->Size);
					CAPTURE(grade.Intensity);
					REQUIRE(grade.Mirror != nullptr);
					const Image image = setup.Run(settings, { .Lut = grade.Mirror, .LutIntensity = grade.Intensity });
					size_t differing = 0;
					for (uint32_t pixel = 0; pixel < colors.Display.size(); ++pixel)
					{
						CAPTURE(pixel);
						const glm::ivec3 expected = GradeReference(*grade.Lut, colors.Display[pixel], grade.Intensity);
						const glm::ivec3 actual(GetChannel(image, pixel, 0), GetChannel(image, pixel, 1), GetChannel(image, pixel, 2));
						CHECK(IsWithinOneLevel(actual, expected));
						const glm::ivec3 plain(GetChannel(ungraded, pixel, 0), GetChannel(ungraded, pixel, 1), GetChannel(ungraded, pixel, 2));
						differing += plain != actual ? 1 : 0;
						CHECK(GetChannel(image, pixel, 3) == 255);
					}
					// Every grade changes most of the image: the step ran.
					CHECK(differing > colors.Display.size() / 2);
				}
				// A share of 0 leaves the image exactly as the record without a LUT, and a share outside [0, 1] is clamped.
				CHECK(setup.Run(settings, { .Lut = cache.GetLut(swapHandle), .LutIntensity = 0.0f }).Pixels == ungraded.Pixels);
				CHECK(setup.Run(settings, { .Lut = cache.GetLut(swapHandle), .LutIntensity = 4.0f }).Pixels
					== setup.Run(settings, { .Lut = cache.GetLut(swapHandle), .LutIntensity = 1.0f }).Pixels);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: the identity LUT leaves every value within one level of the ungraded image" * doctest::test_suite(Test::GpuSuite))
		{
			// decision 15: an identity LUT changes an image only by the sampling's and the binary16 storage's rounding; the
			// built-in engine://Luts/Identity (33^3) over a ramp of every channel.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				std::vector<glm::dvec3> colors;
				for (uint32_t index = 0; index < 256; ++index)
					colors.emplace_back(index / 255.0, ((index * 3) % 256) / 255.0, ((index * 11) % 256) / 255.0);
				TonemapSetup setup(gpu, colors, 1.0f);
				Test::InMemoryAssetManager assets;
				GpuResourceCache cache(device, assets);
				const GpuLut* identity = cache.GetLut(BuiltinAssetHandles::IdentityLut);
				REQUIRE(identity != nullptr);
				const TonemapSettings settings{ .Tonemapper = RenderTonemapper::Linear, .Dither = false, .EncodeSrgb = true };
				const Image ungraded = setup.Run(settings);
				const Image graded = setup.Run(settings, { .Lut = identity, .LutIntensity = 1.0f });
				REQUIRE(graded.Pixels.size() == ungraded.Pixels.size());
				for (size_t index = 0; index < graded.Pixels.size(); ++index)
				{
					CAPTURE(index);
					CHECK(std::abs(std::to_integer<int>(graded.Pixels[index]) - std::to_integer<int>(ungraded.Pixels[index])) <= 1);
				}
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: a scene's grading LUT grades the Lit view, and neither the 2D colour path nor a LUT that does not load"
			* doctest::test_suite(Test::GpuSuite))
		{
			// decisions 10 and 15: the SceneRenderer passes PostProcessSettings::Lut to the pass where the tonemapper follows
			// Post; in pixel-art mode no LUT grades the image (the 2D colour path delivers its texels unchanged), and a LUT
			// that cannot be loaded grades nothing, the cache reporting why once.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				const AssetHandle swap(0x1D73);
				setup.Assets().Publish(swap, CreateRef<LutData>(MakeSwapLut()));
				const glm::ivec3 plain = GetCentre(setup.Render(MakeClearSnapshot(setup, AssetHandle())));
				CHECK(plain == glm::ivec3(137, 0, 255));
				const glm::ivec3 graded = GetCentre(setup.Render(MakeClearSnapshot(setup, swap)));
				CHECK(IsWithinOneLevel(graded, glm::ivec3(255, 0, 137)));

				RenderSnapshot pixelArt = MakeClearSnapshot(setup, swap);
				pixelArt.Output.PixelArt = true;
				CHECK(GetCentre(setup.Render(pixelArt)) == plain);

				Test::ExpectLog missing(LogLevel::Error, std::string(AssetMissingCode));
				CHECK(GetCentre(setup.Render(MakeClearSnapshot(setup, AssetHandle(0x1D7F)))) == plain);
				CHECK(GetCentre(setup.Render(MakeClearSnapshot(setup, AssetHandle(0x1D7F)))) == plain);
				CHECK(missing.GetMatchCount() == 1);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: a renderer grading with a LUT recovers from a device loss with the same image" * doctest::test_suite(Test::GpuSuite))
		{
			// The recreation protocol (Docs/Decisions/0037-m26-contract.md decision 29, place 3): the LUT's mirror is the cache's,
			// which the loss drops and the next GetLut uploads again; the pass's own stand-in 3D texture is the pipeline set's
			// (place 1).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			const AssetHandle swap(0x1D74);
			setup.Assets().Publish(swap, CreateRef<LutData>(MakeSwapLut()));
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 50;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			// One use renders the snapshot into each of the renderer's frames in flight, so its profiler holds the timer queries
			// of every frame slot before the loss as after the recovery.
			test.Use = [&device, &setup, &frame, swap]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					RenderSnapshot snapshot = MakeClearSnapshot(setup, swap);
					snapshot.FrameIndex = ++frame;
					ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
					list->open();
					const Status rendered = setup.Renderer().Render(*list, snapshot);
					list->close();
					setup.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
					ENGINE_TRY(rendered);
					nvrhi::ITexture* image = setup.Renderer().GetFinalTexture();
					if (image == nullptr)
						return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
					Readback readback(device);
					ENGINE_TRY_ASSIGN(Image read, readback.ReadTexture(*image));
					pixels = std::move(read.Pixels);
				}
				return pixels;
			};
			// The renderer reaches its steady state first, so the live counts before the loss are the ones the recovery
			// reproduces.
			for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
			{
				const Result<std::vector<std::byte>> steady = test.Use();
				REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
			}
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			// The image was graded: the centre's red and blue swapped.
			REQUIRE(report->Before.size() == 64 * 64 * 4);
			const size_t centre = (32 * 64 + 32) * 4;
			CHECK(std::to_integer<int>(report->Before[centre]) >= 254);
			CHECK(std::abs(std::to_integer<int>(report->Before[centre + 2]) - 137) <= 1);
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
