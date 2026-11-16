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
#include <string_view>

// The fog goldens of M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 8; Roadmap M32's acceptance), the scenes of the golden
// scaffold Scaffold/GoldenFog.jsonl (IdSeed 2902), rendered through Test::RenderGoldenScene:
//   - FogPillars: two rows of pillars receding into distance fog, which also hides the far ground and the fogged sky;
//   - FogHollow: an isometric (orthographic) view of a hollow whose floor lies under height fog, the rim in clear air;
//   - FogSunGlow: looking toward a low sun through haze that glows around its direction, silhouettes against it;
//   - FogTransparent: a transparent pane, an emitting block and a sprite disc standing in fog before a wall.
// Each case also renders its scene with the fog off and checks that the fog changed the image, and renders it twice.

namespace Engine {

	namespace {

		// The golden scene `name` rendered as every golden is, after `edit`, with `inspect` observing the final snapshot.
		[[nodiscard]] Image CaptureFogScene(Test::HeadlessGpuFixture& gpu, std::string_view name, const std::function<void(RenderSnapshot&)>& edit = {},
			const std::function<void(const RenderSnapshot&)>& inspect = {})
		{
			const Test::GoldenSceneOptions options{ .EditSnapshot = edit, .InspectSnapshot = inspect };
			const Result<Image> image = Test::RenderGoldenScene(gpu, name, options);
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

		// Renders the fog golden `name` (checked by `inspect`), the same scene without its fog, and the scene again; checks that
		// the fog changes at least `changedPixels` pixels and that the second render repeats the first, then compares the
		// golden.
		void CheckFogGolden(Test::HeadlessGpuFixture& gpu, std::string_view name, size_t changedPixels, const std::function<void(const RenderSnapshot&)>& inspect)
		{
			const Image fogged = CaptureFogScene(gpu, name, {}, [&inspect](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Fog.Enabled);
				inspect(snapshot);
			});
			const Image clear = CaptureFogScene(gpu, name, [](RenderSnapshot& snapshot)
			{
				snapshot.Fog.Enabled = false;
			});
			const size_t changed = CountChangedPixels(fogged, clear, 8);
			CAPTURE(changed);
			CHECK(changed > changedPixels);
			CHECK(CaptureFogScene(gpu, name).Pixels == fogged.Pixels);
			ENGINE_CHECK_GOLDEN(name, fogged, gpu.GetDevice().GetInfo().DeviceClass);
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: FogPillars")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			CheckFogGolden(gpu, "FogPillars", 60000, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Camera.ProjectionKind == RenderProjection::Perspective);
				CHECK(snapshot.Fog.Density > 0.0f);
				CHECK(snapshot.Fog.FogSkybox);
			});
		}

		TEST_CASE("Golden: FogHollow")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			CheckFogGolden(gpu, "FogHollow", 20000, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Camera.ProjectionKind == RenderProjection::Orthographic);
				CHECK(snapshot.Fog.Density == 0.0f);
				CHECK(snapshot.Fog.HeightDensity > 0.0f);
			});
		}

		TEST_CASE("Golden: FogSunGlow")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			CheckFogGolden(gpu, "FogSunGlow", 60000, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Fog.SunGlow > 0.0f);
				CHECK(snapshot.Fog.FogSkybox);
				CHECK(std::ranges::any_of(snapshot.Lights, [](const LightData& light)
				{
					return light.Type == RenderLightType::Directional;
				}));
			});
		}

		TEST_CASE("Golden: FogTransparent")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			CheckFogGolden(gpu, "FogTransparent", 40000, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Sprites.size() == 1);
				CHECK(snapshot.Fog.AmbientTint > 0.0f);
			});
		}
	}

}
