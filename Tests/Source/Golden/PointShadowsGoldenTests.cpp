#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <string_view>

// The point-shadow golden (M23, R-PLS; Docs/Decisions/0034-m23-contract.md decisions 3 and 17): the golden scaffold
// Scaffold/GoldenPointShadows.jsonl (IdSeed 2104) writes Assets/Scenes/GoldenPointShadows/PointShadows.scene, a floor,
// a wall and four casters around two shadow-casting point lights whose shadows cross several faces of their cubes.

namespace Engine {

	namespace {

		constexpr std::string_view PointShadowsGoldenScene = "PointShadows";

		// The scene rendered as every golden is, with the shadow map size pinned (the golden-images skill: a shadow-casting
		// light changes with Rendering.ShadowMapSize, so the project's setting never changes the golden), then `edit`.
		[[nodiscard]] Image CapturePointShadows(Test::HeadlessGpuFixture& gpu, const std::function<void(RenderSnapshot&)>& edit = {},
			const std::function<void(const RenderSnapshot&)>& inspect = {})
		{
			const Test::GoldenSceneOptions options{ .EditSnapshot = [&edit](RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Post.FxaaEnabled);
				snapshot.Quality.ShadowMapSize = 1024;
				if (edit)
					edit(snapshot);
			},
				.InspectSnapshot = inspect };
			const Result<Image> image = Test::RenderGoldenScene(gpu, PointShadowsGoldenScene, options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->IsValid());
			CHECK(image->Width == 640);
			CHECK(image->Height == 360);
			return *image;
		}

		[[nodiscard]] int GoldenChannel(const Image& image, size_t pixel, size_t channel)
		{
			return std::to_integer<int>(image.Pixels[pixel * 4 + channel]);
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		// The scene's two point lights cast shadows: turning their shadows off brightens many pixels clearly, as does making
		// every mesh stop casting or receiving, which renders exactly the unshadowed image; a second capture repeats the
		// first exactly.
		TEST_CASE("Golden: PointShadows")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image shadowed = CapturePointShadows(gpu, {}, [](const RenderSnapshot& snapshot)
			{
				REQUIRE(snapshot.Lights.size() == 2);
				for (const LightData& light : snapshot.Lights)
				{
					CHECK(light.Type == RenderLightType::Point);
					CHECK(light.CastShadows);
				}
			});
			const Image unshadowed = CapturePointShadows(gpu, [](RenderSnapshot& snapshot)
			{
				for (LightData& light : snapshot.Lights)
					light.CastShadows = false;
			});
			REQUIRE(shadowed.Pixels.size() == unshadowed.Pixels.size());
			size_t darker = 0;
			int largest = 0;
			for (size_t pixel = 0; pixel < shadowed.Pixels.size() / 4; ++pixel)
			{
				int delta = 0;
				for (size_t channel = 0; channel < 3; ++channel)
					delta = std::max(delta, GoldenChannel(unshadowed, pixel, channel) - GoldenChannel(shadowed, pixel, channel));
				largest = std::max(largest, delta);
				darker += delta > 4 ? 1U : 0U;
			}
			CAPTURE(darker);
			CAPTURE(largest);
			CHECK(darker > 2000);
			CHECK(largest > 24);
			CHECK(CapturePointShadows(gpu, [](RenderSnapshot& snapshot)
			{
				for (MeshDrawItem& mesh : snapshot.Meshes)
					mesh.ReceiveShadows = false;
			}).Pixels
				== unshadowed.Pixels);
			CHECK(CapturePointShadows(gpu, [](RenderSnapshot& snapshot)
			{
				for (MeshDrawItem& mesh : snapshot.Meshes)
					mesh.CastShadows = false;
			}).Pixels
				== unshadowed.Pixels);
			CHECK(CapturePointShadows(gpu).Pixels == shadowed.Pixels);
			ENGINE_CHECK_GOLDEN(PointShadowsGoldenScene, shadowed, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
