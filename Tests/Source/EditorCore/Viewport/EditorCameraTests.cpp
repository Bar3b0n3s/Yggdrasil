#include "TestsPCH.h"
#include "EditorCore/Viewport/EditorCamera.h"

#include "EditorCore/Viewport/EditorViewportState.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorCamera: orbit preserves its target and distance")
		{
			const ExplicitRenderCamera camera;
			const auto moved = EditorCamera::Navigate(camera, { .PointerDelta = { 120.0f, -60.0f }, .Orbit = true });
			REQUIRE(moved.has_value());
			CHECK(moved->Target == camera.Target);
			CHECK(moved->Position != camera.Position);
			CHECK(glm::length(moved->Position - moved->Target) == doctest::Approx(glm::length(camera.Position - camera.Target)));
			const auto pole = EditorCamera::Navigate(*moved, { .PointerDelta = { 0.0f, 100000.0f }, .Orbit = true });
			REQUIRE(pole.has_value());
			CHECK(std::abs(glm::normalize(pole->Target - pole->Position).y) < 1.0f);
		}

		TEST_CASE("EditorCamera: pan and fly preserve the viewing direction")
		{
			const ExplicitRenderCamera camera;
			const auto pan = EditorCamera::Navigate(camera, { .PointerDelta = { 30.0f, 15.0f }, .Pan = true });
			REQUIRE(pan.has_value());
			CHECK(glm::length((pan->Target - pan->Position) - (camera.Target - camera.Position)) < 1.0e-5f);
			CHECK(pan->Position != camera.Position);
			const auto fly = EditorCamera::Navigate(camera, { .FlyAxis = { 1.0f, 1.0f, 1.0f }, .DeltaSeconds = 0.5f, .MoveSpeed = 4.0f, .Fly = true });
			REQUIRE(fly.has_value());
			CHECK(glm::length((fly->Target - fly->Position) - (camera.Target - camera.Position)) < 1.0e-5f);
			const auto look = EditorCamera::Navigate(camera, { .PointerDelta = { 50.0f, 0.0f }, .Fly = true });
			REQUIRE(look.has_value());
			CHECK(look->Position == camera.Position);
			CHECK(look->Target != camera.Target);
		}

		TEST_CASE("EditorCamera: invalid navigation leaves the camera unchanged")
		{
			const ExplicitRenderCamera camera;
			for (const EditorCameraInput& input : {
					 EditorCameraInput{ .PointerDelta = { std::numeric_limits<float>::infinity(), 0.0f } },
					 EditorCameraInput{ .FlyAxis = { 1.1f, 0.0f, 0.0f } },
					 EditorCameraInput{ .Wheel = std::numeric_limits<float>::quiet_NaN() },
					 EditorCameraInput{ .DeltaSeconds = -1.0f }, EditorCameraInput{ .MoveSpeed = -1.0f } })
			{
				const auto result = EditorCamera::Navigate(camera, input);
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK(camera.Position == ExplicitRenderCamera{}.Position);
			CHECK(camera.Target == ExplicitRenderCamera{}.Target);
			const auto idle = EditorCamera::Navigate(camera, {});
			REQUIRE(idle.has_value());
			CHECK(idle->Position == camera.Position);
			CHECK(idle->Target == camera.Target);
		}

		TEST_CASE("EditorCamera: frame bounds fits every corner in perspective and orthographic views")
		{
			for (const RenderProjection projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				for (const glm::uvec2& size : { glm::uvec2(300, 1000), glm::uvec2(1920, 1080) })
				{
					ExplicitRenderCamera camera;
					camera.Position = { 20.0f, 11.0f, 5.0f };
					camera.Projection = projection;
					camera.FarClip = 0.2f;
					const glm::vec3 minimum(-23.0f, -2.0f, -7.0f);
					const glm::vec3 maximum(9.0f, 6.0f, 18.0f);
					const auto fitted = EditorCamera::FrameBounds(camera, minimum, maximum, static_cast<float>(size.x) / size.y);
					REQUIRE(fitted.has_value());
					CHECK(fitted->Target == (minimum + maximum) * 0.5f);
					CHECK(glm::length(glm::normalize(fitted->Target - fitted->Position) - glm::normalize(camera.Target - camera.Position)) < 1.0e-5f);
					const glm::mat4 view = glm::lookAt(fitted->Position, fitted->Target, glm::vec3(0.0f, 1.0f, 0.0f));
					const glm::mat4 matrix = ComputeReverseZProjection(projection, fitted->VerticalFov, fitted->OrthographicSize,
												 fitted->NearClip, fitted->FarClip, size.x, size.y)
						* view;
					for (int corner = 0; corner < 8; ++corner)
					{
						const glm::vec4 point((corner & 1) ? maximum.x : minimum.x, (corner & 2) ? maximum.y : minimum.y,
							(corner & 4) ? maximum.z : minimum.z, 1.0f);
						const glm::vec4 clip = matrix * point;
						REQUIRE(clip.w > 0.0f);
						CHECK(std::abs(clip.x / clip.w) <= 1.0f / 1.1f + 1.0e-5f);
						CHECK(std::abs(clip.y / clip.w) <= 1.0f / 1.1f + 1.0e-5f);
						CHECK(clip.z / clip.w > 0.0f);
						CHECK(clip.z / clip.w < 1.0f);
						CHECK(-(view * point).z < fitted->FarClip);
					}
				}
			}
		}

		TEST_CASE("EditorCamera: a point bound and a vertical view are handled without nonfinite output")
		{
			ExplicitRenderCamera camera;
			const auto point = EditorCamera::FrameBounds(camera, glm::vec3(2.0f), glm::vec3(2.0f), 1.0f);
			REQUIRE(point.has_value());
			CHECK(glm::length(point->Position - point->Target) >= 0.5f);
			CHECK_FALSE(EditorCamera::FrameBounds(camera, glm::vec3(1.0f), glm::vec3(-1.0f), 1.0f).has_value());
			CHECK_FALSE(EditorCamera::FrameBounds(camera, glm::vec3(0.0f), glm::vec3(1.0f), 0.0f).has_value());
			CHECK_FALSE(EditorCamera::FrameBounds(camera, glm::vec3(std::numeric_limits<float>::quiet_NaN()), glm::vec3(1.0f), 1.0f).has_value());
			camera.Position = { 0.0f, 5.0f, 0.0f };
			const auto vertical = EditorCamera::FrameBounds(camera, glm::vec3(0.0f), glm::vec3(1.0f), 1.0f);
			REQUIRE_FALSE(vertical.has_value());
			CHECK(vertical.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("EditorCamera: wheel zoom changes perspective distance and orthographic size")
		{
			ExplicitRenderCamera camera;
			const auto perspective = EditorCamera::Navigate(camera, { .Wheel = 2.0f });
			REQUIRE(perspective.has_value());
			CHECK(glm::length(perspective->Target - perspective->Position) < glm::length(camera.Target - camera.Position));
			CHECK(perspective->Target == camera.Target);
			camera.Projection = RenderProjection::Orthographic;
			const auto orthographic = EditorCamera::Navigate(camera, { .Wheel = 2.0f });
			REQUIRE(orthographic.has_value());
			CHECK(orthographic->OrthographicSize < camera.OrthographicSize);
			CHECK(orthographic->Position == camera.Position);
			CHECK(orthographic->Target == camera.Target);
		}
	}

}
