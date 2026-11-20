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

// The planar reflections' golden of M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3; Roadmap M42's
// acceptance), the scene of the golden scaffold Scaffold/GoldenPlanarReflections.jsonl (IdSeed 3902), rendered through
// Test::RenderGoldenScene: a sharp mirror (roughness 0.045) in a stone frame shows, mirrored, the back of the figure before
// it with the marker only the mirror shows, the ground whole and the sky in its upper part, and the green box that pokes
// through the glass, whose part behind the glass the clip removes; a directional light with cascades and a shadowed point
// light; fog; a rough mirror (roughness 0.35) whose reflection its mips blur; and a slab mirror turned away from the
// camera, which shows its fallback. The test also renders the scene without its planar reflections and checks that they
// changed the image, and renders it twice.

namespace Engine {

	namespace {

		// The golden scene `name` rendered as every golden is, after `edit`, with `inspect` observing the final snapshot.
		[[nodiscard]] Image CapturePlanarReflectionScene(Test::HeadlessGpuFixture& gpu, std::string_view name,
			const std::function<void(RenderSnapshot&)>& edit = {}, const std::function<void(const RenderSnapshot&)>& inspect = {})
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

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: PlanarReflections")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image mirrored = CapturePlanarReflectionScene(gpu, "PlanarReflections", {}, [](const RenderSnapshot& snapshot)
			{
				// The sharp, the rough and the turned-away mirror; the sun with its cascades, the shadowed lantern and the fog.
				CHECK(snapshot.PlanarReflections.size() == 3);
				CHECK(snapshot.Fog.Enabled);
				CHECK(std::ranges::any_of(snapshot.Lights, [](const LightData& light)
				{
					return light.Type == RenderLightType::Directional && light.CastShadows;
				}));
				CHECK(std::ranges::any_of(snapshot.Lights, [](const LightData& light)
				{
					return light.Type == RenderLightType::Point && light.CastShadows;
				}));
			});
			const Image fallback = CapturePlanarReflectionScene(gpu, "PlanarReflections", [](RenderSnapshot& snapshot)
			{
				snapshot.PlanarReflections.clear();
			});
			const size_t changed = CountChangedPixels(mirrored, fallback, 8);
			CAPTURE(changed);
			CHECK(changed > 10000);
			CHECK(CapturePlanarReflectionScene(gpu, "PlanarReflections").Pixels == mirrored.Pixels);
			ENGINE_CHECK_GOLDEN("PlanarReflections", mirrored, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
