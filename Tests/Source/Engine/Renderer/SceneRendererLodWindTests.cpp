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
#include <optional>
#include <vector>

// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3D, 3E, 3G, 3K and 3L): a mesh's levels of
// detail and the scene's wind drawn by the scene renderer. Each view draws the level its camera's screen size selects, two
// levels inside a fade band with complementary dither, nothing beyond the item's CullDistance; casters draw a coarser level;
// a material with wind sways by the snapshot's Wind identically in every pipeline, and its motion vectors follow the sway.

namespace Engine {

	namespace {

		constexpr AssetHandle TriangleLodQuad{ 0x4646000000000001ULL }; // level 1: the quad's lower-right triangle
		constexpr AssetHandle TwinLodQuad{ 0x4646000000000002ULL };     // level 1: the whole quad again
		constexpr AssetHandle PlainGrid{ 0x4646000000000003ULL };       // a grid without a wind stream (bends by height)
		constexpr AssetHandle SweptGrid{ 0x4646000000000004ULL };       // a grid with a _WIND stream
		constexpr AssetHandle WhiteMaterial{ 0x4646000000000010ULL };
		constexpr AssetHandle RedMaterial{ 0x4646000000000011ULL };
		constexpr AssetHandle GlassMaterial{ 0x4646000000000012ULL }; // Blend, half transparent, emissive white
		constexpr AssetHandle LeafMaterial{ 0x4646000000000013ULL };  // wind: Bend 0.25, no branch or flutter
		constexpr AssetHandle StormMaterial{ 0x4646000000000014ULL }; // wind: every term
		constexpr AssetHandle StillMaterial{ 0x4646000000000015ULL }; // LeafMaterial's look, wind disabled
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
			assets.Publish(WhiteMaterial, MakeMaterial(glm::vec4(1.0f)));
			assets.Publish(RedMaterial, MakeMaterial(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));
			auto glass = MakeMaterial(glm::vec4(0.0f, 0.0f, 0.0f, 0.5f));
			glass->AlphaMode = AlphaMode::Blend;
			glass->Emissive = glm::vec3(1.0f);
			assets.Publish(GlassMaterial, glass);
			assets.Publish(LeafMaterial, MakeMaterial(glm::vec4(1.0f), { .Enabled = true, .Bend = LeafBend, .Branch = 0.0f, .Flutter = 0.0f, .Stiffness = 0.0f }));
			assets.Publish(StormMaterial, MakeMaterial(glm::vec4(1.0f), { .Enabled = true, .Bend = 0.25f, .Branch = 0.4f, .Flutter = 0.2f, .Stiffness = 0.1f }));
			assets.Publish(StillMaterial, MakeMaterial(glm::vec4(1.0f), { .Enabled = false, .Bend = LeafBend, .Branch = 0.0f, .Flutter = 0.0f, .Stiffness = 0.0f }));
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
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishAssets(fixture);
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.DebugView = RenderDebugView::Albedo;
			snapshot.Meshes[0].Mesh = TriangleLodQuad;
			snapshot.Meshes[0].Materials = { WhiteMaterial };

			const Image coarse = fixture.Render(snapshot);
			CHECK(Green(coarse, 23, 23) < 8);
			CHECK(Green(coarse, 41, 41) > 200);
			RenderStats stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.MeshLodDraws[0] == 0);
			CHECK(stats.MeshLodDraws[1] == 1);
			CHECK(stats.LodCrossFades == 0);

			// A larger switch size keeps level 0 ... (the mesh's own level 1 now switches at 0.5).
			fixture.Assets().Publish(TriangleLodQuad, MakeLodQuad({ 0, 1, 2 }, 0.5f));
			++snapshot.FrameIndex;
			const Image fine = fixture.Render(snapshot);
			CHECK(Green(fine, 23, 23) > 200);
			CHECK(Green(fine, 41, 41) > 200);
			stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.MeshLodDraws[0] == 1);
			CHECK(stats.MeshLodDraws[1] == 0);

			// ... until LodBias halves the screen size the switch compares.
			snapshot.Meshes[0].Distance.LodBias = 0.5f;
			++snapshot.FrameIndex;
			const Image biased = fixture.Render(snapshot);
			CHECK(biased.Pixels == coarse.Pixels);
			CHECK(fixture.Renderer().GetRenderStats().MeshLodDraws[1] == 1);
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
				const Image whole = fixture.Render(snapshot);
				CHECK(fixture.Renderer().GetRenderStats().LodCrossFades == 0);
				CHECK(Green(whole, 32, 32) > 32);

				snapshot.Meshes[0].Distance = banded;
				++snapshot.FrameIndex;
				const Image faded = fixture.Render(snapshot);
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

		// Decisions 3D and 3E: the camera draws the quad caster at 5.1 m (screen size 0.48) at level 0, and its shadow comes
		// from level 1 (ShadowLodOffset 1 by default), its lower-right triangle: the wall at A, in the triangle's shadow, is
		// dark, and at B, in the shadow only level 0 would cast, lit. With ShadowLodOffset 0 the whole quad casts.
		TEST_CASE("MeshLods: a caster's shadow comes from its coarser level" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
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
			const Image bare = fixture.Render(snapshot);
			REQUIRE(Green(bare, a.x, a.y) > 48);
			REQUIRE(Green(bare, b.x, b.y) > 48);
			REQUIRE(Green(bare, c.x, c.y) > 48);

			snapshot.Meshes.push_back(caster);
			++snapshot.FrameIndex;
			const Image coarser = fixture.Render(snapshot);
			const RenderStats stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.MeshLodDraws[0] == 2); // the wall and the caster
			CHECK(stats.MeshLodDraws[1] == 0);
			CHECK(Green(coarser, a.x, a.y) + 24 < Green(bare, a.x, a.y));
			CHECK(std::abs(Green(coarser, b.x, b.y) - Green(bare, b.x, b.y)) <= 2);
			CHECK(Green(coarser, c.x, c.y) == Green(bare, c.x, c.y));

			snapshot.Meshes[1].Distance.ShadowLodOffset = 0;
			++snapshot.FrameIndex;
			const Image own = fixture.Render(snapshot);
			CHECK(Green(own, a.x, a.y) + 24 < Green(bare, a.x, a.y));
			CHECK(Green(own, b.x, b.y) + 24 < Green(bare, b.x, b.y));
		}

		// Decisions 3C and 3D: the caster of the case above at 5.1 m from the camera with a CullDistance of 4 m draws nothing
		// and casts nothing, so the image is the wall's alone, byte for byte, and the stats count it; at 10 m it draws again.
		TEST_CASE("MeshLods: a mesh beyond its CullDistance draws nothing and casts no shadow" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishAssets(fixture);
			fixture.Assets().Publish(TriangleLodQuad, MakeLodQuad({ 0, 1, 2 }, 0.3f));
			RenderSnapshot snapshot = MakeScene(fixture, TriangleLodQuad, glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, -5.0f)), WhiteMaterial);
			AddSun(snapshot, glm::vec3(1.0f, 0.0f, -1.0f));
			const MeshDrawItem caster = snapshot.Meshes[1];
			snapshot.Meshes.pop_back();
			const Image bare = fixture.Render(snapshot);

			snapshot.Meshes.push_back(caster);
			snapshot.Meshes[1].Distance.CullDistance = 4.0f;
			++snapshot.FrameIndex;
			const Image culled = fixture.Render(snapshot);
			CHECK(culled.Pixels == bare.Pixels);
			RenderStats stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.DistanceCulledMeshes == 1);
			CHECK(stats.MeshLodDraws[0] == 1);

			snapshot.Meshes[1].Distance.CullDistance = 10.0f;
			++snapshot.FrameIndex;
			const Image drawn = fixture.Render(snapshot);
			CHECK(drawn.Pixels != bare.Pixels);
			const glm::uvec2 shadow = glm::uvec2(ProjectToPixel(snapshot, glm::vec3(2.5f, -0.5f, -8.0f)));
			CHECK(Green(drawn, shadow.x, shadow.y) + 24 < Green(bare, shadow.x, shadow.y));
			stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.DistanceCulledMeshes == 0);
			CHECK(stats.MeshLodDraws[0] == 2);
		}

		// Decision 3G's bit-identical positions: the forward pass tests against the prepass's depth without writing it, so a
		// pipeline that displaced a vertex by a different bit would leave pixels where neither the grid nor the red wall
		// behind it passes, showing the clear colour (black). A storm with gusts, turbulence, branch sway and flutter over a
		// dense grid leaves none, and does move it.
		TEST_CASE("Wind: the forward pass leaves no hole where the prepass covered a wind-swept mesh" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishAssets(fixture);
			RenderSnapshot snapshot = MakeScene(fixture, SweptGrid, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.2f, -3.0f)), StormMaterial);
			snapshot.Meshes[0].Materials = { RedMaterial };
			snapshot.DebugView = RenderDebugView::Albedo;
			const Image still = fixture.Render(snapshot);
			for (const double time : { 12.345, 13.0, 40.5 })
			{
				CAPTURE(time);
				snapshot.Wind = MakeStorm();
				snapshot.Wind.Time = time;
				++snapshot.FrameIndex;
				const Image swept = fixture.Render(snapshot);
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

		// Decision 3G: a Wind without strength or gusts sets DrawFlagWind on the leaf draws, which then add an offset of exactly
		// 0 in every pipeline (the shadow casters' included), so the image equals the one without a Wind byte for byte; the
		// storm changes it.
		TEST_CASE("Wind: a calm Wind renders the same bytes as no Wind" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishAssets(fixture);
			RenderSnapshot snapshot = MakeScene(fixture, SweptGrid, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.2f, -4.0f)), StormMaterial);
			AddSun(snapshot, glm::vec3(1.0f, -0.3f, -1.0f));
			const Image none = fixture.Render(snapshot);
			snapshot.Wind = MakeCalm();
			++snapshot.FrameIndex;
			const Image calm = fixture.Render(snapshot);
			CHECK(calm.Pixels == none.Pixels);
			snapshot.Wind = MakeStorm();
			++snapshot.FrameIndex;
			const Image storm = fixture.Render(snapshot);
			CHECK(storm.Pixels != none.Pixels);
		}

		// Decision 3G: a caster outside the camera's view whose shadow falls on the wall leans along the gale, so on a row
		// high on the grid its shadow's centroid moves along +X by the lean the displacement gives that height (Bend h^2,
		// projected onto the wall), and on its bottom row it stays; a material without wind casts the shadow it cast calm.
		TEST_CASE("Wind: a wind-swept caster's shadow sways with it" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishAssets(fixture);
			// The grid spans x in [-6.5, -5.5] and y in [-1, 1] at z = -5, outside the view; the light moves 2 m along X per metre
			// of depth, so its shadow on the wall spans x in [-0.5, 0.5].
			RenderSnapshot snapshot = MakeScene(fixture, PlainGrid, glm::translate(glm::mat4(1.0f), glm::vec3(-6.0f, -1.0f, -5.0f)), LeafMaterial);
			AddSun(snapshot, glm::vec3(2.0f, 0.0f, -1.0f));
			const MeshDrawItem caster = snapshot.Meshes[1];
			snapshot.Meshes.pop_back();
			const Image bare = fixture.Render(snapshot);
			snapshot.Meshes.push_back(caster);
			++snapshot.FrameIndex;
			const Image calm = fixture.Render(snapshot);
			snapshot.Wind = MakeGale();
			++snapshot.FrameIndex;
			const Image gale = fixture.Render(snapshot);

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
			CHECK(fixture.Render(snapshot).Pixels == calm.Pixels);
		}

		// Decisions 3G and the motion vectors: after a calm render, a gale bends the grid; the prepass's previous positions take
		// the previous render's (calm) wind, so a pixel on the grid moves by its point's displacement there (Bend h^2 along +X,
		// Bend^2 h^3 / 2 down, projected) and its bottom row not at all. The next render under the same gale has no motion,
		// and a reset history (no previous camera) none either.
		TEST_CASE("Wind: the prepass's motion vectors follow the sway" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishAssets(fixture);
			GraphicsDevice& device = gpu.GetDevice();
			const glm::vec3 origin(0.0f, -1.2f, -4.0f);
			RenderSnapshot snapshot = MakeScene(fixture, PlainGrid, glm::translate(glm::mat4(1.0f), origin), LeafMaterial);
			const glm::mat4 viewProjection = snapshot.Camera.Projection * snapshot.Camera.View;
			snapshot.Wind = MakeCalm();
			fixture.Submit(snapshot);

			RenderSnapshot continued = snapshot;
			continued.FrameIndex += 1;
			continued.Camera.PreviousViewProjection = viewProjection;
			for (MeshDrawItem& item : continued.Meshes)
				item.PreviousWorld = item.World;
			continued.Wind = MakeGale();
			fixture.Submit(continued);
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
			fixture.Submit(steady);
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
			fixture.Submit(reset);
			const std::vector<glm::vec2> none = ReadVelocity(device, fixture.Renderer());
			CHECK(std::all_of(none.begin(), none.end(), [](const glm::vec2& value)
			{
				return value == glm::vec2(0.0f);
			}));
		}

		// Decision 3L: the levels' ranges and the wind streams live in GpuMesh, the shared zero wind stream in the cache and the
		// WindConstants in the renderer's state, all released and created again by a recovery: a scene with a cross-fading
		// level mesh, a grid with its own wind stream and one on the shared stream, under a storm with a shadow-casting sun,
		// renders the same bytes after a device loss.
		TEST_CASE("MeshLods: a scene with levels and wind renders the same bytes after a device loss" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
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
			GraphicsDevice& device = gpu.GetDevice();
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &fixture, &snapshot]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					snapshot.FrameIndex += 1;
					ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
					list->open();
					const Status rendered = fixture.Renderer().Render(*list, snapshot);
					list->close();
					fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
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

}
