#include "TestsPCH.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/glm.hpp>

#include <cmath>
#include <functional>
#include <span>
#include <vector>

// The sky's goldens of M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 6; Roadmap M45's acceptance), the scene
// of the golden scaffold Scaffold/GoldenSky.jsonl (IdSeed 4203), rendered through Test::RenderGoldenScene: SkyDay.scene, a
// cove (sand, a glossy water plane that reflects the sky's image-based light as M47's water will, a rock, a hut) under a
// Sky whose Sun and Moon are two shadow-casting directional lights, with haze, a cloud layer and automatic exposure.
//   - SkyDay: six 320 x 180 tiles, the sun at dawn, in the morning, at noon, in the afternoon and at dusk, and the night with
//     the moon and the stars, the sun (and at night the moon) turned in the extracted snapshot and the lights driven again
//     (ApplySkyLightDriving; the roles stay resolved, so no ResolveSkyLights, whose precondition is the authored colours).
//   - SkyOvercast: two tiles, a thick haze with broken clouds and the storm's full overcast, the sun in the morning.
// Each grid is rendered twice, to the same bytes.

namespace Engine {

	namespace {

		// The unit direction toward a body `elevation` degrees above the horizon at `azimuth` degrees from -Z toward +X.
		[[nodiscard]] glm::vec3 SkyDirection(float elevation, float azimuth)
		{
			const float up = glm::radians(elevation);
			const float around = glm::radians(azimuth);
			return glm::vec3(std::sin(around) * std::cos(up), std::sin(up), -std::cos(around) * std::cos(up));
		}

		// One tile of the cove: the sun toward `sun`, the moon toward `moon`, and `edit` applied to the Sky before the lights are
		// driven again.
		struct SkyTile
		{
			glm::vec3 Sun{};
			glm::vec3 Moon{};
			std::function<void(RenderSky&)> Edit{};
		};

		// The cove's tiles rendered at 320 x 180 and laid out in `columns` columns.
		[[nodiscard]] Image RenderSkyTiles(Test::HeadlessGpuFixture& gpu, std::span<const SkyTile> tiles, uint32_t columns)
		{
			std::vector<Image> images;
			const auto inspect = [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Sky.Enabled);
				CHECK(snapshot.Sky.HasMoon);
				CHECK(snapshot.Sky.DriveLights);
				CHECK(snapshot.Camera.ClearToSkybox);
			};
			for (const SkyTile& tile : tiles)
			{
				const auto edit = [&tile](RenderSnapshot& snapshot)
				{
					REQUIRE(snapshot.Sky.SunLight.has_value());
					REQUIRE(snapshot.Sky.MoonLight.has_value());
					snapshot.Lights[*snapshot.Sky.SunLight].Direction = -tile.Sun;
					snapshot.Lights[*snapshot.Sky.MoonLight].Direction = -tile.Moon;
					if (tile.Edit)
						tile.Edit(snapshot.Sky);
					ApplySkyLightDriving(snapshot);
				};
				const Test::GoldenSceneOptions options{ .Width = 320, .Height = 180, .EditSnapshot = edit, .InspectSnapshot = inspect };
				Result<Image> image = Test::RenderGoldenScene(gpu, "SkyDay", options);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				REQUIRE(image->IsValid());
				images.push_back(std::move(*image));
			}
			Result<Image> grid = Test::ComposeImageGrid(images, columns);
			REQUIRE_MESSAGE(grid.has_value(), grid.error().ToString());
			return std::move(*grid);
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: SkyDay")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// The moon stands where the night tile shows it all day, gibbous against the night's sun below the horizon.
			const glm::vec3 moon = SkyDirection(25.0f, -15.0f);
			const std::vector<SkyTile> tiles = {
				{ .Sun = SkyDirection(2.0f, -40.0f), .Moon = moon },   // dawn
				{ .Sun = SkyDirection(20.0f, -30.0f), .Moon = moon },  // morning
				{ .Sun = SkyDirection(65.0f, 0.0f), .Moon = moon },    // noon
				{ .Sun = SkyDirection(25.0f, 35.0f), .Moon = moon },   // afternoon
				{ .Sun = SkyDirection(1.0f, 42.0f), .Moon = moon },    // dusk
				{ .Sun = SkyDirection(-25.0f, 100.0f), .Moon = moon }, // night: the moon and the stars
			};
			const Image day = RenderSkyTiles(gpu, tiles, 3);
			CHECK(day.Width == 960);
			CHECK(day.Height == 360);
			CHECK(RenderSkyTiles(gpu, tiles, 3).Pixels == day.Pixels);
			ENGINE_CHECK_GOLDEN("SkyDay", day, gpu.GetDevice().GetInfo().DeviceClass);
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("Golden: SkyOvercast")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const glm::vec3 sun = SkyDirection(30.0f, -25.0f);
			const glm::vec3 moon = SkyDirection(25.0f, -15.0f);
			const auto haze = [](RenderSky& sky)
			{
				sky.Haze = 3.0f;
				sky.Clouds = 0.6f;
			};
			const auto storm = [](RenderSky& sky)
			{
				sky.Overcast = 1.0f;
				sky.Clouds = 1.0f;
			};
			const std::vector<SkyTile> tiles = { { .Sun = sun, .Moon = moon, .Edit = haze }, { .Sun = sun, .Moon = moon, .Edit = storm } };
			const Image overcast = RenderSkyTiles(gpu, tiles, 2);
			CHECK(overcast.Width == 640);
			CHECK(overcast.Height == 180);
			CHECK(RenderSkyTiles(gpu, tiles, 2).Pixels == overcast.Pixels);
			ENGINE_CHECK_GOLDEN("SkyOvercast", overcast, gpu.GetDevice().GetInfo().DeviceClass);
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
