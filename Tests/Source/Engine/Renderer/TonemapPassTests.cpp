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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

// Tonemapping and encoding (Architecture §8.9, §15.3 "tonemapper curves vs CPU at 1,024 points"). Skeletons of the M8
// contract (Docs/Decisions/0013-m8-decisions.md decision 8); stream C implements TonemapPass and the TonemapCurves program
// (Tests/Source/Support/TonemapCurvesProgram.h) and removes the skips; the comparison needs stream E's references.

namespace Engine {

	namespace {

		// A SceneColor of `width` x 1 RGBA16F texels holding `colors`, an LdrColor of the same size and a ViewConstants buffer
		// with `exposure`, on the fixture's device; Run records the pass with `settings` and reads LdrColor back.
		class TonemapSetup
		{
		public:
			TonemapSetup(Test::HeadlessGpuFixture& gpu, std::span<const glm::dvec3> colors, float exposure)
				: m_Device(gpu.GetDevice()), m_Width(static_cast<uint32_t>(colors.size()))
			{
				Result<Scope<TonemapPass>> pass = TonemapPass::Create(m_Device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				m_Pass = std::move(*pass);

				std::vector<std::array<uint16_t, 4>> texels;
				for (const glm::dvec3& color : colors)
					texels.push_back({ Test::DoubleToHalf(color.r), Test::DoubleToHalf(color.g), Test::DoubleToHalf(color.b), Test::DoubleToHalf(1.0) });
				nvrhi::TextureDesc sceneDesc;
				sceneDesc.width = m_Width;
				sceneDesc.height = 1;
				sceneDesc.format = SceneColorFormat;
				sceneDesc.isShaderResource = true;
				sceneDesc.initialState = nvrhi::ResourceStates::ShaderResource;
				sceneDesc.keepInitialState = true;
				sceneDesc.debugName = "TonemapPassTests.SceneColor";
				const std::array<TextureSubresourceData, 1> subresources = { { { .Data = std::as_bytes(std::span(texels)) } } };
				Result<TextureUpload> scene = m_Device.GetHostImageUpload().CreateTexture(sceneDesc, subresources, TextureUploadPath::Staging);
				REQUIRE_MESSAGE(scene.has_value(), scene.error().ToString());
				m_SceneColor = scene->Texture;

				nvrhi::TextureDesc ldrDesc = sceneDesc;
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

			Image Run(const TonemapSettings& settings, nvrhi::ITexture* blueNoise = nullptr)
			{
				Result<nvrhi::CommandListHandle> commandList = m_Device.CreateCommandList();
				REQUIRE(commandList.has_value());
				(*commandList)->open();
				(*commandList)->writeBuffer(m_ViewBuffer, &m_View, sizeof(m_View));
				PassBindingCache bindings;
				const Status recorded = m_Pass->Record(**commandList, bindings, { .ViewConstants = m_ViewBuffer, .SceneColor = m_SceneColor, .Bloom = nullptr, .BloomIntensity = 0.0f, .BlueNoise = blueNoise, .LdrColor = m_LdrColor, .Settings = settings });
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

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("TonemapPass: every tonemapper matches its CPU reference at 1,024 points" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// §8.9: "CPU reference implementations of every tonemapper are compared with the GPU at 1,024 sample points". The
			// TonemapCurves program evaluates the same Slang functions as the pass in 32-bit float (no 8-bit quantization); the
			// result must match the double-precision references within 1e-3 (display-linear, before the OETF).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				const std::vector<glm::dvec3> points = Test::MakeTonemapSamplePoints();
				REQUIRE(points.size() == 1024);
				for (const RenderTonemapper tonemapper : { RenderTonemapper::AgX, RenderTonemapper::Aces, RenderTonemapper::PbrNeutral, RenderTonemapper::Linear })
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

		TEST_CASE("TonemapPass: Linear with exposure encodes exactly through the sRGB OETF" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// The walking skeleton's tonemap, which the contract moved into the pass (the scene renderer's M7 tests cover it
			// until then): 2^EV, clamp, OETF, no dither. The setup's binary16 upload needs stream E's Test::DoubleToHalf.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				const std::array<glm::dvec3, 4> colors = { glm::dvec3(0.0), glm::dvec3(0.25), glm::dvec3(0.5, 0.125, 0.0), glm::dvec3(4.0) };
				TonemapSetup setup(gpu, colors, 2.0f);
				const Image image = setup.Run({ .Tonemapper = RenderTonemapper::Linear, .Dither = false, .EncodeSrgb = true });
				const std::span<const std::byte> row = image.GetRow(0);
				const auto channel = [&row](uint32_t pixel, uint32_t index)
				{
					return std::to_integer<int>(row[static_cast<size_t>(pixel) * 4 + index]);
				};
				CHECK(channel(0, 0) == 0);
				CHECK(channel(1, 0) == 188); // 0.25 * 2 = 0.5 -> OETF 0.7354 -> 188
				CHECK(channel(2, 0) == 255); // 1.0
				CHECK(channel(2, 1) == 137); // 0.25 -> OETF 0.5371 -> 137
				CHECK(channel(3, 2) == 255); // clamped
				CHECK(channel(0, 3) == 255); // alpha 1
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: the dither stays within one LSB and depends only on the pixel" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// §8.9: blue-noise triangular dither of +-1 LSB; no temporal variation (§8.3), so two records are identical.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				const std::vector<glm::dvec3> colors(128, glm::dvec3(0.2));
				TonemapSetup setup(gpu, colors, 1.0f);
				const std::vector<uint8_t> noise = GenerateBlueNoise();
				nvrhi::TextureDesc noiseDesc;
				noiseDesc.width = BlueNoiseSize;
				noiseDesc.height = BlueNoiseSize;
				noiseDesc.format = nvrhi::Format::R8_UNORM;
				noiseDesc.isShaderResource = true;
				noiseDesc.initialState = nvrhi::ResourceStates::ShaderResource;
				noiseDesc.keepInitialState = true;
				noiseDesc.debugName = "TonemapPassTests.BlueNoise";
				const std::array<TextureSubresourceData, 1> subresources = { { { .Data = std::as_bytes(std::span(noise)) } } };
				Result<TextureUpload> blueNoise = device.GetHostImageUpload().CreateTexture(noiseDesc, subresources, TextureUploadPath::Staging);
				REQUIRE_MESSAGE(blueNoise.has_value(), blueNoise.error().ToString());

				const TonemapSettings settings{ .Tonemapper = RenderTonemapper::Linear, .Dither = true, .EncodeSrgb = true };
				const Image first = setup.Run(settings, blueNoise->Texture);
				const Image second = setup.Run(settings, blueNoise->Texture);
				CHECK(first.Pixels == second.Pixels);
				const int undithered = static_cast<int>(std::lround(Test::LinearToSrgb(0.2) * 255.0));
				bool varies = false;
				for (uint32_t pixel = 0; pixel < 128; ++pixel)
				{
					const int value = std::to_integer<int>(first.GetRow(0)[static_cast<size_t>(pixel) * 4]);
					CHECK(std::abs(value - undithered) <= 1);
					varies = varies || value != undithered;
				}
				CHECK(varies);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("TonemapPass: EncodeSrgb off stores the tonemapped value directly" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// The data debug views (RenderSnapshot.h): Linear, exposure 1, no OETF, so 0.25 is stored as 64.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				const std::array<glm::dvec3, 1> colors = { glm::dvec3(0.25, 0.5, 1.0) };
				TonemapSetup setup(gpu, colors, 1.0f);
				const Image image = setup.Run({ .Tonemapper = RenderTonemapper::Linear, .Dither = false, .EncodeSrgb = false });
				CHECK(std::to_integer<int>(image.GetRow(0)[0]) == 64);
				CHECK(std::to_integer<int>(image.GetRow(0)[1]) == 128);
				CHECK(std::to_integer<int>(image.GetRow(0)[2]) == 255);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
