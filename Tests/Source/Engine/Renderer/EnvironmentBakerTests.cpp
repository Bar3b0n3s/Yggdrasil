#include "TestsPCH.h"

#include "Engine/Renderer/EnvironmentBaker.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

// The GPU environment bake (Architecture §8.6; Docs/Decisions/0013-m8-decisions.md decision 9) against its device-independent
// oracles (§15.3): the white furnace for the prefilter and the SH, the prefilter against the CPU reference at 16²
// (Support/RenderReference.h), and the continuity of the equirectangular-to-cube seams.

namespace Engine {

	namespace {

		// An equirectangular input of `width` x width / 2 texels of constant `radiance`.
		EnvironmentBakeInput MakeConstantInput(uint32_t width, float radiance)
		{
			EnvironmentBakeInput input;
			input.Width = width;
			input.Height = width / 2;
			input.Texels.assign(static_cast<size_t>(input.Width) * input.Height * 3, radiance);
			return input;
		}

		// A smooth but non-constant equirectangular input: radiance varies with the direction's components, so every face and
		// seam carries a gradient.
		EnvironmentBakeInput MakeGradientInput(uint32_t width)
		{
			EnvironmentBakeInput input;
			input.Width = width;
			input.Height = width / 2;
			input.Texels.reserve(static_cast<size_t>(input.Width) * input.Height * 3);
			for (uint32_t row = 0; row < input.Height; ++row)
			{
				for (uint32_t column = 0; column < input.Width; ++column)
				{
					const glm::dvec3 direction = Test::EquirectUvToDirection(
						glm::dvec2((column + 0.5) / input.Width, (row + 0.5) / input.Height));
					input.Texels.push_back(static_cast<float>(1.0 + 0.5 * direction.x));
					input.Texels.push_back(static_cast<float>(1.0 + 0.5 * direction.y));
					input.Texels.push_back(static_cast<float>(1.0 + 0.5 * direction.z));
				}
			}
			return input;
		}

		// An equirectangular input whose red channel turns eight times around the horizon, 1 + 0.5 sin(8 phi) for the azimuth
		// phi = (u - 0.5) 2 pi: continuous across the image's u = 0 / 1 seam, but changing by about 0.4 per texel there at a
		// width of 64, so a sample that does not wrap around the seam is far off. Green follows the height, blue is 1.
		EnvironmentBakeInput MakeAzimuthInput(uint32_t width)
		{
			EnvironmentBakeInput input;
			input.Width = width;
			input.Height = width / 2;
			input.Texels.reserve(static_cast<size_t>(input.Width) * input.Height * 3);
			for (uint32_t row = 0; row < input.Height; ++row)
			{
				for (uint32_t column = 0; column < input.Width; ++column)
				{
					const double azimuth = ((column + 0.5) / input.Width - 0.5) * 2.0 * glm::pi<double>();
					const double height = 1.0 - 2.0 * (row + 0.5) / input.Height;
					input.Texels.push_back(static_cast<float>(1.0 + 0.5 * std::sin(8.0 * azimuth)));
					input.Texels.push_back(static_cast<float>(1.0 + 0.5 * height));
					input.Texels.push_back(1.0f);
				}
			}
			return input;
		}

		// The RGB of texel (column, row) of `face` at `level` of a cube (CubeMapData's layout, binary16 texels).
		glm::dvec3 ReadTexel(const CubeMapData& cube, uint32_t level, uint32_t face, uint32_t column, uint32_t row)
		{
			size_t offset = 0;
			for (uint32_t mip = 0; mip < level; ++mip)
			{
				const size_t edge = std::max(cube.FaceSize >> mip, 1U);
				offset += edge * edge * CubeMapData::FaceCount;
			}
			const size_t edge = std::max(cube.FaceSize >> level, 1U);
			offset += (static_cast<size_t>(face) * edge + row) * edge + column;
			std::array<uint16_t, 4> halves{};
			std::memcpy(halves.data(), cube.Texels.data() + offset * CubeMapData::BytesPerTexel, sizeof(halves));
			return glm::dvec3(Test::HalfToDouble(halves[0]), Test::HalfToDouble(halves[1]), Test::HalfToDouble(halves[2]));
		}

		// Creates the baker on the fixture's device; fails the test case on error.
		Scope<EnvironmentBaker> CreateBaker(Test::HeadlessGpuFixture& gpu)
		{
			Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
			return std::move(*baker);
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("EnvironmentBaker: furnace: a constant environment prefilters to the constant in every mip"
			* doctest::test_suite(Test::GpuSuite))
		{
			// §15.3: "prefiltered constant environment = 1 +- 0.5% in every mip".
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Scope<EnvironmentBaker> baker = CreateBaker(gpu);
				CHECK(baker->GetPipelineCount() == EnvironmentBaker::PipelineCount);
				const Result<EnvironmentData> baked = baker->Bake(MakeConstantInput(256, 1.0f));
				REQUIRE_MESSAGE(baked.has_value(), baked.error().ToString());
				CHECK(baked->Specular.FaceSize == EnvironmentData::SpecularFaceSize);
				REQUIRE(baked->Specular.MipCount == EnvironmentData::SpecularMipCount);
				CHECK(baked->Skybox.FaceSize == 64);
				for (uint32_t level = 0; level < baked->Specular.MipCount; ++level)
				{
					const uint32_t edge = std::max(EnvironmentData::SpecularFaceSize >> level, 1U);
					for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
					{
						for (const uint32_t texel : { 0U, edge / 2, edge - 1 })
						{
							CAPTURE(level);
							CAPTURE(face);
							const glm::dvec3 value = ReadTexel(baked->Specular, level, face, texel, texel);
							CHECK(std::abs(value.x - 1.0) <= 0.005);
							CHECK(std::abs(value.y - 1.0) <= 0.005);
							CHECK(std::abs(value.z - 1.0) <= 0.005);
						}
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("EnvironmentBaker: furnace: the SH irradiance of a constant environment is constant"
			* doctest::test_suite(Test::GpuSuite))
		{
			// §15.3: "SH irradiance constant": only the L0 band, evaluating to the radiance in every direction (irradiance / pi,
			// Asset/EnvironmentData.h).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Scope<EnvironmentBaker> baker = CreateBaker(gpu);
				const Result<EnvironmentData> baked = baker->Bake(MakeConstantInput(256, 0.75f));
				REQUIRE_MESSAGE(baked.has_value(), baked.error().ToString());
				std::array<glm::dvec3, 9> coefficients{};
				for (size_t index = 0; index < coefficients.size(); ++index)
					coefficients[index] = glm::dvec3(baked->IrradianceSH9[index]);
				for (const glm::dvec3& normal : { glm::dvec3(0, 1, 0), glm::dvec3(0, -1, 0), glm::dvec3(1, 0, 0), glm::normalize(glm::dvec3(1, 2, -3)) })
				{
					const glm::dvec3 value = Test::EvaluateIrradianceSH9(coefficients, normal);
					CHECK(value.x == doctest::Approx(0.75).epsilon(0.005));
					CHECK(value.z == doctest::Approx(0.75).epsilon(0.005));
				}
				for (size_t index = 1; index < coefficients.size(); ++index)
					CHECK(glm::length(coefficients[index]) < 1e-3);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("EnvironmentBaker: prefilter vs CPU at 16x16" * doctest::test_suite(Test::GpuSuite))
		{
			// §15.3 "GPU prefilter vs CPU reference at 16²": the same source cube (the bake's own skybox mip 0 at 16²) filtered
			// on the GPU (importance sampled) and by brute force on the CPU agree within the sampling error.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Scope<EnvironmentBaker> baker = CreateBaker(gpu);
				const EnvironmentBakeOptions options{ .SkyboxFaceSize = 16, .SpecularFaceSize = 16, .SpecularMipCount = 5, .MinSamples = 1024, .MaxSamples = 4096 };
				const Result<EnvironmentData> baked = baker->BakeWithOptions(MakeGradientInput(64), options);
				REQUIRE_MESSAGE(baked.has_value(), baked.error().ToString());
				Test::ReferenceCube source;
				source.Size = 16;
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					for (uint32_t row = 0; row < 16; ++row)
					{
						for (uint32_t column = 0; column < 16; ++column)
							source.Faces[face].push_back(ReadTexel(baked->Skybox, 0, face, column, row));
					}
				}
				for (uint32_t level = 1; level < options.SpecularMipCount; ++level)
				{
					const double roughness = static_cast<double>(level) / (options.SpecularMipCount - 1);
					const uint32_t edge = 16 >> level;
					const Test::ReferenceCube expected = Test::PrefilterSpecular(source, roughness, edge);
					REQUIRE(expected.Size == edge);
					double worst = 0.0;
					for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
					{
						REQUIRE(expected.Faces[face].size() == static_cast<size_t>(edge) * edge);
						for (uint32_t row = 0; row < edge; ++row)
						{
							for (uint32_t column = 0; column < edge; ++column)
							{
								const glm::dvec3 difference = ReadTexel(baked->Specular, level, face, column, row) - expected.Faces[face][row * edge + column];
								worst = std::max(worst, std::max({ std::abs(difference.x), std::abs(difference.y), std::abs(difference.z) }));
							}
						}
					}
					CAPTURE(level);
					CHECK(worst < 0.02);
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("EnvironmentBaker: equirect-to-cube seams are continuous" * doctest::test_suite(Test::GpuSuite))
		{
			// §15.3 "equirect-to-cube seam continuity": a smooth input stays smooth across every cube edge and across the
			// equirectangular image's u = 0 / 1 seam (behind the default camera, at +Z): every edge texel holds the input's value
			// in its own direction.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Scope<EnvironmentBaker> baker = CreateBaker(gpu);
				const Result<EnvironmentData> baked = baker->BakeWithOptions(MakeGradientInput(256), EnvironmentBakeOptions{ .SkyboxFaceSize = 32 });
				REQUIRE_MESSAGE(baked.has_value(), baked.error().ToString());
				double worst = 0.0;
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					for (uint32_t index = 0; index < 32; ++index)
					{
						for (const auto& [column, row] : { std::pair<uint32_t, uint32_t>(0, index), std::pair<uint32_t, uint32_t>(31, index),
								 std::pair<uint32_t, uint32_t>(index, 0), std::pair<uint32_t, uint32_t>(index, 31) })
						{
							const glm::dvec3 direction = Test::CubeTexelDirection(face, column, row, 32);
							const glm::dvec3 expected = glm::dvec3(1.0) + 0.5 * direction;
							const glm::dvec3 actual = ReadTexel(baked->Skybox, 0, face, column, row);
							worst = std::max(worst, glm::length(actual - expected));
						}
					}
				}
				CHECK(worst < 0.02);

				// The u = 0 / 1 seam itself, behind the default camera at +Z: at a width of 64 the +Z face's central columns of a
				// 32-texel face sample 0.3 texels from the seam, so their bilinear footprints take the last and the first column
				// of the image. Every texel of every face must equal the CPU resampling (Test::EquirectToCube, wrapping in u),
				// which an unwrapped, clamped or out-of-range read of those columns misses by far more than the tolerance.
				const EnvironmentBakeInput azimuth = MakeAzimuthInput(64);
				const Result<EnvironmentData> wrapped = baker->BakeWithOptions(azimuth, EnvironmentBakeOptions{ .SkyboxFaceSize = 32 });
				REQUIRE_MESSAGE(wrapped.has_value(), wrapped.error().ToString());
				const Test::ReferenceCube expected = Test::EquirectToCube(azimuth.Texels, azimuth.Width, azimuth.Height, 32);
				uint32_t wrappingTexels = 0;
				double worstWrapped = 0.0;
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					for (uint32_t row = 0; row < 32; ++row)
					{
						for (uint32_t column = 0; column < 32; ++column)
						{
							// The bilinear footprint's left column is the image's last one exactly when the sample wraps.
							const glm::dvec2 uv = Test::DirectionToEquirectUv(Test::CubeTexelDirection(face, column, row, 32));
							wrappingTexels += std::floor(uv.x * azimuth.Width - 0.5) == azimuth.Width - 1.0 || uv.x * azimuth.Width < 0.5 ? 1U : 0U;
							const glm::dvec3 actual = ReadTexel(wrapped->Skybox, 0, face, column, row);
							worstWrapped = std::max(worstWrapped, glm::length(actual - expected.Faces[face][row * 32 + column]));
						}
					}
				}
				CHECK(wrappingTexels >= 32); // the +Z face's two central columns, at least
				CHECK(worstWrapped < 0.01);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("EnvironmentBaker: malformed inputs are InvalidArgument" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Scope<EnvironmentBaker> baker = CreateBaker(gpu);
				EnvironmentBakeInput square = MakeConstantInput(64, 1.0f);
				square.Height = 64;
				EnvironmentBakeInput nan = MakeConstantInput(64, 1.0f);
				nan.Texels[5] = std::numeric_limits<float>::quiet_NaN();
				EnvironmentBakeInput negative = MakeConstantInput(64, 1.0f);
				negative.Texels[7] = -1.0f;
				EnvironmentBakeInput truncated = MakeConstantInput(64, 1.0f);
				truncated.Texels.pop_back();
				for (const EnvironmentBakeInput* input : { &square, &nan, &negative, &truncated })
				{
					const Result<EnvironmentData> baked = baker->Bake(*input);
					REQUIRE_FALSE(baked.has_value());
					CHECK(baked.error().GetCode() == ErrorCode::InvalidArgument);
				}
				const Result<EnvironmentData> badOptions = baker->BakeWithOptions(MakeConstantInput(64, 1.0f), EnvironmentBakeOptions{ .SpecularFaceSize = 3 });
				REQUIRE_FALSE(badOptions.has_value());
				CHECK(badOptions.error().GetCode() == ErrorCode::InvalidArgument);
				// Wider than 8,192 texels (decision 21, B): refused by its size before the texels are looked at, so no 16k
				// image is allocated here.
				EnvironmentBakeInput wide;
				wide.Width = 16384;
				wide.Height = 8192;
				const Result<EnvironmentData> tooWide = baker->Bake(wide);
				REQUIRE_FALSE(tooWide.has_value());
				CHECK(tooWide.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(tooWide.error().ToString().contains("16384"));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("EnvironmentBaker: ClampLuminance scales texels above the limit down to it" * doctest::test_suite(Test::GpuSuite))
		{
			// A constant environment of 100 with ClampLuminance at 10 bakes like a constant environment of 10.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Scope<EnvironmentBaker> baker = CreateBaker(gpu);
				EnvironmentBakeInput input = MakeConstantInput(64, 100.0f);
				input.ClampLuminance = true;
				input.ClampLuminanceMax = 10.0f;
				const Result<EnvironmentData> baked = baker->Bake(input);
				REQUIRE_MESSAGE(baked.has_value(), baked.error().ToString());
				CHECK(ReadTexel(baked->Skybox, 0, 0, 3, 3).x == doctest::Approx(10.0).epsilon(0.01));
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
