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
#include <span>
#include <vector>

// FXAA 3.11 (Architecture §8.3 pass 12). Skeletons of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 8);
// stream C implements the pass and removes the skips.

namespace Engine {

	namespace {

		constexpr uint32_t Size = 32;

		// An LDR source with a hard diagonal edge (white above the diagonal, black below) and an LDR destination.
		std::array<nvrhi::TextureHandle, 2> CreateEdgeTargets(GraphicsDevice& device)
		{
			std::vector<std::array<uint8_t, 4>> texels(static_cast<size_t>(Size) * Size);
			for (uint32_t row = 0; row < Size; ++row)
			{
				for (uint32_t column = 0; column < Size; ++column)
				{
					const uint8_t value = column > row ? 255 : 0;
					texels[static_cast<size_t>(row) * Size + column] = { value, value, value, 255 };
				}
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

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("FxaaPass: softens a hard edge and leaves flat areas alone" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<FxaaPass>> pass = FxaaPass::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				CHECK((*pass)->GetPipelineCount() == FxaaPass::PipelineCount);
				const std::array<nvrhi::TextureHandle, 2> targets = CreateEdgeTargets(device);
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE(commandList.has_value());
				(*commandList)->open();
				PassBindingCache bindings;
				const Result<nvrhi::ITexture*> result = (*pass)->Record(**commandList, bindings, { .Source = targets[0], .Destination = targets[1] });
				(*commandList)->close();
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				REQUIRE(*result == targets[1].Get());
				device.ExecuteCommandList(**commandList);

				Readback readback(device);
				const Result<Image> image = readback.ReadTexture(*targets[1]);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				const auto red = [&image](uint32_t column, uint32_t row)
				{
					return std::to_integer<int>(image->GetRow(row)[static_cast<size_t>(column) * 4]);
				};
				// Flat areas far from the edge keep their values; texels along the edge become intermediate.
				CHECK(red(28, 2) == 255);
				CHECK(red(2, 28) == 0);
				bool blended = false;
				for (uint32_t index = 4; index < Size - 4; ++index)
					blended = blended || (red(index + 1, index) > 0 && red(index + 1, index) < 255) || (red(index, index) > 0 && red(index, index) < 255);
				CHECK(blended);
			}
			device.RunGarbageCollection();
		}
	}

}
