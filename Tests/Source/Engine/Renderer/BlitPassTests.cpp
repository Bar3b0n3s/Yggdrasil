#include "TestsPCH.h"

#include "Engine/Renderer/BlitPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/Readback.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

// The blit of §8.3 pass 15 (Architecture §8.3; Roadmap M7; Docs/Decisions/0012-m7-decisions.md decision 7).

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("BlitPass: draws a texture into a framebuffer of another format and size"
			* doctest::test_suite(Test::GpuSuite))
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

		TEST_CASE("BlitPass: keeps the source's orientation: each quadrant lands where it was" * doctest::test_suite(Test::GpuSuite))
		{
			// §8.3: the presented game view must not be flipped or mirrored (screenshots and goldens read LdrColor directly,
			// so only this test sees the blit's orientation). An 8x8 source whose quadrants differ top from bottom and left
			// from right, row 0 at the top as Readback returns it, blitted into a 16x16 target.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				constexpr uint32_t SourceSize = 8;
				constexpr uint32_t TargetSize = 16;
				// RGBA of the top-left, top-right, bottom-left and bottom-right quadrants.
				constexpr std::array<std::array<uint8_t, 4>, 4> Quadrants = { {
					{ 255, 0, 0, 255 },
					{ 0, 255, 0, 255 },
					{ 0, 0, 255, 255 },
					{ 255, 255, 255, 255 },
				} };
				const auto quadrantOf = [](uint32_t x, uint32_t y, uint32_t size) -> size_t
				{
					return (y < size / 2 ? 0u : 2u) + (x < size / 2 ? 0u : 1u);
				};
				std::vector<uint8_t> pixels(static_cast<size_t>(SourceSize) * SourceSize * 4);
				for (uint32_t y = 0; y < SourceSize; ++y)
				{
					for (uint32_t x = 0; x < SourceSize; ++x)
					{
						const std::array<uint8_t, 4>& color = Quadrants[quadrantOf(x, y, SourceSize)];
						std::copy(color.begin(), color.end(), pixels.begin() + static_cast<std::ptrdiff_t>((static_cast<size_t>(y) * SourceSize + x) * 4));
					}
				}

				Result<OffscreenTarget> source = OffscreenTarget::Create(device, { .Width = SourceSize, .Height = SourceSize, .DebugName = "BlitQuadrants" });
				REQUIRE_MESSAGE(source.has_value(), source.error().ToString());
				Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = TargetSize, .Height = TargetSize, .DebugName = "BlitTarget" });
				REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
				nvrhi::FramebufferInfo info;
				info.addColorFormat(nvrhi::Format::RGBA8_UNORM);
				Result<Scope<BlitPass>> blit = BlitPass::Create(device, gpu.GetPipelines(), info);
				REQUIRE_MESSAGE(blit.has_value(), blit.error().ToString());

				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				(*commandList)->open();
				(*commandList)->writeTexture(source->GetColorTexture(), 0, 0, pixels.data(), static_cast<size_t>(SourceSize) * 4);
				REQUIRE((*blit)->Record(**commandList, *source->GetColorTexture(), *target->GetFramebuffer()).has_value());
				(*commandList)->close();
				device.ExecuteCommandList(**commandList);

				Readback readback(device);
				const Result<Image> image = readback.ReadTexture(*target->GetColorTexture());
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				REQUIRE(image->Width == TargetSize);
				REQUIRE(image->Height == TargetSize);
				// The centre of each target quadrant, away from the edges a filtered blit blends.
				for (const auto& [x, y] : { std::pair{ 4u, 4u }, std::pair{ 12u, 4u }, std::pair{ 4u, 12u }, std::pair{ 12u, 12u } })
				{
					const std::array<uint8_t, 4>& expected = Quadrants[quadrantOf(x, y, TargetSize)];
					const std::span<const std::byte> row = image->GetRow(y);
					for (size_t channel = 0; channel < expected.size(); ++channel)
					{
						CAPTURE(x);
						CAPTURE(y);
						CAPTURE(channel);
						CHECK(std::abs(std::to_integer<int>(row[static_cast<size_t>(x) * 4 + channel]) - static_cast<int>(expected[channel])) <= 1);
					}
				}
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("BlitPass: refuses a framebuffer with depth" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			nvrhi::FramebufferInfo withDepth;
			withDepth.addColorFormat(nvrhi::Format::BGRA8_UNORM);
			withDepth.setDepthFormat(nvrhi::Format::D32);
			const Result<Scope<BlitPass>> refused = BlitPass::Create(gpu.GetDevice(), gpu.GetPipelines(), withDepth);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);

			// Not exactly one colour attachment.
			nvrhi::FramebufferInfo twoColors;
			twoColors.addColorFormat(nvrhi::Format::RGBA8_UNORM);
			twoColors.addColorFormat(nvrhi::Format::RGBA8_UNORM);
			const nvrhi::FramebufferInfo noColor;
			for (const nvrhi::FramebufferInfo& info : { twoColors, noColor })
			{
				const Result<Scope<BlitPass>> wrong = BlitPass::Create(gpu.GetDevice(), gpu.GetPipelines(), info);
				REQUIRE_FALSE(wrong.has_value());
				CHECK(wrong.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

		TEST_CASE("BlitPass: a new source texture replaces the previous one" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				// Two sources of different colours blitted in turn into one RGBA8 target: each blit shows its own source.
				Result<OffscreenTarget> red = OffscreenTarget::Create(device, { .Width = 4, .Height = 4, .DebugName = "BlitRed" });
				REQUIRE_MESSAGE(red.has_value(), red.error().ToString());
				Result<OffscreenTarget> blue = OffscreenTarget::Create(device, { .Width = 4, .Height = 4, .DebugName = "BlitBlue" });
				REQUIRE_MESSAGE(blue.has_value(), blue.error().ToString());
				Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 4, .Height = 4, .DebugName = "BlitTarget" });
				REQUIRE_MESSAGE(target.has_value(), target.error().ToString());

				nvrhi::FramebufferInfo info;
				info.addColorFormat(nvrhi::Format::RGBA8_UNORM);
				Result<Scope<BlitPass>> blit = BlitPass::Create(device, gpu.GetPipelines(), info);
				REQUIRE_MESSAGE(blit.has_value(), blit.error().ToString());

				Readback readback(device);
				const auto blitAndRead = [&device, &blit, &target, &readback](OffscreenTarget& source, const nvrhi::Color& color) -> Image
				{
					Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
					REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
					(*commandList)->open();
					(*commandList)->clearTextureFloat(source.GetColorTexture(), nvrhi::AllSubresources, color);
					REQUIRE((*blit)->Record(**commandList, *source.GetColorTexture(), *target->GetFramebuffer()).has_value());
					(*commandList)->close();
					device.ExecuteCommandList(**commandList);
					Result<Image> image = readback.ReadTexture(*target->GetColorTexture());
					REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
					return std::move(*image);
				};
				const Image first = blitAndRead(*red, nvrhi::Color(1.0f, 0.0f, 0.0f, 1.0f));
				CHECK(std::to_integer<int>(first.GetRow(2)[8]) == 255);
				CHECK(std::to_integer<int>(first.GetRow(2)[10]) == 0);
				const Image second = blitAndRead(*blue, nvrhi::Color(0.0f, 0.0f, 1.0f, 1.0f));
				CHECK(std::to_integer<int>(second.GetRow(2)[8]) == 0);
				CHECK(std::to_integer<int>(second.GetRow(2)[10]) == 255);
				const Image again = blitAndRead(*red, nvrhi::Color(1.0f, 0.0f, 0.0f, 1.0f));
				CHECK(again.Pixels == first.Pixels);
			}
			device.RunGarbageCollection();
		}
	}

}
