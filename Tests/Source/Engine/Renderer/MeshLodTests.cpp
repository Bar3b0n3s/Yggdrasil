#include "TestsPCH.h"

#include "Engine/Renderer/MeshLod.h"

#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <utility>

// The per-view level of detail (M46; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3D): the screen
// size, the level a size picks, the cross-fade bands, the fade-out before the cull distance, and the casters' level.

namespace Engine {

	namespace {

		// The fixture mesh's switch sizes: level 1 below 0.5, level 2 below 0.25, level 3 below 0.125.
		constexpr std::array<float, 3> SwitchSizes = { 0.5f, 0.25f, 0.125f };

		MeshLodParameters Sharp()
		{
			return MeshLodParameters{ .CullDistance = 0.0f, .LodBias = 1.0f, .ShadowLodOffset = 1, .FadeWidth = 0.0f };
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("MeshLod: a mesh's level follows its projected screen size times LodBias")
		{
			// The bounding sphere: the box's centre placed by the world matrix, its half diagonal times the largest axis scale.
			const Aabb box{ .Min = glm::vec3(-1.0f, 0.0f, -1.0f), .Max = glm::vec3(1.0f, 4.0f, 1.0f) };
			const glm::mat4 world = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)), glm::vec3(1.0f, 2.0f, 0.5f));
			const MeshLodSphere sphere = ComputeMeshLodSphere(box, world);
			CHECK(sphere.Center == glm::vec3(10.0f, 4.0f, 0.0f));
			CHECK(sphere.Radius == doctest::Approx(std::sqrt(24.0f) * 0.5f * 2.0f));

			// Perspective: r x P[1][1] / d; orthographic: r x P[1][1]; inside the sphere: infinite (level 0).
			CHECK(ComputeMeshScreenSize(1.0f, 10.0f, 2.0f, false) == 0.2f);
			CHECK(ComputeMeshScreenSize(1.0f, 10.0f, 0.25f, true) == 0.25f);
			CHECK(std::isinf(ComputeMeshScreenSize(2.0f, 1.5f, 2.0f, false)));

			// The finest level whose switch size the biased size still reaches.
			const std::array<std::pair<float, uint32_t>, 7> cases = { {
				{ std::numeric_limits<float>::infinity(), 0 },
				{ 0.9f, 0 },
				{ 0.5f, 0 },
				{ 0.49f, 1 },
				{ 0.2f, 2 },
				{ 0.125f, 2 },
				{ 0.01f, 3 },
			} };
			for (const auto& [size, level] : cases)
			{
				CAPTURE(size);
				const MeshLodSelection selection = SelectMeshLod(SwitchSizes, size, 10.0f, Sharp());
				CHECK(selection.Level == level);
				REQUIRE(selection.DrawCount == 1);
				CHECK(selection.Draws[0] == MeshLodDraw{ .Level = level, .Fade = 0.0f });
				CHECK_FALSE(selection.CrossFade);
			}

			// LodBias 2 keeps each level until half its switch size; 0.5 switches twice as early.
			MeshLodParameters biased = Sharp();
			biased.LodBias = 2.0f;
			CHECK(SelectMeshLod(SwitchSizes, 0.3f, 10.0f, biased).Level == 0);
			CHECK(SelectMeshLod(SwitchSizes, 0.2f, 10.0f, biased).Level == 1);
			biased.LodBias = 0.5f;
			CHECK(SelectMeshLod(SwitchSizes, 0.9f, 10.0f, biased).Level == 1);
			// A mesh without levels always draws level 0.
			CHECK(SelectMeshLod({}, 0.001f, 10.0f, Sharp()).Draws[0] == MeshLodDraw{ .Level = 0, .Fade = 0.0f });
		}

		TEST_CASE("MeshLod: inside a fade band the two levels take complementary fades")
		{
			MeshLodParameters parameters = Sharp();
			parameters.FadeWidth = 0.2f;
			// The band around 0.25 runs from 0.225 to 0.275: at its middle both levels draw with fades -1/2 and 1/2.
			const MeshLodSelection middle = SelectMeshLod(SwitchSizes, 0.25f, 10.0f, parameters);
			REQUIRE(middle.DrawCount == 2);
			CHECK(middle.CrossFade);
			CHECK(middle.Draws[0].Level == 1);
			CHECK(middle.Draws[1].Level == 2);
			CHECK(middle.Draws[0].Fade == doctest::Approx(-0.5f));
			CHECK(middle.Draws[1].Fade == -middle.Draws[0].Fade);

			// The coarser level's fade rises as the item shrinks, through the whole band, and the finer one's is its negative.
			float previous = 0.0f;
			for (const float size : { 0.274f, 0.26f, 0.251f, 0.249f, 0.24f, 0.226f })
			{
				CAPTURE(size);
				const MeshLodSelection selection = SelectMeshLod(SwitchSizes, size, 10.0f, parameters);
				REQUIRE(selection.DrawCount == 2);
				CHECK(selection.Draws[0].Level == 1);
				CHECK(selection.Draws[1].Level == 2);
				const float t = selection.Draws[1].Fade;
				CHECK(t > previous);
				CHECK(t < 1.0f);
				CHECK(selection.Draws[0].Fade == -t);
				previous = t;
			}
			// Outside the band one level draws whole, on either side.
			CHECK(SelectMeshLod(SwitchSizes, 0.28f, 10.0f, parameters).DrawCount == 1);
			CHECK(SelectMeshLod(SwitchSizes, 0.22f, 10.0f, parameters).DrawCount == 1);
			// LodBias moves the bands with the switches: biased by 2, 0.125 is the middle of the band around 0.25.
			parameters.LodBias = 2.0f;
			CHECK(SelectMeshLod(SwitchSizes, 0.125f, 10.0f, parameters).CrossFade);
			// FadeWidth 0 switches sharply.
			parameters.FadeWidth = 0.0f;
			CHECK(SelectMeshLod(SwitchSizes, 0.125f, 10.0f, parameters).DrawCount == 1);
		}

		TEST_CASE("MeshLod: an item fades out across FadeWidth before its CullDistance and is culled beyond it")
		{
			MeshLodParameters parameters = Sharp();
			parameters.CullDistance = 100.0f;
			parameters.FadeWidth = 0.1f;
			// Before 90 m the item draws whole; from 90 m to 100 m its level fades out; from 100 m on it is culled.
			const MeshLodSelection near = SelectMeshLod(SwitchSizes, 0.3f, 80.0f, parameters);
			REQUIRE(near.DrawCount == 1);
			CHECK(near.Draws[0].Fade == 0.0f);
			CHECK_FALSE(near.DistanceCulled);

			float previous = 0.0f;
			for (const float distance : { 90.5f, 95.0f, 99.5f })
			{
				CAPTURE(distance);
				const MeshLodSelection fading = SelectMeshLod(SwitchSizes, 0.3f, distance, parameters);
				REQUIRE(fading.DrawCount == 1);
				CHECK(fading.Draws[0].Level == 1);
				CHECK(fading.Draws[0].Fade < previous);
				CHECK(fading.Draws[0].Fade > -1.0f);
				CHECK_FALSE(fading.DistanceCulled);
				previous = fading.Draws[0].Fade;
			}
			CHECK(SelectMeshLod(SwitchSizes, 0.3f, 95.0f, parameters).Draws[0].Fade == doctest::Approx(-0.5f));
			// Inside the fade-out a level's cross-fade band does not apply: the level alone fades.
			CHECK(SelectMeshLod(SwitchSizes, 0.25f, 95.0f, parameters).DrawCount == 1);

			for (const float distance : { 100.0f, 250.0f })
			{
				const MeshLodSelection culled = SelectMeshLod(SwitchSizes, 0.3f, distance, parameters);
				CHECK(culled.DistanceCulled);
				CHECK(culled.DrawCount == 0);
			}
			// FadeWidth 0 culls sharply; CullDistance 0 never culls.
			parameters.FadeWidth = 0.0f;
			CHECK(SelectMeshLod(SwitchSizes, 0.3f, 99.9f, parameters).Draws[0].Fade == 0.0f);
			parameters.CullDistance = 0.0f;
			CHECK(SelectMeshLod(SwitchSizes, 0.3f, 1.0e6f, parameters).DrawCount == 1);
		}

		TEST_CASE("MeshLod: a caster draws its view's level plus ShadowLodOffset")
		{
			MeshLodParameters parameters = Sharp();
			CHECK(SelectShadowLod(SwitchSizes, 0, 10.0f, parameters) == 1u);
			CHECK(SelectShadowLod(SwitchSizes, 2, 10.0f, parameters) == 3u);
			// Clamped to the last level, and the mesh's own level for a mesh without levels.
			CHECK(SelectShadowLod(SwitchSizes, 3, 10.0f, parameters) == 3u);
			CHECK(SelectShadowLod({}, 0, 10.0f, parameters) == 0u);
			parameters.ShadowLodOffset = 0;
			CHECK(SelectShadowLod(SwitchSizes, 2, 10.0f, parameters) == 2u);
			parameters.ShadowLodOffset = 4;
			CHECK(SelectShadowLod(SwitchSizes, 1, 10.0f, parameters) == 3u);
			// Beyond the cull distance the caster casts nothing.
			parameters.CullDistance = 50.0f;
			CHECK(SelectShadowLod(SwitchSizes, 0, 49.0f, parameters).has_value());
			CHECK_FALSE(SelectShadowLod(SwitchSizes, 0, 50.0f, parameters).has_value());
			// The view's level is the one the selection reports, inside a band too.
			parameters.CullDistance = 0.0f;
			parameters.ShadowLodOffset = 1;
			parameters.FadeWidth = 0.2f;
			const MeshLodSelection band = SelectMeshLod(SwitchSizes, 0.26f, 10.0f, parameters);
			REQUIRE(band.CrossFade);
			CHECK(SelectShadowLod(SwitchSizes, band.Level, 10.0f, parameters) == 2u);
		}

		// Decision 3D's per-view measure (the W46.2 review, finding 8): SelectViewMeshLod and SelectViewShadowLod measure the
		// item's bounding sphere from the camera's Position, by |Projection[1][1]| (never [0][0], the horizontal scale), as r
		// |P11| / d in perspective and r |P11| whatever the distance in an orthographic view (ProjectionKind), and select with
		// the item's RenderDistance as ToMeshLodParameters maps it. Every size below lies outside the bands of FadeWidth 0.1.
		TEST_CASE("MeshLod: a view measures an item by its camera's distance and projection, perspective and orthographic")
		{
			// A 2 x 2 m quad (radius sqrt(2)) and a camera away from the origin whose viewport is twice as wide as it is tall, so
			// P[0][0] is half P[1][1]; the item lies `distance` metres from the camera along -Z, its centre exactly that far.
			const Aabb quad{ .Min = glm::vec3(-1.0f, -1.0f, 0.0f), .Max = glm::vec3(1.0f, 1.0f, 0.0f) };
			const glm::vec3 eye(5.0f, 1.0f, -2.0f);
			const auto makeCamera = [&eye](RenderProjection kind, float orthographicSize, float sign)
			{
				CameraData camera;
				camera.Position = eye;
				camera.ProjectionKind = kind;
				camera.VerticalFov = 60.0f;
				camera.OrthographicSize = orthographicSize;
				camera.ViewportWidth = 1280;
				camera.ViewportHeight = 640;
				camera.Projection = ComputeReverseZProjection(kind, camera.VerticalFov, orthographicSize, camera.NearClip, camera.FarClip, camera.ViewportWidth,
					camera.ViewportHeight);
				camera.Projection[1][1] *= sign;
				return camera;
			};
			const auto at = [&eye](float distance)
			{
				return glm::translate(glm::mat4(1.0f), eye - glm::vec3(0.0f, 0.0f, distance));
			};
			const MeshDrawDistance defaults{};

			// Perspective, P11 = 1 / tan(30 degrees) = sqrt(3): r P11 = sqrt(6), so 4 m gives 0.61 (level 0), 7 m 0.35 (level
			// 1), 14 m 0.17 (level 2) and 30 m 0.08 (level 3). P[0][0] would give 0.31, 0.17 and 0.09 at the first three (levels
			// 1, 2 and 3), and the distance from the world origin 0.31 at 4 m (level 1). A negative P11 (a projection that flips
			// Y) gives the same levels.
			for (const float sign : { 1.0f, -1.0f })
			{
				CAPTURE(sign);
				const CameraData camera = makeCamera(RenderProjection::Perspective, 10.0f, sign);
				for (const auto& [distance, level] : std::array<std::pair<float, uint32_t>, 4>{ { { 4.0f, 0 }, { 7.0f, 1 }, { 14.0f, 2 }, { 30.0f, 3 } } })
				{
					CAPTURE(distance);
					const MeshLodSelection selection = SelectViewMeshLod(SwitchSizes, quad, at(distance), defaults, camera);
					CHECK(selection.Level == level);
					REQUIRE(selection.DrawCount == 1);
					CHECK(selection.Draws[0] == MeshLodDraw{ .Level = level, .Fade = 0.0f });
				}
			}

			// Orthographic, P11 = 1 / OrthographicSize: r P11 is 0.94 at a size of 1.5 m (level 0), 0.35 at 4 m (level 1) and
			// 0.07 at 20 m (level 3), at any distance, the camera at the sphere's centre included (where a perspective view
			// would draw level 0), and with a negative P11 alike.
			for (const auto& [size, level] : std::array<std::pair<float, uint32_t>, 3>{ { { 1.5f, 0 }, { 4.0f, 1 }, { 20.0f, 3 } } })
			{
				for (const float sign : { 1.0f, -1.0f })
				{
					const CameraData camera = makeCamera(RenderProjection::Orthographic, size, sign);
					for (const float distance : { 0.0f, 4.0f, 14.0f, 30.0f })
					{
						CAPTURE(size);
						CAPTURE(sign);
						CAPTURE(distance);
						const MeshLodSelection selection = SelectViewMeshLod(SwitchSizes, quad, at(distance), defaults, camera);
						CHECK(selection.Level == level);
						CHECK(selection.DrawCount == 1);
					}
				}
			}

			// The item's RenderDistance: LodBias 2 doubles 14 m's size (0.35, level 1); at or beyond CullDistance it is culled.
			const CameraData camera = makeCamera(RenderProjection::Perspective, 10.0f, 1.0f);
			CHECK(SelectViewMeshLod(SwitchSizes, quad, at(14.0f), { .LodBias = 2.0f }, camera).Level == 1);
			for (const float cullDistance : { 14.0f, 10.0f })
			{
				CAPTURE(cullDistance);
				const MeshLodSelection culled = SelectViewMeshLod(SwitchSizes, quad, at(14.0f), { .CullDistance = cullDistance }, camera);
				CHECK(culled.DistanceCulled);
				CHECK(culled.DrawCount == 0);
			}

			// A caster draws the view's level plus ShadowLodOffset, clamped to the last level (3), and nothing at or beyond its
			// CullDistance from the camera, inside the fade-out before it included.
			CHECK(SelectViewShadowLod(SwitchSizes, quad, at(4.0f), defaults, camera) == 1u);
			CHECK(SelectViewShadowLod(SwitchSizes, quad, at(7.0f), defaults, camera) == 2u);
			CHECK(SelectViewShadowLod(SwitchSizes, quad, at(14.0f), defaults, camera) == 3u);
			CHECK(SelectViewShadowLod(SwitchSizes, quad, at(30.0f), defaults, camera) == 3u);
			CHECK(SelectViewShadowLod(SwitchSizes, quad, at(14.0f), { .ShadowLodOffset = 0 }, camera) == 2u);
			CHECK(SelectViewShadowLod(SwitchSizes, quad, at(7.0f), { .ShadowLodOffset = 3 }, camera) == 3u);
			CHECK(SelectViewShadowLod(SwitchSizes, quad, at(14.0f), { .LodBias = 2.0f }, camera) == 2u);
			CHECK(SelectViewShadowLod(SwitchSizes, quad, at(14.0f), { .CullDistance = 14.5f }, camera) == 3u);
			CHECK_FALSE(SelectViewShadowLod(SwitchSizes, quad, at(14.0f), { .CullDistance = 14.0f }, camera).has_value());
			CHECK_FALSE(SelectViewShadowLod(SwitchSizes, quad, at(14.0f), { .CullDistance = 10.0f }, camera).has_value());

			// ToMeshLodParameters maps every field, the defaults (a MeshRenderer without RenderDistance) to the selection's.
			const MeshLodParameters mapped = ToMeshLodParameters({ .CullDistance = 25.0f, .LodBias = 1.5f, .ShadowLodOffset = 3, .FadeWidth = 0.2f });
			CHECK(mapped.CullDistance == 25.0f);
			CHECK(mapped.LodBias == 1.5f);
			CHECK(mapped.ShadowLodOffset == 3);
			CHECK(mapped.FadeWidth == 0.2f);
			const MeshLodParameters standard = ToMeshLodParameters(defaults);
			const MeshLodParameters expected{};
			CHECK(standard.CullDistance == expected.CullDistance);
			CHECK(standard.LodBias == expected.LodBias);
			CHECK(standard.ShadowLodOffset == expected.ShadowLodOffset);
			CHECK(standard.FadeWidth == expected.FadeWidth);
		}
	}

}
