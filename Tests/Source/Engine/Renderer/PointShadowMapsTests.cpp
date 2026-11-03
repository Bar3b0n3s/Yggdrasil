#include "TestsPCH.h"

#include "Engine/Renderer/PointShadowMaps.h"

#include "Engine/Core/Log.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// Point-light shadows (M23, C23.1, R-PLS; Docs/Decisions/0034-m23-contract.md decision 3, stream B).

namespace Engine {

	namespace {

		LightData MakeShadowedPoint(const glm::vec3& position, float intensity, uint64_t entity)
		{
			return LightData{ .Type = RenderLightType::Point, .Intensity = intensity, .Position = position, .Range = 5.0f, .CastShadows = true, .Entity = UUID(entity) };
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("PointShadowMaps: the face size is a quarter of the shadow map size, at least 128")
		{
			CHECK(GetPointShadowFaceSize(2048) == 512);
			CHECK(GetPointShadowFaceSize(8192) == 2048);
			CHECK(GetPointShadowFaceSize(256) == 128);
			CHECK(MaxPointShadowLights == 4);
		}

		// The four most important visible shadow-casting point lights are planned, in CullLights' order, each with its six
		// faces and its layer; the fifth is dropped, a point light without CastShadows is not a candidate, and one whose range
		// is not above twice its near plane is rejected without costing another its shadow.
		TEST_CASE("PointShadowMaps: the four most important shadow-casting point lights are planned and the rest dropped")
		{
			std::vector<LightData> lights;
			for (uint64_t index = 0; index < 5; ++index)
				lights.push_back(MakeShadowedPoint(glm::vec3(static_cast<float>(index), 0.0f, -5.0f), 10.0f - static_cast<float>(index), 100 + index));
			lights.push_back(LightData{ .Type = RenderLightType::Point, .Intensity = 50.0f, .Position = glm::vec3(0.0f, 0.0f, -4.0f), .Range = 5.0f, .Entity = UUID(200) });
			LightData tiny = MakeShadowedPoint(glm::vec3(0.0f, 1.0f, -5.0f), 20.0f, 300);
			tiny.Range = 0.05f;
			lights.push_back(tiny);
			const std::vector<uint32_t> visible = { 6, 5, 0, 1, 2, 3, 4 };

			const Result<PointShadowSet> set = AllocatePointShadows(lights, visible);
			REQUIRE_MESSAGE(set.has_value(), set.error().ToString());
			REQUIRE(set->Lights.size() == MaxPointShadowLights);
			for (uint32_t layer = 0; layer < MaxPointShadowLights; ++layer)
			{
				CHECK(set->Lights[layer].LightIndex == layer);
				CHECK(set->Lights[layer].Layer == layer);
				CHECK(set->Lights[layer].FarClip == 5.0f);
			}
			CHECK(set->Dropped == 1);
			CHECK(set->Rejected == 1);
		}

		// The plan's other rules: an index out of range or given twice is InvalidArgument; a spot or directional light is never
		// a candidate; a non-finite position is rejected; the near plane is max(SourceRadius, 0.05) and the far plane the Range,
		// which each face's projection takes; a source radius at least half the range is rejected.
		TEST_CASE("PointShadowMaps: the plan refuses bad indices, skips other light types and rejects lights it cannot project")
		{
			std::vector<LightData> lights = { MakeShadowedPoint(glm::vec3(0.0f, 0.0f, -5.0f), 10.0f, 1) };
			for (const std::vector<uint32_t>& indices : { std::vector<uint32_t>{ 1 }, std::vector<uint32_t>{ 0, 0 } })
			{
				const Result<PointShadowSet> refused = AllocatePointShadows(lights, indices);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}

			LightData spot = MakeShadowedPoint(glm::vec3(1.0f, 0.0f, -5.0f), 10.0f, 2);
			spot.Type = RenderLightType::Spot;
			LightData sun = MakeShadowedPoint(glm::vec3(0.0f), 10.0f, 3);
			sun.Type = RenderLightType::Directional;
			LightData lost = MakeShadowedPoint(glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, -5.0f), 10.0f, 4);
			LightData wide = MakeShadowedPoint(glm::vec3(0.0f, 2.0f, -5.0f), 10.0f, 5);
			wide.SourceRadius = 2.5f; // a near plane of Range / 2
			LightData soft = MakeShadowedPoint(glm::vec3(0.0f, -2.0f, -5.0f), 10.0f, 6);
			soft.SourceRadius = 0.5f;
			lights.insert(lights.end(), { spot, sun, lost, wide, soft });
			const Result<PointShadowSet> set = AllocatePointShadows(lights, std::vector<uint32_t>{ 2, 1, 3, 4, 5, 0 });
			REQUIRE_MESSAGE(set.has_value(), set.error().ToString());
			REQUIRE(set->Lights.size() == 2);
			CHECK(set->Lights[0].LightIndex == 5);
			CHECK(set->Lights[0].NearClip == 0.5f);
			CHECK(set->Lights[1].LightIndex == 0);
			CHECK(set->Lights[1].NearClip == 0.05f);
			CHECK(set->Lights[1].FarClip == 5.0f);
			CHECK(set->Rejected == 2);
			CHECK(set->Dropped == 0);
			for (uint32_t face = 0; face < PointShadowFaceCount; ++face)
			{
				CAPTURE(face);
				const glm::mat4 expected = ComputePointShadowFaceViewProjection(lights[5].Position, face, 0.5f, 5.0f);
				for (glm::length_t column = 0; column < 4; ++column)
					CHECK(set->Lights[0].FaceViewProjections[face][column] == expected[column]);
			}
		}

		// Each face looks down its major axis in Vulkan's cube face order: a point on the face's axis maps to the face's
		// centre, at depth 1 on the near plane and 0 on the far plane (reverse-Z). Off the axis, a direction from the light
		// lands on the texel a cube sampler reads for it (Vulkan's face selection table, RenderReference's CubeTexelDirection):
		// texel (column, row) of a face of N texels at NDC x = 2 (column + 0.5) / N - 1 and y = 1 - 2 (row + 0.5) / N, since
		// NVRHI's flipped viewport puts NDC +Y on row 0. A face that is mirrored, rotated or swapped with another fails.
		TEST_CASE("PointShadowMaps: each face's projection centres its axis, near at depth 1 and far at depth 0")
		{
			const glm::vec3 position(1.0f, 2.0f, 3.0f);
			const std::array<glm::vec3, PointShadowFaceCount> axes = { glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
				glm::vec3(0, 0, 1), glm::vec3(0, 0, -1) };
			constexpr uint32_t Size = 8;
			// Two off-axis texels in different quadrants, neither on a diagonal, so a mirror, a rotation or a transpose moves them.
			constexpr std::array<std::array<uint32_t, 2>, 2> Texels = { { { 1, 2 }, { 6, 5 } } };
			for (uint32_t face = 0; face < PointShadowFaceCount; ++face)
			{
				CAPTURE(face);
				const glm::mat4 viewProjection = ComputePointShadowFaceViewProjection(position, face, 0.1f, 10.0f);
				const glm::vec4 nearPoint = viewProjection * glm::vec4(position + axes[face] * 0.1f, 1.0f);
				const glm::vec4 farPoint = viewProjection * glm::vec4(position + axes[face] * 10.0f, 1.0f);
				CHECK(nearPoint.x / nearPoint.w == doctest::Approx(0.0f));
				CHECK(nearPoint.y / nearPoint.w == doctest::Approx(0.0f));
				CHECK(nearPoint.z / nearPoint.w == doctest::Approx(1.0f));
				CHECK(farPoint.z / farPoint.w == doctest::Approx(0.0f).epsilon(1e-5));
				for (const auto& [column, row] : Texels)
				{
					CAPTURE(column);
					CAPTURE(row);
					const glm::vec3 direction(Test::CubeTexelDirection(face, column, row, Size));
					const glm::vec4 clip = viewProjection * glm::vec4(position + direction * 2.0f, 1.0f);
					CHECK(clip.x / clip.w == doctest::Approx(2.0f * (static_cast<float>(column) + 0.5f) / Size - 1.0f).epsilon(1e-4));
					CHECK(clip.y / clip.w == doctest::Approx(1.0f - 2.0f * (static_cast<float>(row) + 0.5f) / Size).epsilon(1e-4));
				}
			}
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		// M23 acceptance (R-PLS, stream B): a shadow-casting point light in front of a floor quad, with a smaller quad between
		// them, leaves the floor black (no direct light, no ambient) exactly where the CPU oracle (RenderReference's
		// IsPointLightVisible) says the occluder hides it from the light, and every other floor pixel as it renders without
		// CastShadows. The scene is asymmetric: the light is off the camera's axis and the occluder off the light's, so the
		// shadow falls to one side of each face it crosses (the -Z face and the +X face), and a face projection that mirrors,
		// rotates or swaps a face moves it, which the oracle's pixels catch. Only pixels whose 5 x 5 neighbourhood the oracle
		// gives one answer are compared, so the shadow map's resolution, bias and filtering at the edge never matter.
		TEST_CASE("SceneRenderer: a shadow-casting point light leaves the occluded floor unlit, as the CPU oracle predicts")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 64, 64);
			RenderSnapshot snapshot = setup.Snapshot();
			// The camera at the origin looks down -Z. The floor (6 m wide) faces it at z = -6; the occluder (0.6 m wide, centred
			// at (0, 0.2)) faces it at z = -5; the light is at (-0.5, 0, -4.5), 1.5 m in front of the floor. The shadow on the
			// floor is the occluder scaled by 3 about the light: x in [0.1, 1.9], y in [-0.3, 1.5].
			const glm::mat4 floorWorld = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -6.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(3.0f));
			const glm::mat4 occluderWorld = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.2f, -5.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.3f));
			snapshot.Meshes = {
				{ .World = floorWorld, .Mesh = UUID(8910), .Materials = { UUID(8911) } },
				{ .World = occluderWorld, .Mesh = UUID(8910), .Materials = { UUID(8911) } },
			};
			const glm::vec3 lightPosition(-0.5f, 0.0f, -4.5f);
			snapshot.Lights = { MakeShadowedPoint(lightPosition, 40.0f, 500) };
			snapshot.Environment.FallbackColor = glm::vec3(0.0f);

			const Image shadowed = setup.Render(snapshot);
			snapshot.Lights.front().CastShadows = false;
			++snapshot.FrameIndex;
			const Image unshadowed = setup.Render(snapshot);

			// The fixture's quad (corners (+-1, +-1, 0), triangles 0 1 2 and 0 2 3) in world space.
			const auto quadTriangles = [](const glm::mat4& world)
			{
				const auto corner = [&world](float x, float y)
				{
					return glm::dvec3(world * glm::vec4(x, y, 0.0f, 1.0f));
				};
				return std::array<Test::ReferenceTriangle, 2>{ { { .A = corner(-1, -1), .B = corner(1, -1), .C = corner(1, 1) },
					{ .A = corner(-1, -1), .B = corner(1, 1), .C = corner(-1, 1) } } };
			};
			const std::array<Test::ReferenceTriangle, 2> occluder = quadTriangles(occluderWorld);
			const glm::dvec3 light(lightPosition);
			// The oracle's answer for pixel (x, y): empty unless the pixel shows the floor within 3.5 m of the light (well
			// inside its 5 m range, so the unshadowed render lights it), else whether the light reaches that floor point.
			const auto oracle = [&snapshot, &occluder, &light](uint32_t x, uint32_t y) -> std::optional<bool>
			{
				const Result<RenderRay> ray = ComputeViewPixelRay(snapshot.Camera, x, y);
				REQUIRE(ray.has_value());
				const double distance = (-6.0 - static_cast<double>(ray->Origin.z)) / static_cast<double>(ray->Direction.z);
				const glm::dvec3 floorPoint = glm::dvec3(ray->Origin) + distance * glm::dvec3(ray->Direction);
				const bool onFloor = std::abs(floorPoint.x) <= 3.0 && std::abs(floorPoint.y) <= 3.0;
				// The camera sees the floor point when the occluder is not in front of it.
				if (!onFloor || !Test::IsPointLightVisible(floorPoint, glm::dvec3(ray->Origin), occluder) || glm::length(floorPoint - light) > 3.5)
					return std::nullopt;
				return Test::IsPointLightVisible(floorPoint, light, occluder);
			};

			uint32_t lit = 0;
			uint32_t unlit = 0;
			for (uint32_t y = 2; y + 2 < 64; ++y)
			{
				for (uint32_t x = 2; x + 2 < 64; ++x)
				{
					const std::optional<bool> centre = oracle(x, y);
					bool uniform = centre.has_value();
					for (uint32_t ny = y - 2; uniform && ny <= y + 2; ++ny)
					{
						for (uint32_t nx = x - 2; uniform && nx <= x + 2; ++nx)
							uniform = oracle(nx, ny) == centre;
					}
					if (!uniform)
						continue;
					CAPTURE(x);
					CAPTURE(y);
					CHECK(Test::SceneRendererIntegrationFixture::Channel(unshadowed, x, y) > 0);
					for (size_t channel = 0; channel < 3; ++channel)
					{
						const int expected = *centre ? Test::SceneRendererIntegrationFixture::Channel(unshadowed, x, y, channel) : 0;
						CHECK(Test::SceneRendererIntegrationFixture::Channel(shadowed, x, y, channel) == expected);
					}
					if (*centre)
						++lit;
					else
						++unlit;
				}
			}
			CHECK(lit >= 100);
			CHECK(unlit >= 20);
		}

		// R-PLS's budget (decision 3): five visible shadow-casting point lights over a floor; the four most important are
		// shadowed and the fifth, the dimmest, is lit without a shadow. RENDER_POINT_SHADOW_BUDGET is logged once per
		// renderer, the PointShadows row counts the faces' caster draws (in ShadowDraws too), and the render repeats exactly.
		TEST_CASE("SceneRenderer: more visible shadow-casting point lights than the budget warn once and stay lit")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 64, 64);
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes = { { .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -6.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(3.0f)),
				.Mesh = UUID(8910),
				.Materials = { UUID(8911) } } };
			snapshot.Environment.FallbackColor = glm::vec3(0.0f);
			for (uint32_t index = 0; index < MaxPointShadowLights; ++index)
				snapshot.Lights.push_back(MakeShadowedPoint(glm::vec3(-1.5f, -0.6f + 0.4f * static_cast<float>(index), -4.5f), 40.0f, 100 + index));
			snapshot.Lights.push_back(MakeShadowedPoint(glm::vec3(1.5f, 0.0f, -4.5f), 4.0f, 200));
			Test::ExpectLog warning(LogLevel::Warn, std::string(RenderPointShadowBudgetCode));
			const Image first = setup.Render(snapshot);
			++snapshot.FrameIndex;
			const Image second = setup.Render(snapshot);
			CHECK(warning.GetMatchCount() == 1);
			const RenderStats stats = setup.Renderer().GetRenderStats();
			CHECK(stats.ShadowedPointLights == MaxPointShadowLights);
			CHECK(stats.DroppedPointShadows == 1);
			const RenderPassStats* pass = Test::SceneRendererIntegrationFixture::FindPass(stats, "PointShadows");
			REQUIRE(pass != nullptr);
			CHECK(pass->DrawCalls >= MaxPointShadowLights);
			CHECK(stats.ShadowDraws == pass->DrawCalls);
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "SpotShadows") == nullptr);
			// The dropped light still lights the floor below it (x = 1.5 at z = -6 is pixel 32 + 1.5 / (6 tan 30) * 32 = 45.9).
			CHECK(Test::SceneRendererIntegrationFixture::Channel(second, 46, 32) > 0);
			CHECK(second.Pixels == first.Pixels);
		}

		// Decision 7: a device loss's release replaces the renderer's state but keeps RENDER_POINT_SHADOW_BUDGET's
		// once-per-renderer flag (SceneRendererRecovery.cpp's kept members), so the recovered renderer, still over the budget,
		// does not warn again.
		TEST_CASE("SceneRenderer: the point-shadow budget warning stays once per renderer across a release and recreation")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 64, 64);
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes = { { .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -6.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(3.0f)),
				.Mesh = UUID(8910),
				.Materials = { UUID(8911) } } };
			for (uint32_t index = 0; index <= MaxPointShadowLights; ++index)
				snapshot.Lights.push_back(MakeShadowedPoint(glm::vec3(-1.5f + 0.6f * static_cast<float>(index), 0.0f, -4.5f), 40.0f - static_cast<float>(index), 100 + index));
			Test::ExpectLog warning(LogLevel::Warn, std::string(RenderPointShadowBudgetCode));
			const Image before = setup.Render(snapshot);
			CHECK(warning.GetMatchCount() == 1);

			setup.Renderer().ReleaseGpuObjects();
			const Status recreated = setup.Renderer().RecreateGpuObjects();
			REQUIRE_MESSAGE(recreated.has_value(), recreated.error().ToString());
			++snapshot.FrameIndex;
			const Image after = setup.Render(snapshot);
			CHECK(warning.GetMatchCount() == 1);
			CHECK(setup.Renderer().GetRenderStats().DroppedPointShadows == 1);
			CHECK(after.Pixels == before.Pixels);
		}

		// A shadow-casting point light the plan cannot project (a source radius of half its range, so no near plane fits) is
		// lit without a shadow, reported once per renderer, and costs the valid light beside it nothing; with only it, no
		// PointShadows pass is recorded.
		TEST_CASE("SceneRenderer: a point light that cannot be shadowed is lit unshadowed and reported once")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 64, 64);
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes = { { .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -6.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(3.0f)),
				.Mesh = UUID(8910),
				.Materials = { UUID(8911) } } };
			snapshot.Environment.FallbackColor = glm::vec3(0.0f);
			LightData wide = MakeShadowedPoint(glm::vec3(0.0f, 0.0f, -4.5f), 40.0f, 300);
			wide.SourceRadius = 2.5f;
			snapshot.Lights = { wide };
			Test::ExpectLog warning(LogLevel::Warn, "cannot be shadowed");
			const Image rejected = setup.Render(snapshot);
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(setup.Renderer().GetRenderStats(), "PointShadows") == nullptr);
			CHECK(setup.Renderer().GetRenderStats().ShadowedPointLights == 0);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(rejected, 32, 32) > 0);
			snapshot.Lights.push_back(MakeShadowedPoint(glm::vec3(1.0f, 0.0f, -4.5f), 40.0f, 301));
			++snapshot.FrameIndex;
			static_cast<void>(setup.Render(snapshot));
			CHECK(warning.GetMatchCount() == 1);
			CHECK(setup.Renderer().GetRenderStats().ShadowedPointLights == 1);
			CHECK(setup.Renderer().GetRenderStats().DroppedPointShadows == 0);
		}

		// Once the cube array, its framebuffers and the forward set exist, a point-shadowed render creates no GPU object.
		TEST_CASE("SceneRenderer: steady-state point-shadowed renders create no GPU objects")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 64, 64);
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Lights = { MakeShadowedPoint(glm::vec3(0.5f, 0.0f, -1.0f), 20.0f, 31), MakeShadowedPoint(glm::vec3(-0.5f, 0.0f, -1.0f), 20.0f, 32) };
			for (uint32_t frame = 0; frame < 2; ++frame, ++snapshot.FrameIndex)
				setup.Submit(snapshot);
			const GpuResourceTracker& tracker = gpu.GetDevice().GetResourceTracker();
			const auto created = [&tracker]()
			{
				return std::array<uint64_t, 4>{ tracker.GetCounts(GpuResourceType::GpuBuffer).Created, tracker.GetCounts(GpuResourceType::BindingSet).Created,
					tracker.GetCounts(GpuResourceType::Framebuffer).Created, tracker.GetCounts(GpuResourceType::Texture).Created };
			};
			const std::array<uint64_t, 4> before = created();
			for (uint32_t frame = 0; frame < 3; ++frame, ++snapshot.FrameIndex)
				setup.Submit(snapshot);
			CHECK(setup.Renderer().GetRenderStats().ShadowedPointLights == 2);
			if (GpuResourceTracker::IsEnabled())
				CHECK(created() == before);
		}
	}

}
