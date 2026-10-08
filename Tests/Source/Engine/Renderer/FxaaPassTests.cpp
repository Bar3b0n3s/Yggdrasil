#include "TestsPCH.h"

#include "Engine/Renderer/FxaaPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Support/HeadlessGpuFixture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// FXAA 3.11 (Architecture §8.3 pass 12; Docs/Decisions/0013-m8-decisions.md decision 8) on small synthetic LDR images.

namespace Engine {

	namespace {

		constexpr uint32_t Size = 32;

		using TexelFunction = std::function<std::array<uint8_t, 4>(uint32_t column, uint32_t row)>;

		// An LDR source of Size² texels from `texel` and an LDR destination of the same size.
		std::array<nvrhi::TextureHandle, 2> CreateTargets(GraphicsDevice& device, const TexelFunction& texel)
		{
			std::vector<std::array<uint8_t, 4>> texels(static_cast<size_t>(Size) * Size);
			for (uint32_t row = 0; row < Size; ++row)
			{
				for (uint32_t column = 0; column < Size; ++column)
					texels[static_cast<size_t>(row) * Size + column] = texel(column, row);
			}
			nvrhi::TextureDesc desc;
			desc.width = Size;
			desc.height = Size;
			desc.format = LdrColorFormat;
			desc.isShaderResource = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "FxaaPassTests.Source";
			const std::array<TextureSubresourceData, 1> subresources = { { { .Data = std::as_bytes(std::span(texels)) } } };
			Result<TextureUpload> source = device.GetHostImageUpload().CreateTexture(desc, subresources, TextureUploadPath::Staging);
			REQUIRE_MESSAGE(source.has_value(), source.error().ToString());
			desc.isUAV = true;
			desc.debugName = "FxaaPassTests.Destination";
			Result<nvrhi::TextureHandle> destination = device.CreateTexture(desc);
			REQUIRE(destination.has_value());
			return { source->Texture, *destination };
		}

		// Records the pass from targets[0] into targets[1], executes it and reads both back.
		std::array<Image, 2> RunFxaa(GraphicsDevice& device, FxaaPass& pass, const std::array<nvrhi::TextureHandle, 2>& targets)
		{
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE(commandList.has_value());
			(*commandList)->open();
			PassBindingCache bindings;
			const Result<nvrhi::ITexture*> result = pass.Record(**commandList, bindings, { .Source = targets[0], .Destination = targets[1] });
			(*commandList)->close();
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			REQUIRE(*result == targets[1].Get());
			device.ExecuteCommandList(**commandList);

			Readback readback(device);
			Result<Image> source = readback.ReadTexture(*targets[0]);
			REQUIRE_MESSAGE(source.has_value(), source.error().ToString());
			Result<Image> destination = readback.ReadTexture(*targets[1]);
			REQUIRE_MESSAGE(destination.has_value(), destination.error().ToString());
			return { std::move(*source), std::move(*destination) };
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("FxaaPass: softens a hard edge and leaves flat areas alone" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<FxaaPass>> pass = FxaaPass::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				CHECK((*pass)->GetPipelineCount() == FxaaPass::PipelineCount);
				// A hard diagonal edge: white above the diagonal, black below.
				const std::array<nvrhi::TextureHandle, 2> targets = CreateTargets(device, [](uint32_t column, uint32_t row)
				{
					const uint8_t value = column > row ? 255 : 0;
					return std::array<uint8_t, 4>{ value, value, value, 255 };
				});
				const std::array<Image, 2> images = RunFxaa(device, **pass, targets);
				const Image& image = images[1];
				const auto red = [&image](uint32_t column, uint32_t row)
				{
					return std::to_integer<int>(image.GetRow(row)[static_cast<size_t>(column) * 4]);
				};
				// Flat areas far from the edge keep their values; texels along the edge become intermediate.
				CHECK(red(28, 2) == 255);
				CHECK(red(2, 28) == 0);
				bool blended = false;
				for (uint32_t index = 4; index < Size - 4; ++index)
					blended = blended || (red(index + 1, index) > 0 && red(index + 1, index) < 255) || (red(index, index) > 0 && red(index, index) < 255);
				CHECK(blended);
				// Alpha stays opaque.
				CHECK(std::to_integer<int>(image.GetRow(16)[16 * 4 + 3]) == 255);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("FxaaPass: a flat image and a gentle gradient pass through unchanged" * doctest::test_suite(Test::GpuSuite))
		{
			// Below FXAA's contrast thresholds (EdgeThresholdMin 0.0833, EdgeThreshold 0.166 of the local maximum) a texel takes
			// the early exit and keeps its exact value: a uniform colour, and a ramp rising one level per texel.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<FxaaPass>> pass = FxaaPass::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				const std::array<TexelFunction, 2> images = {
					[](uint32_t /*column*/, uint32_t /*row*/)
				{
					return std::array<uint8_t, 4>{ 100, 150, 200, 255 };
				},
					[](uint32_t column, uint32_t row)
				{
					const uint8_t value = static_cast<uint8_t>(96 + column + row);
					return std::array<uint8_t, 4>{ value, value, static_cast<uint8_t>(value / 2), 255 };
				},
				};
				for (size_t index = 0; index < images.size(); ++index)
				{
					CAPTURE(index);
					const std::array<nvrhi::TextureHandle, 2> targets = CreateTargets(device, images[index]);
					const std::array<Image, 2> result = RunFxaa(device, **pass, targets);
					CHECK(result[1].Pixels == result[0].Pixels);
				}
			}
			device.RunGarbageCollection();
		}
	}

}
