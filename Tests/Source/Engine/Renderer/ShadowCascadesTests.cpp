#include "TestsPCH.h"

#include "Engine/Renderer/ShadowCascades.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
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

		// The light-space basis BuildShadowCascades snaps along (ShadowCascades.h): back points toward the light.
		struct ShadowTestBasis
		{
			glm::dvec3 Right{};
			glm::dvec3 Up{};
			glm::dvec3 Back{};
		};

		[[nodiscard]] static ShadowTestBasis GetShadowTestBasis(const LightData& light)
		{
			const glm::dvec3 back = -glm::normalize(glm::dvec3(light.Direction));
			const glm::dvec3 reference = std::abs(back.y) < 0.99 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
			const glm::dvec3 right = glm::normalize(glm::cross(reference, back));
			return { .Right = right, .Up = glm::cross(back, right), .Back = back };
		}

		// A view matrix with a fixed rotation (identical bits at every position) and the camera at `position`.
		[[nodiscard]] static glm::mat4 ShadowTestView(const glm::mat3& rotation, const glm::vec3& position)
		{
			return glm::mat4(glm::transpose(rotation)) * glm::translate(glm::mat4(1.0f), -position);
		}

		// The world-space centre of cascade `index`'s fitted slice (the sphere centre before snapping), from its corners.
		[[nodiscard]] static glm::dvec3 ShadowSliceCentre(const ShadowCascade& cascade)
		{
			glm::dvec3 centre(0.0);
			for (const glm::vec3& corner : cascade.Corners)
				centre += glm::dvec3(corner) / 8.0;
			return centre;
		}

		// The snapped light-space centre a cascade matrix encodes: projection[3] = (-c.x / R, -c.y / R, (R - c.z) / D, 1).
		[[nodiscard]] static glm::dvec3 ShadowSnappedCentre(const ShadowCascade& cascade)
		{
			const glm::dvec4 translation(cascade.ViewProjection[3]);
			const double extent = cascade.Radius;
			return { -translation.x * extent, -translation.y * extent, extent - translation.z * cascade.LightFar };
		}

		// The stabilized radius of a slice whose bounding sphere has radius `radius`, bracketed by a relative 1e-6 so float
		// rounding of the corner inputs cannot hide or fake a mismatch.
		static void CheckShadowRadius(const ShadowCascade& cascade, double radius, uint32_t shadowMapSize)
		{
			const auto lower = ComputeStabilizedCascadeRadius(static_cast<float>(radius * (1.0 - 1.0e-6)), shadowMapSize);
			const auto upper = ComputeStabilizedCascadeRadius(static_cast<float>(radius * (1.0 + 1.0e-6)), shadowMapSize);
			REQUIRE(lower);
			REQUIRE(upper);
			CHECK(cascade.Radius >= *lower);
			CHECK(cascade.Radius <= *upper);
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
			// Each cascade's sphere is the fitted box's: its centre is the box centre on the view axis and its radius the box's
			// half diagonal, stabilized. An oversized fit (a ShadowDistance-sized sphere) wastes resolution and fails here.
			const Utils::ShadowTestBasis basis = Utils::GetShadowTestBasis(light);
			const glm::dmat4 inverseView = glm::inverse(glm::dmat4(camera.View));
			for (uint32_t index = 0; index < cascades->Count; ++index)
			{
				CAPTURE(index);
				const ShadowCascade& cascade = cascades->Cascades[index];
				const double sliceNear = index == 0 ? cascade.SplitNear : cascades->Cascades[index - 1].BlendStart;
				const double halfDepth = (static_cast<double>(cascade.SplitFar) - sliceNear) * 0.5;
				Utils::CheckShadowRadius(cascade, std::sqrt(20.0 * 20.0 + 10.0 * 10.0 + halfDepth * halfDepth), 256);
				const glm::dvec3 boxCentre(inverseView * glm::dvec4(0, 0, -(sliceNear + halfDepth), 1));
				const glm::dvec3 lightCentre(glm::dot(basis.Right, boxCentre), glm::dot(basis.Up, boxCentre), glm::dot(basis.Back, boxCentre));
				const glm::dvec3 snapped = Utils::ShadowSnappedCentre(cascade);
				const double tolerance = cascade.TexelWorldSize * 0.5 + 1.0e-4 * cascade.Radius;
				for (glm::length_t axis = 0; axis < 3; ++axis)
					CHECK(std::abs(snapped[axis] - lightCentre[axis]) <= tolerance);
			}
		}

		TEST_CASE("Shadows: a perspective slice fits the bounding sphere of its corners")
		{
			const CameraData camera = Utils::ShadowTestCamera();
			LightData light;
			light.Direction = glm::vec3(-0.7f, -1, -0.2f);
			const auto cascades = BuildShadowCascades(camera, light, 0, 1024);
			REQUIRE(cascades);
			const Utils::ShadowTestBasis basis = Utils::GetShadowTestBasis(light);
			for (uint32_t index = 0; index < cascades->Count; ++index)
			{
				CAPTURE(index);
				const ShadowCascade& cascade = cascades->Cascades[index];
				// The sphere is centred on the corners' centroid and reaches the farthest corner.
				const glm::dvec3 centre = Utils::ShadowSliceCentre(cascade);
				double radius = 0.0;
				for (const glm::vec3& corner : cascade.Corners)
					radius = std::max(radius, glm::length(glm::dvec3(corner) - centre));
				Utils::CheckShadowRadius(cascade, radius, 1024);
				const glm::dvec3 lightCentre(glm::dot(basis.Right, centre), glm::dot(basis.Up, centre), glm::dot(basis.Back, centre));
				const glm::dvec3 snapped = Utils::ShadowSnappedCentre(cascade);
				const double tolerance = cascade.TexelWorldSize * 0.5 + 1.0e-4 * cascade.Radius;
				for (glm::length_t axis = 0; axis < 3; ++axis)
					CHECK(std::abs(snapped[axis] - lightCentre[axis]) <= tolerance);
			}
		}

		TEST_CASE("Shadows: sub-texel moves of a rotated camera leave an oblique light's matrices unchanged")
		{
			// An oblique light's basis differs from the world axes, so snapping in world space or along a wrong basis would
			// change the matrices under these moves (the default light's light space is world space, M9-F05).
			LightData light;
			light.Direction = glm::vec3(-0.7f, -1, -0.2f);
			light.CascadeCount = 2;
			const Utils::ShadowTestBasis basis = Utils::GetShadowTestBasis(light);
			for (const RenderProjection kind : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				CAPTURE(static_cast<uint32_t>(kind));
				CameraData camera = Utils::ShadowTestCamera(kind);
				const glm::mat3 rotation = glm::mat3(glm::rotate(glm::mat4(1.0f), 0.6f, glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f))));
				const glm::vec3 start(3.25f, 1.5f, -7.0f);
				camera.Position = start;
				camera.View = Utils::ShadowTestView(rotation, camera.Position);
				const auto original = BuildShadowCascades(camera, light, 0, 2048);
				REQUIRE(original);
				const ShadowCascade& first = original->Cascades[0];
				// Put cascade 0's sphere centre on a texel-grid point in light space, the middle of its snap cell.
				const glm::dvec3 centre = Utils::ShadowSliceCentre(first);
				const double texel = first.TexelWorldSize;
				glm::dvec3 offset(0.0);
				for (const glm::dvec3& axis : { basis.Right, basis.Up, basis.Back })
				{
					const double coordinate = glm::dot(axis, centre);
					offset += axis * (std::floor(coordinate / texel + 0.5) * texel - coordinate);
				}
				camera.Position = glm::vec3(glm::dvec3(start) + offset);
				camera.View = Utils::ShadowTestView(rotation, camera.Position);
				const auto centred = BuildShadowCascades(camera, light, 0, 2048);
				REQUIRE(centred);
				for (const glm::dvec3& axis : { basis.Right, basis.Up, basis.Back, basis.Right + basis.Up + basis.Back })
				{
					CameraData moved = camera;
					moved.Position = glm::vec3(glm::dvec3(camera.Position) + axis * (0.37 * texel));
					moved.View = Utils::ShadowTestView(rotation, moved.Position);
					const auto stepped = BuildShadowCascades(moved, light, 0, 2048);
					REQUIRE(stepped);
					CHECK(stepped->Cascades[0].ViewProjection == centred->Cascades[0].ViewProjection);
					CHECK(stepped->Cascades[0].Radius == centred->Cascades[0].Radius);
					Utils::CheckShadowContainment(*stepped);
				}
				// A whole texel along the light's right axis moves the snapped centre by exactly one texel.
				CameraData across = camera;
				across.Position = glm::vec3(glm::dvec3(camera.Position) + basis.Right * texel);
				across.View = Utils::ShadowTestView(rotation, across.Position);
				const auto crossed = BuildShadowCascades(across, light, 0, 2048);
				REQUIRE(crossed);
				const glm::dvec3 delta = Utils::ShadowSnappedCentre(crossed->Cascades[0]) - Utils::ShadowSnappedCentre(centred->Cascades[0]);
				CHECK(delta.x == doctest::Approx(texel).epsilon(1.0e-3));
				CHECK(std::abs(delta.y) < texel * 0.5);
				CHECK(std::abs(delta.z) < texel * 0.5);
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
				if (index + 1 < cascades->Count)
					CHECK(cascade.BlendStart == doctest::Approx(cascade.SplitFar - 0.1f * (cascade.SplitFar - cascade.SplitNear)));
				else
					CHECK(cascade.BlendStart == doctest::Approx(0.9f * cascade.SplitFar));
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

		TEST_CASE("Shadows: the final fade covers the last tenth of the effective shadow distance")
		{
			// Default light: ShadowDistance 100, four cascades, lambda 0.75; the last cascade spans [32.09, 100].
			CameraData camera = Utils::ShadowTestCamera();
			const auto cascades = BuildShadowCascades(camera, LightData{}, 0, 1024);
			REQUIRE(cascades);
			const ShadowCascade& last = cascades->Cascades[cascades->Count - 1];
			CHECK(last.SplitFar == doctest::Approx(100.0f));
			CHECK(last.BlendStart == doctest::Approx(90.0f));
			// A far plane nearer than ShadowDistance is the effective distance.
			camera.FarClip = 50.0f;
			const auto clipped = BuildShadowCascades(camera, LightData{}, 0, 1024);
			REQUIRE(clipped);
			CHECK(clipped->Cascades[clipped->Count - 1].SplitFar == doctest::Approx(50.0f));
			CHECK(clipped->Cascades[clipped->Count - 1].BlendStart == doctest::Approx(45.0f));
			// A single cascade fades over the same last tenth.
			LightData single;
			single.CascadeCount = 1;
			single.ShadowDistance = 20.0f;
			const auto one = BuildShadowCascades(Utils::ShadowTestCamera(), single, 0, 1024);
			REQUIRE(one);
			CHECK(one->Cascades[0].BlendStart == doctest::Approx(18.0f));
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

		TEST_CASE("Shadows: snapped slices of an oblique light stay contained under worst-case light-axis shifts")
		{
			LightData light;
			light.Direction = glm::vec3(-0.7f, -1, -0.2f);
			light.CascadeCount = 1;
			const Utils::ShadowTestBasis basis = Utils::GetShadowTestBasis(light);
			CameraData camera = Utils::ShadowTestCamera(RenderProjection::Orthographic, 1000, 1);
			const glm::mat3 rotation = glm::mat3(glm::rotate(glm::mat4(1.0f), -0.9f, glm::normalize(glm::vec3(0.2f, 1.0f, -0.4f))));
			camera.View = Utils::ShadowTestView(rotation, camera.Position);
			const auto initial = BuildShadowCascades(camera, light, 0, 256);
			REQUIRE(initial);
			const double texel = initial->Cascades[0].TexelWorldSize;
			const glm::dvec3 centre = Utils::ShadowSliceCentre(initial->Cascades[0]);
			glm::dvec3 offset(0.0);
			for (const glm::dvec3& axis : { basis.Right, basis.Up, basis.Back })
			{
				const double coordinate = glm::dot(axis, centre);
				offset += axis * (std::floor(coordinate / texel + 0.5) * texel - coordinate);
			}
			// From a grid point, shifts just inside and just past half a texel on every light axis at once: the snapped box
			// moves by up to R/N per axis and must still contain every slice corner.
			for (float fraction : { -0.50001f, -0.49999f, 0.49999f, 0.50001f })
			{
				CAPTURE(fraction);
				camera.Position = glm::vec3(offset + (basis.Right + basis.Up + basis.Back) * (fraction * texel));
				camera.View = Utils::ShadowTestView(rotation, camera.Position);
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
