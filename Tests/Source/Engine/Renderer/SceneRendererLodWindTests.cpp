#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStaticSet.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3D, 3E, 3G, 3K and 3L): a mesh's levels of
// detail and the scene's wind drawn by the scene renderer. Each view draws the level its camera's screen size selects, two
// levels inside a fade band with complementary dither, nothing beyond the item's CullDistance; casters draw a coarser level;
// a material with wind sways by the snapshot's Wind identically in every pipeline, and its motion vectors follow the sway.
// W46.4 (M43's static path): every case runs with its meshes dynamic, then static (MakeStaticTwin), with the same outcome,
// and the static path's own cases check a static item against its dynamic twin: its levels, its fades, each submesh's
// wind, its casters under the three kinds of shadow maps, and a material reload under an unchanged cell.

namespace Engine {

	namespace {

		constexpr AssetHandle TriangleLodQuad{ 0x4646000000000001ULL }; // level 1: the quad's lower-right triangle
		constexpr AssetHandle TwinLodQuad{ 0x4646000000000002ULL };     // level 1: the whole quad again
		constexpr AssetHandle PlainGrid{ 0x4646000000000003ULL };       // a grid without a wind stream (bends by height)
		constexpr AssetHandle SweptGrid{ 0x4646000000000004ULL };       // a grid with a _WIND stream
		constexpr AssetHandle PartedGrid{ 0x4646000000000006ULL };      // SweptGrid as two submeshes, its lower and upper halves
		constexpr AssetHandle WhiteMaterial{ 0x4646000000000010ULL };
		constexpr AssetHandle RedMaterial{ 0x4646000000000011ULL };
		constexpr AssetHandle GlassMaterial{ 0x4646000000000012ULL }; // Blend, half transparent, emissive white
		constexpr AssetHandle LeafMaterial{ 0x4646000000000013ULL };  // wind: Bend 0.25, no branch or flutter
		constexpr AssetHandle StormMaterial{ 0x4646000000000014ULL }; // wind: every term
		constexpr AssetHandle StillMaterial{ 0x4646000000000015ULL }; // LeafMaterial's look, wind disabled
		constexpr AssetHandle BarkMaterial{ 0x4646000000000018ULL };  // wind: stiff, a little bend, no flutter (W46.4)
		constexpr float LeafBend = 0.25f;
		constexpr uint32_t Size = 64;

		// The integration fixture's quad (positions (+-1, +-1, 0), facing +Z) with one further level of index-only triangles
		// `level`, which takes over below the screen size `switchSize`.
		Ref<MeshData> MakeLodQuad(std::vector<uint32_t> level, float switchSize)
		{
			auto mesh = CreateRef<MeshData>();
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
			const auto count = static_cast<uint32_t>(level.size());
			mesh->LodIndices = std::move(level);
			mesh->Lods = { { .ScreenSize = switchSize, .Submeshes = { { .IndexOffset = 0, .IndexCount = count } } } };
			const Status valid = ValidateMeshData(*mesh);
			REQUIRE_MESSAGE(valid.has_value(), valid.error().ToString());
			return mesh;
		}

		// A vertical grid of 8 x 32 quads in its entity's XY plane, x in [-0.5, 0.5] and y in [0, 2] (the entity's origin at its
		// bottom, so a vertex's height is its y), facing +Z; with `swept`, a _WIND stream whose branch weight rises across the
		// grid, whose phases vary per vertex, whose flutter is 200 / 255 and whose bottom row's bend mask holds it still.
		Ref<MeshData> MakeGrid(bool swept)
		{
			constexpr uint32_t Columns = 8;
			constexpr uint32_t Rows = 32;
			auto mesh = CreateRef<MeshData>();
			for (uint32_t row = 0; row <= Rows; ++row)
			{
				for (uint32_t column = 0; column <= Columns; ++column)
				{
					const float u = static_cast<float>(column) / static_cast<float>(Columns);
					const float v = static_cast<float>(row) / static_cast<float>(Rows);
					mesh->Vertices.push_back({ .Position = { u - 0.5f, 2.0f * v, 0.0f }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { u, 1.0f - v } });
					if (swept)
					{
						mesh->Wind.push_back({ .Branch = static_cast<uint8_t>(255U * column / Columns),
							.Phase = static_cast<uint8_t>((37U * (row * (Columns + 1) + column)) % 256U),
							.Flutter = 200,
							.BendMask = static_cast<uint8_t>(row == 0 ? 255 : 0) });
					}
				}
			}
			for (uint32_t row = 0; row < Rows; ++row)
			{
				for (uint32_t column = 0; column < Columns; ++column)
				{
					const uint32_t a = row * (Columns + 1) + column;
					const uint32_t d = a + Columns + 1;
					mesh->Indices.insert(mesh->Indices.end(), { a, a + 1, d + 1, a, d + 1, d });
				}
			}
			mesh->Bounds = { { -0.5f, 0.0f, 0.0f }, { 0.5f, 2.0f, 0.0f } };
			mesh->Submeshes = { { .IndexCount = static_cast<uint32_t>(mesh->Indices.size()), .Bounds = mesh->Bounds } };
			mesh->Slots = { {} };
			const Status valid = ValidateMeshData(*mesh);
			REQUIRE_MESSAGE(valid.has_value(), valid.error().ToString());
			return mesh;
		}

		// W46.4: SweptGrid as two submeshes, its lower 16 rows (slot 0, the bark) and its upper 16 rows (slot 1, the leaves), so
		// one item's two submeshes sway by two materials' wind.
		Ref<MeshData> MakePartedGrid()
		{
			Ref<MeshData> mesh = MakeGrid(true);
			const auto half = static_cast<uint32_t>(mesh->Indices.size() / 2);
			mesh->Submeshes = { { .IndexOffset = 0, .IndexCount = half, .MaterialSlot = 0, .Bounds = { { -0.5f, 0.0f, 0.0f }, { 0.5f, 1.0f, 0.0f } } },
				{ .IndexOffset = half, .IndexCount = half, .MaterialSlot = 1, .Bounds = { { -0.5f, 1.0f, 0.0f }, { 0.5f, 2.0f, 0.0f } } } };
			mesh->Slots = { {}, {} };
			const Status valid = ValidateMeshData(*mesh);
			REQUIRE_MESSAGE(valid.has_value(), valid.error().ToString());
			return mesh;
		}

		Ref<MaterialData> MakeMaterial(const glm::vec4& baseColor, const MaterialWind& wind = {})
		{
			auto material = CreateRef<MaterialData>();
			material->BaseColor = baseColor;
			material->Wind = wind;
			return material;
		}

		void PublishAssets(Test::SceneRendererIntegrationFixture& fixture)
		{
			Test::InMemoryAssetManager& assets = fixture.Assets();
			assets.Publish(TriangleLodQuad, MakeLodQuad({ 0, 1, 2 }, 0.9f));
			assets.Publish(TwinLodQuad, MakeLodQuad({ 0, 1, 2, 0, 2, 3 }, 0.5f));
			assets.Publish(PlainGrid, MakeGrid(false));
			assets.Publish(SweptGrid, MakeGrid(true));
			assets.Publish(PartedGrid, MakePartedGrid());
			assets.Publish(WhiteMaterial, MakeMaterial(glm::vec4(1.0f)));
			assets.Publish(RedMaterial, MakeMaterial(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));
			auto glass = MakeMaterial(glm::vec4(0.0f, 0.0f, 0.0f, 0.5f));
			glass->AlphaMode = AlphaMode::Blend;
			glass->Emissive = glm::vec3(1.0f);
			assets.Publish(GlassMaterial, glass);
			assets.Publish(LeafMaterial, MakeMaterial(glm::vec4(1.0f), { .Enabled = true, .Bend = LeafBend, .Branch = 0.0f, .Flutter = 0.0f, .Stiffness = 0.0f }));
			assets.Publish(StormMaterial, MakeMaterial(glm::vec4(1.0f), { .Enabled = true, .Bend = 0.25f, .Branch = 0.4f, .Flutter = 0.2f, .Stiffness = 0.1f }));
			assets.Publish(StillMaterial, MakeMaterial(glm::vec4(1.0f), { .Enabled = false, .Bend = LeafBend, .Branch = 0.0f, .Flutter = 0.0f, .Stiffness = 0.0f }));
			assets.Publish(BarkMaterial, MakeMaterial(glm::vec4(0.6f, 0.4f, 0.2f, 1.0f), { .Enabled = true, .Bend = 0.05f, .Branch = 0.02f, .Flutter = 0.0f, .Stiffness = 0.8f }));
		}

		// A gale of 20 m/s along +X without gusts or turbulence: a Stiffness 0 material's response is exactly 1, so a vertex at
		// height h leans by Bend h^2 along +X and drops by Bend^2 h^3 / 2 (Common/Wind.slang).
		RenderWind MakeGale()
		{
			return { .Enabled = true, .Direction = glm::vec3(1.0f, 0.0f, 0.0f), .Strength = 20.0f, .GustStrength = 0.0f, .GustFrequency = 0.2f, .Turbulence = 0.0f, .Time = 0.0 };
		}

		// The same Wind without strength or gusts: every offset is exactly 0.
		RenderWind MakeCalm()
		{
			RenderWind calm = MakeGale();
			calm.Strength = 0.0f;
			calm.Time = 3.7;
			return calm;
		}

		// A storm: gusts, turbulence and a direction off the X axis, at a time where every wave is mid-cycle.
		RenderWind MakeStorm()
		{
			return { .Enabled = true, .Direction = glm::vec3(1.0f, 0.3f, 0.5f), .Strength = 18.0f, .GustStrength = 9.0f, .GustFrequency = 0.7f, .Turbulence = 1.0f, .Time = 12.345 };
		}

		// The fixture's snapshot with a white wall of 12 x 12 m at z = -8 (filling the view) and one item `mesh` placed by
		// `world` with `material`.
		RenderSnapshot MakeScene(const Test::SceneRendererIntegrationFixture& fixture, AssetHandle mesh, const glm::mat4& world, AssetHandle material)
		{
			RenderSnapshot snapshot = fixture.Snapshot();
			MeshDrawItem wall = snapshot.Meshes.front();
			wall.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -8.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(6.0f));
			wall.Materials = { WhiteMaterial };
			MeshDrawItem item = wall;
			item.World = world;
			item.Mesh = mesh;
			item.Materials = { material };
			item.Entity = UUID(40);
			item.PickId = 2;
			snapshot.Meshes = { wall, item };
			return snapshot;
		}

		// A directional light shining along `direction` that casts shadows.
		void AddSun(RenderSnapshot& snapshot, const glm::vec3& direction)
		{
			snapshot.Lights.push_back({ .Type = RenderLightType::Directional, .Intensity = 3.0f, .Direction = glm::normalize(direction), .CastShadows = true, .ShadowDistance = 20.0f });
		}

		int Green(const Image& image, uint32_t x, uint32_t y)
		{
			return Test::SceneRendererIntegrationFixture::Channel(image, x, y, 1);
		}

		// The pixel (x right, y down, continuous) of world point `world` in the fixture's 64 x 64 view.
		glm::vec2 ProjectToPixel(const RenderSnapshot& snapshot, const glm::vec3& world)
		{
			const glm::vec4 clip = snapshot.Camera.Projection * snapshot.Camera.View * glm::vec4(world, 1.0f);
			const glm::vec2 ndc = glm::vec2(clip) / clip.w;
			return { (ndc.x * 0.5f + 0.5f) * static_cast<float>(Size), (0.5f - ndc.y * 0.5f) * static_cast<float>(Size) };
		}

		// The last render's SceneVelocity, one motion vector (pixels, current minus previous) per scene pixel.
		std::vector<glm::vec2> ReadVelocity(GraphicsDevice& device, SceneRenderer& renderer)
		{
			nvrhi::ITexture* texture = renderer.GetSceneVelocityTexture();
			REQUIRE(texture != nullptr);
			REQUIRE(texture->getDesc().format == SceneVelocityFormat);
			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(*texture);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			std::vector<uint16_t> halves(image->Pixels.size() / sizeof(uint16_t));
			std::memcpy(halves.data(), image->Pixels.data(), halves.size() * sizeof(uint16_t));
			std::vector<glm::vec2> motion;
			for (size_t index = 0; index + 1 < halves.size(); index += 2)
				motion.emplace_back(glm::unpackHalf1x16(halves[index]), glm::unpackHalf1x16(halves[index + 1]));
			return motion;
		}

		// The gale's lean (m along +X) of a LeafMaterial vertex whose displaced height above its origin is `height`: the rest
		// height h with h - Bend^2 h^3 / 2 = height, then Bend h^2, Bend as the record's unorm8 carries it.
		float PredictLean(float height)
		{
			const float bend = std::round(LeafBend * 255.0f) / 255.0f;
			float rest = height;
			for (int iteration = 0; iteration < 16; ++iteration)
				rest = height + 0.5f * bend * bend * rest * rest * rest;
			return bend * rest * rest;
		}

		// W46.4 (M43's static path, Renderer/RenderStaticSet.h): the static twin of `snapshot`, its mesh items moved into a static
		// set built as the scene's RenderStaticCache builds one (each in the cell of its world origin, floor(x / 32) and floor(z /
		// 32), cells in key order, items in snapshot order, without a pick id or a previous matrix), but a planar reflection's
		// glass, which is never static scenery. A case renders the twin to check the static path against the dynamic one.
		RenderSnapshot MakeStaticTwin(RenderSnapshot snapshot)
		{
			const auto key = [](float coordinate)
			{
				return static_cast<int32_t>(std::floor(static_cast<double>(coordinate) / static_cast<double>(StaticCellSize)));
			};
			std::map<std::pair<int32_t, int32_t>, Ref<RenderStaticCell>> cells;
			std::vector<MeshDrawItem> kept;
			uint64_t version = 0;
			for (const MeshDrawItem& item : snapshot.Meshes)
			{
				const bool glass = std::any_of(snapshot.PlanarReflections.begin(), snapshot.PlanarReflections.end(), [&item](const PlanarReflectionItem& reflection)
				{
					return reflection.Entity == item.Entity;
				});
				if (glass)
				{
					kept.push_back(item);
					continue;
				}
				Ref<RenderStaticCell>& cell = cells[{ key(item.World[3].x), key(item.World[3].z) }];
				if (cell == nullptr)
				{
					cell = CreateRef<RenderStaticCell>();
					cell->Key = glm::ivec2(key(item.World[3].x), key(item.World[3].z));
					cell->Version = ++version;
				}
				MeshDrawItem copy = item;
				copy.PickId = 0;
				copy.PreviousWorld.reset();
				cell->Items.push_back(copy);
				cell->PickTable.push_back(item.Entity);
			}
			Ref<RenderStaticSet> set = CreateRef<RenderStaticSet>();
			for (const auto& entry : cells)
				set->Cells.push_back(entry.second);
			set->Version = ++version;
			snapshot.StaticSet = set;
			snapshot.Meshes.assign(kept.begin(), kept.end());
			snapshot.PickTable.clear();
			for (size_t index = 0; index < snapshot.Meshes.size(); ++index)
			{
				snapshot.Meshes[index].PickId = static_cast<uint32_t>(index + 1);
				snapshot.PickTable.push_back(snapshot.Meshes[index].Entity);
			}
			return snapshot;
		}

		// `snapshot` rendered with its meshes dynamic, or with `statics` as its static twin (a new static set each call).
		Image RenderAs(Test::SceneRendererIntegrationFixture& fixture, bool statics, const RenderSnapshot& snapshot)
		{
			return fixture.Render(statics ? MakeStaticTwin(snapshot) : snapshot);
		}

		void SubmitAs(Test::SceneRendererIntegrationFixture& fixture, bool statics, const RenderSnapshot& snapshot)
		{
			fixture.Submit(statics ? MakeStaticTwin(snapshot) : snapshot);
		}

		// The horizontal centroid (pixels) of the shadow on row `y`: each pixel weighted by how much darker `image` is than the
		// unshadowed `bare`; nullopt where the row holds almost no shadow.
		std::optional<float> ShadowCentroid(const Image& image, const Image& bare, uint32_t y)
		{
			double weight = 0.0;
			double moment = 0.0;
			for (uint32_t x = 0; x < Size; ++x)
			{
				const int darker = Green(bare, x, y) - Green(image, x, y);
				if (darker <= 0)
					continue;
				weight += darker;
				moment += static_cast<double>(darker) * (static_cast<double>(x) + 0.5);
			}
			if (weight < 200.0)
				return std::nullopt;
			return static_cast<float>(moment / weight);
		}

	}

	TEST_SUITE("Renderer")
	{
		// Decisions 3D and 3E: the quad at 3 m covers about 0.816 of the view's height; its level 1 is its lower-right triangle
		// alone, so pixel (23, 23) sees only level 0 and pixel (41, 41) both. Below the level's switch size (0.9), or once
		// LodBias brings the size below it, the view draws level 1; above it, level 0. The stats count the draws per level.
		TEST_CASE("MeshLods: a mesh draws its coarser level below its switch's screen size" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// W46.4: with its meshes dynamic, then static (MakeStaticTwin), the same outcome.
			for (const bool statics : { false, true })
			{
				CAPTURE(statics);
				Test::SceneRendererIntegrationFixture fixture(gpu);
				PublishAssets(fixture);
				RenderSnapshot snapshot = fixture.Snapshot();
				snapshot.DebugView = RenderDebugView::Albedo;
				snapshot.Meshes[0].Mesh = TriangleLodQuad;
				snapshot.Meshes[0].Materials = { WhiteMaterial };

				const Image coarse = RenderAs(fixture, statics, snapshot);
				CHECK(Green(coarse, 23, 23) < 8);
				CHECK(Green(coarse, 41, 41) > 200);
				RenderStats stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.MeshLodDraws[0] == 0);
				CHECK(stats.MeshLodDraws[1] == 1);
				CHECK(stats.LodCrossFades == 0);

				// A larger switch size keeps level 0 ... (the mesh's own level 1 now switches at 0.5).
				fixture.Assets().Publish(TriangleLodQuad, MakeLodQuad({ 0, 1, 2 }, 0.5f));
				++snapshot.FrameIndex;
				const Image fine = RenderAs(fixture, statics, snapshot);
				CHECK(Green(fine, 23, 23) > 200);
				CHECK(Green(fine, 41, 41) > 200);
				stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.MeshLodDraws[0] == 1);
				CHECK(stats.MeshLodDraws[1] == 0);

				// ... until LodBias halves the screen size the switch compares.
				snapshot.Meshes[0].Distance.LodBias = 0.5f;
				++snapshot.FrameIndex;
				const Image biased = RenderAs(fixture, statics, snapshot);
				CHECK(biased.Pixels == coarse.Pixels);
				CHECK(fixture.Renderer().GetRenderStats().MeshLodDraws[1] == 1);
			}
		}

		// Decision 3E: inside a band, the finer level keeps the pixels whose dither threshold is at least t and the coarser
		// level the others, so where both levels cover the item every pixel is drawn exactly once. Two levels of one quad
		// (LodBias 0.6124 puts the item at its switch size 0.5, FadeWidth 0.5, so t is about 0.5): an Opaque material (the Mask
		// variants) leaves no hole, the image equal to one unfaded draw's; a half-transparent Blend material blends each pixel
		// once, neither twice nor never, the image again equal to one unfaded draw's.
		TEST_CASE("MeshLods: across a fade band the two levels cover every pixel exactly once" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// W46.4: with its meshes dynamic, then static (MakeStaticTwin), the same outcome.
			for (const bool statics : { false, true })
			{
				CAPTURE(statics);
				Test::SceneRendererIntegrationFixture fixture(gpu);
				PublishAssets(fixture);
				RenderSnapshot snapshot = fixture.Snapshot();
				snapshot.Meshes[0].Mesh = TwinLodQuad;
				const MeshDrawDistance banded{ .LodBias = 0.6124f, .FadeWidth = 0.5f };
				for (const AssetHandle material : { WhiteMaterial, GlassMaterial })
				{
					CAPTURE(material);
					snapshot.Meshes[0].Materials = { material };
					snapshot.DebugView = material == WhiteMaterial ? RenderDebugView::Albedo : RenderDebugView::Lit;
					snapshot.Meshes[0].Distance = {};
					++snapshot.FrameIndex;
					const Image whole = RenderAs(fixture, statics, snapshot);
					CHECK(fixture.Renderer().GetRenderStats().LodCrossFades == 0);
					CHECK(Green(whole, 32, 32) > 32);

					snapshot.Meshes[0].Distance = banded;
					++snapshot.FrameIndex;
					const Image faded = RenderAs(fixture, statics, snapshot);
					const RenderStats stats = fixture.Renderer().GetRenderStats();
					CHECK(stats.LodCrossFades == 1);
					CHECK(stats.MeshLodDraws[0] == 1);
					CHECK(stats.MeshLodDraws[1] == 1);
					CHECK(faded.Pixels == whole.Pixels);
					if (material == WhiteMaterial)
					{
						const RenderPassStats* depth = Test::SceneRendererIntegrationFixture::FindPass(stats, "DepthNormal");
						REQUIRE(depth != nullptr);
						CHECK(depth->DrawCalls == 2);
					}
				}
			}
		}

		// Decisions 3D and 3E: the camera draws the quad caster at 5.1 m (screen size 0.48) at level 0, and its shadow comes
		// from level 1 (ShadowLodOffset 1 by default), its lower-right triangle: the wall at A, in the triangle's shadow, is
		// dark, and at B, in the shadow only level 0 would cast, lit. With ShadowLodOffset 0 the whole quad casts.
		TEST_CASE("MeshLods: a caster's shadow comes from its coarser level" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// W46.4: with its meshes dynamic, then static (MakeStaticTwin), the same outcome.
			for (const bool statics : { false, true })
			{
				CAPTURE(statics);
				Test::SceneRendererIntegrationFixture fixture(gpu);
				PublishAssets(fixture);
				fixture.Assets().Publish(TriangleLodQuad, MakeLodQuad({ 0, 1, 2 }, 0.3f));
				RenderSnapshot snapshot = MakeScene(fixture, TriangleLodQuad, glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, -5.0f)), WhiteMaterial);
				AddSun(snapshot, glm::vec3(1.0f, 0.0f, -1.0f));
				// The quad's shadow on the wall spans x in [1, 3] and y in [-1, 1]: A = (2.5, -0.5) lies in the lower-right
				// triangle's shadow, B = (1.5, 0.5) in the upper-left one's, C = (4, 0) outside both.
				const glm::uvec2 a = glm::uvec2(ProjectToPixel(snapshot, glm::vec3(2.5f, -0.5f, -8.0f)));
				const glm::uvec2 b = glm::uvec2(ProjectToPixel(snapshot, glm::vec3(1.5f, 0.5f, -8.0f)));
				const glm::uvec2 c = glm::uvec2(ProjectToPixel(snapshot, glm::vec3(4.0f, 0.0f, -8.0f)));

				const MeshDrawItem caster = snapshot.Meshes[1];
				snapshot.Meshes.pop_back();
				const Image bare = RenderAs(fixture, statics, snapshot);
				REQUIRE(Green(bare, a.x, a.y) > 48);
				REQUIRE(Green(bare, b.x, b.y) > 48);
				REQUIRE(Green(bare, c.x, c.y) > 48);

				snapshot.Meshes.push_back(caster);
				++snapshot.FrameIndex;
				const Image coarser = RenderAs(fixture, statics, snapshot);
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.MeshLodDraws[0] == 2); // the wall and the caster
				CHECK(stats.MeshLodDraws[1] == 0);
				CHECK(Green(coarser, a.x, a.y) + 24 < Green(bare, a.x, a.y));
				CHECK(std::abs(Green(coarser, b.x, b.y) - Green(bare, b.x, b.y)) <= 2);
				CHECK(Green(coarser, c.x, c.y) == Green(bare, c.x, c.y));

				snapshot.Meshes[1].Distance.ShadowLodOffset = 0;
				++snapshot.FrameIndex;
				const Image own = RenderAs(fixture, statics, snapshot);
				CHECK(Green(own, a.x, a.y) + 24 < Green(bare, a.x, a.y));
				CHECK(Green(own, b.x, b.y) + 24 < Green(bare, b.x, b.y));
			}
		}

		// Decisions 3C and 3D: the caster of the case above at 5.1 m from the camera with a CullDistance of 4 m draws nothing
		// and casts nothing, so the image is the wall's alone, byte for byte, and the stats count it; at 10 m it draws again.
		TEST_CASE("MeshLods: a mesh beyond its CullDistance draws nothing and casts no shadow" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// W46.4: with its meshes dynamic, then static (MakeStaticTwin), the same outcome.
			for (const bool statics : { false, true })
			{
				CAPTURE(statics);
				Test::SceneRendererIntegrationFixture fixture(gpu);
				PublishAssets(fixture);
				fixture.Assets().Publish(TriangleLodQuad, MakeLodQuad({ 0, 1, 2 }, 0.3f));
				RenderSnapshot snapshot = MakeScene(fixture, TriangleLodQuad, glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, -5.0f)), WhiteMaterial);
				AddSun(snapshot, glm::vec3(1.0f, 0.0f, -1.0f));
				const MeshDrawItem caster = snapshot.Meshes[1];
				snapshot.Meshes.pop_back();
				const Image bare = RenderAs(fixture, statics, snapshot);

				snapshot.Meshes.push_back(caster);
				snapshot.Meshes[1].Distance.CullDistance = 4.0f;
				++snapshot.FrameIndex;
				const Image culled = RenderAs(fixture, statics, snapshot);
				CHECK(culled.Pixels == bare.Pixels);
				RenderStats stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.DistanceCulledMeshes == 1);
				CHECK(stats.MeshLodDraws[0] == 1);

				snapshot.Meshes[1].Distance.CullDistance = 10.0f;
				++snapshot.FrameIndex;
				const Image drawn = RenderAs(fixture, statics, snapshot);
				CHECK(drawn.Pixels != bare.Pixels);
				const glm::uvec2 shadow = glm::uvec2(ProjectToPixel(snapshot, glm::vec3(2.5f, -0.5f, -8.0f)));
				CHECK(Green(drawn, shadow.x, shadow.y) + 24 < Green(bare, shadow.x, shadow.y));
				stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.DistanceCulledMeshes == 0);
				CHECK(stats.MeshLodDraws[0] == 2);
			}
		}

		// Decision 3G's bit-identical positions: the forward pass tests against the prepass's depth without writing it, so a
		// pipeline that displaced a vertex by a different bit would leave pixels where neither the grid nor the red wall
		// behind it passes, showing the clear colour (black). A storm with gusts, turbulence, branch sway and flutter over a
		// dense grid leaves none, and does move it.
		TEST_CASE("Wind: the forward pass leaves no hole where the prepass covered a wind-swept mesh" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// W46.4: with its meshes dynamic, then static (MakeStaticTwin), the same outcome.
			for (const bool statics : { false, true })
			{
				CAPTURE(statics);
				Test::SceneRendererIntegrationFixture fixture(gpu);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeScene(fixture, SweptGrid, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.2f, -3.0f)), StormMaterial);
				snapshot.Meshes[0].Materials = { RedMaterial };
				snapshot.DebugView = RenderDebugView::Albedo;
				const Image still = RenderAs(fixture, statics, snapshot);
				for (const double time : { 12.345, 13.0, 40.5 })
				{
					CAPTURE(time);
					snapshot.Wind = MakeStorm();
					snapshot.Wind.Time = time;
					++snapshot.FrameIndex;
					const Image swept = RenderAs(fixture, statics, snapshot);
					size_t holes = 0;
					size_t moved = 0;
					for (uint32_t y = 0; y < Size; ++y)
					{
						for (uint32_t x = 0; x < Size; ++x)
						{
							const int red = Test::SceneRendererIntegrationFixture::Channel(swept, x, y, 0);
							holes += red == 0 && Green(swept, x, y) == 0 ? 1U : 0U;
							moved += Green(swept, x, y) != Green(still, x, y) ? 1U : 0U;
						}
					}
					CHECK(holes == 0);
					CHECK(moved > 40);
				}
			}
		}

		// Decision 3G: a Wind without strength or gusts sets DrawFlagWind on the leaf draws, which then add an offset of exactly
		// 0 in every pipeline (the shadow casters' included), so the image equals the one without a Wind byte for byte; the
		// storm changes it.
		TEST_CASE("Wind: a calm Wind renders the same bytes as no Wind" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// W46.4: with its meshes dynamic, then static (MakeStaticTwin), the same outcome.
			for (const bool statics : { false, true })
			{
				CAPTURE(statics);
				Test::SceneRendererIntegrationFixture fixture(gpu);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeScene(fixture, SweptGrid, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.2f, -4.0f)), StormMaterial);
				AddSun(snapshot, glm::vec3(1.0f, -0.3f, -1.0f));
				const Image none = RenderAs(fixture, statics, snapshot);
				snapshot.Wind = MakeCalm();
				++snapshot.FrameIndex;
				const Image calm = RenderAs(fixture, statics, snapshot);
				CHECK(calm.Pixels == none.Pixels);
				snapshot.Wind = MakeStorm();
				++snapshot.FrameIndex;
				const Image storm = RenderAs(fixture, statics, snapshot);
				CHECK(storm.Pixels != none.Pixels);
			}
		}

		// Decision 3G: a caster outside the camera's view whose shadow falls on the wall leans along the gale, so on a row
		// high on the grid its shadow's centroid moves along +X by the lean the displacement gives that height (Bend h^2,
		// projected onto the wall), and on its bottom row it stays; a material without wind casts the shadow it cast calm.
		TEST_CASE("Wind: a wind-swept caster's shadow sways with it" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// W46.4: with its meshes dynamic, then static (MakeStaticTwin), the same outcome.
			for (const bool statics : { false, true })
			{
				CAPTURE(statics);
				Test::SceneRendererIntegrationFixture fixture(gpu);
				PublishAssets(fixture);
				// The grid spans x in [-6.5, -5.5] and y in [-1, 1] at z = -5, outside the view; the light moves 2 m along X per metre
				// of depth, so its shadow on the wall spans x in [-0.5, 0.5].
				RenderSnapshot snapshot = MakeScene(fixture, PlainGrid, glm::translate(glm::mat4(1.0f), glm::vec3(-6.0f, -1.0f, -5.0f)), LeafMaterial);
				AddSun(snapshot, glm::vec3(2.0f, 0.0f, -1.0f));
				const MeshDrawItem caster = snapshot.Meshes[1];
				snapshot.Meshes.pop_back();
				const Image bare = RenderAs(fixture, statics, snapshot);
				snapshot.Meshes.push_back(caster);
				++snapshot.FrameIndex;
				const Image calm = RenderAs(fixture, statics, snapshot);
				snapshot.Wind = MakeGale();
				++snapshot.FrameIndex;
				const Image gale = RenderAs(fixture, statics, snapshot);

				// The wall's row of world height 0.3 (1.3 m above the grid's origin, the shadow falling level) and a row near the
				// grid's bottom; the world height of the high row's pixel centres.
				const glm::vec2 wallOrigin = ProjectToPixel(snapshot, glm::vec3(0.0f, 0.0f, -8.0f));
				const float pixelsPerMetre = ProjectToPixel(snapshot, glm::vec3(1.0f, 0.0f, -8.0f)).x - wallOrigin.x;
				const auto high = static_cast<uint32_t>(ProjectToPixel(snapshot, glm::vec3(0.0f, 0.3f, -8.0f)).y);
				const auto low = static_cast<uint32_t>(ProjectToPixel(snapshot, glm::vec3(0.0f, -0.8f, -8.0f)).y);
				const float worldHigh = (wallOrigin.y - (static_cast<float>(high) + 0.5f)) / pixelsPerMetre;
				const float predicted = PredictLean(worldHigh + 1.0f) * pixelsPerMetre;
				CAPTURE(predicted);
				REQUIRE(predicted > 2.0f);
				const std::optional<float> calmHigh = ShadowCentroid(calm, bare, high);
				const std::optional<float> galeHigh = ShadowCentroid(gale, bare, high);
				const std::optional<float> calmLow = ShadowCentroid(calm, bare, low);
				const std::optional<float> galeLow = ShadowCentroid(gale, bare, low);
				REQUIRE(calmHigh.has_value());
				REQUIRE(galeHigh.has_value());
				REQUIRE(calmLow.has_value());
				REQUIRE(galeLow.has_value());
				const float shift = *galeHigh - *calmHigh;
				CAPTURE(shift);
				CHECK(shift > 0.65f * predicted);
				CHECK(shift < 1.35f * predicted);
				CHECK(std::abs(*galeLow - *calmLow) < 0.35f);

				snapshot.Meshes[1].Materials = { StillMaterial };
				++snapshot.FrameIndex;
				CHECK(RenderAs(fixture, statics, snapshot).Pixels == calm.Pixels);
			}
		}

		// Decisions 3G and the motion vectors: after a calm render, a gale bends the grid; the prepass's previous positions take
		// the previous render's (calm) wind, so a pixel on the grid moves by its point's displacement there (Bend h^2 along +X,
		// Bend^2 h^3 / 2 down, projected) and its bottom row not at all. The next render under the same gale has no motion,
		// and a reset history (no previous camera) none either.
		TEST_CASE("Wind: the prepass's motion vectors follow the sway" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// W46.4: with its meshes dynamic, then static (MakeStaticTwin), the same outcome.
			for (const bool statics : { false, true })
			{
				CAPTURE(statics);
				Test::SceneRendererIntegrationFixture fixture(gpu);
				PublishAssets(fixture);
				GraphicsDevice& device = gpu.GetDevice();
				const glm::vec3 origin(0.0f, -1.2f, -4.0f);
				RenderSnapshot snapshot = MakeScene(fixture, PlainGrid, glm::translate(glm::mat4(1.0f), origin), LeafMaterial);
				const glm::mat4 viewProjection = snapshot.Camera.Projection * snapshot.Camera.View;
				snapshot.Wind = MakeCalm();
				SubmitAs(fixture, statics, snapshot);

				RenderSnapshot continued = snapshot;
				continued.FrameIndex += 1;
				continued.Camera.PreviousViewProjection = viewProjection;
				for (MeshDrawItem& item : continued.Meshes)
					item.PreviousWorld = item.World;
				continued.Wind = MakeGale();
				SubmitAs(fixture, statics, continued);
				const std::vector<glm::vec2> motion = ReadVelocity(device, fixture.Renderer());
				const float bend = std::round(LeafBend * 255.0f) / 255.0f;
				for (const float height : { 1.2f, 1.5f })
				{
					CAPTURE(height);
					const glm::vec3 rest = origin + glm::vec3(0.0f, height, 0.0f);
					const glm::vec3 swept = rest + glm::vec3(bend * height * height, -0.5f * bend * bend * height * height * height, 0.0f);
					const glm::vec2 now = ProjectToPixel(continued, swept);
					const glm::vec2 expected = now - ProjectToPixel(continued, rest);
					const glm::vec2 measured = motion[static_cast<size_t>(now.y) * Size + static_cast<size_t>(now.x)];
					CAPTURE(expected.x);
					CAPTURE(measured.x);
					REQUIRE(expected.x > 1.0f);
					CHECK(std::abs(measured.x - expected.x) <= 0.15f * expected.x + 0.15f);
					CHECK(std::abs(measured.y - expected.y) <= 0.15f * std::abs(expected.y) + 0.15f);
				}
				const glm::vec2 bottom = ProjectToPixel(continued, origin + glm::vec3(0.0f, 0.05f, 0.0f));
				CHECK(glm::length(motion[static_cast<size_t>(bottom.y) * Size + static_cast<size_t>(bottom.x)]) < 0.05f);

				RenderSnapshot steady = continued;
				steady.FrameIndex += 1;
				SubmitAs(fixture, statics, steady);
				const std::vector<glm::vec2> still = ReadVelocity(device, fixture.Renderer());
				CHECK(std::all_of(still.begin(), still.end(), [](const glm::vec2& value)
				{
					return value == glm::vec2(0.0f);
				}));

				RenderSnapshot reset = continued;
				reset.FrameIndex += 2;
				reset.Camera.PreviousViewProjection.reset();
				for (MeshDrawItem& item : reset.Meshes)
					item.PreviousWorld.reset();
				reset.Wind.Strength = 10.0f;
				SubmitAs(fixture, statics, reset);
				const std::vector<glm::vec2> none = ReadVelocity(device, fixture.Renderer());
				CHECK(std::all_of(none.begin(), none.end(), [](const glm::vec2& value)
				{
					return value == glm::vec2(0.0f);
				}));
			}
		}

		// Decision 3L: the levels' ranges and the wind streams live in GpuMesh, the shared zero wind stream in the cache and the
		// WindConstants in the renderer's state, all released and created again by a recovery: a scene with a cross-fading
		// level mesh, a grid with its own wind stream and one on the shared stream, under a storm with a shadow-casting sun,
		// renders the same bytes after a device loss.
		TEST_CASE("MeshLods: a scene with levels and wind renders the same bytes after a device loss" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// W46.4: with its meshes dynamic, then static (MakeStaticTwin), the same outcome.
			for (const bool statics : { false, true })
			{
				CAPTURE(statics);
				Test::SceneRendererIntegrationFixture fixture(gpu);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeScene(fixture, SweptGrid, glm::translate(glm::mat4(1.0f), glm::vec3(0.8f, -1.2f, -4.0f)), StormMaterial);
				MeshDrawItem plain = snapshot.Meshes[1];
				plain.World = glm::translate(glm::mat4(1.0f), glm::vec3(-0.8f, -1.2f, -4.0f));
				plain.Mesh = PlainGrid;
				plain.Materials = { LeafMaterial };
				plain.Entity = UUID(700);
				plain.PickId = 3;
				MeshDrawItem twin = snapshot.Meshes[1];
				twin.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, -3.0f));
				twin.Mesh = TwinLodQuad;
				twin.Materials = { WhiteMaterial };
				twin.Entity = UUID(41);
				twin.PickId = 0;
				twin.Distance = { .LodBias = 0.6124f, .FadeWidth = 0.5f };
				snapshot.Meshes.push_back(plain);
				snapshot.Meshes.push_back(twin);
				snapshot.Wind = MakeStorm();
				AddSun(snapshot, glm::vec3(1.0f, -0.3f, -1.0f));
				RenderSnapshot drawn = statics ? MakeStaticTwin(snapshot) : snapshot;
				GraphicsDevice& device = gpu.GetDevice();
				Test::DeviceLossTest test;
				test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
				test.Use = [&device, &fixture, &drawn]() -> Result<std::vector<std::byte>>
				{
					std::vector<std::byte> pixels;
					for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
					{
						drawn.FrameIndex += 1;
						ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
						list->open();
						const Status rendered = fixture.Renderer().Render(*list, drawn);
						list->close();
						fixture.Renderer().OnSubmitted(drawn.FrameIndex, device.ExecuteCommandList(*list));
						ENGINE_TRY(rendered);
						if (fixture.Renderer().GetRenderStats().LodCrossFades != 1)
							return MakeError(ErrorCode::InvalidState, "the level mesh did not cross-fade");
						nvrhi::ITexture* image = fixture.Renderer().GetFinalTexture();
						if (image == nullptr)
							return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
						Readback readback(device);
						ENGINE_TRY_ASSIGN(Image read, readback.ReadTexture(*image));
						pixels = std::move(read.Pixels);
					}
					return pixels;
				};
				// The view's targets reach their steady state in the pool first, as the other renderer loss cases do.
				for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
				{
					const Result<std::vector<std::byte>> steady = test.Use();
					REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
				}
				const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
				REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			}
		}

		// Decision 3D (the W46.2 review's finding 1): a mirror view chooses an item's levels by its own camera, never by its cull
		// camera, whose projection is cropped to the glass's rectangle. A small mirror (0.8 m at z = -3, a sixth of the view's
		// height) reflects a double-sided, emissive green TriangleLodQuad scaled 1.5 behind the main camera at z = 1.5: 7.5 m from
		// the mirror camera its screen size is about 0.49, below the level's switch (0.9), so the reflection shows its triangle
		// alone and the glass holds green and the red clear colour; the cull camera's projection, cropped to the glass's
		// rectangle widened by the blur margin (about 3.4 times as large with SsrMaxRoughness 0.05, whose chain has one level), would
		// choose level 0, the whole quad, filling the glass with green. LodBias 10 keeps level 0 in the mirror's own camera too.
		TEST_CASE("MeshLods: a mirror view chooses an item's levels by its own camera" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// W46.4: with its meshes dynamic, then static (MakeStaticTwin), the same outcome.
			for (const bool statics : { false, true })
			{
				CAPTURE(statics);
				Test::SceneRendererIntegrationFixture fixture(gpu);
				PublishAssets(fixture);
				constexpr AssetHandle MirrorMaterial{ 0x4646000000000016ULL };
				constexpr AssetHandle BeaconMaterial{ 0x4646000000000017ULL };
				auto mirror = MakeMaterial(glm::vec4(1.0f));
				mirror->Metallic = 1.0f;
				mirror->Roughness = 0.0f;
				fixture.Assets().Publish(MirrorMaterial, mirror);
				auto beacon = MakeMaterial(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
				beacon->Emissive = glm::vec3(0.0f, 2.0f, 0.0f);
				beacon->DoubleSided = true;
				fixture.Assets().Publish(BeaconMaterial, beacon);

				RenderSnapshot snapshot = fixture.Snapshot();
				snapshot.Camera.ClearColor = glm::vec3(0.6f, 0.0f, 0.0f);
				snapshot.Environment = RenderEnvironment{ .Environment = {}, .Intensity = 1.0f, .ShowSkybox = false, .FallbackColor = glm::vec3(0.0f) };
				snapshot.Post.SsrEnabled = false;
				// A one-level chain (a roughness this low blurs less than a texel), so the cull rectangle's blur margin is two texels
				// and the crop about 3.4 times the view; the polished glass (roughness 0) still reflects below it.
				snapshot.Post.SsrMaxRoughness = 0.05f;
				const AssetHandle quad = snapshot.Meshes.front().Mesh;
				const glm::mat4 glass = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.4f));
				snapshot.Meshes = {
					MeshDrawItem{ .World = glass, .Mesh = quad, .Materials = { MirrorMaterial }, .CastShadows = false, .Entity = UUID(900) },
					MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.5f)) * glm::scale(glm::mat4(1.0f), glm::vec3(1.5f)),
						.Mesh = TriangleLodQuad,
						.Materials = { BeaconMaterial },
						.Entity = UUID(40) },
				};
				snapshot.PlanarReflections = { PlanarReflectionItem{ .World = glass, .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .ResolutionScale = 1.0f, .Entity = UUID(900) } };

				// The glass's pixels two in from its edges (it spans 32 +- 7.4 pixels) that show the green beacon.
				const auto countGreen = [](const Image& image)
				{
					uint32_t green = 0;
					for (uint32_t y = 27; y <= 37; ++y)
					{
						for (uint32_t x = 27; x <= 37; ++x)
							green += Green(image, x, y) > 100 ? 1U : 0U;
					}
					return green;
				};
				constexpr uint32_t GlassPixels = 11 * 11;
				const Image triangle = RenderAs(fixture, statics, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);
				const uint32_t triangleGreen = countGreen(triangle);
				CAPTURE(triangleGreen);
				CHECK(triangleGreen > GlassPixels / 4);
				CHECK(triangleGreen < GlassPixels * 3 / 4);

				snapshot.Meshes[1].Distance.LodBias = 10.0f;
				++snapshot.FrameIndex;
				const Image whole = RenderAs(fixture, statics, snapshot);
				CHECK(countGreen(whole) == GlassPixels);
			}
		}

		// Decisions 3A and 3E (the W46.2 review's finding 7): a level with vertices of its own draws them, past level 0's in the
		// joined vertex buffer, with the shared default attribute stream covering them in its white: a quad whose level 1 is a
		// 4 x 1 m card of its own four vertices half a metre in front of it, wider than level 0, drawn at LodBias 0.01. The card
		// spans the view's width in white (the vertex colour of the default stream, which a stream sized to level 0 alone would
		// read as zeros, black), and the quad's part outside the card is not drawn.
		TEST_CASE("MeshLods: a level with vertices of its own draws them with the default streams" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			// W46.4: with its meshes dynamic, then static (MakeStaticTwin), the same outcome.
			for (const bool statics : { false, true })
			{
				CAPTURE(statics);
				Test::SceneRendererIntegrationFixture fixture(gpu);
				PublishAssets(fixture);
				constexpr AssetHandle CardLodQuad{ 0x4646000000000005ULL };
				Ref<MeshData> mesh = MakeLodQuad({ 0, 1, 2, 0, 2, 3 }, 0.5f);
				mesh->LodVertices = {
					{ .Position = { -2.0f, -0.5f, 0.5f }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 1 } },
					{ .Position = { 2.0f, -0.5f, 0.5f }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 1 } },
					{ .Position = { 2.0f, 0.5f, 0.5f }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 0 } },
					{ .Position = { -2.0f, 0.5f, 0.5f }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 0 } },
				};
				mesh->LodIndices = { 4, 5, 6, 4, 6, 7 };
				const Status valid = ValidateMeshData(*mesh);
				REQUIRE_MESSAGE(valid.has_value(), valid.error().ToString());
				fixture.Assets().Publish(CardLodQuad, mesh);

				RenderSnapshot snapshot = fixture.Snapshot();
				snapshot.DebugView = RenderDebugView::Albedo;
				snapshot.Meshes[0].Mesh = CardLodQuad;
				snapshot.Meshes[0].Materials = { WhiteMaterial };
				snapshot.Meshes[0].Distance.LodBias = 0.01f;
				const Image card = RenderAs(fixture, statics, snapshot);
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.MeshLodDraws[0] == 0);
				CHECK(stats.MeshLodDraws[1] == 1);
				// The card at z = -2.5 spans x in +-1.39 and y in +-0.35 of normalized device coordinates: every column of row 32.
				for (const uint32_t x : { 1U, 20U, 32U, 44U, 62U })
				{
					CAPTURE(x);
					CHECK(Green(card, x, 32) > 200);
				}
				// Level 0's quad would cover row 14 (y = 0.56); the card does not.
				CHECK(Green(card, 32, 14) < 8);

				// Level 0 at its own size draws the quad, not the card.
				snapshot.Meshes[0].Distance = {};
				++snapshot.FrameIndex;
				const Image quad = RenderAs(fixture, statics, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().MeshLodDraws[0] == 1);
				CHECK(Green(quad, 32, 14) > 200);
				CHECK(Green(quad, 2, 32) < 8);
			}
		}

		// The W46.4 review, finding 1: a static wind caster keys by its material, so two static casters of one mesh, one windy and
		// one calm, push their own materials' wind. Off the view, under a storm, their shadows on the wall equal their dynamic
		// twins', whichever material comes first in their cell, and differ from two calm casters' shadows.
		TEST_CASE("MeshLods: static casters of one mesh whose materials' winds differ each sway as their dynamic twins do" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishAssets(fixture);
			// The grids at x = -6 are outside the view; the light moves 2 m along X per metre of depth, so the shadows of the grids
			// at z = -5 and z = -3 fall on the wall around x = 0 and x = 4.
			RenderSnapshot snapshot = MakeScene(fixture, SweptGrid, glm::translate(glm::mat4(1.0f), glm::vec3(-6.0f, -1.0f, -5.0f)), StormMaterial);
			MeshDrawItem calm = snapshot.Meshes[1];
			calm.World = glm::translate(glm::mat4(1.0f), glm::vec3(-6.0f, -1.0f, -3.0f));
			calm.Materials = { StillMaterial };
			calm.Entity = UUID(41);
			calm.PickId = 3;
			snapshot.Meshes.push_back(calm);
			AddSun(snapshot, glm::vec3(2.0f, 0.0f, -1.0f));
			snapshot.Wind = MakeStorm();
			for (const bool windyFirst : { true, false })
			{
				CAPTURE(windyFirst);
				snapshot.Meshes[1].Materials = { windyFirst ? StormMaterial : StillMaterial };
				snapshot.Meshes[2].Materials = { windyFirst ? StillMaterial : StormMaterial };
				++snapshot.FrameIndex;
				const Image dynamic = fixture.Render(snapshot);
				++snapshot.FrameIndex;
				CHECK(RenderAs(fixture, true, snapshot).Pixels == dynamic.Pixels);

				RenderSnapshot both = snapshot;
				both.Meshes[1].Materials = { StillMaterial };
				both.Meshes[2].Materials = { StillMaterial };
				++both.FrameIndex;
				CHECK(fixture.Render(both).Pixels != dynamic.Pixels);
			}
		}

		// W46.4 (the lead's ledger amendment fc41d563): a static item's one record serves every submesh of it, so each submesh's
		// draw carries its own material's wind (DrawConstantsWind): the parted grid's stiff bark and stormy leaves sway in a storm
		// exactly as the dynamic item's two records make them, and differently from the same grid whose halves share the
		// leaves' wind.
		TEST_CASE("MeshLods: a static item whose submeshes' materials have different winds sways each as its dynamic twin does" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishAssets(fixture);
			RenderSnapshot snapshot = MakeScene(fixture, PartedGrid, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.2f, -3.0f)), BarkMaterial);
			snapshot.Meshes[1].Materials = { BarkMaterial, StormMaterial };
			snapshot.Meshes[0].Materials = { RedMaterial };
			snapshot.DebugView = RenderDebugView::Albedo;
			snapshot.Wind = MakeStorm();
			AddSun(snapshot, glm::vec3(1.0f, -0.3f, -1.0f));
			const Image dynamic = fixture.Render(snapshot);
			++snapshot.FrameIndex;
			const Image staticImage = RenderAs(fixture, true, snapshot);
			CHECK(staticImage.Pixels == dynamic.Pixels);

			snapshot.Meshes[1].Materials = { StormMaterial, StormMaterial };
			++snapshot.FrameIndex;
			const Image shared = fixture.Render(snapshot);
			CHECK(shared.Pixels != dynamic.Pixels);
			++snapshot.FrameIndex;
			CHECK(RenderAs(fixture, true, snapshot).Pixels == shared.Pixels);
		}

		// W46.4 (decision 3E): a static item's fade is its view's, in its InstanceIndices entry, which Common/Instances.slang's
		// LoadInstanceRecord returns to EvaluateSurface: a half-transparent Blend item at its switch size (two levels, t about
		// 0.5) blends each pixel once, as its dynamic twin does and as one unfaded draw does.
		TEST_CASE("MeshLods: a static Blend item inside a fade band fades exactly as its dynamic twin" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishAssets(fixture);
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.Meshes[0].Mesh = TwinLodQuad;
			snapshot.Meshes[0].Materials = { GlassMaterial };
			const Image whole = RenderAs(fixture, true, snapshot);
			CHECK(fixture.Renderer().GetRenderStats().LodCrossFades == 0);

			snapshot.Meshes[0].Distance = { .LodBias = 0.6124f, .FadeWidth = 0.5f };
			++snapshot.FrameIndex;
			const Image dynamic = fixture.Render(snapshot);
			++snapshot.FrameIndex;
			const Image staticImage = RenderAs(fixture, true, snapshot);
			const RenderStats stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.LodCrossFades == 1);
			CHECK(stats.MeshLodDraws[0] == 1);
			CHECK(stats.MeshLodDraws[1] == 1);
			CHECK(stats.StaticItems == 1);
			CHECK(staticImage.Pixels == dynamic.Pixels);
			CHECK(staticImage.Pixels == whole.Pixels);
		}

		// W46.4: a static record holds nothing from a material; its draws take their material's wind from the cell's keys,
		// which the renderer prepares again when a material's generation moves (M43's mirror check). Under one static set, a
		// leaf material reloaded with a stronger bend sways the grid as the dynamic twin does with the new material, and one
		// reloaded without wind holds it still.
		TEST_CASE("MeshLods: a material reload under an unchanged static cell changes its draws' wind" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishAssets(fixture);
			RenderSnapshot snapshot = MakeScene(fixture, PlainGrid, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.2f, -3.0f)), LeafMaterial);
			snapshot.Meshes[0].Materials = { RedMaterial };
			snapshot.DebugView = RenderDebugView::Albedo;
			snapshot.Wind = MakeGale();
			RenderSnapshot twin = MakeStaticTwin(snapshot);
			const Image before = fixture.Render(twin);

			fixture.Assets().Publish(LeafMaterial, MakeMaterial(glm::vec4(1.0f), { .Enabled = true, .Bend = 0.45f, .Branch = 0.0f, .Flutter = 0.0f, .Stiffness = 0.0f }));
			++twin.FrameIndex;
			const Image bent = fixture.Render(twin);
			CHECK(bent.Pixels != before.Pixels);
			++snapshot.FrameIndex;
			CHECK(fixture.Render(snapshot).Pixels == bent.Pixels);

			fixture.Assets().Publish(LeafMaterial, MakeMaterial(glm::vec4(1.0f), { .Enabled = false, .Bend = 0.45f, .Branch = 0.0f, .Flutter = 0.0f, .Stiffness = 0.0f }));
			++twin.FrameIndex;
			const Image still = fixture.Render(twin);
			snapshot.Wind = {};
			++snapshot.FrameIndex;
			CHECK(still.Pixels == fixture.Render(snapshot).Pixels);
		}

		// Acceptance (W46.4) and M43's byte identity on a scene with levels and wind: the scenery rendered static draws what it
		// draws dynamic, byte for byte, under a storm and without a wind, with a shadow-casting sun, spot light and point light:
		// the parted grid's two winds, an Opaque and a Blend item cross-fading, a coarser level, an item fading out before its
		// CullDistance, one beyond it in view, and two cells out of view, one wholly beyond its items' CullDistance and one with
		// an item beyond its CullDistance beside one without; the counters follow the dynamic path's.
		TEST_CASE("MeshLods: a scene with levels and wind renders the same bytes with its scenery static and without" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishAssets(fixture);
			RenderSnapshot snapshot = MakeScene(fixture, PartedGrid, glm::translate(glm::mat4(1.0f), glm::vec3(-1.6f, -1.2f, -4.0f)), BarkMaterial);
			snapshot.Meshes[1].Materials = { BarkMaterial, StormMaterial };
			MeshDrawItem item = snapshot.Meshes[1];
			const auto add = [&snapshot, &item](AssetHandle mesh, AssetHandle material, const glm::vec3& position, const MeshDrawDistance& distance)
			{
				item.Mesh = mesh;
				item.Materials = { material };
				item.World = glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
				item.Distance = distance;
				item.Entity = UUID(item.Entity.GetValue() + 1);
				snapshot.Meshes.push_back(item);
			};
			add(SweptGrid, StormMaterial, glm::vec3(1.4f, -1.2f, -4.5f), {});
			add(TwinLodQuad, WhiteMaterial, glm::vec3(-1.0f, 1.0f, -3.5f), { .LodBias = 0.6124f * 2.0f, .FadeWidth = 0.5f });
			add(TwinLodQuad, GlassMaterial, glm::vec3(0.6f, 0.9f, -3.0f), { .LodBias = 0.6124f * 2.0f, .FadeWidth = 0.5f });
			add(TriangleLodQuad, WhiteMaterial, glm::vec3(1.8f, 1.2f, -5.0f), { .LodBias = 0.5f });
			add(TriangleLodQuad, RedMaterial, glm::vec3(0.0f, -0.2f, -5.0f), { .CullDistance = 5.4f, .FadeWidth = 0.3f });
			add(TwinLodQuad, RedMaterial, glm::vec3(-2.6f, 0.4f, -6.0f), { .CullDistance = 4.0f });
			add(TwinLodQuad, RedMaterial, glm::vec3(80.0f, 0.0f, -6.0f), { .CullDistance = 10.0f });
			add(TwinLodQuad, RedMaterial, glm::vec3(-80.0f, 0.0f, -6.0f), { .CullDistance = 10.0f });
			add(TwinLodQuad, RedMaterial, glm::vec3(-81.0f, 0.0f, -6.0f), {});
			AddSun(snapshot, glm::vec3(1.0f, -0.6f, -1.0f));
			snapshot.Lights.push_back({ .Type = RenderLightType::Spot, .Intensity = 40.0f, .Position = glm::vec3(0.0f, 3.0f, -3.5f), .Direction = glm::vec3(0.0f, -1.0f, 0.0f), .Range = 12.0f, .CastShadows = true, .Entity = UUID(201) });
			snapshot.Lights.push_back({ .Type = RenderLightType::Point, .Intensity = 20.0f, .Position = glm::vec3(1.0f, 1.0f, -2.0f), .Range = 6.0f, .CastShadows = true, .Entity = UUID(202) });
			for (const bool windy : { true, false })
			{
				CAPTURE(windy);
				snapshot.Wind = windy ? MakeStorm() : RenderWind{};
				++snapshot.FrameIndex;
				const Image dynamic = fixture.Render(snapshot);
				const RenderStats dynamicStats = fixture.Renderer().GetRenderStats();
				++snapshot.FrameIndex;
				const Image staticImage = RenderAs(fixture, true, snapshot);
				const RenderStats staticStats = fixture.Renderer().GetRenderStats();
				CHECK(staticImage.Pixels == dynamic.Pixels);
				CHECK(staticStats.StaticItems == 11);
				CHECK(staticStats.DynamicItems == 0);
				CHECK(dynamicStats.LodCrossFades == 2);
				CHECK(dynamicStats.DistanceCulledMeshes == 3);
				CHECK(dynamicStats.MeshLodDraws[1] >= 3);
				CHECK(staticStats.MeshLodDraws == dynamicStats.MeshLodDraws);
				CHECK(staticStats.LodCrossFades == dynamicStats.LodCrossFades);
				CHECK(staticStats.DistanceCulledMeshes == dynamicStats.DistanceCulledMeshes);
				CHECK(staticStats.VisibleMeshes == dynamicStats.VisibleMeshes);
				CHECK(staticStats.CulledSubmeshes == dynamicStats.CulledSubmeshes);
				CHECK(staticStats.ShadowedSpotLights == 1);
				CHECK(staticStats.ShadowedPointLights == 1);
			}
		}
	}

}
