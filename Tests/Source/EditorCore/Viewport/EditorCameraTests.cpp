#include "TestsPCH.h"
#include "EditorCore/Viewport/EditorCamera.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Viewport/EditorViewportState.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Support/EditorTestFixture.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>
#include <numbers>

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

		TEST_CASE("EditorCamera: framing only widens the clip range, so the surrounding scene and a later zoom-out stay visible")
		{
			const ExplicitRenderCamera camera; // the default range: 0.1 to 1000 m
			const glm::vec3 minimum(-0.5f);
			const glm::vec3 maximum(0.5f);
			const auto fitted = EditorCamera::FrameBounds(camera, minimum, maximum, 16.0f / 9.0f);
			REQUIRE(fitted.has_value());
			CHECK(fitted->NearClip <= camera.NearClip);
			CHECK(fitted->FarClip >= camera.FarClip);
			// A ground plane or another object 50 m behind the framed cube is still inside the range.
			const glm::vec3 forward = glm::normalize(fitted->Target - fitted->Position);
			const float behind = glm::dot((fitted->Target + forward * 50.0f) - fitted->Position, forward);
			CHECK(behind > fitted->NearClip);
			CHECK(behind < fitted->FarClip);
			// Zooming out (wheel away) never pushes the framed cube beyond the far plane.
			ExplicitRenderCamera zoomed = *fitted;
			for (int notch = 0; notch < 20; ++notch)
			{
				const auto next = EditorCamera::Navigate(zoomed, { .Wheel = -1.0f });
				REQUIRE(next.has_value());
				zoomed = *next;
			}
			const glm::vec3 direction = glm::normalize(zoomed.Target - zoomed.Position);
			for (int corner = 0; corner < 8; ++corner)
			{
				const glm::vec3 point((corner & 1) ? maximum.x : minimum.x, (corner & 2) ? maximum.y : minimum.y, (corner & 4) ? maximum.z : minimum.z);
				const float depth = glm::dot(point - zoomed.Position, direction);
				CHECK(depth > zoomed.NearClip);
				CHECK(depth < zoomed.FarClip);
			}
			// A range that cannot hold the framed bounds still grows to hold them.
			ExplicitRenderCamera tight = camera;
			tight.Position = { 0.0f, 300.0f, 2000.0f };
			tight.FarClip = 1.0f;
			const auto distant = EditorCamera::FrameBounds(tight, glm::vec3(-100.0f), glm::vec3(100.0f), 1.0f);
			REQUIRE(distant.has_value());
			CHECK(distant->FarClip > glm::length(distant->Target - distant->Position) + 100.0f);
			CHECK(distant->NearClip == tight.NearClip);
		}

		TEST_CASE("EditorCamera: pan follows the rendered image's axes for steep accepted cameras")
		{
			Test::EditorTestFixture fixture("EditorCameraBasis");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			// Just outside the renderer's degenerate band around -Y: |forward x +Y| = 2e-5.
			ExplicitRenderCamera camera;
			camera.Position = { 0.0002f, 10.0f, 0.0f };
			camera.Target = glm::vec3(0.0f);
			const auto snapshot = ExtractRenderSnapshot(fixture.GetEditor().GetScene(), { .Camera = RenderCameraSource::Explicit, .ExplicitCamera = camera, .Width = 160, .Height = 90 });
			REQUIRE(snapshot.has_value());
			const glm::vec3 renderedRight(snapshot->Camera.View[0][0], snapshot->Camera.View[1][0], snapshot->Camera.View[2][0]);
			const auto panned = EditorCamera::Navigate(camera, { .PointerDelta = { 10.0f, 0.0f }, .Pan = true });
			REQUIRE(panned.has_value());
			// Dragging the image to the right moves the camera along the image's left: the axes agree.
			CHECK(glm::dot(glm::normalize(panned->Position - camera.Position), renderedRight) == doctest::Approx(-1.0f).epsilon(1e-3));
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

		// Docs/Decisions/0032-m21-contract.md decision 14 (stream D): the scene view's 2D mode.
		TEST_CASE("EditorCamera: the 2D camera looks down -Z orthographically and navigates by pan and zoom only")
		{
			const ExplicitRenderCamera perspective{ .Position = glm::vec3(4.0f, 3.0f, 10.0f), .Target = glm::vec3(4.0f, 1.0f, 0.0f) };
			const Result<ExplicitRenderCamera> flat = EditorCamera::Make2D(perspective);
			REQUIRE_MESSAGE(flat.has_value(), flat.error().ToString());
			CHECK(flat->Projection == RenderProjection::Orthographic);
			CHECK(flat->Target == glm::vec3(4.0f, 1.0f, 0.0f));
			CHECK(flat->Position == glm::vec3(4.0f, 1.0f, EditorCamera::Make2DDistance));
			CHECK(flat->OrthographicSize > 0.0f);

			// Orbit and fly are ignored; pan moves both points in X and Y; the wheel zooms.
			const Result<ExplicitRenderCamera> orbited = EditorCamera::Navigate2D(*flat, { .PointerDelta = glm::vec2(50.0f, 0.0f), .Orbit = true }, 360.0f);
			REQUIRE(orbited.has_value());
			CHECK(orbited->Position == flat->Position);
			const Result<ExplicitRenderCamera> panned = EditorCamera::Navigate2D(*flat, { .PointerDelta = glm::vec2(36.0f, 0.0f), .Pan = true }, 360.0f);
			REQUIRE(panned.has_value());
			CHECK(panned->Position.z == flat->Position.z);
			CHECK(panned->Target.z == 0.0f);
			CHECK(panned->Position.x != flat->Position.x);
			CHECK(panned->Position.x - flat->Position.x == doctest::Approx(panned->Target.x - flat->Target.x));
			const Result<ExplicitRenderCamera> zoomed = EditorCamera::Navigate2D(*flat, { .Wheel = 1.0f }, 360.0f);
			REQUIRE(zoomed.has_value());
			CHECK(zoomed->OrthographicSize == doctest::Approx(flat->OrthographicSize / 1.2f));
			CHECK(zoomed->Projection == RenderProjection::Orthographic);
		}

		TEST_CASE("EditorCamera: the 2D camera keeps an orthographic size, clamps its zoom and refuses invalid input")
		{
			// A perspective camera's half height at its target distance: 10 m away at 60 degrees shows 10 tan 30 degrees.
			const ExplicitRenderCamera ahead{ .Position = glm::vec3(0.0f, 0.0f, 10.0f), .Target = glm::vec3(0.0f) };
			const Result<ExplicitRenderCamera> fromPerspective = EditorCamera::Make2D(ahead);
			REQUIRE(fromPerspective.has_value());
			CHECK(fromPerspective->OrthographicSize == doctest::Approx(10.0 * std::tan(std::numbers::pi / 6.0)));
			CHECK(fromPerspective->NearClip == 0.1f);
			CHECK(fromPerspective->FarClip == 2.0f * EditorCamera::Make2DDistance);

			// An orthographic camera keeps its size, and a 2D camera is its own 2D camera.
			const ExplicitRenderCamera side{ .Position = glm::vec3(10.0f, 2.0f, -3.0f), .Target = glm::vec3(0.0f, 2.0f, -3.0f), .Projection = RenderProjection::Orthographic, .OrthographicSize = 3.5f };
			const Result<ExplicitRenderCamera> flat = EditorCamera::Make2D(side);
			REQUIRE(flat.has_value());
			CHECK(flat->OrthographicSize == 3.5f);
			CHECK(flat->Target == glm::vec3(0.0f, 2.0f, 0.0f));
			const Result<ExplicitRenderCamera> again = EditorCamera::Make2D(*flat);
			REQUIRE(again.has_value());
			CHECK(again->Position == flat->Position);
			CHECK(again->Target == flat->Target);
			CHECK(again->OrthographicSize == flat->OrthographicSize);

			// The wheel's zoom stays in [0.01, 100000]; navigating a camera that is not 2D makes it 2D first.
			const Result<ExplicitRenderCamera> closest = EditorCamera::Navigate2D(*flat, { .Wheel = 100.0f }, 360.0f);
			REQUIRE(closest.has_value());
			CHECK(closest->OrthographicSize == doctest::Approx(0.01f));
			const Result<ExplicitRenderCamera> farthest = EditorCamera::Navigate2D(*flat, { .Wheel = -200.0f }, 360.0f);
			REQUIRE(farthest.has_value());
			CHECK(farthest->OrthographicSize == doctest::Approx(100000.0f));
			const Result<ExplicitRenderCamera> made = EditorCamera::Navigate2D(ahead, {}, 360.0f);
			REQUIRE(made.has_value());
			CHECK(made->Projection == RenderProjection::Orthographic);
			CHECK(made->Position == glm::vec3(0.0f, 0.0f, EditorCamera::Make2DDistance));

			// Invalid cameras, views and input are refused.
			const Result<ExplicitRenderCamera> coincident = EditorCamera::Make2D({ .Position = glm::vec3(1.0f), .Target = glm::vec3(1.0f) });
			REQUIRE_FALSE(coincident.has_value());
			CHECK(coincident.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<ExplicitRenderCamera> flatView = EditorCamera::Navigate2D(*flat, {}, 0.5f);
			REQUIRE_FALSE(flatView.has_value());
			CHECK(flatView.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<ExplicitRenderCamera> infinite = EditorCamera::Navigate2D(*flat, { .Wheel = std::numeric_limits<float>::infinity() }, 360.0f);
			REQUIRE_FALSE(infinite.has_value());
			CHECK(infinite.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
