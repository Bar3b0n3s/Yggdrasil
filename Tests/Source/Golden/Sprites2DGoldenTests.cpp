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

// M21's pixel-art golden (Roadmap M21, Docs/Decisions/0032-m21-contract.md decision 17): the scene of the golden scaffold
// Projects/FeatureTest/Scaffold/Sprites2DGolden.jsonl, Sprites2DPixelArt, rendered by Test::RenderGoldenScene. A
// PixelPerfectCamera renders its 200 x 120 reference resolution, which the 640 x 360 golden view upscales 3 times into
// 600 x 360 between two 20-pixel letterbox bars: unlit sprites in the project's four sorting layers (created, and given
// OrderInLayer values, against the layers' order), sheet frames flipped in X and in Y, a whole texture and the four
// built-in sprite shapes. Its image is committed for the device class nvidia-61x; the upscale, the bars and the layer
// order are also checked on every device, in smoke mode too. Reference pixel (x, y), counted from the top left, covers
// world x in [-6.25 + x / 16, ...) and y in (3.75 - (y + 1) / 16, ...], and view pixels [20 + 3x, 23 + 3x) x [3y, 3y + 3).

namespace Engine {

	namespace {

		// The RGBA8 pixel of `image` at (x, y).
		std::array<uint8_t, 4> ReadPixel(const Image& image, uint32_t x, uint32_t y)
		{
			const std::span<const std::byte> row = image.GetRow(y);
			std::array<uint8_t, 4> pixel{};
			for (size_t channel = 0; channel < pixel.size(); ++channel)
				pixel[channel] = static_cast<uint8_t>(row[static_cast<size_t>(x) * 4 + channel]);
			return pixel;
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: Sprites2DPixelArt")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Result<Image> image = Test::RenderGoldenScene(gpu, "Sprites2DPixelArt");
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->Width == 640);
			REQUIRE(image->Height == 360);
			// The integer upscale on every device: each reference pixel is a 3 x 3 block, and the bars beside the 600-pixel
			// image are one colour.
			for (uint32_t block = 0; block < 200; block += 7)
			{
				CAPTURE(block);
				const uint32_t x = 20 + block * 3;
				const std::array<uint8_t, 4> corner = ReadPixel(*image, x, 150);
				for (uint32_t dy = 0; dy < 3; ++dy)
				{
					for (uint32_t dx = 0; dx < 3; ++dx)
						CHECK(ReadPixel(*image, x + dx, 150 + dy) == corner);
				}
			}
			CHECK(ReadPixel(*image, 0, 0) == ReadPixel(*image, 19, 359));
			CHECK(ReadPixel(*image, 620, 0) == ReadPixel(*image, 639, 359));
			CHECK(ReadPixel(*image, 0, 0) != ReadPixel(*image, 20, 0));
			// The sorting layers on every device: the opaque Backdrop (layer Background, OrderInLayer 20) is behind the Hero
			// (Characters, 0) and the translucent Overlay (Foreground, -10). Drawn by OrderInLayer alone, the Backdrop would
			// cover both, and their pixels would show its colour. The centres of the 3 x 3 blocks of the reference pixels at
			// world (-1.5, -0.5) (the Backdrop alone), (-0.75, 0.25) (the Hero) and (0.5, 1) (the Overlay over the Hero).
			const std::array<uint8_t, 4> backdrop = ReadPixel(*image, 249, 205);
			CHECK(ReadPixel(*image, 285, 169) != backdrop);
			CHECK(ReadPixel(*image, 345, 133) != backdrop);
			ENGINE_CHECK_GOLDEN("Sprites2DPixelArt", *image, gpu.GetDevice().GetInfo().DeviceClass);
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
