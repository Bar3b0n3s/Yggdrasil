#include "TestsPCH.h"

#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/SpotShadowAtlas.h"

#include <algorithm>
#include <limits>
#include <numeric>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static CameraData SpotTestCamera()
		{
			CameraData camera;
			camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60, 10, 0.1f, 1000, 100, 100);
			return camera;
		}

		[[nodiscard]] static std::vector<LightData> SpotTestLights(uint32_t count)
		{
			std::vector<LightData> lights(count);
			for (uint32_t index = 0; index < count; ++index)
			{
				lights[index].Type = RenderLightType::Spot;
				lights[index].CastShadows = true;
				lights[index].Entity = UUID(count - index);
			}
			return lights;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SpotShadowAtlas: importance ties use UUID and tiles do not overlap")
		{
			const auto lights = Utils::SpotTestLights(8);
			std::vector<uint32_t> indices{ 0, 1, 2, 3, 4, 5, 6, 7 };
			const auto atlas = AllocateSpotShadowAtlas(lights, indices, Utils::SpotTestCamera());
			REQUIRE(atlas);
			REQUIRE(atlas->Tiles.size() == 8);
			for (uint32_t index = 0; index < 8; ++index)
			{
				const auto& tile = atlas->Tiles[index];
				CHECK(tile.LightIndex == 7 - index);
				CHECK(tile.X == index % 4 * 1024);
				CHECK(tile.Y == index / 4 * 1024);
				CHECK(tile.UvScaleBias.x == 1020.0f / 4096);
				CHECK(tile.UvScaleBias.z == static_cast<float>(tile.X + 2) / 4096);
				CHECK(tile.UvScaleBias.w == static_cast<float>(tile.Y + 2) / 4096);
				const glm::vec4 nearPoint = tile.ViewProjection * glm::vec4(0, 0, -tile.NearClip, 1);
				const glm::vec4 farPoint = tile.ViewProjection * glm::vec4(0, 0, -tile.FarClip, 1);
				CHECK(nearPoint.z / nearPoint.w == doctest::Approx(1));
				CHECK(farPoint.z / farPoint.w == doctest::Approx(0).epsilon(0.000001));
			}
			std::ranges::reverse(indices);
			const auto reversed = AllocateSpotShadowAtlas(lights, indices, Utils::SpotTestCamera());
			REQUIRE(reversed);
			for (size_t index = 0; index < atlas->Tiles.size(); ++index)
				CHECK(reversed->Tiles[index].LightIndex == atlas->Tiles[index].LightIndex);
		}

		TEST_CASE("SpotShadowAtlas: excess shadowed spots are reported without dropping their light")
		{
			auto lights = Utils::SpotTestLights(MaxSpotShadowLights + 1);
			lights[0].Intensity = 10;
			const auto visible = CullLights(lights, Utils::SpotTestCamera());
			REQUIRE(visible.Visible.size() == MaxSpotShadowLights + 1);
			const auto atlas = AllocateSpotShadowAtlas(lights, visible.Visible, Utils::SpotTestCamera());
			REQUIRE(atlas);
			CHECK(atlas->Dropped == 1);
			REQUIRE(atlas->Tiles.size() == MaxSpotShadowLights);
			for (size_t index = 0; index < MaxSpotShadowLights; ++index)
				CHECK(atlas->Tiles[index].LightIndex == visible.Visible[index]);
			CHECK(visible.Dropped == 0);
			lights[1].Type = RenderLightType::Point;
			lights[2].CastShadows = false;
			const auto filtered = AllocateSpotShadowAtlas(lights, visible.Visible, Utils::SpotTestCamera());
			REQUIRE(filtered);
			CHECK(filtered->Tiles.size() == MaxSpotShadowLights - 1);
			CHECK(filtered->Dropped == 0);
		}

		// M29 (Docs/Decisions/0044-m29-light-limits.md decisions 2 and 3): the atlas's sixteen tiles take sixteen spots, slot t
		// at (t % 4, t / 4) * 1024 in importance order after a reset (the first eight where M23 put its eight); at the cap, a
		// spot the previous atlas shadowed keeps its shadow against a newcomer less than a quarter more important, and every kept
		// spot keeps its slot while a newcomer takes the freed one.
		TEST_CASE("SpotShadowAtlas: sixteen spot lights take the atlas's sixteen tiles, and a kept light keeps its tile")
		{
			CHECK(MaxSpotShadowLights == 16);
			auto lights = Utils::SpotTestLights(MaxSpotShadowLights + 1);
			for (uint32_t index = 0; index <= MaxSpotShadowLights; ++index)
				lights[index].Intensity = 100.0f - static_cast<float>(index); // the seventeenth, at 84, is the least important
			const CameraData camera = Utils::SpotTestCamera();
			const auto visible = CullLights(lights, camera);
			const auto first = AllocateSpotShadowAtlas(lights, visible.Visible, camera);
			REQUIRE(first);
			REQUIRE(first->Tiles.size() == MaxSpotShadowLights);
			CHECK(first->Dropped == 1);
			for (uint32_t index = 0; index < MaxSpotShadowLights; ++index)
			{
				const SpotShadowTile& tile = first->Tiles[index];
				CHECK(tile.LightIndex == index);
				CHECK(tile.Entity == lights[index].Entity);
				CHECK(tile.Slot == index);
				CHECK(tile.X == index % 4 * 1024);
				CHECK(tile.Y == index / 4 * 1024);
			}

			// The dropped spot grows to 95, above the weakest shadowed (85) but below 1.25 times it: kept out with the previous
			// atlas, in without it.
			lights[MaxSpotShadowLights].Intensity = 95.0f;
			const auto contested = CullLights(lights, camera);
			const auto kept = AllocateSpotShadowAtlas(lights, contested.Visible, camera, &*first);
			REQUIRE(kept);
			CHECK(std::ranges::find(kept->Tiles, MaxSpotShadowLights, &SpotShadowTile::LightIndex) == kept->Tiles.end());
			for (const SpotShadowTile& tile : kept->Tiles)
				CHECK(tile.Slot == first->Tiles[tile.LightIndex].Slot);
			const auto fresh = AllocateSpotShadowAtlas(lights, contested.Visible, camera);
			REQUIRE(fresh);
			CHECK(std::ranges::find(fresh->Tiles, MaxSpotShadowLights, &SpotShadowTile::LightIndex) != fresh->Tiles.end());

			// At 120 it takes the weakest spot's place and its slot; the others keep theirs, though the newcomer now ranks first.
			lights[MaxSpotShadowLights].Intensity = 120.0f;
			const auto overtaken = CullLights(lights, camera);
			const auto taken = AllocateSpotShadowAtlas(lights, overtaken.Visible, camera, &*first);
			REQUIRE(taken);
			REQUIRE(taken->Tiles.size() == MaxSpotShadowLights);
			CHECK(taken->Tiles.front().LightIndex == MaxSpotShadowLights);
			CHECK(taken->Tiles.front().Slot == first->Tiles.back().Slot);
			CHECK(std::ranges::find(taken->Tiles, MaxSpotShadowLights - 1, &SpotShadowTile::LightIndex) == taken->Tiles.end());
			for (const SpotShadowTile& tile : taken->Tiles)
			{
				if (tile.LightIndex != MaxSpotShadowLights)
					CHECK(tile.Slot == first->Tiles[tile.LightIndex].Slot);
				CHECK(tile.X == tile.Slot % 4 * 1024);
				CHECK(tile.Y == tile.Slot / 4 * 1024);
			}
		}

		TEST_CASE("SpotShadowAtlas: invalid indices and cameras fail")
		{
			const auto lights = Utils::SpotTestLights(2);
			CHECK_FALSE(AllocateSpotShadowAtlas(lights, std::array<uint32_t, 2>{ 0, 0 }, Utils::SpotTestCamera()));
			CHECK_FALSE(AllocateSpotShadowAtlas(lights, std::array<uint32_t, 1>{ 2 }, Utils::SpotTestCamera()));
			CameraData camera = Utils::SpotTestCamera();
			camera.Projection = glm::mat4(0);
			const auto result = AllocateSpotShadowAtlas({}, {}, camera);
			REQUIRE_FALSE(result);
			CHECK(result.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("SpotShadowAtlas: a spot that cannot be shadowed is rejected alone")
		{
			// Each kind of invalid data leaves that spot unshadowed and counted, while the valid spot keeps its tile (M9-F03:
			// one loaded spot with Inner > Outer used to remove every spot's shadow and fail the render).
			const std::array<uint32_t, 2> indices{ 0, 1 };
			const auto check = [&indices](const std::vector<LightData>& lights)
			{
				const auto atlas = AllocateSpotShadowAtlas(lights, indices, Utils::SpotTestCamera());
				REQUIRE_MESSAGE(atlas, atlas.error().ToString());
				CHECK(atlas->Rejected == 1);
				CHECK(atlas->Dropped == 0);
				REQUIRE(atlas->Tiles.size() == 1);
				CHECK(atlas->Tiles[0].LightIndex == 1);
				CHECK(atlas->Tiles[0].X == 0);
				CHECK(atlas->Tiles[0].Y == 0);
			};
			for (float invalid : { 0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
			{
				CAPTURE(invalid);
				auto lights = Utils::SpotTestLights(2);
				lights[0].Range = invalid;
				check(lights);
			}
			auto lights = Utils::SpotTestLights(2);
			lights[0].Direction = glm::vec3(0);
			check(lights);
			lights = Utils::SpotTestLights(2);
			lights[0].OuterConeAngle = 90;
			check(lights);
			lights = Utils::SpotTestLights(2);
			lights[0].InnerConeAngle = 40;
			lights[0].OuterConeAngle = 30;
			check(lights);
			lights = Utils::SpotTestLights(2);
			lights[0].InnerConeAngle = 0;
			lights[0].OuterConeAngle = std::numeric_limits<float>::min();
			check(lights);
			lights = Utils::SpotTestLights(2);
			lights[0].Intensity = std::numeric_limits<float>::max();
			lights[0].Color = glm::vec3(std::numeric_limits<float>::max());
			check(lights);
		}
	}

}
