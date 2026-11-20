#include "TestsPCH.h"

#include "Engine/Renderer/PlanarReflections.h"

#include "Engine/Renderer/RenderPrepare.h"
#include "Support/GlmApprox.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

// The planar reflections' CPU part (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3): the plane, the mirror
// and cull cameras, the facing test, the ranking and the cap.

namespace Engine {

	namespace {

		// A perspective camera at `position` looking at `target`, at `width` x `height`.
		CameraData MakePerspectiveCamera(const glm::vec3& position, const glm::vec3& target, uint32_t width = 160, uint32_t height = 90)
		{
			CameraData camera;
			camera.ProjectionKind = RenderProjection::Perspective;
			camera.VerticalFov = 60.0f;
			camera.NearClip = 0.1f;
			camera.FarClip = 1000.0f;
			camera.Position = position;
			camera.View = glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
			camera.ViewportWidth = width;
			camera.ViewportHeight = height;
			camera.Projection = ComputeReverseZProjection(camera.ProjectionKind, camera.VerticalFov, camera.OrthographicSize, camera.NearClip,
				camera.FarClip, width, height);
			return camera;
		}

		// An orthographic camera at `position` looking along -Z.
		CameraData MakeOrthographicCamera(const glm::vec3& position)
		{
			CameraData camera;
			camera.ProjectionKind = RenderProjection::Orthographic;
			camera.OrthographicSize = 5.0f;
			camera.NearClip = 0.1f;
			camera.FarClip = 100.0f;
			camera.Position = position;
			camera.View = glm::translate(glm::mat4(1.0f), -position);
			camera.ViewportWidth = 16;
			camera.ViewportHeight = 9;
			camera.Projection = ComputeReverseZProjection(camera.ProjectionKind, camera.VerticalFov, camera.OrthographicSize, camera.NearClip,
				camera.FarClip, 16, 9);
			return camera;
		}

		// The normalized device coordinates of the world point `point` through `viewProjection`.
		glm::vec3 Project(const glm::mat4& viewProjection, const glm::vec3& point)
		{
			const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
			return glm::vec3(clip) / clip.w;
		}

		// A small box around the world point `point` (the identity world matrix).
		Aabb MakeBox(const glm::vec3& point)
		{
			Aabb box;
			box.Extend(point - glm::vec3(0.01f));
			box.Extend(point + glm::vec3(0.01f));
			return box;
		}

		Aabb MakeQuadBounds()
		{
			Aabb quad;
			quad.Extend(glm::vec3(-0.5f, -0.5f, 0.0f));
			quad.Extend(glm::vec3(0.5f, 0.5f, 0.0f));
			return quad;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("PlanarReflections: the mirrored camera is rigid and maps the plane to the view's pixels mirrored")
		{
			const CameraData camera = MakePerspectiveCamera(glm::vec3(1.0f, 2.0f, 5.0f), glm::vec3(0.0f, 1.0f, -3.0f));
			// A wall mirror at z = -3 facing +Z.
			const PlanarReflectionPlane plane{ .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .Distance = 3.0f };
			const CameraData mirror = MakePlanarReflectionCamera(camera, plane, 80, 45);

			// Rigid: an orthonormal rotation with the determinant +1, so no draw's winding or face test turns.
			const glm::mat3 rotation(mirror.View);
			CHECK(glm::determinant(rotation) == doctest::Approx(1.0f));
			CHECK(Test::ApproxEqual(glm::transpose(rotation) * rotation, glm::mat3(1.0f), 1e-5f));
			// The camera reflected in the plane, at the origin of its own view.
			CHECK(Test::ApproxEqual(mirror.Position, glm::vec3(1.0f, 2.0f, -11.0f), 1e-5f));
			CHECK(Test::ApproxEqual(glm::vec3(mirror.View * glm::vec4(mirror.Position, 1.0f)), glm::vec3(0.0f), 1e-4f));
			// The camera's projection and clip range at the reflection's size, with no history or jitter.
			CHECK(mirror.Projection == camera.Projection);
			CHECK(mirror.ViewportWidth == 80);
			CHECK(mirror.ViewportHeight == 45);
			CHECK_FALSE(mirror.PreviousViewProjection.has_value());
			CHECK(mirror.Jitter == glm::vec2(0.0f));

			// Every point of the plane projects to the main view's position mirrored left for right, at its depth.
			const glm::mat4 main = camera.Projection * camera.View;
			const glm::mat4 reflected = mirror.Projection * mirror.View;
			for (const glm::vec3 point : { glm::vec3(0.0f, 1.0f, -3.0f), glm::vec3(-1.5f, 0.25f, -3.0f), glm::vec3(2.0f, 2.5f, -3.0f), glm::vec3(0.75f, -0.5f, -3.0f) })
			{
				CAPTURE(point);
				const glm::vec3 seen = Project(main, point);
				const glm::vec3 inMirror = Project(reflected, point);
				CHECK(inMirror.x == doctest::Approx(-seen.x).epsilon(1e-4));
				CHECK(inMirror.y == doctest::Approx(seen.y).epsilon(1e-4));
				CHECK(inMirror.z == doctest::Approx(seen.z).epsilon(1e-4));
			}

			// A point in front of the mirror is seen in it where its reflection behind the glass would be seen, mirrored.
			const glm::vec3 object(0.5f, 1.0f, -1.0f);
			const glm::mat4 reflection = ComputePlanarReflectionMatrix(plane);
			const glm::vec3 image = glm::vec3(reflection * glm::vec4(object, 1.0f));
			CHECK(Test::ApproxEqual(image, glm::vec3(0.5f, 1.0f, -5.0f), 1e-5f));
			const glm::vec3 seenImage = Project(main, image);
			const glm::vec3 objectInMirror = Project(reflected, object);
			CHECK(objectInMirror.x == doctest::Approx(-seenImage.x).epsilon(1e-4));
			CHECK(objectInMirror.y == doctest::Approx(seenImage.y).epsilon(1e-4));
		}

		TEST_CASE("PlanarReflections: a reflection faces the camera only from its reflecting side")
		{
			const PlanarReflectionPlane wall{ .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .Distance = 3.0f };
			const PlanarReflectionPlane back{ .Normal = glm::vec3(0.0f, 0.0f, -1.0f), .Distance = -3.0f };
			// Perspective: by the camera's position, whatever it looks at.
			CHECK(IsPlanarReflectionFacing(MakePerspectiveCamera(glm::vec3(0.0f, 1.0f, 5.0f), glm::vec3(0.0f, 1.0f, -3.0f)), wall));
			CHECK(IsPlanarReflectionFacing(MakePerspectiveCamera(glm::vec3(0.0f, 1.0f, 5.0f), glm::vec3(0.0f, 1.0f, 10.0f)), wall));
			CHECK_FALSE(IsPlanarReflectionFacing(MakePerspectiveCamera(glm::vec3(0.0f, 1.0f, 5.0f), glm::vec3(0.0f, 1.0f, -3.0f)), back));
			CHECK_FALSE(IsPlanarReflectionFacing(MakePerspectiveCamera(glm::vec3(0.0f, 1.0f, -8.0f), glm::vec3(0.0f, 1.0f, -3.0f)), wall));
			CHECK(IsPlanarReflectionFacing(MakePerspectiveCamera(glm::vec3(0.0f, 1.0f, -8.0f), glm::vec3(0.0f, 1.0f, -3.0f)), back));
			// A camera on the plane sees it edge on.
			CHECK_FALSE(IsPlanarReflectionFacing(MakePerspectiveCamera(glm::vec3(0.0f, 1.0f, -3.0f), glm::vec3(0.0f, 1.0f, -10.0f)), wall));
			// Orthographic: by the view direction (-Z here), wherever the camera stands.
			CHECK(IsPlanarReflectionFacing(MakeOrthographicCamera(glm::vec3(0.0f, 0.0f, 5.0f)), wall));
			CHECK(IsPlanarReflectionFacing(MakeOrthographicCamera(glm::vec3(0.0f, 0.0f, -8.0f)), wall));
			CHECK_FALSE(IsPlanarReflectionFacing(MakeOrthographicCamera(glm::vec3(0.0f, 0.0f, 5.0f)), back));
			// A plane seen edge on by an orthographic camera faces none.
			CHECK_FALSE(IsPlanarReflectionFacing(MakeOrthographicCamera(glm::vec3(0.0f, 0.0f, 5.0f)),
				PlanarReflectionPlane{ .Normal = glm::vec3(1.0f, 0.0f, 0.0f), .Distance = 0.0f }));
		}

		TEST_CASE("PlanarReflections: reflections rank by screen area with ties by UUID")
		{
			const std::vector<PlanarReflectionCandidate> candidates = {
				{ .Item = 0, .Entity = UUID(10), .ScreenArea = 10.0f },
				{ .Item = 1, .Entity = UUID(3), .ScreenArea = 50.0f },
				{ .Item = 2, .Entity = UUID(2), .ScreenArea = 50.0f },
				{ .Item = 3, .Entity = UUID(1), .ScreenArea = 5.0f },
				{ .Item = 4, .Entity = UUID(7), .ScreenArea = 30.0f },
				{ .Item = 5, .Entity = UUID(4), .ScreenArea = std::numeric_limits<float>::quiet_NaN() },
			};
			const PlanarReflectionSelection selection = RankPlanarReflections(candidates);
			CHECK(selection.Slots == std::vector<size_t>{ 2, 1, 4, 0 });
			CHECK(selection.Dropped == 2);
			// The order is a function of the candidates, not of their order.
			const std::vector<PlanarReflectionCandidate> reversed(candidates.rbegin(), candidates.rend());
			CHECK(RankPlanarReflections(reversed).Slots == selection.Slots);
			// A lower cap keeps the largest.
			const PlanarReflectionSelection two = RankPlanarReflections(candidates, 2);
			CHECK(two.Slots == std::vector<size_t>{ 2, 1 });
			CHECK(two.Dropped == 4);
			CHECK(RankPlanarReflections({}).Slots.empty());
			CHECK(MaxPlanarReflectionsPerView == 4);
		}

		TEST_CASE("PlanarReflections: the cull camera keeps what is seen through the glass and nothing beside it")
		{
			const CameraData camera = MakePerspectiveCamera(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 0.0f));
			const PlanarReflectionPlane plane{ .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .Distance = 0.0f };
			// A unit glass on the plane, two metres to the right of the camera's axis.
			const glm::mat4 glassWorld = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f));
			const std::optional<PlanarReflectionScreenRect> rect = ComputePlanarReflectionScreenRect(MakeQuadBounds(), glassWorld, camera);
			REQUIRE(rect.has_value());
			CHECK(rect->Min.x > 0.0f);
			const CameraData mirror = MakePlanarReflectionCamera(camera, plane, 80, 45);
			const CameraData cull = MakePlanarReflectionCullCamera(mirror, *rect);
			CHECK(cull.View == mirror.View);

			// On the line from the mirror camera through the glass's centre: seen through the glass.
			const Aabb through = MakeBox(glm::vec3(3.2f, 0.0f, 3.0f));
			CHECK_FALSE(IsOutsideView(through, glm::mat4(1.0f), cull));
			// In the mirror camera's view but beside the glass (its reflection is left of the camera's axis): culled.
			const Aabb beside = MakeBox(glm::vec3(-2.0f, 0.0f, 3.0f));
			CHECK_FALSE(IsOutsideView(beside, glm::mat4(1.0f), mirror));
			CHECK(IsOutsideView(beside, glm::mat4(1.0f), cull));
			// The glass's own corners, mirrored, are the cull camera's clip volume's sides.
			const glm::vec3 corner = Project(cull.Projection * cull.View, glm::vec3(1.5f, -0.5f, 0.0f));
			CHECK(std::abs(corner.x) == doctest::Approx(1.0f).epsilon(1e-4));
			CHECK(corner.y == doctest::Approx(-1.0f).epsilon(1e-4));
			const PlanarReflectionScreenRect mirrored = MirrorPlanarReflectionScreenRect(*rect);
			CHECK(mirrored.Min.x == -rect->Max.x);
			CHECK(mirrored.Max.x == -rect->Min.x);
			CHECK(mirrored.Min.y == rect->Min.y);
		}

		TEST_CASE("PlanarReflections: the plane follows a scaled, turned and moved World")
		{
			// Local +Z through (0, 0, 0.5), scaled by 4 along Z, turned to +X and moved: the plane x = 3.
			PlanarReflectionItem item;
			item.World = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)) * glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f))
				* glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 4.0f));
			item.Normal = glm::vec3(0.0f, 0.0f, 3.0f);
			item.Offset = 0.5f;
			const std::optional<PlanarReflectionPlane> plane = ComputePlanarReflectionPlane(item);
			REQUIRE(plane.has_value());
			CHECK(Test::ApproxEqual(plane->Normal, glm::vec3(1.0f, 0.0f, 0.0f), 1e-6f));
			CHECK(plane->Distance == doctest::Approx(-3.0f));

			// A slanted normal under a non-uniform scale goes through the inverse transpose: the plane x + y = 0 scaled by 2
			// along X is 2y + x = 0.
			item.World = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f));
			item.Normal = glm::vec3(1.0f, 1.0f, 0.0f);
			item.Offset = 0.0f;
			const std::optional<PlanarReflectionPlane> slanted = ComputePlanarReflectionPlane(item);
			REQUIRE(slanted.has_value());
			CHECK(Test::ApproxEqual(slanted->Normal, glm::normalize(glm::vec3(1.0f, 2.0f, 0.0f)), 1e-6f));
			CHECK(slanted->SignedDistance(glm::vec3(2.0f, -1.0f, 5.0f)) == doctest::Approx(0.0f).epsilon(1e-6));

			// A mirroring World keeps the reflecting side on the image of the local positive side.
			item.World = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f));
			item.Normal = glm::vec3(0.0f, 0.0f, 1.0f);
			item.Offset = 1.0f;
			const std::optional<PlanarReflectionPlane> mirrored = ComputePlanarReflectionPlane(item);
			REQUIRE(mirrored.has_value());
			CHECK(Test::ApproxEqual(mirrored->Normal, glm::vec3(0.0f, 0.0f, -1.0f), 1e-6f));
			CHECK(mirrored->SignedDistance(glm::vec3(0.0f, 0.0f, -2.0f)) == doctest::Approx(1.0f));

			// A zero or non-finite normal, a singular or non-finite World and a non-finite offset give no plane.
			const auto planeOf = [](const glm::mat4& world, const glm::vec3& normal, float offset)
			{
				return ComputePlanarReflectionPlane(PlanarReflectionItem{ .World = world, .Normal = normal, .Offset = offset });
			};
			CHECK_FALSE(planeOf(glm::mat4(1.0f), glm::vec3(0.0f), 0.0f).has_value());
			CHECK_FALSE(planeOf(glm::mat4(1.0f), glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f), 0.0f).has_value());
			CHECK_FALSE(planeOf(glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, 0.0f)), glm::vec3(0.0f, 0.0f, 1.0f), 0.0f).has_value());
			CHECK_FALSE(planeOf(glm::mat4(std::numeric_limits<float>::infinity()), glm::vec3(0.0f, 0.0f, 1.0f), 0.0f).has_value());
			CHECK_FALSE(planeOf(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.0f), std::numeric_limits<float>::infinity()).has_value());
			// A tiny normal is a direction all the same.
			CHECK(planeOf(glm::mat4(1.0f), glm::vec3(0.0f, 1.0e-30f, 0.0f), 0.0f).has_value());
		}

		TEST_CASE("PlanarReflections: the reflection matrix is an involution that fixes the plane")
		{
			const PlanarReflectionPlane plane{ .Normal = glm::normalize(glm::vec3(1.0f, 2.0f, -2.0f)), .Distance = 1.5f };
			const glm::mat4 reflection = ComputePlanarReflectionMatrix(plane);
			CHECK(Test::ApproxEqual(reflection * reflection, glm::mat4(1.0f), 1e-5f));
			CHECK(glm::determinant(glm::mat3(reflection)) == doctest::Approx(-1.0f));
			const glm::vec3 onPlane = -1.5f * plane.Normal + glm::vec3(2.0f, -1.0f, 0.0f);
			CHECK(plane.SignedDistance(onPlane) == doctest::Approx(0.0f).epsilon(1e-5));
			CHECK(Test::ApproxEqual(glm::vec3(reflection * glm::vec4(onPlane, 1.0f)), onPlane, 1e-5f));
			const glm::vec3 point(3.0f, 1.0f, 4.0f);
			const glm::vec3 image = glm::vec3(reflection * glm::vec4(point, 1.0f));
			CHECK(plane.SignedDistance(image) == doctest::Approx(-plane.SignedDistance(point)).epsilon(1e-5));
		}

		TEST_CASE("PlanarReflections: a reflection's size is its scale of the scene resolution, rounded up")
		{
			CHECK(ComputePlanarReflectionSize(1920, 1080, 0.5f) == glm::uvec2(960, 540));
			CHECK(ComputePlanarReflectionSize(1366, 768, 0.5f) == glm::uvec2(683, 384));
			CHECK(ComputePlanarReflectionSize(101, 51, 0.25f) == glm::uvec2(26, 13));
			CHECK(ComputePlanarReflectionSize(1920, 1080, 1.0f) == glm::uvec2(1920, 1080));
			// Clamped to [0.25, 1]; a non-finite scale takes the default; never below one texel.
			CHECK(ComputePlanarReflectionSize(400, 200, 0.1f) == glm::uvec2(100, 50));
			CHECK(ComputePlanarReflectionSize(400, 200, 2.0f) == glm::uvec2(400, 200));
			CHECK(ComputePlanarReflectionSize(400, 200, std::numeric_limits<float>::quiet_NaN()) == glm::uvec2(200, 100));
			CHECK(ComputePlanarReflectionSize(1, 1, 0.25f) == glm::uvec2(1, 1));
		}

		TEST_CASE("PlanarReflections: a glass's screen rectangle is clipped to the view, and the whole view when it reaches behind the camera")
		{
			const CameraData camera = MakePerspectiveCamera(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 0.0f));
			const std::optional<PlanarReflectionScreenRect> centred = ComputePlanarReflectionScreenRect(MakeQuadBounds(), glm::mat4(1.0f), camera);
			REQUIRE(centred.has_value());
			CHECK(centred->Min.x == doctest::Approx(-centred->Max.x));
			CHECK(centred->Min.y == doctest::Approx(-centred->Max.y));
			const float halfHeight = 0.5f / (5.0f * std::tan(glm::radians(30.0f)));
			CHECK(centred->Max.y == doctest::Approx(halfHeight).epsilon(1e-4));
			CHECK(centred->GetPixelArea(160, 90) == doctest::Approx(centred->Max.x * 160.0f * centred->Max.y * 90.0f).epsilon(1e-4));
			// Partly beyond the right edge: clipped there.
			const std::optional<PlanarReflectionScreenRect> edge =
				ComputePlanarReflectionScreenRect(MakeQuadBounds(), glm::scale(glm::mat4(1.0f), glm::vec3(20.0f, 1.0f, 1.0f)), camera);
			REQUIRE(edge.has_value());
			CHECK(edge->Max.x == 1.0f);
			CHECK(edge->Min.x == -1.0f);
			// Wholly outside: none.
			CHECK_FALSE(ComputePlanarReflectionScreenRect(MakeQuadBounds(), glm::translate(glm::mat4(1.0f), glm::vec3(40.0f, 0.0f, 0.0f)), camera).has_value());
			CHECK_FALSE(ComputePlanarReflectionScreenRect(Aabb{}, glm::mat4(1.0f), camera).has_value());
			// A floor reaching behind the camera: the whole view.
			const glm::mat4 floor = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(40.0f, 1.0f, 40.0f))
				* glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			const std::optional<PlanarReflectionScreenRect> whole = ComputePlanarReflectionScreenRect(MakeQuadBounds(), floor, camera);
			REQUIRE(whole.has_value());
			CHECK(*whole == PlanarReflectionScreenRect{});
		}

		TEST_CASE("PlanarReflections: the clip plane lies the bias in front of the plane, and what lies wholly behind it is culled")
		{
			const PlanarReflectionPlane plane{ .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .Distance = 3.0f };
			const glm::vec4 clip = ComputePlanarReflectionClipPlane(plane);
			CHECK(clip == glm::vec4(0.0f, 0.0f, 1.0f, 3.0f - PlanarReflectionClipBias));
			CHECK(PlanarReflectionClipBias == 0.01f);
			// Behind the glass, and the glass itself on the plane: culled. In front, or crossing the clip plane: kept.
			CHECK(IsBehindPlanarReflectionClipPlane(MakeBox(glm::vec3(0.0f, 0.0f, -4.0f)), glm::mat4(1.0f), clip));
			CHECK(IsBehindPlanarReflectionClipPlane(MakeQuadBounds(), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)), clip));
			CHECK_FALSE(IsBehindPlanarReflectionClipPlane(MakeBox(glm::vec3(0.0f, 0.0f, -2.0f)), glm::mat4(1.0f), clip));
			Aabb crossing;
			crossing.Extend(glm::vec3(0.0f, 0.0f, -5.0f));
			crossing.Extend(glm::vec3(0.0f, 0.0f, -1.0f));
			CHECK_FALSE(IsBehindPlanarReflectionClipPlane(crossing, glm::mat4(1.0f), clip));
			CHECK_FALSE(IsBehindPlanarReflectionClipPlane(Aabb{}, glm::mat4(1.0f), clip));
		}

		TEST_CASE("PlanarReflections: the cascade depth plane gives the main camera's view depth of a point")
		{
			const CameraData camera = MakePerspectiveCamera(glm::vec3(1.0f, 2.0f, 5.0f), glm::vec3(-2.0f, 0.0f, -3.0f));
			const glm::vec4 depthPlane = ComputeCascadeDepthPlane(camera);
			for (const glm::vec3 point : { glm::vec3(0.0f), glm::vec3(-2.0f, 0.0f, -3.0f), glm::vec3(4.0f, -1.0f, 2.0f) })
			{
				CAPTURE(point);
				CHECK(glm::dot(depthPlane, glm::vec4(point, 1.0f)) == doctest::Approx(-(camera.View * glm::vec4(point, 1.0f)).z).epsilon(1e-5));
			}
		}
	}

}
