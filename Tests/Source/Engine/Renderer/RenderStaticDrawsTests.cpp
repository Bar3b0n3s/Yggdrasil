#include "TestsPCH.h"
#include "Engine/Renderer/RenderStaticDraws.h"

#include "Engine/Core/Random.h"
#include "Engine/Renderer/PlanarReflections.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/ShadowPass.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdint>
#include <vector>

// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): the static path's cell tests (RenderStaticDraws.h). A cell the
// tests call Outside holds no draw the per-draw test keeps, and one they call Inside no draw it culls, which the property
// cases check against the per-draw tests themselves (IsOutsideView, IsBehindPlanarReflectionClipPlane, ShadowBoxesOverlap).

namespace Engine {

	namespace {

		// A camera at `position` looking along -Z, 60 degrees of vertical field of view at 3:2, culling beyond `farClip`.
		CameraData MakeCamera(const glm::vec3& position, float farClip = 50.0f)
		{
			CameraData camera;
			camera.Position = position;
			camera.View = glm::translate(glm::mat4(1.0f), -position);
			camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 2.0f, 0.1f, farClip, 96, 64);
			camera.FarClip = farClip;
			return camera;
		}

		// The world box of `bounds` at each of `worlds` (ShadowPass.h's caster boxes, as a prepared cell makes its box).
		Aabb MakeCellBox(const Aabb& bounds, const std::vector<glm::mat4>& worlds)
		{
			Aabb box;
			for (const glm::mat4& world : worlds)
			{
				const ShadowBox moved = ComputeShadowCasterBox(bounds, world);
				box.Extend(Aabb{ .Min = moved.Centre - moved.HalfExtent, .Max = moved.Centre + moved.HalfExtent });
			}
			return box;
		}

		float Uniform(Random& random, float low, float high)
		{
			return low + (high - low) * random.NextFloat();
		}

		// Seeded cells of eight items near a point that may lie anywhere around the camera, each item turned, scaled and
		// sheared at random.
		std::vector<glm::mat4> MakeCellItems(Random& random, const glm::vec3& centre)
		{
			std::vector<glm::mat4> worlds;
			for (uint32_t item = 0; item < 8; ++item)
			{
				const glm::vec3 offset(Uniform(random, -6.0f, 6.0f), Uniform(random, -3.0f, 3.0f), Uniform(random, -6.0f, 6.0f));
				glm::mat4 world = glm::translate(glm::mat4(1.0f), centre + offset);
				world = glm::rotate(world, Uniform(random, 0.0f, 6.28f), glm::normalize(glm::vec3(Uniform(random, -1.0f, 1.0f), 1.0f, Uniform(random, -1.0f, 1.0f))));
				world = glm::scale(world, glm::vec3(Uniform(random, 0.2f, 2.0f), Uniform(random, 0.2f, 2.0f), Uniform(random, 0.2f, 2.0f)));
				worlds.push_back(world);
			}
			return worlds;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderStaticDraws: a cell is outside a view past one plane or FarClip, inside within all, else partial")
		{
			const CameraData camera = MakeCamera(glm::vec3(0.0f), 20.0f);
			const Aabb unit{ .Min = glm::vec3(-0.5f), .Max = glm::vec3(0.5f) };
			const auto at = [&unit](const glm::vec3& position)
			{
				return InflateStaticCellBox(Aabb{ .Min = unit.Min + position, .Max = unit.Max + position }, glm::vec3(0.0f));
			};
			CHECK(ClassifyStaticCell(at(glm::vec3(0.0f, 0.0f, -5.0f)), camera) == StaticCellVisibility::Inside);
			CHECK(ClassifyStaticCell(at(glm::vec3(0.0f, 0.0f, 5.0f)), camera) == StaticCellVisibility::Outside);   // behind
			CHECK(ClassifyStaticCell(at(glm::vec3(40.0f, 0.0f, -5.0f)), camera) == StaticCellVisibility::Outside); // beside
			CHECK(ClassifyStaticCell(at(glm::vec3(0.0f, 0.0f, -25.0f)), camera) == StaticCellVisibility::Outside); // past FarClip
			CHECK(ClassifyStaticCell(at(glm::vec3(0.0f, 0.0f, -20.0f)), camera) == StaticCellVisibility::Partial); // across FarClip
			CHECK(ClassifyStaticCell(at(glm::vec3(0.0f, 0.0f, 0.0f)), camera) == StaticCellVisibility::Partial);   // across the near plane
			CHECK(ClassifyStaticCell(Aabb{}, camera) == StaticCellVisibility::Partial);                            // empty: never culled
		}

		TEST_CASE("RenderStaticDraws: a mirror's clip plane, a shadow face and a light's box classify a cell as their per-draw tests would")
		{
			const Aabb box{ .Min = glm::vec3(-1.0f), .Max = glm::vec3(1.0f) };
			// The plane z = 2 facing +Z keeps z >= 2.
			const glm::vec4 plane(0.0f, 0.0f, 1.0f, -2.0f);
			CHECK(ClassifyStaticCellClipPlane(box, plane) == StaticCellVisibility::Outside);
			CHECK(ClassifyStaticCellClipPlane(Aabb{ .Min = glm::vec3(-1.0f, -1.0f, 1.0f), .Max = glm::vec3(1.0f, 1.0f, 3.0f) }, plane) == StaticCellVisibility::Partial);
			CHECK(ClassifyStaticCellClipPlane(Aabb{ .Min = glm::vec3(-1.0f, -1.0f, 3.0f), .Max = glm::vec3(1.0f, 1.0f, 5.0f) }, plane) == StaticCellVisibility::Inside);
			// An orthographic face over [-10, 10]^2 with depth along -Z from z = 10: the pancake keeps what lies nearer the light.
			const glm::mat4 face = glm::orthoRH_ZO(-10.0f, 10.0f, -10.0f, 10.0f, 20.0f, 0.0f) * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f));
			CHECK(ClassifyStaticCellShadow(box, face, false) == StaticCellVisibility::Inside);
			const Aabb nearLight{ .Min = glm::vec3(-1.0f, -1.0f, 11.0f), .Max = glm::vec3(1.0f, 1.0f, 12.0f) };
			CHECK(ClassifyStaticCellShadow(nearLight, face, false) == StaticCellVisibility::Outside);
			CHECK(ClassifyStaticCellShadow(nearLight, face, true) == StaticCellVisibility::Inside);
			CHECK(ClassifyStaticCellShadow(Aabb{ .Min = glm::vec3(30.0f), .Max = glm::vec3(31.0f) }, face, true) == StaticCellVisibility::Outside);
			CHECK(ClassifyStaticCellShadow(Aabb{ .Min = glm::vec3(9.0f, -1.0f, -1.0f), .Max = glm::vec3(11.0f, 1.0f, 1.0f) }, face, true)
				== StaticCellVisibility::Partial);
			// A light's box of half extent 4 around (0, 0, 0).
			CHECK(ClassifyStaticCellLight(box, glm::vec3(0.0f), glm::vec3(4.0f)) == StaticCellVisibility::Inside);
			CHECK(ClassifyStaticCellLight(Aabb{ .Min = glm::vec3(3.0f), .Max = glm::vec3(5.0f) }, glm::vec3(0.0f), glm::vec3(4.0f)) == StaticCellVisibility::Partial);
			CHECK(ClassifyStaticCellLight(Aabb{ .Min = glm::vec3(9.0f), .Max = glm::vec3(10.0f) }, glm::vec3(0.0f), glm::vec3(4.0f)) == StaticCellVisibility::Outside);
		}

		TEST_CASE("RenderStaticDraws: an inflated box grows by its rounding margin, and an empty one stays empty")
		{
			const Aabb box{ .Min = glm::vec3(1000.0f), .Max = glm::vec3(1001.0f) };
			const Aabb inflated = InflateStaticCellBox(box, glm::vec3(0.0f));
			CHECK(inflated.Min.x == doctest::Approx(1000.0f - (1001.0f * 1e-4f + 1e-3f)));
			CHECK(inflated.Max.z == doctest::Approx(1001.0f + (1001.0f * 1e-4f + 1e-3f)));
			// A far reference widens it too.
			CHECK(InflateStaticCellBox(box, glm::vec3(0.0f, -5000.0f, 0.0f)).Max.x > inflated.Max.x);
			CHECK(InflateStaticCellBox(Aabb{}, glm::vec3(1.0f)).IsEmpty());
		}

		// The property the static path relies on: over seeded cells around seeded cameras (near the origin and 10 km out), every
		// draw of a cell its inflated box puts Outside is culled by IsOutsideView (and the mirror's plane, and a light's box), and
		// every draw of an Inside cell is kept. (The shadow faces' per-draw test is ShadowPass.cpp's own; the GPU cases compare
		// the static casters' shadow maps with the dynamic ones'.)
		TEST_CASE("RenderStaticDraws: a cell's Outside and Inside agree with every one of its draws' own tests")
		{
			Random random(4301);
			const Aabb unit{ .Min = glm::vec3(-0.5f), .Max = glm::vec3(0.5f) };
			uint32_t outside = 0;
			uint32_t inside = 0;
			uint32_t lightOutside = 0;
			for (uint32_t trial = 0; trial < 400; ++trial)
			{
				CAPTURE(trial);
				const glm::vec3 origin = trial % 2 == 0 ? glm::vec3(0.0f) : glm::vec3(10000.0f, 0.0f, -10000.0f);
				// A quarter of the cells straight ahead, well within FarClip, so some lie wholly in view.
				const bool ahead = trial % 4 == 1;
				const CameraData camera = MakeCamera(origin, ahead ? 80.0f : Uniform(random, 10.0f, 60.0f));
				const glm::vec3 centre = origin
					+ (ahead ? glm::vec3(Uniform(random, -3.0f, 3.0f), Uniform(random, -2.0f, 2.0f), Uniform(random, -45.0f, -30.0f))
							 : glm::vec3(Uniform(random, -40.0f, 40.0f), Uniform(random, -20.0f, 20.0f), Uniform(random, -70.0f, 20.0f)));
				const std::vector<glm::mat4> worlds = MakeCellItems(random, centre);
				const Aabb box = MakeCellBox(unit, worlds);
				const StaticCellVisibility view = ClassifyStaticCell(InflateStaticCellBox(box, camera.Position), camera);
				const glm::vec4 plane = glm::vec4(glm::normalize(glm::vec3(Uniform(random, -1.0f, 1.0f), 1.0f, 0.0f)), -centre.y - Uniform(random, -4.0f, 4.0f));
				const StaticCellVisibility clip = ClassifyStaticCellClipPlane(InflateStaticCellBox(box, camera.Position), plane);
				const ShadowBox light{ .Centre = origin + glm::vec3(0.0f, 0.0f, -20.0f), .HalfExtent = glm::vec3(12.0f) };
				const StaticCellVisibility lightBox = ClassifyStaticCellLight(InflateStaticCellBox(box, light.Centre), light.Centre, light.HalfExtent);
				for (const glm::mat4& world : worlds)
				{
					const bool culled = IsOutsideView(unit, world, camera);
					if (view == StaticCellVisibility::Outside)
						CHECK(culled);
					if (view == StaticCellVisibility::Inside)
						CHECK_FALSE(culled);
					const bool behind = IsBehindPlanarReflectionClipPlane(unit, world, plane);
					if (clip == StaticCellVisibility::Outside)
						CHECK(behind);
					if (clip == StaticCellVisibility::Inside)
						CHECK_FALSE(behind);
					const bool overlaps = ShadowBoxesOverlap(ComputeShadowCasterBox(unit, world), light);
					if (lightBox == StaticCellVisibility::Outside)
						CHECK_FALSE(overlaps);
					if (lightBox == StaticCellVisibility::Inside)
						CHECK(overlaps);
				}
				outside += view == StaticCellVisibility::Outside ? 1U : 0U;
				inside += view == StaticCellVisibility::Inside ? 1U : 0U;
				lightOutside += lightBox == StaticCellVisibility::Outside ? 1U : 0U;
			}
			// The trials reach every answer.
			CHECK(outside > 20);
			CHECK(inside > 10);
			CHECK(lightOutside > 10);
		}
	}

}
