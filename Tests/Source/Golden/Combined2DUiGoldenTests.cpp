#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// The combined 2D/UI golden (ADR 0028's phase-2 ledger, "Golden scenes and goldens": the later lander of M21 and M22 adds
// it; Docs/Decisions/0033-m22-contract.md, "Landing"): the scene of the golden scaffold
// Projects/FeatureTest/Scaffold/Combined2DUiGolden.jsonl (IdSeed 2090), Combined2DUi, rendered by Test::RenderGoldenScene.
// A PixelPerfectCamera renders its 200 x 120 reference resolution, which the 640 x 360 golden view upscales 3 times into
// 600 x 360 between two 20-pixel letterbox bars: three sprites (the Ground strip along the bottom 16 rows, the Hero standing
// on it and a Coin) and a Screen canvas laid out at the reference resolution, its Step button at scene pixels (8, 8) to
// (64, 26) with its label and a Counter label at the top right. The UI draws at the scene resolution before the upscale, so its pixels are 3 x 3 blocks as the sprites'
// are. Reference pixel (x, y) covers view pixels [20 + 3x, 23 + 3x) x [3y, 3y + 3). Its image is committed for the device
// class nvidia-62x; the upscale, the bars and the button's place are also checked on every device, in smoke mode too.
// Assets/Tests/Replays/Combined2DUi.replay presses the button in this scene through the same camera.

namespace Engine {

	namespace {

		// The RGBA8 pixel of `image` at (x, y).
		std::array<uint8_t, 4> ReadCombinedPixel(const Image& image, uint32_t x, uint32_t y)
		{
			const std::span<const std::byte> row = image.GetRow(y);
			std::array<uint8_t, 4> pixel{};
			for (size_t channel = 0; channel < pixel.size(); ++channel)
				pixel[channel] = static_cast<uint8_t>(row[static_cast<size_t>(x) * 4 + channel]);
			return pixel;
		}

		// Whether the 3 x 3 block of view pixels of reference pixel (x, y) is one colour.
		bool IsCombinedBlockUniform(const Image& image, uint32_t x, uint32_t y)
		{
			const std::array<uint8_t, 4> corner = ReadCombinedPixel(image, 20 + 3 * x, 3 * y);
			for (uint32_t dy = 0; dy < 3; ++dy)
			{
				for (uint32_t dx = 0; dx < 3; ++dx)
				{
					if (ReadCombinedPixel(image, 20 + 3 * x + dx, 3 * y + dy) != corner)
						return false;
				}
			}
			return true;
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: Combined2DUi")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Result<Image> image = Test::RenderGoldenScene(gpu, "Combined2DUi");
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->Width == 640);
			REQUIRE(image->Height == 360);
			// The letterbox bars beside the 600-pixel image are one colour, not the scene's.
			CHECK(ReadCombinedPixel(*image, 0, 0) == ReadCombinedPixel(*image, 19, 359));
			CHECK(ReadCombinedPixel(*image, 620, 0) == ReadCombinedPixel(*image, 639, 359));
			CHECK(ReadCombinedPixel(*image, 0, 0) != ReadCombinedPixel(*image, 20, 0));
			// The button, its corner and its interior, the sky, the Hero sprite (world (-3, -2.25), reference pixels 44 to 59 by
			// 88 to 103) and the Ground, each a uniform 3 x 3 block: the UI and the sprites are drawn at the reference resolution
			// and upscaled together.
			const std::array<uint8_t, 4> sky = ReadCombinedPixel(*image, 20 + 3 * 100, 3 * 40);
			const std::array<uint8_t, 4> button = ReadCombinedPixel(*image, 20 + 3 * 10, 3 * 10);
			const std::array<uint8_t, 4> hero = ReadCombinedPixel(*image, 20 + 3 * 52, 3 * 96);
			const std::array<uint8_t, 4> ground = ReadCombinedPixel(*image, 20 + 3 * 100, 3 * 112);
			for (const auto& [x, y] : { std::array<uint32_t, 2>{ 8, 8 }, std::array<uint32_t, 2>{ 10, 10 }, std::array<uint32_t, 2>{ 100, 40 },
					 std::array<uint32_t, 2>{ 52, 96 }, std::array<uint32_t, 2>{ 100, 112 } })
			{
				CAPTURE(x);
				CAPTURE(y);
				CHECK(IsCombinedBlockUniform(*image, x, y));
			}
			CHECK(ReadCombinedPixel(*image, 20 + 3 * 8, 3 * 8) == button);
			CHECK(button != sky);
			CHECK(hero != sky);
			CHECK(hero != button);
			CHECK(ground != sky);
			CHECK(ground != hero);
			// The button ends at scene pixel 64: the block after it shows the sky.
			CHECK(ReadCombinedPixel(*image, 20 + 3 * 64, 3 * 10) == sky);
			ENGINE_CHECK_GOLDEN("Combined2DUi", *image, gpu.GetDevice().GetInfo().DeviceClass);
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
