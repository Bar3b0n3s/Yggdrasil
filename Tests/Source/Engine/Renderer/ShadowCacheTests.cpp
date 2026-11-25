#include "TestsPCH.h"

#include "Engine/Renderer/ShadowPass.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderStats.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

// The static shadow cache (M29, Docs/Decisions/0044-m29-light-limits.md decision 4): a shadowed point light whose light and
// casters did not change reuses its map, the spot atlas likewise as a whole, and a change of any kind of caster in a map's
// range, or of its light, draws that map again, each image equal to a render without the cache (a reset history).

namespace Engine {

	namespace {

		constexpr AssetHandle CacheStripMesh(0x5c29000000000001);
		constexpr AssetHandle CacheBlendMaterial(0x5c29000000000002);
		constexpr AssetHandle CacheMaskMaterial(0x5c29000000000003);
		// M46: a material with wind (every term), the same one with its wind disabled, and the fixture's quad with a level.
		constexpr AssetHandle CacheWindMaterial(0x5c29000000000004);
		constexpr AssetHandle CacheStillMaterial(0x5c29000000000005);
		constexpr AssetHandle CacheLodQuad(0x5c29000000000006);

		// A strip of two quads along +Y facing +Z (1 m wide, 2 m tall), skinned to two joints (the bottom row to joint 0, the
		// top to joint 1, the middle half to each), with one morph target that pushes the top row 0.5 m along +Z.
		Ref<MeshData> MakeCacheStrip()
		{
			Ref<MeshData> mesh = CreateRef<MeshData>();
			for (int row = 0; row < 3; ++row)
			{
				for (int column = 0; column < 2; ++column)
				{
					const glm::vec3 position(static_cast<float>(column) - 0.5f, static_cast<float>(row) - 1.0f, 0.0f);
					mesh->Vertices.push_back({ .Position = position, .Normal = { 0.0f, 0.0f, 1.0f }, .Tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .TexCoord = { static_cast<float>(column), static_cast<float>(row) / 2.0f } });
					mesh->Bounds.Extend(position);
					const float top = static_cast<float>(row) / 2.0f;
					mesh->Skin.push_back({ .Joints = { 0, 1, 0, 0 }, .Weights = { 1.0f - top, top, 0.0f, 0.0f } });
				}
			}
			mesh->SkinJointCount = 2;
			mesh->Indices = { 0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4 };
			mesh->Submeshes.push_back({ .IndexOffset = 0, .IndexCount = 12, .MaterialSlot = 0, .Bounds = mesh->Bounds });
			mesh->Slots.push_back({ .Name = "Default" });
			MeshMorphTarget push{ .Name = "Push" };
			for (size_t index = 0; index < mesh->Vertices.size(); ++index)
				push.Deltas.push_back({ .Position = { 0.0f, 0.0f, index >= 4 ? 0.5f : 0.0f } });
			mesh->MorphTargets.push_back(push);
			return mesh;
		}

		// The snapshot's meshes and lights, by index.
		constexpr size_t CasterA = 1;   // below light A and in the spot's cone
		constexpr size_t SkinnedB = 2;  // the skinned and morphed strip below light B
		constexpr size_t FarCaster = 3; // beyond both lights' ranges and the spot's cone
		constexpr size_t LightA = 0;

		// An 8 x 8 m receiver at z = -5 seen from the origin; point light A at (-1.5, 0, -3) with a caster below it, point
		// light B at (1.5, 0, -3) with the skinned strip below it, each casting into a cube of 2.5 m around it that the other's
		// caster stays outside; a spot light at (-1.5, 0, -2) whose cone holds caster A alone; and a caster above both cubes.
		RenderSnapshot MakeCacheScene(Test::SceneRendererIntegrationFixture& fixture)
		{
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.Environment.FallbackColor = glm::vec3(0.0f);
			snapshot.Meshes[0].World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(4.0f, 4.0f, 1.0f));
			snapshot.Meshes[0].CastShadows = false;
			const auto caster = [&snapshot](const glm::vec3& position, float scale, uint64_t entity)
			{
				MeshDrawItem item = snapshot.Meshes[0];
				item.World = glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
				item.CastShadows = true;
				item.Entity = UUID(entity);
				item.PickId = 0;
				return item;
			};
			snapshot.Meshes.push_back(caster(glm::vec3(-1.5f, 0.0f, -4.0f), 0.3f, 41));
			snapshot.Meshes.back().Materials = { CacheMaskMaterial }; // a Mask caster, whose material is part of its signature
			MeshDrawItem strip = caster(glm::vec3(1.5f, 0.0f, -4.0f), 0.3f, 42);
			strip.Mesh = CacheStripMesh;
			strip.JointPalette = { glm::mat4(1.0f), glm::mat4(1.0f) };
			strip.MorphWeights = { 0.0f };
			snapshot.Meshes.push_back(strip);
			snapshot.Meshes.push_back(caster(glm::vec3(0.0f, 3.2f, -4.0f), 0.3f, 43));
			const auto point = [](const glm::vec3& position, uint64_t entity)
			{
				return LightData{ .Type = RenderLightType::Point, .Intensity = 0.6f, .Position = position, .Range = 2.5f, .SourceRadius = 0.0f, .CastShadows = true, .Entity = UUID(entity) };
			};
			snapshot.Lights.push_back(point(glm::vec3(-1.5f, 0.0f, -3.0f), 100));
			snapshot.Lights.push_back(point(glm::vec3(1.5f, 0.0f, -3.0f), 101));
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Spot,
				.Intensity = 0.6f,
				.Position = glm::vec3(-1.5f, 0.0f, -2.0f),
				.Direction = glm::vec3(0.0f, 0.0f, -1.0f),
				.Range = 4.0f,
				.InnerConeAngle = 18.0f,
				.OuterConeAngle = 25.0f,
				.SourceRadius = 0.0f,
				.CastShadows = true,
				.Entity = UUID(102) });
			return snapshot;
		}

		// The view continues its temporal history (the cache and the hysteresis are used), or starts it afresh.
		void SetHistory(RenderSnapshot& snapshot, bool continues)
		{
			snapshot.Camera.PreviousViewProjection = continues ? std::optional<glm::mat4>(snapshot.Camera.Projection * snapshot.Camera.View) : std::nullopt;
		}

		// The Mask caster's material (a white base colour, so every texel passes its alpha test), published again with each
		// call: a new asset version, so a new GpuMaterial::Generation.
		void PublishMaskMaterial(Test::SceneRendererIntegrationFixture& fixture, float cutoff)
		{
			Ref<MaterialData> mask = CreateRef<MaterialData>();
			mask->AlphaMode = AlphaMode::Mask;
			mask->AlphaCutoff = cutoff;
			fixture.Assets().Publish(CacheMaskMaterial, mask);
		}

		void PublishCacheAssets(Test::SceneRendererIntegrationFixture& fixture)
		{
			fixture.Assets().Publish(CacheStripMesh, MakeCacheStrip());
			Ref<MaterialData> blend = CreateRef<MaterialData>();
			blend->AlphaMode = AlphaMode::Blend;
			fixture.Assets().Publish(CacheBlendMaterial, blend);
			PublishMaskMaterial(fixture, 0.5f);
		}

		// M46: the fixture's quad (positions (+-1, +-1, 0), facing +Z) whose level 1, below the screen size 0.3, is its
		// lower-right triangle.
		Ref<MeshData> MakeCacheLodQuad()
		{
			Ref<MeshData> mesh = CreateRef<MeshData>();
			mesh->Vertices = {
				{ .Position = { -1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 1 } },
				{ .Position = { 1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 1 } },
				{ .Position = { 1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 0 } },
				{ .Position = { -1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 0 } },
			};
			mesh->Indices = { 0, 1, 2, 0, 2, 3 };
			mesh->Bounds = { { -1, -1, 0 }, { 1, 1, 0 } };
			mesh->Submeshes = { { .IndexCount = 6, .Bounds = mesh->Bounds } };
			mesh->Slots = { {} };
			mesh->LodIndices = { 0, 1, 2 };
			mesh->Lods = { { .ScreenSize = 0.3f, .Submeshes = { { .IndexOffset = 0, .IndexCount = 3 } } } };
			const Status valid = ValidateMeshData(*mesh);
			REQUIRE_MESSAGE(valid.has_value(), valid.error().ToString());
			return mesh;
		}

		// M46: a storm (gusts, turbulence and a direction off the X axis) at a time where every wave is mid-cycle.
		RenderWind MakeCacheWind()
		{
			return { .Enabled = true, .Direction = glm::vec3(1.0f, 0.3f, 0.5f), .Strength = 18.0f, .GustStrength = 9.0f, .GustFrequency = 0.7f, .Turbulence = 1.0f, .Time = 12.345 };
		}

		void PublishCacheLodWindAssets(Test::SceneRendererIntegrationFixture& fixture)
		{
			const MaterialWind wind{ .Enabled = true, .Bend = 0.25f, .Branch = 0.4f, .Flutter = 0.2f, .Stiffness = 0.1f };
			Ref<MaterialData> swaying = CreateRef<MaterialData>();
			swaying->Wind = wind;
			fixture.Assets().Publish(CacheWindMaterial, swaying);
			Ref<MaterialData> still = CreateRef<MaterialData>();
			still->Wind = wind;
			still->Wind.Enabled = false;
			fixture.Assets().Publish(CacheStillMaterial, still);
			fixture.Assets().Publish(CacheLodQuad, MakeCacheLodQuad());
		}

		// One change of the cached scene: what it does and the maps it leaves cached.
		struct CacheChange
		{
			std::string_view Name{};
			std::function<void(RenderSnapshot&, Test::SceneRendererIntegrationFixture&)> Apply{};
			uint32_t CachedPoints = 0;
			uint32_t CachedSpots = 0;
		};

		// For each change: the scene (after `prepare`, when given) drawn afresh and then reused, the change rendered continuing
		// the history, its cached counts checked, and its image compared with a render of the changed scene without the cache.
		void CheckCacheChanges(Test::SceneRendererIntegrationFixture& fixture, const std::vector<CacheChange>& changes,
			const std::function<void(RenderSnapshot&)>& prepare = {})
		{
			for (const CacheChange& change : changes)
			{
				CAPTURE(change.Name);
				// The base, drawn afresh and then reused.
				RenderSnapshot base = MakeCacheScene(fixture);
				if (prepare)
					prepare(base);
				SetHistory(base, false);
				static_cast<void>(fixture.Render(base));
				SetHistory(base, true);
				++base.FrameIndex;
				static_cast<void>(fixture.Render(base));
				REQUIRE(fixture.Renderer().GetRenderStats().CachedPointShadows == 2);
				REQUIRE(fixture.Renderer().GetRenderStats().CachedSpotShadows == 1);

				RenderSnapshot changed = base;
				++changed.FrameIndex;
				change.Apply(changed, fixture);
				const Image cached = fixture.Render(changed);
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.CachedPointShadows == change.CachedPoints);
				CHECK(stats.CachedSpotShadows == change.CachedSpots);
				SetHistory(changed, false);
				++changed.FrameIndex;
				CHECK(fixture.Render(changed).Pixels == cached.Pixels);
			}
		}

	}

	TEST_SUITE(Test::GpuSuite)
	{
		// A view that continues its history and changes nothing draws its point and spot maps once: later renders reuse them,
		// recording no caster in the PointShadows and SpotShadows rows, and render the image the first render drew.
		TEST_CASE("ShadowCache: an unchanged view reuses its shadow maps and draws no caster")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishCacheAssets(fixture);
			RenderSnapshot snapshot = MakeCacheScene(fixture);
			SetHistory(snapshot, false);
			const Image fresh = fixture.Render(snapshot);
			RenderStats stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.ShadowedPointLights == 2);
			CHECK(stats.ShadowedSpotLights == 1);
			CHECK(stats.CachedPointShadows == 0);
			CHECK(stats.CachedSpotShadows == 0);
			const RenderPassStats* points = Test::SceneRendererIntegrationFixture::FindPass(stats, "PointShadows");
			REQUIRE(points != nullptr);
			CHECK(points->DrawCalls > 0);
			CHECK(stats.ShadowDraws > 0);

			SetHistory(snapshot, true);
			for (uint32_t frame = 0; frame < 2; ++frame)
			{
				CAPTURE(frame);
				++snapshot.FrameIndex;
				CHECK(fixture.Render(snapshot).Pixels == fresh.Pixels);
				stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.CachedPointShadows == 2);
				CHECK(stats.CachedSpotShadows == 1);
				const RenderPassStats* cachedPoints = Test::SceneRendererIntegrationFixture::FindPass(stats, "PointShadows");
				const RenderPassStats* cachedSpots = Test::SceneRendererIntegrationFixture::FindPass(stats, "SpotShadows");
				REQUIRE(cachedPoints != nullptr);
				REQUIRE(cachedSpots != nullptr);
				CHECK(cachedPoints->DrawCalls == 0);
				CHECK(cachedSpots->DrawCalls == 0);
				CHECK(stats.ShadowDraws == 0);
			}
		}

		// Each kind of change to a caster in a map's range (its transform, its skinning pose, its morph weights, its
		// visibility, its material, its Mask material or mesh published again, a caster added) and a moved light draws that map
		// again, while the maps it does not reach stay cached; a caster outside every map moving redraws none; new shadow targets
		// and a shader reload draw every map. Each image equals the render without the cache.
		TEST_CASE("ShadowCache: a moved, skinned, morphed, hidden, re-materialized or added caster, or a moved light, draws its map again")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishCacheAssets(fixture);
			const std::vector<CacheChange> changes = {
				{ "a moved caster", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[CasterA].World = glm::translate(glm::mat4(1.0f), glm::vec3(-0.1f, 0.0f, 0.0f)) * snapshot.Meshes[CasterA].World;
			}, 1, 0 },
				{ "a skinned caster posed", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[SkinnedB].JointPalette[1] = glm::translate(glm::mat4(1.0f), glm::vec3(0.25f, 0.0f, 0.0f));
			}, 1, 1 },
				{ "a morphed caster", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[SkinnedB].MorphWeights[0] = 1.0f;
			}, 1, 1 },
				{ "a hidden caster", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[CasterA].CastShadows = false;
			}, 1, 0 },
				{ "a re-materialized caster", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[CasterA].Materials = { CacheBlendMaterial };
			}, 1, 0 },
				{ "an added caster", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				MeshDrawItem added = snapshot.Meshes[CasterA];
				added.World = glm::translate(glm::mat4(1.0f), glm::vec3(1.5f, 0.6f, -3.8f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.15f));
				added.Entity = UUID(44);
				snapshot.Meshes.push_back(added);
			}, 1, 1 },
				{ "a moved light", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Lights[LightA].Position.x -= 0.1f;
			}, 1, 1 },
				{ "a caster outside every map moved", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[FarCaster].World = glm::translate(glm::mat4(1.0f), glm::vec3(0.3f, 0.0f, 0.0f)) * snapshot.Meshes[FarCaster].World;
			}, 2, 1 },
				{ "a Mask material published again", [](RenderSnapshot&, Test::SceneRendererIntegrationFixture& fixture)
			{
				PublishMaskMaterial(fixture, 0.25f);
			}, 1, 0 },
				{ "a mesh published again", [](RenderSnapshot&, Test::SceneRendererIntegrationFixture& fixture)
			{
				fixture.Assets().Publish(CacheStripMesh, MakeCacheStrip());
			}, 1, 1 },
				{ "new shadow targets", [](RenderSnapshot&, Test::SceneRendererIntegrationFixture& fixture)
			{
				fixture.Renderer().ReleaseTargets();
			}, 0, 0 },
				{ "a shader reload", [](RenderSnapshot&, Test::SceneRendererIntegrationFixture& fixture)
			{
				const Status reloaded = fixture.Pipelines().ReloadShaders();
				REQUIRE_MESSAGE(reloaded.has_value(), reloaded.error().ToString());
			}, 0, 0 },
			};
			CheckCacheChanges(fixture, changes);
		}

		// M31's material parameters (batch 1's landing; the review of W31.1, finding 4, and the landing's review, finding 2): a
		// Mask caster's map depends on exactly the record values Shadow.slang reads, so a change of its dissolve, of its noise
		// scale while it dissolves partly, or of a Mask material's texture offset draws the maps it reaches again, while a change
		// the casters never read keeps them: its tint, its fade, the noise scale of a caster dissolved whole and the offset of an
		// Opaque material that dissolves (its alpha is never tested).
		TEST_CASE("ShadowCache: a Mask caster's dissolve, noise scale or texture offset draws its map again, and its tint and fade do not")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishCacheAssets(fixture);
			// Caster A, a Mask material's caster in light A's cube and the spot's cone, tinted and half dissolved, so its record
			// holds the parameters and each change moves one value.
			const std::vector<CacheChange> read = {
				{ "a caster dissolved further", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[CasterA].Parameters.DissolveAmount = 0.75f;
			}, 1, 0 },
				{ "a half-dissolved caster's noise scaled", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[CasterA].Parameters.DissolveNoiseScale = 4.0f;
			}, 1, 0 },
				{ "a Mask material's texture offset scrolled", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[CasterA].Parameters.UVOffset = glm::vec2(0.25f, 0.0f);
			}, 1, 0 },
				{ "a caster faded", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[CasterA].Parameters.Opacity = 0.5f;
			}, 2, 1 },
				{ "a caster tinted again", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[CasterA].Parameters.Tint = glm::vec3(1.0f, 0.5f, 0.25f);
			}, 2, 1 },
			};
			CheckCacheChanges(fixture, read, [](RenderSnapshot& snapshot)
			{
				snapshot.Meshes[CasterA].Parameters = { .Tint = glm::vec3(0.5f, 0.75f, 1.0f), .DissolveAmount = 0.5f };
			});

			// Caster A dissolved whole, which casts nothing whatever its noise; the skinned strip, an Opaque material's caster in
			// light B's cube, half dissolved, which tests no alpha and samples its noise without the offset.
			const std::vector<CacheChange> unread = {
				{ "a caster dissolved whole has its noise scaled", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[CasterA].Parameters.DissolveNoiseScale = 4.0f;
			}, 2, 1 },
				{ "an Opaque material's texture offset scrolled", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[SkinnedB].Parameters.UVOffset = glm::vec2(0.25f, 0.0f);
			}, 2, 1 },
			};
			CheckCacheChanges(fixture, unread, [](RenderSnapshot& snapshot)
			{
				snapshot.Meshes[CasterA].Parameters = { .DissolveAmount = 1.0f };
				snapshot.Meshes[SkinnedB].Parameters = { .DissolveAmount = 0.5f };
			});
		}

		// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3E; the W46.2 review, finding 6): a caster's
		// signature holds its level and, for a wind caster, its wind response and the render's wind. Caster A with a wind
		// material under a storm: two renders at one wind time reuse every map (each change's base), and a later time draws
		// light A's map and the spot atlas, which hold it, again, while light B's stays cached; with its material's wind
		// disabled the moving wind keeps every map. Caster A as a quad with a level (ShadowLodOffset 0, so it casts the view's
		// level; at 4.27 m its screen size is 0.17): a LodBias that keeps level 0 (4, then 2) keeps the maps, and one that
		// brings it below its switch size 0.3 (1) draws them again. Each image equals the render without the cache.
		TEST_CASE("ShadowCache: a wind caster's map draws again as its wind moves and stays cached while it holds, and a caster's new level draws it again")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishCacheAssets(fixture);
			PublishCacheLodWindAssets(fixture);
			const auto advance = [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Wind.Time += 0.3;
			};
			const auto swaying = [](RenderSnapshot& snapshot)
			{
				snapshot.Meshes[CasterA].Materials = { CacheWindMaterial };
				snapshot.Wind = MakeCacheWind();
			};
			const std::vector<CacheChange> moved = { { "the wind moved on", advance, 1, 0 } };
			CheckCacheChanges(fixture, moved, swaying);

			const auto still = [](RenderSnapshot& snapshot)
			{
				snapshot.Meshes[CasterA].Materials = { CacheStillMaterial };
				snapshot.Wind = MakeCacheWind();
			};
			const std::vector<CacheChange> calm = { { "the wind moved on past a caster without wind", advance, 2, 1 } };
			CheckCacheChanges(fixture, calm, still);

			const auto leveled = [](RenderSnapshot& snapshot)
			{
				snapshot.Meshes[CasterA].Mesh = CacheLodQuad;
				snapshot.Meshes[CasterA].Distance = { .LodBias = 4.0f, .ShadowLodOffset = 0 };
			};
			const std::vector<CacheChange> levels = {
				{ "a LodBias that keeps the caster's level", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[CasterA].Distance.LodBias = 2.0f;
			}, 2, 1 },
				{ "a LodBias that changes the caster's level", [](RenderSnapshot& snapshot, Test::SceneRendererIntegrationFixture&)
			{
				snapshot.Meshes[CasterA].Distance.LodBias = 1.0f;
			}, 1, 0 },
			};
			CheckCacheChanges(fixture, levels, leveled);
		}
	}

}
