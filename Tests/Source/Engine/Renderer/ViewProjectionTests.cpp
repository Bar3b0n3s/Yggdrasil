#include "TestsPCH.h"

#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/GlmApprox.h"

#include <glm/gtc/matrix_transform.hpp>

#include <limits>

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("ViewProjection: pixel centres use top-left coordinates and exact camera clip intervals")
		{
			for (const auto projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				CameraData camera;
				camera.ProjectionKind = projection;
				camera.NearClip = 2;
				camera.FarClip = 20;
				camera.ViewportWidth = 5;
				camera.ViewportHeight = 3;
				camera.Projection = ComputeReverseZProjection(projection, 90, 3, 2, 20, 5, 3);
				const auto centre = ComputeViewPixelRayInterval(camera, 2, 1);
				REQUIRE(centre);
				CHECK(centre->Ray.Direction == glm::vec3(0, 0, -1));
				CHECK(centre->MinDistance == (projection == RenderProjection::Perspective ? 2.0f : 0.0f));
				CHECK(centre->MaxDistance == (projection == RenderProjection::Perspective ? 20.0f : 18.0f));
				for (const auto pixel : { glm::uvec2(0, 0), glm::uvec2(4, 2) })
				{
					const auto ray = ComputeViewPixelRayInterval(camera, pixel.x, pixel.y);
					REQUIRE(ray);
					const glm::vec3 nearPoint = ray->Ray.Origin + ray->Ray.Direction * ray->MinDistance;
					const glm::vec3 farPoint = ray->Ray.Origin + ray->Ray.Direction * ray->MaxDistance;
					CHECK(nearPoint.z == doctest::Approx(-2));
					CHECK(farPoint.z == doctest::Approx(-20));
					CHECK((nearPoint.x < 0) == (pixel.x == 0));
					CHECK((nearPoint.y > 0) == (pixel.y == 0));
					if (projection == RenderProjection::Perspective)
						CHECK(ray->MaxDistance > camera.FarClip);
					else
						CHECK(ray->Ray.Direction == centre->Ray.Direction);
					const glm::vec4 clip = camera.Projection * glm::vec4(nearPoint, 1);
					CHECK(clip.x / clip.w == doctest::Approx(2 * (static_cast<float>(pixel.x) + 0.5f) / 5 - 1));
					CHECK(clip.y / clip.w == doctest::Approx(1 - 2 * (static_cast<float>(pixel.y) + 0.5f) / 3));
				}
				camera.Position = { 10, 3, 7 };
				camera.View = glm::lookAt(camera.Position, camera.Position + glm::vec3(1, 0, 0), glm::vec3(0, 1, 0));
				const auto turned = ComputeViewPixelRayInterval(camera, 2, 1);
				REQUIRE(turned);
				CHECK(turned->Ray.Direction == glm::vec3(1, 0, 0));
				// The inverse projection reconstructs a plane from float coefficients; allow its one-ULP rounding.
				CHECK(Test::ApproxEqual(turned->Ray.Origin, camera.Position + (projection == RenderProjection::Perspective ? glm::vec3(0) : glm::vec3(2, 0, 0)), 2.0e-6f));
			}
		}
		TEST_CASE("ViewProjection: invalid pixels and nonfinite singular cameras fail")
		{
			CameraData camera;
			camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60, 10, 0.1f, 1000, 1, 1);
			CHECK_FALSE(ComputeViewPixelRay(camera, 1, 0));
			CHECK_FALSE(ComputeViewPixelRay(camera, 0, 1));
			camera.ViewportWidth = 0;
			CHECK_FALSE(ComputeViewPixelRay(camera, 0, 0));
			camera.ViewportWidth = 1;
			camera.NearClip = camera.FarClip;
			CHECK_FALSE(ComputeViewPixelRay(camera, 0, 0));
			camera.NearClip = 0.1f;
			camera.View = glm::mat4(0);
			CHECK_FALSE(ComputeViewPixelRay(camera, 0, 0));
			camera.View = glm::mat4(1);
			camera.Projection[0][0] = std::numeric_limits<float>::quiet_NaN();
			CHECK_FALSE(ComputeViewPixelRay(camera, 0, 0));
		}
		TEST_CASE("ViewProjection: debug palettes have stable discrete colours")
		{
			CHECK(GetOverdrawDebugColor(0) == glm::vec3(0));
			CHECK(GetOverdrawDebugColor(1) == glm::vec3(0, 0, 1));
			CHECK(GetOverdrawDebugColor(2) == glm::vec3(0, 1, 1));
			CHECK(GetOverdrawDebugColor(3) == glm::vec3(0, 1, 0));
			CHECK(GetOverdrawDebugColor(4) == glm::vec3(1, 1, 0));
			CHECK(GetOverdrawDebugColor(5) == glm::vec3(1, 0, 0));
			CHECK(GetOverdrawDebugColor(UINT32_MAX) == glm::vec3(1, 0, 0));
			CHECK(GetShadowCascadeDebugColor(0) == glm::vec3(1, 0, 0));
			CHECK(GetShadowCascadeDebugColor(1) == glm::vec3(0, 1, 0));
			CHECK(GetShadowCascadeDebugColor(2) == glm::vec3(0, 0, 1));
			CHECK(GetShadowCascadeDebugColor(3) == glm::vec3(1, 1, 0));
		}
	}

}
