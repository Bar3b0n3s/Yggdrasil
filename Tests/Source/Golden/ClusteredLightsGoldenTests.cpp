#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <optional>
#include <set>
#include <string_view>

// The clustered-lights goldens (M23, R-INST and R-CLC; Docs/Decisions/0034-m23-contract.md decisions 2, 3, 5 and 17): the
// golden scaffold Scaffold/GoldenClusteredLights.jsonl (IdSeed 2106) writes
// Assets/Scenes/GoldenClusteredLights/ClusteredLights.scene, a floor with a 6 x 6 grid of blocks and nine orbs (two kinds
// of one mesh and one material each, so every pass draws each kind as one instanced draw) lit by three shadow-casting
// point lights, thirty small point lights and two spot lights. Its lit image and its LightCount debug view are goldens,
// and both reference paths render the lit image byte for byte.

namespace Engine {

	namespace {

		constexpr std::string_view ClusteredLightsGoldenScene = "ClusteredLights";
		constexpr std::string_view LightCountGolden = "ClusteredLightsLightCount";

		// The scene rendered as every golden is, with the shadow map size pinned (the golden-images skill: a shadow-casting
		// light changes with Rendering.ShadowMapSize, so the project's setting never changes the golden), then `edit`.
		[[nodiscard]] Image CaptureClusteredLights(Test::HeadlessGpuFixture& gpu, const std::function<void(RenderSnapshot&)>& edit = {},
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
			const Result<Image> image = Test::RenderGoldenScene(gpu, ClusteredLightsGoldenScene, options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->IsValid());
			CHECK(image->Width == 640);
			CHECK(image->Height == 360);
			return *image;
		}

		// The LightCount view's bands in palette order (GetLightCountDebugColor): 0 lights black, 1 to 2 blue, 3 to 4 cyan,
		// 5 to 8 green, 9 to 16 yellow, 17 to 32 orange, more red; display bytes of the unencoded data view.
		constexpr std::array<std::array<int, 3>, 7> LightCountBands{ { { 0, 0, 0 }, { 0, 0, 255 }, { 0, 255, 255 }, { 0, 255, 0 },
			{ 255, 255, 0 }, { 255, 128, 0 }, { 255, 0, 0 } } };

		// The band of a LightCount pixel, or nullopt for a colour outside the palette. A channel matches within one step,
		// since orange's 0.5 may store as 127 or 128 in the 8-bit target.
		[[nodiscard]] std::optional<size_t> LightCountBand(const Image& image, size_t pixel)
		{
			for (size_t band = 0; band < LightCountBands.size(); ++band)
			{
				bool matches = true;
				for (size_t channel = 0; channel < 3; ++channel)
					matches = matches && std::abs(std::to_integer<int>(image.Pixels[pixel * 4 + channel]) - LightCountBands[band][channel]) <= 1;
				if (matches)
					return band;
			}
			return std::nullopt;
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		// The lit image: every light of the scene reaches the snapshot; the per-draw render (Instancing false) and the
		// brute-force light loop (ClusteredLights false) each equal the instanced, clustered render byte for byte; the point
		// lights' shadows darken the image; and a second capture repeats the first exactly.
		TEST_CASE("Golden: ClusteredLights")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image clustered = CaptureClusteredLights(gpu, {}, [](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Quality.Instancing);
				CHECK(snapshot.Quality.ClusteredLights);
				CHECK(snapshot.Meshes.size() == 46);
				size_t shadowedPoints = 0;
				size_t points = 0;
				size_t spots = 0;
				for (const LightData& light : snapshot.Lights)
				{
					points += light.Type == RenderLightType::Point ? 1U : 0U;
					spots += light.Type == RenderLightType::Spot ? 1U : 0U;
					shadowedPoints += light.Type == RenderLightType::Point && light.CastShadows ? 1U : 0U;
				}
				CHECK(points == 33);
				CHECK(shadowedPoints == 3);
				CHECK(spots == 2);
				CHECK(snapshot.Lights.size() == 35);
			});
			CHECK(CaptureClusteredLights(gpu, [](RenderSnapshot& snapshot)
			{
				snapshot.Quality.Instancing = false;
			}).Pixels
				== clustered.Pixels);
			CHECK(CaptureClusteredLights(gpu, [](RenderSnapshot& snapshot)
			{
				snapshot.Quality.ClusteredLights = false;
			}).Pixels
				== clustered.Pixels);
			const Image unshadowed = CaptureClusteredLights(gpu, [](RenderSnapshot& snapshot)
			{
				for (LightData& light : snapshot.Lights)
					light.CastShadows = false;
			});
			REQUIRE(unshadowed.Pixels.size() == clustered.Pixels.size());
			size_t darker = 0;
			for (size_t pixel = 0; pixel < clustered.Pixels.size() / 4; ++pixel)
			{
				int delta = 0;
				for (size_t channel = 0; channel < 3; ++channel)
					delta = std::max(delta, std::to_integer<int>(unshadowed.Pixels[pixel * 4 + channel]) - std::to_integer<int>(clustered.Pixels[pixel * 4 + channel]));
				darker += delta > 4 ? 1U : 0U;
			}
			CAPTURE(darker);
			CHECK(darker > 1000);
			CHECK(CaptureClusteredLights(gpu).Pixels == clustered.Pixels);
			ENGINE_CHECK_GOLDEN(ClusteredLightsGoldenScene, clustered, gpu.GetDevice().GetInfo().DeviceClass);
		}

		// The LightCount view: every pixel is a colour of its palette, the clusters hold several bands of counts, and no
		// pixel's cluster holds more lights than the brute-force view's whole light list, which colours every surface alike;
		// the background is black in both.
		TEST_CASE("Golden: ClusteredLightsLightCount")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Image clustered = CaptureClusteredLights(gpu, [](RenderSnapshot& snapshot)
			{
				snapshot.DebugView = RenderDebugView::LightCount;
			});
			const Image bruteForce = CaptureClusteredLights(gpu, [](RenderSnapshot& snapshot)
			{
				snapshot.DebugView = RenderDebugView::LightCount;
				snapshot.Quality.ClusteredLights = false;
			});
			REQUIRE(bruteForce.Pixels.size() == clustered.Pixels.size());
			std::set<size_t> clusteredBands;
			std::set<size_t> bruteForceBands;
			size_t outside = 0;
			size_t above = 0;
			size_t backgroundMismatches = 0;
			for (size_t pixel = 0; pixel < clustered.Pixels.size() / 4; ++pixel)
			{
				const std::optional<size_t> band = LightCountBand(clustered, pixel);
				const std::optional<size_t> whole = LightCountBand(bruteForce, pixel);
				if (!band || !whole)
				{
					++outside;
					continue;
				}
				clusteredBands.insert(*band);
				bruteForceBands.insert(*whole);
				above += *band > *whole ? 1U : 0U;
				backgroundMismatches += (*band == 0) != (*whole == 0) ? 1U : 0U;
			}
			CAPTURE(outside);
			CAPTURE(above);
			CAPTURE(backgroundMismatches);
			CHECK(outside == 0);
			CHECK(above == 0);
			CHECK(backgroundMismatches == 0);
			// Black background and one colour for every surface: the whole list's count, beyond 16 lights.
			REQUIRE(bruteForceBands.size() == 2);
			CHECK(bruteForceBands.contains(0));
			CHECK(*bruteForceBands.rbegin() >= 5);
			// Black plus at least three bands of cluster counts, all below the whole list's.
			CHECK(clusteredBands.contains(0));
			CHECK(clusteredBands.size() >= 4);
			CHECK(*clusteredBands.rbegin() < *bruteForceBands.rbegin());
			CHECK(CaptureClusteredLights(gpu, [](RenderSnapshot& snapshot)
			{
				snapshot.DebugView = RenderDebugView::LightCount;
			}).Pixels
				== clustered.Pixels);
			ENGINE_CHECK_GOLDEN(LightCountGolden, clustered, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
