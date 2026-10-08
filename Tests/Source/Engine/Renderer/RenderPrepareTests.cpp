#include "TestsPCH.h"

#include "Engine/Renderer/RenderPrepare.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

// The CPU rules of §8.3 pass 1 (Architecture §15.2 "Renderer (CPU): ... light culling; transparent sort ties"). Skeletons of
// the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 7); stream A implements RenderPrepare and removes the skips.

namespace Engine {

	namespace {

		// A perspective camera at the origin looking down -Z with a 90 degree field of view, 1:1, culling beyond 100 m.
		CameraData MakeCamera()
		{
			CameraData camera;
			camera.View = glm::mat4(1.0f);
			camera.Position = glm::vec3(0.0f);
			camera.VerticalFov = 90.0f;
			camera.FarClip = 100.0f;
			camera.ViewportWidth = 64;
			camera.ViewportHeight = 64;
			camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 90.0f, 10.0f, 0.1f, 100.0f, 64, 64);
			return camera;
		}

		LightData MakePointLight(const glm::vec3& position, float intensity, uint64_t entity)
		{
			return LightData{ .Type = RenderLightType::Point, .Intensity = intensity, .Position = position, .Range = 2.0f, .Entity = UUID(entity) };
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderPrepare: bounds behind the camera, beside the frustum or beyond FarClip are outside the view" * doctest::skip(true))
		{
			const CameraData camera = MakeCamera();
			const Aabb unit{ .Min = glm::vec3(-0.5f), .Max = glm::vec3(0.5f) };
			CHECK_FALSE(IsOutsideView(unit, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f)), camera));
			CHECK(IsOutsideView(unit, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f)), camera));
			CHECK(IsOutsideView(unit, glm::translate(glm::mat4(1.0f), glm::vec3(20.0f, 0.0f, -5.0f)), camera));
			CHECK(IsOutsideView(unit, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -150.0f)), camera));
			CHECK_FALSE(IsOutsideView(Aabb{}, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f)), camera));
		}

		TEST_CASE("RenderPrepare: directional lights are always visible and point lights outside the view are culled" * doctest::skip(true))
		{
			const CameraData camera = MakeCamera();
			const std::vector<LightData> lights = {
				MakePointLight(glm::vec3(0.0f, 0.0f, 10.0f), 10.0f, 1), // behind the camera, out of range
				LightData{ .Type = RenderLightType::Directional, .Intensity = 3.0f, .Entity = UUID(2) },
				MakePointLight(glm::vec3(0.0f, 0.0f, -5.0f), 10.0f, 3), // in view
				MakePointLight(glm::vec3(0.0f, 0.0f, 1.0f), 10.0f, 4),  // behind, but its range reaches the view
			};
			const LightCullResult result = CullLights(lights, camera);
			REQUIRE(result.Visible.size() == 3);
			CHECK(result.Visible[0] == 1); // directional lights come first
			CHECK(std::ranges::find(result.Visible, 0U) == result.Visible.end());
			CHECK(result.Culled == 1);
			CHECK(result.Dropped == 0);
		}

		TEST_CASE("RenderPrepare: more than 256 visible lights keeps the most important and counts the dropped ones" * doctest::skip(true))
		{
			const CameraData camera = MakeCamera();
			std::vector<LightData> lights;
			for (uint32_t index = 0; index < MaxVisibleLights + 10; ++index)
				lights.push_back(MakePointLight(glm::vec3(0.0f, 0.0f, -5.0f), 1.0f + static_cast<float>(index), index + 1));
			const LightCullResult result = CullLights(lights, camera);
			CHECK(result.Visible.size() == MaxVisibleLights);
			CHECK(result.Dropped == 10);
			// The ten weakest (the first ten) are the ones dropped.
			for (uint32_t index = 0; index < 10; ++index)
				CHECK(std::ranges::find(result.Visible, index) == result.Visible.end());
			// Deterministic: the same input gives the same list.
			CHECK(CullLights(lights, camera).Visible == result.Visible);
		}

		TEST_CASE("RenderPrepare: lights with zero or non-finite radiance are culled" * doctest::skip(true))
		{
			const CameraData camera = MakeCamera();
			std::vector<LightData> lights = { MakePointLight(glm::vec3(0.0f, 0.0f, -5.0f), 0.0f, 1),
				MakePointLight(glm::vec3(0.0f, 0.0f, -5.0f), std::numeric_limits<float>::infinity(), 2),
				LightData{ .Type = RenderLightType::Directional, .Color = glm::vec3(0.0f), .Entity = UUID(3) } };
			const LightCullResult result = CullLights(lights, camera);
			CHECK(result.Visible.empty());
			CHECK(result.Culled == 3);
		}

		TEST_CASE("RenderPrepare: transparent draws sort back to front with ties by entity UUID" * doctest::skip(true))
		{
			std::vector<TransparentSortKey> keys = {
				{ .ViewDepth = 2.0f, .Entity = UUID(5), .Submesh = 0, .DrawIndex = 0 },
				{ .ViewDepth = 9.0f, .Entity = UUID(7), .Submesh = 0, .DrawIndex = 1 },
				{ .ViewDepth = 2.0f, .Entity = UUID(3), .Submesh = 1, .DrawIndex = 2 },
				{ .ViewDepth = 2.0f, .Entity = UUID(3), .Submesh = 0, .DrawIndex = 3 },
				{ .ViewDepth = std::numeric_limits<float>::quiet_NaN(), .Entity = UUID(9), .Submesh = 0, .DrawIndex = 4 },
			};
			SortTransparentDraws(keys);
			std::vector<uint32_t> order;
			for (const TransparentSortKey& key : keys)
				order.push_back(key.DrawIndex);
			CHECK(order == std::vector<uint32_t>{ 4, 1, 3, 2, 0 });
		}

		TEST_CASE("RenderPrepare: opaque draws sort by pipeline, material and mesh, ties in snapshot order" * doctest::skip(true))
		{
			std::vector<OpaqueSortKey> keys = {
				{ .Pipeline = 1, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .DrawIndex = 0 },
				{ .Pipeline = 0, .Material = AssetHandle(20), .Mesh = AssetHandle(1), .DrawIndex = 1 },
				{ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(2), .DrawIndex = 2 },
				{ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .DrawIndex = 3 },
				{ .Pipeline = 0, .Material = AssetHandle(10), .Mesh = AssetHandle(1), .DrawIndex = 4 },
			};
			SortOpaqueDraws(keys);
			std::vector<uint32_t> order;
			for (const OpaqueSortKey& key : keys)
				order.push_back(key.DrawIndex);
			CHECK(order == std::vector<uint32_t>{ 3, 4, 2, 1, 0 });
		}
	}

}
