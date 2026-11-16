#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <cstddef>
#include <functional>
#include <string_view>

// The light-heavy golden (M29, Docs/Decisions/0044-m29-light-limits.md decision 8; Roadmap M29's acceptance): the golden
// scaffold Scaffold/GoldenManyLights.jsonl (IdSeed 2602) writes Assets/Scenes/GoldenManyLights/ManyLights.scene, a floor of
// blocks under 1,000 small point lights, twelve shadow-casting point lights (four at full shadow size, eight at half) and
// twelve shadow-casting spot lights: 1,024 lights, rendered at a shaded-light cap of 1,024, every one shaded. Its lit image is
// the golden, and the brute-force light loop and the per-draw render equal the clustered, instanced render byte for byte.

namespace Engine {

	namespace {

		constexpr std::string_view ManyLightsGoldenScene = "ManyLights";

		// The scene rendered as every golden is, with the shadow map size and the cap pinned (the project's settings never
		// change the golden), then `edit`.
		[[nodiscard]] Image CaptureManyLights(Test::HeadlessGpuFixture& gpu, const std::function<void(RenderSnapshot&)>& edit = {},
			const std::function<void(const RenderSnapshot&)>& inspect = {})
		{
			const Test::GoldenSceneOptions options{ .EditSnapshot = [&edit](RenderSnapshot& snapshot)
			{
				snapshot.Quality.ShadowMapSize = 1024;
				snapshot.Quality.MaxShadedLights = 1024;
				if (edit)
					edit(snapshot);
			},
				.InspectSnapshot = inspect };
			const Result<Image> image = Test::RenderGoldenScene(gpu, ManyLightsGoldenScene, options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->IsValid());
			CHECK(image->Width == 640);
			CHECK(image->Height == 360);
			return *image;
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: ManyLights")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image clustered = CaptureManyLights(gpu, {}, [](const RenderSnapshot& snapshot)
			{
				size_t points = 0;
				size_t shadowedPoints = 0;
				size_t shadowedSpots = 0;
				for (const LightData& light : snapshot.Lights)
				{
					points += light.Type == RenderLightType::Point ? 1U : 0U;
					shadowedPoints += light.Type == RenderLightType::Point && light.CastShadows ? 1U : 0U;
					shadowedSpots += light.Type == RenderLightType::Spot && light.CastShadows ? 1U : 0U;
				}
				CHECK(points == 1012);
				CHECK(shadowedPoints == 12);
				CHECK(shadowedSpots == 12);
				CHECK(snapshot.Lights.size() == 1024);
				// The renderer's light list at the cap: no light dropped (so no RENDER_LIGHT_LIMIT_EXCEEDED), where the old cap
				// of 256 would drop hundreds of the lights the view sees.
				const LightCullResult culled = CullLights(snapshot.Lights, snapshot.Camera, snapshot.Quality.MaxShadedLights);
				CHECK(culled.Dropped == 0);
				CHECK(culled.Visible.size() > 900);
				CHECK(CullLights(snapshot.Lights, snapshot.Camera, DefaultMaxShadedLights).Dropped > 600);
			});
			CHECK(CaptureManyLights(gpu, [](RenderSnapshot& snapshot)
			{
				snapshot.Quality.ClusteredLights = false;
			}).Pixels
				== clustered.Pixels);
			CHECK(CaptureManyLights(gpu, [](RenderSnapshot& snapshot)
			{
				snapshot.Quality.Instancing = false;
			}).Pixels
				== clustered.Pixels);
			CHECK(CaptureManyLights(gpu).Pixels == clustered.Pixels);
			ENGINE_CHECK_GOLDEN(ManyLightsGoldenScene, clustered, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
