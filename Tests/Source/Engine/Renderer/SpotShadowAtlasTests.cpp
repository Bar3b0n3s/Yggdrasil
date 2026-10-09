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
			auto lights = Utils::SpotTestLights(9);
			lights[0].Intensity = 10;
			const auto visible = CullLights(lights, Utils::SpotTestCamera());
			REQUIRE(visible.Visible.size() == 9);
			const auto atlas = AllocateSpotShadowAtlas(lights, visible.Visible, Utils::SpotTestCamera());
			REQUIRE(atlas);
			CHECK(atlas->Dropped == 1);
			REQUIRE(atlas->Tiles.size() == 8);
			for (size_t index = 0; index < 8; ++index)
				CHECK(atlas->Tiles[index].LightIndex == visible.Visible[index]);
			CHECK(visible.Dropped == 0);
			lights[1].Type = RenderLightType::Point;
			lights[2].CastShadows = false;
			const auto filtered = AllocateSpotShadowAtlas(lights, visible.Visible, Utils::SpotTestCamera());
			REQUIRE(filtered);
			CHECK(filtered->Tiles.size() == 7);
			CHECK(filtered->Dropped == 0);
		}

		TEST_CASE("SpotShadowAtlas: invalid indices and non-finite lights fail")
		{
			auto lights = Utils::SpotTestLights(2);
			CHECK_FALSE(AllocateSpotShadowAtlas(lights, std::array<uint32_t, 2>{ 0, 0 }, Utils::SpotTestCamera()));
			CHECK_FALSE(AllocateSpotShadowAtlas(lights, std::array<uint32_t, 1>{ 2 }, Utils::SpotTestCamera()));
			const std::array<uint32_t, 2> indices{ 0, 1 };
			for (float invalid : { 0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
			{
				lights[0].Range = invalid;
				const auto result = AllocateSpotShadowAtlas(lights, indices, Utils::SpotTestCamera());
				REQUIRE_FALSE(result);
				CHECK(result.error().GetCode() == ErrorCode::InvalidArgument);
			}
			lights = Utils::SpotTestLights(2);
			lights[0].Direction = glm::vec3(0);
			CHECK_FALSE(AllocateSpotShadowAtlas(lights, indices, Utils::SpotTestCamera()));
			lights = Utils::SpotTestLights(2);
			lights[0].OuterConeAngle = 90;
			CHECK_FALSE(AllocateSpotShadowAtlas(lights, indices, Utils::SpotTestCamera()));
			CameraData camera = Utils::SpotTestCamera();
			lights = Utils::SpotTestLights(2);
			lights[0].InnerConeAngle = 0;
			lights[0].OuterConeAngle = std::numeric_limits<float>::min();
			CHECK_FALSE(AllocateSpotShadowAtlas(lights, indices, camera));
			camera.Projection = glm::mat4(0);
			CHECK_FALSE(AllocateSpotShadowAtlas({}, {}, camera));
		}
	}

}
