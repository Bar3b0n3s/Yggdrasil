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

// M21's 2D lighting golden (Roadmap M21, Docs/Decisions/0032-m21-contract.md decision 32): the scene of the golden scaffold
// Projects/FeatureTest/Scaffold/Lights2DGolden.jsonl, Lights2DPixelArt, rendered by Test::RenderGoldenScene. A
// PixelPerfectCamera renders its 200 x 120 reference resolution, which the 640 x 360 golden view upscales 3 times into
// 600 x 360 between two 20-pixel letterbox bars, in the 2D colour path: a lit wall under a dim global light and three
// point lights of falloff 0, 0.5 and 1, two unlit rounded sprites, one inside the hard-edged light and one outside every
// point light, a lit circle and a lit square with a normal map beside a raised point light, and a lit tilemap of three
// chunks under another. Its image is committed for the device class nvidia-61x; the upscale, the bars, the unlit
// sprites' texels and the hard edge are also checked on every device, in smoke mode too. Reference pixel (x, y), counted
// from the top left, covers world x in [-6.25 + x / 16, ...) and y in (3.75 - (y + 1) / 16, ...], and view pixels
// [20 + 3x, 23 + 3x) x [3y, 3y + 3).

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

		// The view pixel at the centre of the 3 x 3 block of reference pixel (x, y).
		std::array<uint8_t, 4> ReadReferencePixel(const Image& image, uint32_t x, uint32_t y)
		{
			return ReadPixel(image, 21 + x * 3, 1 + y * 3);
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: Lights2DPixelArt")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Result<Image> image = Test::RenderGoldenScene(gpu, "Lights2DPixelArt");
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->Width == 640);
			REQUIRE(image->Height == 360);
			// The integer upscale on every device: each reference pixel is a 3 x 3 block, and the bars beside the 600-pixel
			// image are one colour.
			for (uint32_t block = 0; block < 200; block += 7)
			{
				CAPTURE(block);
				const uint32_t x = 20 + block * 3;
				const std::array<uint8_t, 4> corner = ReadPixel(*image, x, 99);
				for (uint32_t dy = 0; dy < 3; ++dy)
				{
					for (uint32_t dx = 0; dx < 3; ++dx)
						CHECK(ReadPixel(*image, x + dx, 99 + dy) == corner);
				}
			}
			CHECK(ReadPixel(*image, 0, 0) == ReadPixel(*image, 19, 359));
			CHECK(ReadPixel(*image, 620, 0) == ReadPixel(*image, 639, 359));
			CHECK(ReadPixel(*image, 0, 0) != ReadPixel(*image, 20, 0));
			// Unlit sprites ignore every 2D light (decision 20): the same unlit sprite inside the hard-edged red light, at
			// world (-3.97, 1.72), and outside every point light, at world (-1.97, 2.97), shows the same texels.
			const std::array<uint8_t, 4> unlitInLight = ReadReferencePixel(*image, 36, 32);
			CHECK(unlitInLight == ReadReferencePixel(*image, 68, 12));
			// The lit wall takes the lights: inside the hard-edged light, at world (-4.97, 1.72), it is brighter than in the
			// global light alone, at world (-1.97, 0.97); just outside that light's radius of 1.5 (falloff 0, a hard edge), at
			// world (-3.97, 3.27), it has exactly the global light's colour.
			const std::array<uint8_t, 4> wallInShade = ReadReferencePixel(*image, 68, 44);
			const std::array<uint8_t, 4> wallInLight = ReadReferencePixel(*image, 20, 32);
			CHECK(wallInLight != wallInShade);
			CHECK(wallInLight[0] > wallInShade[0]);
			CHECK(ReadReferencePixel(*image, 36, 7) == wallInShade);
			CHECK(unlitInLight != wallInLight);
			ENGINE_CHECK_GOLDEN("Lights2DPixelArt", *image, gpu.GetDevice().GetInfo().DeviceClass);
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
