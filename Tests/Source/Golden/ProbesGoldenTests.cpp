#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

// The Probes golden (M26, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decisions 8, 16 and 25): the golden scaffold
// Scaffold/GoldenProbes.jsonl (IdSeed 2404) writes Assets/Scenes/GoldenProbes/Probes.scene, a room open at the front, its
// left wall red and the rest white, lit by one point light close to the red wall, with a chrome ball, an IrradianceVolume
// over the room and a box-projected ReflectionProbe of the room's box, and bakes them through lighting.bake in its
// rendering editor into Assets/Lighting/Scenes/GoldenProbes/Probes/ (the volume's .irradiance and the reflection's .hdr,
// imported as an Environment). The golden shows the red wall's light bouncing onto the white walls and floor through the
// volume's probes (the colour bleed), and the room reflected in the ball through the probe's box projection.

namespace Engine {

	namespace {

		constexpr std::string_view ProbesGoldenScene = "Probes";

		// The scene rendered as every golden is, after `edit`, then `inspect`.
		[[nodiscard]] Image CaptureProbes(Test::HeadlessGpuFixture& gpu, const std::function<void(RenderSnapshot&)>& edit = {},
			const std::function<void(const RenderSnapshot&)>& inspect = {})
		{
			const Test::GoldenSceneOptions options{ .EditSnapshot = [&edit](RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Post.AntiAliasing == RenderAntiAliasing::Fxaa);
				if (edit)
					edit(snapshot);
			},
				.InspectSnapshot = inspect };
			const Result<Image> image = Test::RenderGoldenScene(gpu, ProbesGoldenScene, options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->IsValid());
			CHECK(image->Width == 640);
			CHECK(image->Height == 360);
			return *image;
		}

		// The right wall's patch of the image, far from the red wall and from the lamp: x in [540, 600), y in [110, 190).
		constexpr uint32_t RightWallLeft = 540;
		constexpr uint32_t RightWallRight = 600;
		constexpr uint32_t RightWallTop = 110;
		constexpr uint32_t RightWallBottom = 190;

		// How much redder than green the right wall's patch of `image` is: the mean of its red channel minus the mean of its
		// green one, in 8-bit levels.
		[[nodiscard]] double GetRightWallRedBias(const Image& image)
		{
			REQUIRE(image.Width >= RightWallRight);
			REQUIRE(image.Height >= RightWallBottom);
			int64_t bias = 0;
			for (uint32_t y = RightWallTop; y < RightWallBottom; ++y)
			{
				for (uint32_t x = RightWallLeft; x < RightWallRight; ++x)
				{
					const size_t pixel = (static_cast<size_t>(y) * image.Width + x) * 4;
					bias += std::to_integer<int>(image.Pixels[pixel]) - std::to_integer<int>(image.Pixels[pixel + 1]);
				}
			}
			return static_cast<double>(bias) / ((RightWallRight - RightWallLeft) * (RightWallBottom - RightWallTop));
		}

		// How many pixels of `a` and `b` differ by more than 4/255 in a colour channel.
		[[nodiscard]] size_t CountChangedPixels(const Image& a, const Image& b)
		{
			REQUIRE(a.Pixels.size() == b.Pixels.size());
			size_t changed = 0;
			for (size_t pixel = 0; pixel < a.Pixels.size() / 4; ++pixel)
			{
				int delta = 0;
				for (size_t channel = 0; channel < 3; ++channel)
				{
					const int left = std::to_integer<int>(a.Pixels[pixel * 4 + channel]);
					const int right = std::to_integer<int>(b.Pixels[pixel * 4 + channel]);
					delta = std::max(delta, left > right ? left - right : right - left);
				}
				changed += delta > 4 ? 1U : 0U;
			}
			return changed;
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		// The committed bake reaches the image: without the volume (the Environment's ambient alone) and without the box
		// projection the image differs clearly; the white right wall, which the lamp lights white and from afar, is tinted red
		// by the red wall's bounce only through the volume (the colour bleed); and a second capture repeats the first exactly.
		TEST_CASE("Golden: Probes")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image probes = CaptureProbes(gpu, {}, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Quality.Probes);
				REQUIRE(snapshot.IrradianceVolumes.size() == 1);
				REQUIRE(snapshot.ReflectionProbes.size() == 1);
				CHECK(snapshot.IrradianceVolumes[0].Data.IsValid());
				CHECK(snapshot.ReflectionProbes[0].Data.IsValid());
				CHECK(snapshot.ReflectionProbes[0].BoxProjection);
			});
			const Image withoutVolume = CaptureProbes(gpu, [](RenderSnapshot& snapshot)
			{
				snapshot.IrradianceVolumes.clear();
			});
			const Image withoutBoxProjection = CaptureProbes(gpu, [](RenderSnapshot& snapshot)
			{
				REQUIRE(snapshot.ReflectionProbes.size() == 1);
				snapshot.ReflectionProbes[0].BoxProjection = false;
			});
			const size_t volumeChanged = CountChangedPixels(probes, withoutVolume);
			const size_t projectionChanged = CountChangedPixels(probes, withoutBoxProjection);
			CAPTURE(volumeChanged);
			CAPTURE(projectionChanged);
			CHECK(volumeChanged > 1000);
			CHECK(projectionChanged > 200);
			const double bleed = GetRightWallRedBias(probes);
			const double withoutBleed = GetRightWallRedBias(withoutVolume);
			CAPTURE(bleed);
			CAPTURE(withoutBleed);
			CHECK(bleed > 6.0);
			CHECK(withoutBleed < 3.0);
			CHECK(CaptureProbes(gpu).Pixels == probes.Pixels);
			ENGINE_CHECK_GOLDEN(ProbesGoldenScene, probes, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
