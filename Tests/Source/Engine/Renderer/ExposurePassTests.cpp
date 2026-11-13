#include "TestsPCH.h"

#include "Engine/Renderer/ExposurePass.h"

#include "Engine/Core/Random.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/AutoExposure.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderPassDeclaration.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/TonemapPass.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Shared/ExposureConstants.h"
#include "Shared/ViewConstants.h"
#include "Support/AssetTestFixture.h"
#include "Support/BufferReadback.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Histogram auto-exposure's GPU half (M26, C26.1, R-AEX; Docs/Decisions/0037-m26-contract.md decisions 10, 15 and 29, stream
// X-A): the Exposure pass against AutoExposure.h's CPU statement, the Tonemap pass's use of the adapted exposure, the scene
// renderer's temporal rule (a reset converges at once, a continued history adapts), the byte-identical captures of the
// acceptance and the pass's loss test.

namespace Engine {

	namespace {

		using HalfTexel = std::array<uint16_t, 4>;

		// The binary16 bits of `value` (round to nearest even).
		uint16_t ToHalf(float value)
		{
			return glm::packHalf1x16(value);
		}

		// The float values the GPU reads from RGBA16F `texels`.
		std::vector<glm::vec4> ToFloats(std::span<const HalfTexel> texels)
		{
			std::vector<glm::vec4> pixels;
			pixels.reserve(texels.size());
			for (const HalfTexel& texel : texels)
				pixels.emplace_back(glm::unpackHalf1x16(texel[0]), glm::unpackHalf1x16(texel[1]), glm::unpackHalf1x16(texel[2]), glm::unpackHalf1x16(texel[3]));
			return pixels;
		}

		// A shader-readable RGBA16F texture of `width` x `height` texels, rows top first.
		nvrhi::TextureHandle CreateHalfImage(GraphicsDevice& device, uint32_t width, uint32_t height, std::span<const HalfTexel> texels, const char* name)
		{
			REQUIRE(texels.size() == static_cast<size_t>(width) * height);
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
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

		// A uniform RGBA16F image of `radiance` with alpha 1.
		std::vector<HalfTexel> MakeUniformTexels(uint32_t width, uint32_t height, float radiance)
		{
			const uint16_t half = ToHalf(radiance);
			return std::vector<HalfTexel>(static_cast<size_t>(width) * height, HalfTexel{ half, half, half, ToHalf(1.0f) });
		}

		// The pass with its histogram and state buffers on the fixture's device; Run records and executes one pass.
		class ExposureSetup
		{
		public:
			explicit ExposureSetup(Test::HeadlessGpuFixture& gpu)
				: m_Device(gpu.GetDevice())
			{
				Result<Scope<ExposurePass>> pass = ExposurePass::Create(m_Device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				m_Pass = std::move(*pass);
				CHECK(m_Pass->GetPipelineCount() == ExposurePass::PipelineCount);
				Result<nvrhi::BufferHandle> histogram = m_Device.CreateBuffer(ExposurePass::GetHistogramDesc());
				REQUIRE(histogram.has_value());
				m_Histogram = *histogram;
				Result<nvrhi::BufferHandle> state = m_Device.CreateBuffer(ExposurePass::GetStateDesc());
				REQUIRE(state.has_value());
				m_State = *state;
			}

			~ExposureSetup() { m_Device.WaitForIdle(); }
			ExposureSetup(const ExposureSetup&) = delete;
			ExposureSetup& operator=(const ExposureSetup&) = delete;

			// One pass over `sceneColor` with `inputs`' settings and bloom, into the setup's buffers, executed and waited for.
			void Run(nvrhi::ITexture& sceneColor, ExposurePassInputs inputs)
			{
				inputs.SceneColor = &sceneColor;
				inputs.Histogram = m_Histogram;
				inputs.State = m_State;
				Result<nvrhi::CommandListHandle> list = m_Device.CreateCommandList();
				REQUIRE(list.has_value());
				(*list)->open();
				RenderPassCounters counters;
				const Status recorded = m_Pass->Record(**list, m_Bindings, inputs, counters);
				(*list)->close();
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				CHECK(counters.Dispatches == 2);
				m_Device.ExecuteCommandList(**list);
				m_Device.WaitForIdle();
			}

			[[nodiscard]] ExposureHistogram ReadHistogram()
			{
				const Result<std::vector<std::byte>> bytes = Test::ReadBufferBytes(m_Device, *m_Histogram, sizeof(ExposureHistogram));
				REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
				ExposureHistogram histogram{};
				std::memcpy(histogram.data(), bytes->data(), sizeof(histogram));
				return histogram;
			}

			[[nodiscard]] ExposureState ReadState()
			{
				const Result<std::vector<std::byte>> bytes = Test::ReadBufferBytes(m_Device, *m_State, sizeof(ExposureState));
				REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
				ExposureState state;
				std::memcpy(&state, bytes->data(), sizeof(state));
				return state;
			}

			[[nodiscard]] ExposurePass& GetPass() { return *m_Pass; }
			[[nodiscard]] nvrhi::IBuffer* GetHistogram() { return m_Histogram; }
			[[nodiscard]] nvrhi::IBuffer* GetState() { return m_State; }
		private:
			GraphicsDevice& m_Device;
			Scope<ExposurePass> m_Pass;
			nvrhi::BufferHandle m_Histogram;
			nvrhi::BufferHandle m_State;
			PassBindingCache m_Bindings;
		};

		// A view of `width` x `height` with no mesh, its camera cleared to `radiance` (no skybox), with auto-exposure and the
		// Linear tonemapper, so its image is uniform and its encoded value follows the exposure alone.
		RenderSnapshot MakeUniformSnapshot(uint32_t width, uint32_t height, float radiance)
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.ViewportWidth = width;
			snapshot.Camera.ViewportHeight = height;
			snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 100.0f, width, height);
			snapshot.Camera.ClearColor = glm::vec3(radiance);
			snapshot.Camera.ClearToSkybox = false;
			snapshot.Post.ExposureMode = RenderExposureMode::Auto;
			// Down to -10 EV, so the brightest scene of the case (30, about 7.2 stops above middle grey) is not clamped.
			snapshot.Post.AutoExposureMinEV = -10.0f;
			snapshot.Post.Tonemap = RenderTonemapper::Linear;
			snapshot.Post.SsaoEnabled = false;
			return snapshot;
		}

		// The fixture's 64 x 64 view made uniform: no mesh, the camera cleared to `radiance`, auto-exposure down to -10 EV.
		RenderSnapshot MakeFixtureSnapshot(const Test::SceneRendererIntegrationFixture& setup, float radiance)
		{
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes.clear();
			snapshot.Camera.ClearColor = glm::vec3(radiance);
			snapshot.Post.ExposureMode = RenderExposureMode::Auto;
			snapshot.Post.AutoExposureMinEV = -10.0f;
			return snapshot;
		}

		// `snapshot` continuing its view's history: the previous frame's camera is its own, `deltaTime` seconds ago.
		RenderSnapshot Continued(RenderSnapshot snapshot, float deltaTime)
		{
			snapshot.Camera.PreviousViewProjection = snapshot.Camera.Projection * snapshot.Camera.View;
			snapshot.Camera.DeltaTime = deltaTime;
			return snapshot;
		}

		// The encoded value (0 to 255) of a uniform radiance `radiance` (stored as binary16 in SceneColor) under auto-exposure
		// with `post`, the Linear tonemapper and the sRGB OETF, as the CPU statement predicts it.
		int PredictEncodedValue(float radiance, const PostProcessSettings& post)
		{
			const float stored = glm::unpackHalf1x16(ToHalf(radiance));
			const std::vector<glm::vec4> image(16, glm::vec4(glm::vec3(stored), 1.0f));
			const float target = ComputeExposureTarget(ComputeLuminanceHistogram(image), post);
			const double exposed = static_cast<double>(stored) * std::exp2(static_cast<double>(target) + static_cast<double>(post.ExposureEV));
			return static_cast<int>(std::lround(255.0 * Test::LinearToSrgb(std::clamp(exposed, 0.0, 1.0))));
		}

		int GetChannel(const Image& image, uint32_t x, uint32_t y, size_t channel)
		{
			return std::to_integer<int>(image.GetRow(y)[4 * static_cast<size_t>(x) + channel]);
		}

	}

	TEST_SUITE("Renderer")
	{
		// The Exposure declaration holds the pass's two pipelines of its program, and the buffers' descriptions hold one count
		// per bin and one adapted exposure.
		TEST_CASE("ExposurePass: its declaration holds its two pipelines of the program Exposure, and its buffers their elements")
		{
			const auto exposure = std::find_if(ScenePassDeclarations.begin(), ScenePassDeclarations.end(), [](const RenderPassDeclaration& declaration)
			{
				return declaration.Name == "Exposure";
			});
			REQUIRE(exposure != ScenePassDeclarations.end());
			CHECK(exposure->Stage == RenderStage::HdrPost);
			CHECK(exposure->PipelineCount == ExposurePass::PipelineCount);
			CHECK(ExposurePass::PipelineCount == 2);
			REQUIRE(exposure->Programs.size() == 1);
			CHECK(exposure->Programs[0] == "Exposure");
			CHECK(exposure->Targets.empty());
			CHECK(ExposurePass::GetLayoutDescriptions().size() == ExposurePass::PipelineCount);
			const nvrhi::BufferDesc histogram = ExposurePass::GetHistogramDesc();
			CHECK(histogram.byteSize == ExposureHistogramBins * sizeof(uint32_t));
			CHECK(histogram.structStride == sizeof(uint32_t));
			CHECK(histogram.canHaveUAVs);
			const nvrhi::BufferDesc state = ExposurePass::GetStateDesc();
			CHECK(state.byteSize == sizeof(ExposureState));
			CHECK(state.structStride == sizeof(ExposureState));
			CHECK(state.canHaveUAVs);
		}

		// The push constants carry the image's size, the reset and bloom flags, and the range and speeds made usable.
		TEST_CASE("ExposurePass: the push constants carry the image, the flags and the usable range and speeds")
		{
			ExposurePassInputs inputs;
			inputs.MinEV = -3.0f;
			inputs.MaxEV = 5.0f;
			inputs.SpeedUp = 2.0f;
			inputs.SpeedDown = 0.5f;
			inputs.DeltaTime = 0.25f;
			inputs.Reset = false;
			const ExposureConstants plain = MakeExposureConstants(inputs, 40, 20);
			CHECK(plain.ImageSize == glm::uvec2(40, 20));
			CHECK(plain.InverseImageSize == glm::vec2(1.0f / 40.0f, 1.0f / 20.0f));
			CHECK(plain.Flags == 0);
			CHECK(plain.BloomIntensity == 0.0f);
			CHECK(plain.MinEV == -3.0f);
			CHECK(plain.MaxEV == 5.0f);
			CHECK(plain.SpeedUp == 2.0f);
			CHECK(plain.SpeedDown == 0.5f);
			CHECK(plain.DeltaTime == 0.25f);

			// Without a bloom chain nothing is composited, whatever the intensity (the GPU case composites one).
			inputs.BloomIntensity = 0.5f;
			inputs.Reset = true;
			const ExposureConstants reset = MakeExposureConstants(inputs, 40, 20);
			CHECK(reset.Flags == ExposureFlagReset);
			CHECK(reset.BloomIntensity == 0.0f);

			// Non-finite bounds take the defaults; negative or non-finite speeds and delta times are 0.
			inputs.MinEV = std::nanf("");
			inputs.MaxEV = std::numeric_limits<float>::infinity();
			inputs.SpeedUp = -1.0f;
			inputs.SpeedDown = std::nanf("");
			inputs.DeltaTime = -0.5f;
			const ExposureConstants sanitized = MakeExposureConstants(inputs, 40, 20);
			CHECK(sanitized.MinEV == PostProcessSettings{}.AutoExposureMinEV);
			CHECK(sanitized.MaxEV == PostProcessSettings{}.AutoExposureMaxEV);
			CHECK(sanitized.SpeedUp == 0.0f);
			CHECK(sanitized.SpeedDown == 0.0f);
			CHECK(sanitized.DeltaTime == 0.0f);
		}

		// The acceptance (decision 15): every bin of the GPU histogram equals the CPU's ComputeLuminanceHistogram of the same
		// texels, over special values (black, negative zero and luminance, NaN, infinities, the largest and smallest binary16
		// values), the halves next to every bin edge in each channel and in grey, and seeded random radiance over 32 stops;
		// with a uniform bloom chain composited at full intensity, every finite pixel meters the bloom's luminance.
		TEST_CASE("ExposurePass: the luminance histogram equals the CPU bins" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				constexpr uint32_t Width = 200;
				constexpr uint32_t Height = 61;
				const uint16_t one = ToHalf(1.0f);
				std::vector<HalfTexel> texels;
				const auto addBits = [&texels, one](uint16_t red, uint16_t green, uint16_t blue)
				{
					texels.push_back({ red, green, blue, one });
				};
				addBits(0x0000, 0x0000, 0x0000);  // black
				addBits(0x8000, 0x8000, 0x8000);  // negative zero
				addBits(0x7E00, one, one);        // NaN
				addBits(0x7C00, one, one);        // +Inf
				addBits(one, 0xFC00, one);        // -Inf
				addBits(0x7BFF, 0x7BFF, 0x7BFF);  // the largest binary16 value
				addBits(0x0400, 0x0400, 0x0400);  // the smallest normal
				addBits(0x0001, 0x0001, 0x0001);  // the smallest subnormal
				addBits(ToHalf(-2.0f), one, one); // a negative luminance
				// The halves around every edge, as grey and in each channel alone (luminance = weight * channel).
				const std::array<float, 4> scales = { 1.0f, 1.0f / std::bit_cast<float>(ExposureLuminanceWeightBits[0]),
					1.0f / std::bit_cast<float>(ExposureLuminanceWeightBits[1]), 1.0f / std::bit_cast<float>(ExposureLuminanceWeightBits[2]) };
				for (uint32_t bin = 1; bin < ExposureHistogramBins; ++bin)
				{
					const float edge = std::bit_cast<float>(ExposureHistogramEdgeBits[bin]);
					for (size_t channel = 0; channel < scales.size(); ++channel)
					{
						if (edge * scales[channel] > 60000.0f)
							continue;
						const int centre = ToHalf(edge * scales[channel]);
						for (int offset = -2; offset <= 2; ++offset)
						{
							const auto bits = static_cast<uint16_t>(centre + offset);
							if (channel == 0)
								addBits(bits, bits, bits);
							else
								addBits(channel == 1 ? bits : uint16_t{ 0 }, channel == 2 ? bits : uint16_t{ 0 }, channel == 3 ? bits : uint16_t{ 0 });
						}
					}
				}
				REQUIRE(texels.size() < static_cast<size_t>(Width) * Height);
				Random random(2601);
				while (texels.size() < static_cast<size_t>(Width) * Height)
				{
					HalfTexel texel{ 0, 0, 0, one };
					for (size_t channel = 0; channel < 3; ++channel)
					{
						if (!random.NextBool(0.1))
							texel[channel] = ToHalf(static_cast<float>(std::exp2(random.RangeDouble(-16.0, 15.9))));
					}
					texels.push_back(texel);
				}
				const nvrhi::TextureHandle image = CreateHalfImage(device, Width, Height, texels, "ExposurePassTests.SceneColor");

				ExposureSetup setup(gpu);
				setup.Run(*image, { .Reset = true });
				const ExposureHistogram expected = ComputeLuminanceHistogram(ToFloats(texels));
				const ExposureHistogram histogram = setup.ReadHistogram();
				uint32_t filled = 0;
				for (uint32_t bin = 0; bin < ExposureHistogramBins; ++bin)
				{
					CAPTURE(bin);
					CHECK(histogram[bin] == expected[bin]);
					filled += expected[bin] != 0 ? 1U : 0U;
				}
				// The image spans the bins binary16 reaches: luminance up to 2^16, 56 of the 64 bins.
				CHECK(filled >= 50);
				CHECK(std::accumulate(histogram.begin(), histogram.end(), uint64_t{ 0 }) == uint64_t{ Width } * Height);

				// A uniform bloom chain of 3 at full intensity: every pixel with only finite channels meters 3's bin, the others
				// stay non-finite (bin 0).
				const nvrhi::TextureHandle bloom = CreateHalfImage(device, Width / 2, Height / 2, MakeUniformTexels(Width / 2, Height / 2, 3.0f), "ExposurePassTests.Bloom");
				setup.Run(*image, { .Bloom = bloom, .BloomIntensity = 1.0f, .Reset = true });
				uint32_t finite = 0;
				for (const glm::vec4& pixel : ToFloats(texels))
					finite += std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b) ? 1U : 0U;
				ExposureHistogram bloomed{};
				bloomed[GetLuminanceHistogramBin(glm::vec3(3.0f))] = finite;
				bloomed[0] = Width * Height - finite;
				CHECK(setup.ReadHistogram() == bloomed);
			}
			device.RunGarbageCollection();
		}

		// The adaptation on the GPU follows AdaptExposure from the target ComputeExposureTarget states (within the division's
		// and exp2's rounding): the target at once after a reset, then at SpeedDown and SpeedUp EV per second towards a new
		// target, and exactly the target once the step reaches it.
		TEST_CASE("ExposurePass: the adapted exposure is the target after a reset, then moves towards a new target at its speeds" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				constexpr uint32_t Size = 24;
				PostProcessSettings post;
				post.ExposureMode = RenderExposureMode::Auto;
				post.AutoExposureMinEV = -10.0f;
				const auto targetOf = [&post](float radiance)
				{
					const std::vector<glm::vec4> pixels(static_cast<size_t>(Size) * Size, glm::vec4(glm::vec3(radiance), 1.0f));
					return ComputeExposureTarget(ComputeLuminanceHistogram(pixels), post);
				};
				const nvrhi::TextureHandle dim = CreateHalfImage(device, Size, Size, MakeUniformTexels(Size, Size, 1.0f), "ExposurePassTests.Dim");
				const nvrhi::TextureHandle bright = CreateHalfImage(device, Size, Size, MakeUniformTexels(Size, Size, 12.0f), "ExposurePassTests.Bright");
				const ExposurePassInputs settings{ .MinEV = post.AutoExposureMinEV, .MaxEV = post.AutoExposureMaxEV, .SpeedUp = post.AutoExposureSpeedUp, .SpeedDown = post.AutoExposureSpeedDown };
				const auto with = [&settings](float deltaTime, bool reset)
				{
					ExposurePassInputs inputs = settings;
					inputs.DeltaTime = deltaTime;
					inputs.Reset = reset;
					return inputs;
				};
				ExposureSetup setup(gpu);

				setup.Run(*dim, with(0.5f, true));
				const ExposureState converged = setup.ReadState();
				CHECK(converged.TargetEV == doctest::Approx(targetOf(1.0f)).epsilon(1e-6));
				CHECK(converged.AdaptedEV == converged.TargetEV);
				CHECK(converged.Multiplier == doctest::Approx(std::exp2(converged.AdaptedEV)).epsilon(1e-5));

				// Towards the brighter scene's darker exposure at SpeedDown (1 EV/s) for half a second.
				setup.Run(*bright, with(0.5f, false));
				const ExposureState darker = setup.ReadState();
				CHECK(darker.TargetEV == doctest::Approx(targetOf(12.0f)).epsilon(1e-6));
				CHECK(darker.TargetEV < converged.AdaptedEV - 1.0f);
				CHECK(darker.AdaptedEV == doctest::Approx(AdaptExposure(converged.AdaptedEV, darker.TargetEV, 0.5f, false, post)).epsilon(1e-6));
				CHECK(darker.AdaptedEV == doctest::Approx(converged.AdaptedEV - 0.5f).epsilon(1e-6));

				// A long frame reaches the target exactly.
				setup.Run(*bright, with(100.0f, false));
				const ExposureState reached = setup.ReadState();
				CHECK(reached.AdaptedEV == reached.TargetEV);

				// Back towards the dim scene's brighter exposure at SpeedUp (3 EV/s) for a quarter of a second.
				setup.Run(*dim, with(0.25f, false));
				const ExposureState brighter = setup.ReadState();
				CHECK(brighter.AdaptedEV == doctest::Approx(reached.AdaptedEV + 0.75f).epsilon(1e-6));
				CHECK(brighter.Multiplier == doctest::Approx(std::exp2(brighter.AdaptedEV)).epsilon(1e-5));

				// A reset converges at once again, and no time adapts nothing.
				setup.Run(*bright, with(0.0f, false));
				CHECK(setup.ReadState().AdaptedEV == brighter.AdaptedEV);
				setup.Run(*bright, with(0.0f, true));
				const ExposureState reset = setup.ReadState();
				CHECK(reset.AdaptedEV == reset.TargetEV);
				CHECK(reset.TargetEV == darker.TargetEV);
			}
			device.RunGarbageCollection();
		}

		// Record checks its inputs before it records anything.
		TEST_CASE("ExposurePass: Record refuses a missing image or buffer and one smaller than its description" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				ExposureSetup setup(gpu);
				const nvrhi::TextureHandle image = CreateHalfImage(device, 4, 4, MakeUniformTexels(4, 4, 1.0f), "ExposurePassTests.Image");
				nvrhi::BufferDesc smallDesc = ExposurePass::GetHistogramDesc();
				smallDesc.byteSize = sizeof(uint32_t);
				Result<nvrhi::BufferHandle> small = device.CreateBuffer(smallDesc);
				REQUIRE(small.has_value());
				const ExposurePassInputs complete{ .SceneColor = image, .Histogram = setup.GetHistogram(), .State = setup.GetState() };
				ExposurePassInputs withoutImage = complete;
				withoutImage.SceneColor = nullptr;
				ExposurePassInputs withoutState = complete;
				withoutState.State = nullptr;
				ExposurePassInputs smallHistogram = complete;
				smallHistogram.Histogram = *small;
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE(list.has_value());
				PassBindingCache bindings;
				RenderPassCounters counters;
				(*list)->open();
				for (const ExposurePassInputs& inputs : { withoutImage, withoutState, smallHistogram })
				{
					const Status refused = setup.GetPass().Record(**list, bindings, inputs, counters);
					REQUIRE_FALSE(refused.has_value());
					CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				}
				(*list)->close();
				device.ExecuteCommandList(**list);
				device.WaitForIdle();
				CHECK(counters.Dispatches == 0);
				CHECK(bindings.GetSize() == 0);
			}
			device.RunGarbageCollection();
		}

		// TonemapPassInputs::AdaptedExposure (decision 15): the adapted exposure's multiplier multiplies the view's exposure
		// (2^ExposureEV, the compensation); without it the view's exposure applies alone.
		TEST_CASE("TonemapPass: an adapted exposure multiplies the view's exposure" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<TonemapPass>> pass = TonemapPass::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				const std::array<HalfTexel, 2> texels = { HalfTexel{ ToHalf(0.25f), ToHalf(0.25f), ToHalf(0.25f), ToHalf(1.0f) },
					HalfTexel{ ToHalf(0.125f), ToHalf(0.0625f), ToHalf(0.03125f), ToHalf(1.0f) } };
				const nvrhi::TextureHandle sceneColor = CreateHalfImage(device, 2, 1, texels, "ExposurePassTests.TonemapSceneColor");
				nvrhi::TextureDesc ldrDesc = sceneColor->getDesc();
				ldrDesc.format = LdrColorFormat;
				ldrDesc.isUAV = true;
				ldrDesc.debugName = "ExposurePassTests.LdrColor";
				Result<nvrhi::TextureHandle> ldr = device.CreateTexture(ldrDesc);
				REQUIRE(ldr.has_value());
				nvrhi::BufferDesc viewDesc;
				viewDesc.byteSize = sizeof(ViewConstants);
				viewDesc.isConstantBuffer = true;
				viewDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				viewDesc.keepInitialState = true;
				Result<nvrhi::BufferHandle> viewBuffer = device.CreateBuffer(viewDesc);
				REQUIRE(viewBuffer.has_value());
				Result<nvrhi::BufferHandle> stateBuffer = device.CreateBuffer(ExposurePass::GetStateDesc());
				REQUIRE(stateBuffer.has_value());
				ViewConstants view{};
				view.ViewportSize = glm::vec2(2.0f, 1.0f);
				view.InverseViewportSize = 1.0f / view.ViewportSize;
				view.Exposure = 2.0f;
				const ExposureState state{ .AdaptedEV = -2.0f, .Multiplier = 0.25f, .TargetEV = -2.0f };
				PassBindingCache bindings;
				const auto run = [&device, &pass, &bindings, &viewBuffer, &stateBuffer, &view, &state, &sceneColor, &ldr](nvrhi::IBuffer* adapted)
				{
					Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
					REQUIRE(list.has_value());
					(*list)->open();
					(*list)->writeBuffer(*viewBuffer, &view, sizeof(view));
					(*list)->writeBuffer(*stateBuffer, &state, sizeof(state));
					const Status recorded = (*pass)->Record(**list, bindings,
						{ .ViewConstants = *viewBuffer, .SceneColor = sceneColor, .LdrColor = *ldr, .Settings = { .Tonemapper = RenderTonemapper::Linear, .Dither = false }, .AdaptedExposure = adapted });
					(*list)->close();
					REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
					device.ExecuteCommandList(**list);
					Readback readback(device);
					Result<Image> image = readback.ReadTexture(**ldr);
					REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
					return std::move(*image);
				};
				const auto encode = [](double linear)
				{
					return static_cast<int>(std::lround(255.0 * Test::LinearToSrgb(linear)));
				};
				const Image adapted = run(*stateBuffer);
				const Image manual = run(nullptr);
				// 0.25 * 2 * 0.25 and 0.125 * 2 * 0.25 with the adapted exposure; 0.25 * 2 and 0.125 * 2 without.
				CHECK(std::abs(GetChannel(adapted, 0, 0, 0) - encode(0.125)) <= 1);
				CHECK(std::abs(GetChannel(adapted, 1, 0, 0) - encode(0.0625)) <= 1);
				CHECK(std::abs(GetChannel(adapted, 1, 0, 2) - encode(0.015625)) <= 1);
				CHECK(std::abs(GetChannel(manual, 0, 0, 0) - encode(0.5)) <= 1);
				CHECK(std::abs(GetChannel(manual, 1, 0, 0) - encode(0.25)) <= 1);
				device.WaitForIdle();
			}
			device.RunGarbageCollection();
		}

		// The acceptance (decisions 10 and 15): a capture renders a snapshot whose history is reset, so its exposure converges
		// at once and two captures are byte-identical; the metered scene lands within half a bin (a quarter stop) of middle
		// grey whatever its brightness, exactly as the CPU statement predicts, and ExposureEV compensates on top.
		TEST_CASE("ViewportCapture: two captures with auto-exposure are byte-identical and expose the scene to middle grey" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<ViewportCapture>> capture = ViewportCapture::CreateForScenes(gpu.GetDevice(), **pipelines, cache, assets.GetManager());
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
				constexpr uint32_t Width = 96;
				constexpr uint32_t Height = 64;
				const int greyLow = static_cast<int>(std::lround(255.0 * Test::LinearToSrgb(0.18 * std::exp2(-0.25))));
				const int greyHigh = static_cast<int>(std::lround(255.0 * Test::LinearToSrgb(0.18 * std::exp2(0.25))));
				struct Case
				{
					float Radiance = 0.0f;
					float ExposureEV = 0.0f;
				};
				for (const Case& scene : { Case{ 0.07f, 0.0f }, Case{ 3.0f, 0.0f }, Case{ 30.0f, 0.0f }, Case{ 3.0f, 1.0f } })
				{
					CAPTURE(scene.Radiance);
					CAPTURE(scene.ExposureEV);
					RenderSnapshot snapshot = MakeUniformSnapshot(Width, Height, scene.Radiance);
					snapshot.Post.ExposureEV = scene.ExposureEV;
					const Result<Image> first = (*capture)->Capture({ .Width = Width, .Height = Height }, snapshot);
					REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
					const Result<Image> second = (*capture)->Capture({ .Width = Width, .Height = Height }, snapshot);
					REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
					CHECK(first->Pixels == second->Pixels);
					const int expected = PredictEncodedValue(scene.Radiance, snapshot.Post);
					for (const uint32_t y : { 0U, Height / 2, Height - 1 })
					{
						for (const uint32_t x : { 0U, Width / 2, Width - 1 })
						{
							for (size_t channel = 0; channel < 3; ++channel)
							{
								CAPTURE(x);
								CAPTURE(y);
								CAPTURE(channel);
								const int value = GetChannel(*first, x, y, channel);
								CHECK(std::abs(value - expected) <= 1);
								if (scene.ExposureEV == 0.0f)
								{
									CHECK(value >= greyLow - 1);
									CHECK(value <= greyHigh + 1);
								}
							}
						}
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// Auto acts as Manual where decision 10 keeps M18's rules: no Exposure row in a debug view, the wireframe or pixel-art
		// mode, whose image equals the Manual one; a Lit view records the row with its two dispatches.
		TEST_CASE("SceneRenderer: auto-exposure records the Exposure pass only in a Lit view outside the wireframe and pixel-art mode" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				uint64_t frame = 300;
				const auto render = [&setup, &frame](RenderSnapshot snapshot)
				{
					snapshot.FrameIndex = ++frame;
					return setup.Render(snapshot);
				};
				const auto submit = [&setup, &frame](RenderSnapshot snapshot)
				{
					snapshot.FrameIndex = ++frame;
					setup.Submit(snapshot);
				};
				// The last render's Exposure row, copied out of its stats.
				const auto recordedExposure = [&setup]() -> std::optional<RenderPassStats>
				{
					const RenderStats stats = setup.Renderer().GetRenderStats();
					const RenderPassStats* row = Test::SceneRendererIntegrationFixture::FindPass(stats, "Exposure");
					return row != nullptr ? std::optional<RenderPassStats>(*row) : std::nullopt;
				};
				const RenderSnapshot lit = MakeFixtureSnapshot(setup, 3.0f);
				submit(lit);
				const std::optional<RenderPassStats> exposure = recordedExposure();
				REQUIRE(exposure.has_value());
				CHECK(exposure->Dispatches == 2);

				RenderSnapshot manual = lit;
				manual.Post.ExposureMode = RenderExposureMode::Manual;
				submit(manual);
				CHECK_FALSE(recordedExposure().has_value());

				RenderSnapshot albedo = lit;
				albedo.DebugView = RenderDebugView::Albedo;
				submit(albedo);
				CHECK_FALSE(recordedExposure().has_value());

				RenderSnapshot wireframe = lit;
				wireframe.Flags = RenderViewFlags::Wireframe;
				submit(wireframe);
				CHECK_FALSE(recordedExposure().has_value());

				RenderSnapshot pixelArt = lit;
				pixelArt.Output.PixelArt = true;
				const Image autoPixelArt = render(pixelArt);
				CHECK_FALSE(recordedExposure().has_value());
				pixelArt.Post.ExposureMode = RenderExposureMode::Manual;
				CHECK(render(pixelArt).Pixels == autoPixelArt.Pixels);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// The temporal-state section (decision 10) in the renderer: a snapshot whose history was reset converges at once; one
		// that continues the history adapts from the view's previous exposure at the speeds, reaching the reset image once
		// the step covers the gap; and a view whose previous render had no Exposure pass converges at once again.
		TEST_CASE("SceneRenderer: auto-exposure adapts across a view's renders and converges at once when its history is reset" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				uint64_t frame = 500;
				const auto render = [&setup, &frame](RenderSnapshot snapshot)
				{
					snapshot.FrameIndex = ++frame;
					return setup.Render(snapshot);
				};
				const auto submit = [&setup, &frame](RenderSnapshot snapshot)
				{
					snapshot.FrameIndex = ++frame;
					setup.Submit(snapshot);
				};
				const RenderSnapshot dim = MakeFixtureSnapshot(setup, 3.0f);
				const RenderSnapshot bright = MakeFixtureSnapshot(setup, 30.0f);

				const Image dimReset = render(dim);
				CHECK(std::abs(GetChannel(dimReset, 32, 32, 0) - PredictEncodedValue(3.0f, dim.Post)) <= 1);
				// A tenth of a second at 1 EV/s leaves the exposure far above the bright scene's: the image is brighter.
				const Image brightContinued = render(Continued(bright, 0.1f));
				const Image brightReached = render(Continued(bright, 100.0f));
				const Image brightReset = render(bright);
				CHECK(std::abs(GetChannel(brightReset, 32, 32, 0) - PredictEncodedValue(30.0f, bright.Post)) <= 1);
				CHECK(GetChannel(brightContinued, 32, 32, 0) > GetChannel(brightReset, 32, 32, 0) + 20);
				CHECK(brightReached.Pixels == brightReset.Pixels);

				// After a Manual render the view has no adapted exposure: the next Auto render converges at once.
				RenderSnapshot manual = bright;
				manual.Post.ExposureMode = RenderExposureMode::Manual;
				submit(Continued(manual, 0.1f));
				CHECK(render(Continued(dim, 0.1f)).Pixels == dimReset.Pixels);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// Decision 29: the Exposure pass is the pipeline set's (place 1) and its buffers the renderer's (place 2); after a device
		// loss both are created again and a reset render followed by a continued one gives the same image as before the loss.
		TEST_CASE("ExposurePass: a renderer with auto-exposure recovers from a device loss with the same image" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				uint64_t frame = 700;
				Test::DeviceLossTest test;
				test.Owners = { "SceneRendererPipelines", "SceneRenderer" };
				// One use renders the dim scene with a reset history, then the bright one continuing it for a tenth of a second,
				// and returns the second image (which depends on the exposure the first adapted).
				test.Use = [&device, &setup, &frame]() -> Result<std::vector<std::byte>>
				{
					std::vector<std::byte> pixels;
					for (const bool reset : { true, false })
					{
						RenderSnapshot snapshot = reset ? MakeFixtureSnapshot(setup, 3.0f) : Continued(MakeFixtureSnapshot(setup, 30.0f), 0.1f);
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
				// The view's targets reach their steady state in the pool first, as the other renderer loss cases do.
				for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
				{
					const Result<std::vector<std::byte>> steady = test.Use();
					REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
				}
				const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
				REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
				REQUIRE(report->Before.size() == 64 * 64 * 4);
				CHECK(std::to_integer<int>(report->Before[(32 * 64 + 32) * 4]) > 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
