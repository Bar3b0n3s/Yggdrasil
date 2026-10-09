#include "TestsPCH.h"

#include "Engine/Renderer/ShadowCascades.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static CameraData ShadowTestCamera(RenderProjection kind = RenderProjection::Perspective, uint32_t width = 1600, uint32_t height = 900)
		{
			CameraData camera;
			camera.ProjectionKind = kind;
			camera.ViewportWidth = width;
			camera.ViewportHeight = height;
			camera.Projection = ComputeReverseZProjection(kind, camera.VerticalFov, camera.OrthographicSize, camera.NearClip, camera.FarClip, width, height);
			return camera;
		}

		static void CheckShadowContainment(const ShadowCascadeSet& cascades)
		{
			for (uint32_t index = 0; index < cascades.Count; ++index)
				for (const glm::vec3& corner : cascades.Cascades[index].Corners)
				{
					const glm::vec4 clip = cascades.Cascades[index].ViewProjection * glm::vec4(corner, 1.0f);
					CHECK(std::abs(clip.x / clip.w) <= 1.000001f);
					CHECK(std::abs(clip.y / clip.w) <= 1.000001f);
					CHECK(clip.z / clip.w >= -0.000001f);
					CHECK(clip.z / clip.w <= 1.000001f);
				}
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Shadows: cascade splits")
		{
			const auto uniform = ComputeCascadeSplits(1.0f, 81.0f, 4, 0.0f);
			const auto logarithmic = ComputeCascadeSplits(1.0f, 81.0f, 4, 1.0f);
			const auto mixed = ComputeCascadeSplits(1.0f, 81.0f, 4, 0.5f);
			REQUIRE(uniform);
			REQUIRE(logarithmic);
			REQUIRE(mixed);
			CHECK(*uniform == std::array<float, 4>{ 21, 41, 61, 81 });
			CHECK(*logarithmic == std::array<float, 4>{ 3, 9, 27, 81 });
			CHECK(*mixed == std::array<float, 4>{ 12, 25, 44, 81 });
			const auto single = ComputeCascadeSplits(1, 81, 1, 0.7f);
			REQUIRE(single);
			CHECK(*single == std::array<float, 4>{ 81, 81, 81, 81 });
			CHECK_FALSE(ComputeCascadeSplits(0, 10, 4, 0.5f));
			CHECK_FALSE(ComputeCascadeSplits(10, 10, 4, 0.5f));
			CHECK_FALSE(ComputeCascadeSplits(1, 10, 0, 0.5f));
			CHECK_FALSE(ComputeCascadeSplits(1, 10, 5, 0.5f));
			CHECK_FALSE(ComputeCascadeSplits(1, 10, 4, 1.1f));
			CHECK_FALSE(ComputeCascadeSplits(1, std::numeric_limits<float>::infinity(), 4, 0.5f));
			CHECK_FALSE(ComputeCascadeSplits(1, 10, 4, std::numeric_limits<float>::quiet_NaN()));
		}

		TEST_CASE("Shadows: sub-texel camera move leaves the light matrix unchanged")
		{
			CameraData camera = Utils::ShadowTestCamera();
			LightData light;
			light.CascadeCount = 1;
			const auto original = BuildShadowCascades(camera, light, 7, 2048);
			REQUIRE(original);
			const ShadowCascade& cascade = original->Cascades[0];
			// Centre the original Z coordinate in its snap cell before taking a three-axis sub-texel step.
			const float centreZ = -(cascade.SplitNear + cascade.SplitFar) * 0.5f;
			const float snappedZ = std::floor(centreZ / cascade.TexelWorldSize + 0.5f) * cascade.TexelWorldSize;
			camera.Position.z = snappedZ - centreZ;
			camera.View = glm::translate(glm::mat4(1.0f), -camera.Position);
			const auto centred = BuildShadowCascades(camera, light, 7, 2048);
			REQUIRE(centred);
			camera.Position += glm::vec3(cascade.TexelWorldSize * 0.37f);
			camera.View = glm::translate(glm::mat4(1.0f), -camera.Position);
			const auto moved = BuildShadowCascades(camera, light, 7, 2048);
			REQUIRE(moved);
			CHECK(moved->LightIndex == 7);
			CHECK(moved->Cascades[0].ViewProjection == centred->Cascades[0].ViewProjection);
			CHECK(moved->Cascades[0].Radius == cascade.Radius);
			Utils::CheckShadowContainment(*moved);
		}

		TEST_CASE("Shadows: orthographic frustum corners fit")
		{
			CameraData camera = Utils::ShadowTestCamera(RenderProjection::Orthographic, 200, 100);
			camera.Position = glm::vec3(12, -4, 9);
			camera.View = glm::lookAt(camera.Position, camera.Position + glm::normalize(glm::vec3(1, -0.2f, -1)), glm::vec3(0, 1, 0));
			LightData light;
			light.Direction = glm::vec3(-0.7f, -1, -0.2f);
			const auto cascades = BuildShadowCascades(camera, light, 0, 256);
			REQUIRE(cascades);
			Utils::CheckShadowContainment(*cascades);
			const auto corners = ComputeFrustumSliceCorners(camera, 2, 12);
			REQUIRE(corners);
			for (size_t index = 0; index < corners->size(); ++index)
			{
				const glm::vec4 view = camera.View * glm::vec4((*corners)[index], 1);
				CHECK(view.z == doctest::Approx(index < 4 ? -2 : -12).epsilon(0.00001));
				CHECK(std::abs(view.x) == doctest::Approx(20));
				CHECK(std::abs(view.y) == doctest::Approx(10));
			}
		}

		TEST_CASE("Shadows: off-screen casters remain inside the extended light volume")
		{
			const CameraData camera = Utils::ShadowTestCamera(RenderProjection::Orthographic);
			LightData light;
			light.CascadeCount = 1;
			const auto cascades = BuildShadowCascades(camera, light, 0, 2048);
			REQUIRE(cascades);
			const ShadowCascade& cascade = cascades->Cascades[0];
			const glm::vec4 caster = cascade.ViewProjection * glm::vec4(0, 0, 30, 1);
			CHECK(caster.z > 0);
			CHECK(caster.z < 1);
			const glm::vec4 receiver = cascade.ViewProjection * glm::vec4(0, 0, -10, 1);
			CHECK(caster.z > receiver.z);
			CHECK(cascade.LightFar == doctest::Approx(2 * cascade.Radius + light.ShadowDistance));
		}

		TEST_CASE("Shadows: softness and blend are continuous across cascade boundaries")
		{
			const auto cascades = BuildShadowCascades(Utils::ShadowTestCamera(), LightData{}, 0, 1024);
			REQUIRE(cascades);
			for (uint32_t index = 0; index < cascades->Count; ++index)
			{
				const ShadowCascade& cascade = cascades->Cascades[index];
				CHECK(cascade.BlendStart == doctest::Approx(cascade.SplitFar - 0.1f * (cascade.SplitFar - cascade.SplitNear)));
				CHECK(cascade.PenumbraUvPerMetre * 2 * cascade.Radius == doctest::Approx(std::tan(glm::radians(0.5f))));
				if (index + 1 < cascades->Count)
				{
					const ShadowCascade& next = cascades->Cascades[index + 1];
					CHECK(next.SplitNear == cascade.SplitFar);
					CHECK(-next.Corners[0].z == doctest::Approx(cascade.BlendStart));
					// Every corner at the outgoing split is in both maps during its fade.
					for (size_t corner = 4; corner < 8; ++corner)
					{
						const glm::vec4 clip = next.ViewProjection * glm::vec4(cascade.Corners[corner], 1);
						CHECK(std::abs(clip.x) <= clip.w);
						CHECK(std::abs(clip.y) <= clip.w);
						CHECK(clip.z >= 0);
						CHECK(clip.z <= clip.w);
					}
				}
			}
		}

		TEST_CASE("Shadows: snapped wide orthographic slices remain wholly contained")
		{
			CameraData camera = Utils::ShadowTestCamera(RenderProjection::Orthographic, 1000, 1);
			LightData light;
			light.CascadeCount = 1;
			const auto initial = BuildShadowCascades(camera, light, 0, 256);
			REQUIRE(initial);
			for (float fraction : { -0.50001f, -0.49999f, 0.49999f, 0.50001f })
			{
				camera.Position = glm::vec3(initial->Cascades[0].TexelWorldSize * fraction);
				camera.View = glm::translate(glm::mat4(1), -camera.Position);
				const auto shifted = BuildShadowCascades(camera, light, 0, 256);
				REQUIRE(shifted);
				CHECK(shifted->Cascades[0].Radius == initial->Cascades[0].Radius);
				Utils::CheckShadowContainment(*shifted);
			}
		}

		TEST_CASE("Shadows: containment radius includes snapping margin without translation refits")
		{
			for (uint32_t size = 256; size <= 8192; size *= 2)
				for (float sphere : { 0.000001f, 1.0f, 10.03125f, 10000.0f, 1e20f })
				{
					const auto radius = ComputeStabilizedCascadeRadius(sphere, size);
					REQUIRE(radius);
					CHECK(static_cast<double>(*radius) >= static_cast<double>(sphere) + static_cast<double>(*radius) / size);
					const double oracle = std::ceil(16.0 * sphere / (1.0 - 1.0 / size)) / 16.0;
					CHECK(static_cast<double>(*radius) >= oracle);
					CHECK(static_cast<double>(std::nextafter(*radius, 0.0f)) < oracle);
				}
			CHECK_FALSE(ComputeStabilizedCascadeRadius(0, 256));
			CHECK_FALSE(ComputeStabilizedCascadeRadius(1, 255));
			CHECK_FALSE(ComputeStabilizedCascadeRadius(1, 300));
			CHECK_FALSE(ComputeStabilizedCascadeRadius(std::numeric_limits<float>::max(), 8192));
		}

		TEST_CASE("Shadows: generic inverse projection supports asymmetric finite perspective and rejects invalid input")
		{
			CameraData camera = Utils::ShadowTestCamera();
			camera.Projection[2][0] = 0.3f;
			camera.Projection[2][1] = -0.2f;
			camera.Projection[2][2] = camera.NearClip / (camera.FarClip - camera.NearClip);
			camera.Projection[3][2] = camera.NearClip * camera.FarClip / (camera.FarClip - camera.NearClip);
			const auto corners = ComputeFrustumSliceCorners(camera, 1, 70);
			REQUIRE(corners);
			for (size_t index = 0; index < corners->size(); ++index)
			{
				const glm::vec4 clip = camera.Projection * glm::vec4((*corners)[index], 1);
				CHECK(std::abs(clip.x / clip.w) == doctest::Approx(1));
				CHECK(std::abs(clip.y / clip.w) == doctest::Approx(1));
				CHECK(-(*corners)[index].z == doctest::Approx(index < 4 ? 1 : 70));
			}
			LightData light;
			light.Direction = glm::vec3(0);
			CHECK_FALSE(BuildShadowCascades(camera, light, 0, 1024));
			camera.View = glm::mat4(0);
			CHECK_FALSE(ComputeFrustumSliceCorners(camera, 1, 70));
			camera.View = glm::mat4(1);
			camera.Projection[0][0] = std::numeric_limits<float>::quiet_NaN();
			CHECK_FALSE(ComputeFrustumSliceCorners(camera, 1, 70));
			camera = Utils::ShadowTestCamera();
			camera.View[0][0] = std::numeric_limits<float>::min();
			CHECK_FALSE(ComputeFrustumSliceCorners(camera, 1, 70));
		}
	}

}
