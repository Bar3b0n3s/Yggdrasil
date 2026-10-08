#include "TestsPCH.h"

#include "Engine/Renderer/BlitPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/Readback.h"
#include "Support/HeadlessGpuFixture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

// The blit of §8.3 pass 15 (Architecture §8.3; Roadmap M7). Skipped skeletons of the M7 contract
// (Docs/Decisions/0012-m7-decisions.md decision 7): stream B implements the pass and removes the skips.

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("BlitPass: draws a texture into a framebuffer of another format and size"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				// An 8x8 RGBA8 source of one known colour, blitted into a 16x16 BGRA8 target (the swapchain's format).
				Result<OffscreenTarget> source = OffscreenTarget::Create(device, { .Width = 8, .Height = 8, .DebugName = "BlitSource" });
				REQUIRE_MESSAGE(source.has_value(), source.error().ToString());
				Result<OffscreenTarget> target = OffscreenTarget::Create(device,
					{ .Width = 16, .Height = 16, .ColorFormat = nvrhi::Format::BGRA8_UNORM, .DebugName = "BlitTarget" });
				REQUIRE_MESSAGE(target.has_value(), target.error().ToString());

				nvrhi::FramebufferInfo info;
				info.addColorFormat(nvrhi::Format::BGRA8_UNORM);
				Result<Scope<BlitPass>> blit = BlitPass::Create(device, gpu.GetPipelines(), info);
				REQUIRE_MESSAGE(blit.has_value(), blit.error().ToString());

				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				(*commandList)->open();
				(*commandList)->clearTextureFloat(source->GetColorTexture(), nvrhi::AllSubresources, nvrhi::Color(1.0f, 0.5f, 0.25f, 1.0f));
				REQUIRE((*blit)->Record(**commandList, *source->GetColorTexture(), *target->GetFramebuffer()).has_value());
				(*commandList)->close();
				device.ExecuteCommandList(**commandList);

				Readback readback(device);
				const Result<Image> image = readback.ReadTexture(*target->GetColorTexture());
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				REQUIRE(image->Width == 16);
				REQUIRE(image->Height == 16);
				// Every pixel holds the source's values, unconverted, in BGRA order (within the UNORM rounding).
				const std::array<int, 4> expected = { 64, 128, 255, 255 };
				for (uint32_t y = 0; y < image->Height; ++y)
				{
					const std::span<const std::byte> row = image->GetRow(y);
					for (uint32_t x = 0; x < image->Width; ++x)
					{
						for (size_t channel = 0; channel < expected.size(); ++channel)
						{
							CAPTURE(x);
							CAPTURE(y);
							CAPTURE(channel);
							CHECK(std::abs(std::to_integer<int>(row[static_cast<size_t>(x) * 4 + channel]) - expected[channel]) <= 1);
						}
					}
				}
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("BlitPass: refuses a framebuffer with depth" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			nvrhi::FramebufferInfo withDepth;
			withDepth.addColorFormat(nvrhi::Format::BGRA8_UNORM);
			withDepth.setDepthFormat(nvrhi::Format::D32);
			const Result<Scope<BlitPass>> refused = BlitPass::Create(gpu.GetDevice(), gpu.GetPipelines(), withDepth);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
