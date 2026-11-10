#include "TestsPCH.h"

#include "Engine/Renderer/Light2DShading.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

// The 2D light model (M21, C21.2, R-2D-LIGHT; Docs/Decisions/0032-m21-contract.md decision 22): the CPU reference of the
// Lights2D pass, and the test the renderer culls the view's lights with.

namespace Engine {

	namespace {

		void CheckLight(const glm::vec3& actual, const glm::vec3& expected)
		{
			CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-6));
			CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-6));
			CHECK(actual.z == doctest::Approx(expected.z).epsilon(1e-6));
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Light2DShading: a point light fades smoothly to its radius and a global light is uniform")
		{
			// Radius 4 and Falloff 0.5: full light within 2 units, the smoothstep from 2 to 4, nothing from 4 on.
			const Light2DItem point{ .Type = RenderLight2DType::Point, .Position = glm::vec3(0.0f, 0.0f, 3.0f), .Color = glm::vec3(1.0f, 0.5f, 0.25f), .Intensity = 2.0f, .Radius = 4.0f, .Falloff = 0.5f };
			CheckLight(EvaluateLight2D(point, glm::vec2(1.0f, 0.0f), std::nullopt), glm::vec3(2.0f, 1.0f, 0.5f));
			CheckLight(EvaluateLight2D(point, glm::vec2(0.0f, 2.0f), std::nullopt), glm::vec3(2.0f, 1.0f, 0.5f));
			// d = 3: t = (4 - 3) / (4 x 0.5) = 0.5, a = 0.25 x (3 - 1) = 0.5.
			CheckLight(EvaluateLight2D(point, glm::vec2(3.0f, 0.0f), std::nullopt), glm::vec3(1.0f, 0.5f, 0.25f));
			CheckLight(EvaluateLight2D(point, glm::vec2(0.0f, -4.0f), std::nullopt), glm::vec3(0.0f));
			CheckLight(EvaluateLight2D(point, glm::vec2(9.0f, 9.0f), std::nullopt), glm::vec3(0.0f));
			// Falloff 0: a hard edge.
			Light2DItem hard = point;
			hard.Falloff = 0.0f;
			CheckLight(EvaluateLight2D(hard, glm::vec2(3.9f, 0.0f), std::nullopt), glm::vec3(2.0f, 1.0f, 0.5f));
			CheckLight(EvaluateLight2D(hard, glm::vec2(4.0f, 0.0f), std::nullopt), glm::vec3(0.0f));

			// A global light is the same everywhere, with or without a normal.
			const Light2DItem global{ .Type = RenderLight2DType::Global, .Position = glm::vec3(50.0f), .Color = glm::vec3(0.2f), .Intensity = 0.5f };
			CheckLight(EvaluateLight2D(global, glm::vec2(-100.0f, 7.0f), std::nullopt), glm::vec3(0.1f));
			CheckLight(EvaluateLight2D(global, glm::vec2(3.0f, 3.0f), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(0.1f));

			// The view's light is the sum, in order; a light of a non-finite value gives none.
			const std::vector<Light2DItem> lights = { point, global };
			CheckLight(EvaluateLights2D(lights, glm::vec2(1.0f, 0.0f), std::nullopt), glm::vec3(2.1f, 1.1f, 0.6f));
			Light2DItem broken = point;
			broken.Radius = std::nanf("");
			CheckLight(EvaluateLight2D(broken, glm::vec2(1.0f, 0.0f), std::nullopt), glm::vec3(0.0f));
			// Only the first MaxVisibleLights2D count.
			const std::vector<Light2DItem> many(MaxVisibleLights2D + 1, Light2DItem{ .Type = RenderLight2DType::Global, .Intensity = 0.001f });
			CHECK(EvaluateLights2D(many, glm::vec2(0.0f), std::nullopt).x == doctest::Approx(0.001 * MaxVisibleLights2D).epsilon(1e-4));
		}

		TEST_CASE("Light2DShading: a normal-mapped pixel takes each point light's cosine toward the light above the plane")
		{
			// A white light 1 unit above the plane at the origin, full strength within its radius.
			const Light2DItem light{ .Type = RenderLight2DType::Point, .Color = glm::vec3(1.0f), .Intensity = 1.0f, .Radius = 10.0f, .Falloff = 0.0f, .Height = 1.0f };
			const glm::vec2 pixel(1.0f, 0.0f);
			// L = normalize(-1, 0, 1).
			const float cosine = 1.0f / std::sqrt(2.0f);
			CheckLight(EvaluateLight2D(light, pixel, glm::vec3(0.0f, 0.0f, 1.0f)), glm::vec3(cosine));
			CheckLight(EvaluateLight2D(light, pixel, glm::normalize(glm::vec3(-1.0f, 0.0f, 1.0f))), glm::vec3(1.0f));
			CheckLight(EvaluateLight2D(light, pixel, glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(0.0f));
			// The flat normal takes the light whole: a lit sprite without a normal map is not dimmed by the light's height.
			CheckLight(EvaluateLight2D(light, pixel, std::nullopt), glm::vec3(1.0f));
			// Right under the light, L is +Z.
			CheckLight(EvaluateLight2D(light, glm::vec2(0.0f), glm::vec3(0.0f, 0.0f, 1.0f)), glm::vec3(1.0f));
		}

		TEST_CASE("Light2DShading: a light reaches the view unless its square lies wholly outside the clip rectangle, and an invalid one never does")
		{
			// An orthographic camera at z = 10 looking down -Z whose view is 8 units high and wide: x and y in [-4, 4].
			const glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f));
			const glm::mat4 viewProjection = ComputeReverseZProjection(RenderProjection::Orthographic, 60.0f, 4.0f, 0.1f, 20.0f, 16, 16) * view;
			const auto point = [](const glm::vec3& position, float radius)
			{
				return Light2DItem{ .Type = RenderLight2DType::Point, .Position = position, .Radius = radius };
			};
			CHECK(Light2DReachesView(point(glm::vec3(0.0f), 1.0f), viewProjection));
			// A square that overlaps the view's edge reaches it; one wholly beside it does not, whichever side.
			CHECK(Light2DReachesView(point(glm::vec3(4.5f, 0.0f, 0.0f), 1.0f), viewProjection));
			CHECK_FALSE(Light2DReachesView(point(glm::vec3(5.5f, 0.0f, 0.0f), 1.0f), viewProjection));
			CHECK_FALSE(Light2DReachesView(point(glm::vec3(-5.5f, 0.0f, 0.0f), 1.0f), viewProjection));
			CHECK_FALSE(Light2DReachesView(point(glm::vec3(0.0f, 5.5f, 0.0f), 1.0f), viewProjection));
			CHECK_FALSE(Light2DReachesView(point(glm::vec3(0.0f, -5.5f, 0.0f), 1.0f), viewProjection));
			CHECK(Light2DReachesView(point(glm::vec3(5.5f, 0.0f, 0.0f), 2.0f), viewProjection));
			// Depth is held inside the clip range: a light behind the camera or beyond its far plane still lights the view.
			CHECK(Light2DReachesView(point(glm::vec3(0.0f, 0.0f, 50.0f), 1.0f), viewProjection));
			CHECK(Light2DReachesView(point(glm::vec3(0.0f, 0.0f, -100.0f), 1.0f), viewProjection));
			// A Global light reaches every view, wherever its entity is.
			CHECK(Light2DReachesView(Light2DItem{ .Type = RenderLight2DType::Global, .Position = glm::vec3(1000.0f) }, viewProjection));
			// Behind a perspective eye nothing reaches the view.
			const glm::mat4 perspective = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 4.0f, 0.1f, 20.0f, 16, 16) * view;
			CHECK(Light2DReachesView(point(glm::vec3(0.0f), 1.0f), perspective));
			CHECK_FALSE(Light2DReachesView(point(glm::vec3(0.0f, 0.0f, 20.0f), 1.0f), perspective));

			// Invalid values: a light gives no light and never reaches a view, so it never counts against the cap.
			const float nan = std::numeric_limits<float>::quiet_NaN();
			const Light2DItem valid = point(glm::vec3(0.0f), 1.0f);
			CHECK(IsLight2DValid(valid));
			// Each one value out of range, or not finite.
			std::vector<Light2DItem> invalid(8, valid);
			invalid[0].Radius = nan;
			invalid[1].Radius = 0.0f;
			invalid[2].Intensity = -1.0f;
			invalid[3].Intensity = MaxLight2DIntensity * 2.0f;
			invalid[4].Color = glm::vec3(2.0f, 1.0f, 1.0f);
			invalid[5].Falloff = 1.5f;
			invalid[6].Height = 0.0f;
			invalid[7].Position.z = std::numeric_limits<float>::infinity();
			for (const Light2DItem& broken : invalid)
			{
				CHECK_FALSE(IsLight2DValid(broken));
				CHECK_FALSE(Light2DReachesView(broken, viewProjection));
				CheckLight(EvaluateLight2D(broken, glm::vec2(0.0f), std::nullopt), glm::vec3(0.0f));
			}
			// A Global light ignores the values only a Point light reads.
			Light2DItem global{ .Type = RenderLight2DType::Global, .Radius = nan, .Falloff = 7.0f, .Height = -1.0f };
			global.Position = glm::vec3(nan);
			CHECK(IsLight2DValid(global));
			CheckLight(EvaluateLight2D(global, glm::vec2(0.0f), std::nullopt), glm::vec3(1.0f));
		}
	}

}
