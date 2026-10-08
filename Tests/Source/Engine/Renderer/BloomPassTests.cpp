#include "TestsPCH.h"

#include "Engine/Renderer/BloomPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

// Bloom (Architecture §8.3 pass 10). The chain's shape is the contract's (implemented); the passes are skeletons of the M8
// contract (Docs/Decisions/0013-m8-decisions.md decision 8): stream C implements them and removes the skips.

namespace Engine {

	namespace {

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
			const std::array<TextureSubresourceData, 1> subresources = { { { .Data = std::as_bytes(std::span(texels)) } } };
			Result<TextureUpload> upload = device.GetHostImageUpload().CreateTexture(desc, subresources, TextureUploadPath::Staging);
			REQUIRE_MESSAGE(upload.has_value(), upload.error().ToString());
			return upload->Texture;
		}

		// A SceneColor of `size`² texels, black but for one bright texel of `radiance` at the centre.
		nvrhi::TextureHandle CreateSpotSceneColor(GraphicsDevice& device, uint32_t size, double radiance)
		{
			std::vector<std::array<uint16_t, 4>> texels(static_cast<size_t>(size) * size,
				std::array<uint16_t, 4>{ Test::DoubleToHalf(0.0), Test::DoubleToHalf(0.0), Test::DoubleToHalf(0.0), Test::DoubleToHalf(1.0) });
			texels[static_cast<size_t>(size / 2) * size + size / 2] = { Test::DoubleToHalf(radiance), Test::DoubleToHalf(radiance), Test::DoubleToHalf(radiance),
				Test::DoubleToHalf(1.0) };
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

		TEST_CASE("BloomPass: a bright texel spreads into its surroundings" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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

		TEST_CASE("BloomPass: a uniform scene blooms to itself" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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
					std::array<uint16_t, 4>{ Test::DoubleToHalf(Color[0]), Test::DoubleToHalf(Color[1]), Test::DoubleToHalf(Color[2]), Test::DoubleToHalf(1.0) });
				const nvrhi::TextureHandle scene = CreateSceneColor(device, 64, texels);
				const nvrhi::TextureHandle chain = RecordBloom(device, **pass, scene, 64);

				Readback readback(device);
				const Result<Image> image = readback.ReadTexture(*chain);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				REQUIRE(image->Format == nvrhi::Format::RGBA16_FLOAT);
				const size_t texelCount = static_cast<size_t>(image->Width) * image->Height;
				REQUIRE(image->Pixels.size() == texelCount * 8);
				double worst = 0.0;
				for (size_t texel = 0; texel < texelCount; ++texel)
				{
					std::array<uint16_t, 4> value{};
					std::memcpy(value.data(), image->Pixels.data() + texel * 8, 8);
					for (size_t channel = 0; channel < 3; ++channel)
						worst = std::max(worst, std::abs(Test::HalfToDouble(value[channel]) - Color[channel]) / Color[channel]);
				}
				CHECK(worst < 1e-2);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("BloomPass: both storage formats create and record (the RGBA16_FLOAT fallback of §8.1)"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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
