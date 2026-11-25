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

// M46's two goldens (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3M and 6; Roadmap M46's
// acceptance), the scenes of the golden scaffolds Scaffold/GoldenMeshLods.jsonl (IdSeed 4304) and Scaffold/GoldenWind.jsonl
// (IdSeed 4305), rendered through Test::RenderGoldenScene:
//   - MeshLods: LodTree.gltf's tree with its authored levels at three distances (level 0, the middle of its first switch's
//     fade band, its last level) and LodSphere.gltf's ball with generated levels at three distances, under a sun with
//     cascades; the nearest tree casts its last level's shadow (ShadowLodOffset 2), and a fourth tree beyond its
//     CullDistance draws and casts nothing. The test also renders the scene with every item at its finest level and without
//     a cull distance, and checks that the levels and the cull changed the image.
//   - Wind: a gale at time 0 (an edit-time image: the lean without the sway's motion) over a tree whose bark and leaves the
//     import's Wind setting gave a wind block and over a row of reeds, beside the same tree with materials without wind and
//     a post without wind. The test also renders the scene without its Wind and checks that the wind changed the image.
// Each renders its scene twice and compares the bytes.

namespace Engine {

	namespace {

		// The golden scene `name` rendered as every golden is, after `edit`, with `inspect` observing the final snapshot.
		[[nodiscard]] Image CaptureLodWindScene(Test::HeadlessGpuFixture& gpu, std::string_view name, const std::function<void(RenderSnapshot&)>& edit = {},
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

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: MeshLods")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image levels = CaptureLodWindScene(gpu, "MeshLods", {}, [](const RenderSnapshot& snapshot)
			{
				// The ground, four trees and three balls; one tree with a cull distance, one with a fade band of 0.4.
				CHECK(snapshot.Meshes.size() == 8);
				CHECK(std::ranges::count_if(snapshot.Meshes, [](const MeshDrawItem& item)
				{
					return item.Distance.CullDistance == 12.0f;
				}) == 1);
				CHECK(std::ranges::count_if(snapshot.Meshes, [](const MeshDrawItem& item)
				{
					return item.Distance.FadeWidth == 0.4f && item.Distance.ShadowLodOffset == 1;
				}) == 1);
				CHECK(std::ranges::any_of(snapshot.Lights, [](const LightData& light)
				{
					return light.Type == RenderLightType::Directional && light.CastShadows;
				}));
			});
			// Every item at its finest level, shadows from it too, nothing culled by distance.
			const Image finest = CaptureLodWindScene(gpu, "MeshLods", [](RenderSnapshot& snapshot)
			{
				for (MeshDrawItem& item : snapshot.Meshes)
					item.Distance = MeshDrawDistance{ .CullDistance = 0.0f, .LodBias = 100.0f, .ShadowLodOffset = 0, .FadeWidth = 0.0f };
			});
			const size_t changed = CountChangedPixels(levels, finest, 8);
			CAPTURE(changed);
			CHECK(changed > 1000);
			CHECK(CaptureLodWindScene(gpu, "MeshLods").Pixels == levels.Pixels);
			ENGINE_CHECK_GOLDEN("MeshLods", levels, gpu.GetDevice().GetInfo().DeviceClass);
		}

		TEST_CASE("Golden: Wind")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image windy = CaptureLodWindScene(gpu, "Wind", {}, [](const RenderSnapshot& snapshot)
			{
				// The gale at an edit-time image's time 0.
				CHECK(snapshot.Wind.Enabled);
				CHECK(snapshot.Wind.Strength == 20.0f);
				CHECK(snapshot.Wind.Time == 0.0);
			});
			const Image still = CaptureLodWindScene(gpu, "Wind", [](RenderSnapshot& snapshot)
			{
				snapshot.Wind.Enabled = false;
			});
			const size_t changed = CountChangedPixels(windy, still, 8);
			CAPTURE(changed);
			CHECK(changed > 2000);
			CHECK(CaptureLodWindScene(gpu, "Wind").Pixels == windy.Pixels);
			ENGINE_CHECK_GOLDEN("Wind", windy, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
