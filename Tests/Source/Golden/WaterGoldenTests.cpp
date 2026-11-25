#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

// The water goldens of M47 (Docs/Decisions/0065-m47-water.md decision 3; Roadmap M47's acceptance): the scene of the golden
// scaffold Scaffold/GoldenWater.jsonl (IdSeed 4403), GoldenWater/Water.scene, an Ocean with a four-wave swell rolling onto a
// beach, through three of its cameras (Test::RenderGoldenScene):
//   - Water: the cove from the beach: the sand seen through the shallows and absorbed in the deep, foam on the shore and
//     round a rock, the swell to the horizon under a low shadowed sun (its glint), the Sky environment, SSR and fog;
//   - WaterUnderwater: its UnderwaterCamera, 1.5 m under the surface looking back at the rock and the beach: the
//     Underwater pass's fog, and the surface seen from below with Snell's window;
//   - WaterWaterline: its WaterlineCamera, the eye at the swell's height there at time 0, so the near plane straddles the
//     surface and one waterline crosses the view: the cove above it, the underwater view below.
// Each case also renders its view without the water and checks that the water changed it, and renders it twice.

namespace Engine {

	namespace {

		constexpr std::string_view WaterGoldenScene = "Water";

		// The water scene through the camera at `camera` (empty for the primary one), after `edit`, with `inspect` observing
		// the final snapshot.
		[[nodiscard]] Image CaptureWaterScene(Test::HeadlessGpuFixture& gpu, std::string_view camera, const std::function<void(RenderSnapshot&)>& edit = {},
			const std::function<void(const RenderSnapshot&)>& inspect = {})
		{
			const Test::GoldenSceneOptions options{ .EditSnapshot = edit, .InspectSnapshot = inspect, .Camera = std::string(camera) };
			const Result<Image> image = Test::RenderGoldenScene(gpu, WaterGoldenScene, options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->IsValid());
			CHECK(image->Width == 640);
			CHECK(image->Height == 360);
			return *image;
		}

		// How many pixels of `a` and `b` differ by more than `tolerance` levels in a colour channel.
		[[nodiscard]] size_t CountChangedPixels(const Image& a, const Image& b, int tolerance)
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
				changed += delta > tolerance ? 1U : 0U;
			}
			return changed;
		}

		// Renders the golden `golden` through `camera` (checked by `inspect`), the same view without its water, and the view
		// again; checks that the water changes more than `changedPixels` pixels and that the second render repeats the first,
		// then compares the golden.
		void CheckWaterGolden(Test::HeadlessGpuFixture& gpu, std::string_view golden, std::string_view camera, size_t changedPixels,
			const std::function<void(const RenderSnapshot&)>& inspect)
		{
			const Image wet = CaptureWaterScene(gpu, camera, {}, [&inspect](const RenderSnapshot& snapshot)
			{
				// The sea alone, its four waves; the low sun with its cascades, the fog and SSR.
				REQUIRE(snapshot.Waters.size() == 1);
				CHECK(snapshot.Waters.front().Shape == RenderWaterShape::Ocean);
				CHECK(snapshot.Waters.front().WaveCount == 4);
				CHECK(snapshot.Fog.Enabled);
				CHECK(snapshot.Post.SsrEnabled);
				CHECK(std::ranges::any_of(snapshot.Lights, [](const LightData& light)
				{
					return light.Type == RenderLightType::Directional && light.CastShadows;
				}));
				inspect(snapshot);
			});
			const Image dry = CaptureWaterScene(gpu, camera, [](RenderSnapshot& snapshot)
			{
				snapshot.Waters.clear();
			});
			const size_t changed = CountChangedPixels(wet, dry, 8);
			CAPTURE(changed);
			CHECK(changed > changedPixels);
			CHECK(CaptureWaterScene(gpu, camera).Pixels == wet.Pixels);
			ENGINE_CHECK_GOLDEN(golden, wet, gpu.GetDevice().GetInfo().DeviceClass);
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: Water")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			CheckWaterGolden(gpu, "Water", {}, 20000, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Camera.Position.y > 2.0f); // above the beach
			});
		}

		TEST_CASE("Golden: WaterUnderwater")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			CheckWaterGolden(gpu, "WaterUnderwater", "/UnderwaterCamera", 100000, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Camera.Position.y < -1.0f); // under every crest
			});
		}

		TEST_CASE("Golden: WaterWaterline")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			CheckWaterGolden(gpu, "WaterWaterline", "/WaterlineCamera", 40000, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Camera.Position.y == doctest::Approx(-0.2f)); // at the swell's height there
			});
		}
	}

}
