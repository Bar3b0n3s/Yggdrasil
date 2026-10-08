#include "TestsPCH.h"

#include "Engine/Renderer/BloomPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

// Bloom (Architecture §8.3 pass 10; Docs/Decisions/0013-m8-decisions.md decision 8): the chain's shape, and the passes on
// small synthetic SceneColors (the Bloom golden image covers the look).

namespace Engine {

	namespace {

		// The binary16 bits of `value` (round to nearest even), for RGBA16_FLOAT uploads.
		uint16_t ToHalf(double value)
		{
			return glm::packHalf1x16(static_cast<float>(value));
		}

		// A SceneColor of `size`² RGBA16F texels from `texels` (RGBA binary16, rows top first).
		nvrhi::TextureHandle CreateSceneColor(GraphicsDevice& device, uint32_t size, std::span<const std::array<uint16_t, 4>> texels)
		{
			nvrhi::TextureDesc desc;
			desc.width = size;
			desc.height = size;
			desc.format = SceneColorFormat;
			desc.isShaderResource = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "BloomPassTests.SceneColor";
			const std::array<TextureSubresourceData, 1> subresources = { { { .Data = std::as_bytes(texels) } } };
			Result<TextureUpload> upload = device.GetHostImageUpload().CreateTexture(desc, subresources, TextureUploadPath::Staging);
			REQUIRE_MESSAGE(upload.has_value(), upload.error().ToString());
			return upload->Texture;
		}

		// A SceneColor of `size`² texels, black but for one bright texel of `radiance` at the centre.
		nvrhi::TextureHandle CreateSpotSceneColor(GraphicsDevice& device, uint32_t size, double radiance)
		{
			std::vector<std::array<uint16_t, 4>> texels(static_cast<size_t>(size) * size, std::array<uint16_t, 4>{ ToHalf(0.0), ToHalf(0.0), ToHalf(0.0), ToHalf(1.0) });
			texels[static_cast<size_t>(size / 2) * size + size / 2] = { ToHalf(radiance), ToHalf(radiance), ToHalf(radiance), ToHalf(1.0) };
			return CreateSceneColor(device, size, texels);
		}

		// Records the pass over `scene` (`size`²) into a new chain and executes it; returns the chain.
		nvrhi::TextureHandle RecordBloom(GraphicsDevice& device, BloomPass& pass, nvrhi::ITexture* scene, uint32_t size)
		{
			Result<nvrhi::TextureHandle> chain = device.CreateTexture(BloomPass::GetChainDesc(size, size, pass.GetFormat()));
			REQUIRE(chain.has_value());
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE(commandList.has_value());
			PassBindingCache bindings;
			(*commandList)->open();
			const Result<nvrhi::ITexture*> bloom = pass.Record(**commandList, bindings, { .SceneColor = scene, .Chain = *chain });
			(*commandList)->close();
			REQUIRE_MESSAGE(bloom.has_value(), bloom.error().ToString());
			REQUIRE(*bloom == chain->Get());
			device.ExecuteCommandList(**commandList);
			return *chain;
		}

		// The RGB of every texel of an RGBA16_FLOAT image, decoded.
		std::vector<glm::vec3> DecodeHalfImage(const Image& image)
		{
			REQUIRE(image.Format == nvrhi::Format::RGBA16_FLOAT);
			const size_t texelCount = static_cast<size_t>(image.Width) * image.Height;
			REQUIRE(image.Pixels.size() == texelCount * 8);
			std::vector<glm::vec3> texels;
			texels.reserve(texelCount);
			for (size_t texel = 0; texel < texelCount; ++texel)
			{
				std::array<uint16_t, 4> value{};
				std::memcpy(value.data(), image.Pixels.data() + texel * 8, 8);
				texels.emplace_back(glm::unpackHalf1x16(value[0]), glm::unpackHalf1x16(value[1]), glm::unpackHalf1x16(value[2]));
			}
			return texels;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("BloomPass: the chain starts at half resolution with at most six mips in the bloom format")
		{
			const nvrhi::TextureDesc large = BloomPass::GetChainDesc(1920, 1080, nvrhi::Format::R11G11B10_FLOAT);
			CHECK(large.width == 960);
			CHECK(large.height == 540);
			CHECK(large.mipLevels == BloomPass::MaxMipCount);
			CHECK(large.format == nvrhi::Format::R11G11B10_FLOAT);
			CHECK(large.isUAV);
			CHECK(large.isShaderResource);
			CHECK(large.keepInitialState);
			const nvrhi::TextureDesc small = BloomPass::GetChainDesc(9, 5, nvrhi::Format::RGBA16_FLOAT);
			CHECK(small.width == 4);
			CHECK(small.height == 2);
			CHECK(small.mipLevels == 3); // 4, 2, 1
			const nvrhi::TextureDesc tiny = BloomPass::GetChainDesc(1, 1, nvrhi::Format::RGBA16_FLOAT);
			CHECK(tiny.width == 1);
			CHECK(tiny.mipLevels == 1);
		}

		TEST_CASE("BloomPass: the layouts declare the storage format of their permutation")
		{
			// §8.1's fallback is a shader permutation: each format's three pipelines name their own BLOOM_RGBA16 value and
			// storage format, which "Shaders: LayoutsMatchReflection" checks against the compiled variants.
			for (const nvrhi::Format format : { nvrhi::Format::R11G11B10_FLOAT, nvrhi::Format::RGBA16_FLOAT })
			{
				CAPTURE(std::string(nvrhi::getFormatInfo(format).name));
				const std::vector<PipelineLayoutDescription> descriptions = BloomPass::GetLayoutDescriptions(format);
				REQUIRE(descriptions.size() == BloomPass::PipelineCount);
				for (const PipelineLayoutDescription& description : descriptions)
				{
					CAPTURE(description.Name);
					CHECK(description.Program == "Bloom");
					REQUIRE(description.Permutation.size() == 1);
					CHECK(description.Permutation[0].Key == "BLOOM_RGBA16");
					CHECK(description.Permutation[0].Value == (format == nvrhi::Format::RGBA16_FLOAT ? "1" : "0"));
					REQUIRE(description.StorageImages.size() == 1);
					CHECK(description.StorageImages[0].Format == format);
				}
				CHECK(descriptions[0].Entries == std::vector<std::string>{ "CSDownsampleKaris" });
				CHECK(descriptions[1].Entries == std::vector<std::string>{ "CSDownsample" });
				CHECK(descriptions[2].Entries == std::vector<std::string>{ "CSUpsample" });
			}
		}

		TEST_CASE("BloomPass: a bright texel spreads into its surroundings" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				const nvrhi::Format format = device.GetInfo().BloomFormat;
				Result<Scope<BloomPass>> pass = BloomPass::Create(device, gpu.GetPipelines(), format);
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				CHECK((*pass)->GetPipelineCount() == BloomPass::PipelineCount);
				CHECK((*pass)->GetFormat() == format);
				const nvrhi::TextureHandle scene = CreateSpotSceneColor(device, 64, 64.0);
				const nvrhi::TextureHandle chain = RecordBloom(device, **pass, scene, 64);

				// Mip 0 (32², half resolution) holds the upsampled bloom: light spread around the bright texel, so the readback
				// is not black (the Karis average damps a lone firefly; the uniform case below checks the normalization and the
				// Bloom golden image covers the look).
				Readback readback(device);
				const Result<Image> image = readback.ReadTexture(*chain);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				CHECK(image->Width == 32);
				CHECK(image->Format == format);
				CHECK(image->Pixels != std::vector<std::byte>(image->Pixels.size(), std::byte{ 0 }));
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("BloomPass: the bloom of a spot is symmetric and falls off with distance" * doctest::test_suite(Test::GpuSuite))
		{
			// RGBA16_FLOAT so the readback decodes exactly. The spot, texel (32, 32) of the 64² SceneColor, lies at (16.25, 16.25)
			// in the 32² chain's coordinates, inside chain texel (16, 16): that texel is the brightest, the bloom is symmetric
			// under swapping x and y, and it falls off with the distance from the spot.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<BloomPass>> pass = BloomPass::Create(device, gpu.GetPipelines(), nvrhi::Format::RGBA16_FLOAT);
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				const nvrhi::TextureHandle scene = CreateSpotSceneColor(device, 64, 16.0);
				const nvrhi::TextureHandle chain = RecordBloom(device, **pass, scene, 64);
				Readback readback(device);
				const Result<Image> image = readback.ReadTexture(*chain);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				const std::vector<glm::vec3> texels = DecodeHalfImage(*image);
				const auto at = [&texels](uint32_t column, uint32_t row)
				{
					return texels[static_cast<size_t>(row) * 32 + column].r;
				};
				const float peak = at(16, 16);
				CHECK(peak > 0.0f);
				for (const glm::vec3& texel : texels)
				{
					CHECK(texel.r <= peak);
					CHECK(std::isfinite(texel.r));
				}
				CHECK(at(16, 12) > at(16, 8));
				CHECK(at(16, 8) > at(16, 2));
				CHECK(at(12, 16) == doctest::Approx(at(16, 12)).epsilon(0.001));
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("BloomPass: a uniform scene blooms to itself" * doctest::test_suite(Test::GpuSuite))
		{
			// BloomPass.h's normalization: mip 0 holds the average of the chain's levels, so a uniform SceneColor c gives a
			// uniform bloom c (the Karis average of equal texels is the texel) and the composite leaves the image unchanged.
			// RGBA16_FLOAT, which every device supports as a storage image, so the readback decodes as binary16; the powers
			// of two are exact in it.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<BloomPass>> pass = BloomPass::Create(device, gpu.GetPipelines(), nvrhi::Format::RGBA16_FLOAT);
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				constexpr std::array<double, 3> Color = { 0.25, 1.0, 4.0 };
				const std::vector<std::array<uint16_t, 4>> texels(static_cast<size_t>(64) * 64,
					std::array<uint16_t, 4>{ ToHalf(Color[0]), ToHalf(Color[1]), ToHalf(Color[2]), ToHalf(1.0) });
				const nvrhi::TextureHandle scene = CreateSceneColor(device, 64, texels);
				const nvrhi::TextureHandle chain = RecordBloom(device, **pass, scene, 64);

				Readback readback(device);
				const Result<Image> image = readback.ReadTexture(*chain);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				double worst = 0.0;
				for (const glm::vec3& texel : DecodeHalfImage(*image))
				{
					for (glm::length_t channel = 0; channel < 3; ++channel)
						worst = std::max(worst, std::abs(static_cast<double>(texel[channel]) - Color[static_cast<size_t>(channel)]) / Color[static_cast<size_t>(channel)]);
				}
				CHECK(worst < 1e-2);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("BloomPass: non-finite SceneColor texels do not spread through the chain" * doctest::test_suite(Test::GpuSuite))
		{
			// One NaN and one +Inf texel in a uniform scene: the first downsample replaces NaN by 0 and clamps the rest, so every
			// texel of the bloom stays finite (a NaN would otherwise blur over the whole chain and the composited image).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<BloomPass>> pass = BloomPass::Create(device, gpu.GetPipelines(), nvrhi::Format::RGBA16_FLOAT);
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				std::vector<std::array<uint16_t, 4>> texels(static_cast<size_t>(32) * 32, std::array<uint16_t, 4>{ ToHalf(0.5), ToHalf(0.5), ToHalf(0.5), ToHalf(1.0) });
				constexpr uint16_t QuietNaN = 0x7E00;
				constexpr uint16_t PositiveInfinity = 0x7C00;
				texels[5 * 32 + 7] = { QuietNaN, QuietNaN, QuietNaN, ToHalf(1.0) };
				texels[20 * 32 + 18] = { PositiveInfinity, PositiveInfinity, PositiveInfinity, ToHalf(1.0) };
				const nvrhi::TextureHandle scene = CreateSceneColor(device, 32, texels);
				const nvrhi::TextureHandle chain = RecordBloom(device, **pass, scene, 32);

				Readback readback(device);
				const Result<Image> image = readback.ReadTexture(*chain);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				bool finite = true;
				for (const glm::vec3& texel : DecodeHalfImage(*image))
					finite = finite && std::isfinite(texel.r) && std::isfinite(texel.g) && std::isfinite(texel.b);
				CHECK(finite);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("BloomPass: both storage formats create and record (the RGBA16_FLOAT fallback of §8.1)" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				const nvrhi::TextureHandle scene = CreateSpotSceneColor(device, 16, 4.0);
				for (const nvrhi::Format format : { nvrhi::Format::R11G11B10_FLOAT, nvrhi::Format::RGBA16_FLOAT })
				{
					// R11G11B10_FLOAT is optional as a storage image (§8.1): where device selection fell back to
					// RGBA16_FLOAT, only that format is created.
					if (format == nvrhi::Format::R11G11B10_FLOAT && device.GetInfo().BloomFormat != format)
						continue;
					CAPTURE(std::string(nvrhi::getFormatInfo(format).name));
					Result<Scope<BloomPass>> pass = BloomPass::Create(device, gpu.GetPipelines(), format);
					REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
					static_cast<void>(RecordBloom(device, **pass, scene, 16));
				}
			}
			device.RunGarbageCollection();
		}
	}

}
