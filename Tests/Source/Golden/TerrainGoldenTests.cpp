#include "TestsPCH.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/TerrainLod.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>
#include <vector>

// The terrain golden (M26, C-TERR and C-TERR-EDIT; Docs/Decisions/0037-m26-contract.md decisions 22, 23 and 25): the
// golden scaffold Scaffold/GoldenTerrain.jsonl (IdSeed 2411) makes the flat 129 x 129 heightfield of asset.create {type:
// "Heightfield"} in Assets/Heightfields/GoldenTerrain/, sculpts it with terrain.edit (a hill of eight Raise dabs, two of five
// and four, and three Lower dabs cutting a pass beside the first), paints its splat map (rock on the hills, a sand path and
// snow on the highest top) and writes Assets/Scenes/GoldenTerrain/Terrain.scene: the 64 m terrain's four layers (a tiled
// checker under a grass tint, then rock, sand and snow tints) seen from beyond one corner along its diagonal with LodBias
// 0.25, so its chunks draw at five levels of detail, stitched where neighbouring levels differ.

namespace Engine {

	namespace {

		constexpr std::string_view TerrainGoldenScene = "Terrain";

		struct TerrainCapture
		{
			Image Picture{};
			std::vector<uint32_t> Lods{}; // each chunk's level at the view's camera, row-major, as the terrain passes pick them
		};

		// The scene rendered as every golden is, its splat map kept or dropped from the snapshot (a dropped one draws the first
		// layer alone).
		[[nodiscard]] TerrainCapture CaptureTerrain(Test::HeadlessGpuFixture& gpu, bool splatted)
		{
			TerrainCapture capture;
			uint32_t resolution = 0;
			const Test::GoldenSceneOptions options{ .PrepareScene = [&resolution](Scene& scene, AssetManager& assets) -> Status
			{
				// The heightfield's resolution (the chunk grid) and that the strokes raised it.
				std::vector<AssetHandle> heightfields;
				scene.ForEachCanonical([&heightfields](Entity entity)
				{
					if (const TerrainComponent* terrain = entity.TryGetComponent<TerrainComponent>())
						heightfields.push_back(terrain->Heightfield.GetHandle());
				});
				REQUIRE(heightfields.size() == 1);
				Result<AssetRef<Asset>> loaded = assets.Load(heightfields.front());
				REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
				const AssetRef<HeightfieldData> heightfield = AssetCast<HeightfieldData>(*loaded);
				REQUIRE(heightfield);
				resolution = heightfield->Resolution;
				CHECK(std::ranges::max(heightfield->Heights) > MaxHeightfieldSample / 2);
				return {};
			},
				.EditSnapshot = [splatted](RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Post.AntiAliasing == RenderAntiAliasing::Fxaa);
				if (!splatted)
				{
					for (TerrainItem& terrain : snapshot.Terrains)
						terrain.SplatMap = AssetHandle();
				}
			},
				.InspectSnapshot = [&capture, &resolution](const RenderSnapshot& snapshot)
			{
				REQUIRE(snapshot.HasCamera);
				REQUIRE(snapshot.Terrains.size() == 1);
				const TerrainItem& terrain = snapshot.Terrains.front();
				CHECK(terrain.Layers.size() == 4);
				CHECK(terrain.LodBias == 0.25f);
				REQUIRE(resolution == 129);
				const glm::vec3 cameraLocal = glm::vec3(glm::inverse(terrain.World) * glm::vec4(snapshot.Camera.Position, 1.0f));
				capture.Lods = SelectTerrainLods(ComputeTerrainLodLayout(resolution), { .Size = terrain.Size, .HeightScale = terrain.HeightScale },
					cameraLocal, terrain.LodBias);
			} };
			const Result<Image> image = Test::RenderGoldenScene(gpu, TerrainGoldenScene, options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->IsValid());
			CHECK(image->Width == 640);
			CHECK(image->Height == 360);
			capture.Picture = *image;
			return capture;
		}

		// How many pixels of `a` and `b` differ by more than 4/255 in a colour channel.
		[[nodiscard]] size_t CountChangedPixels(const Image& a, const Image& b)
		{
			REQUIRE(a.Pixels.size() == b.Pixels.size());
			size_t changed = 0;
			for (size_t pixel = 0; pixel < a.Pixels.size() / 4; ++pixel)
			{
				int delta = 0;
				for (size_t channel = 0; channel < 3; ++channel)
				{
					const int left = std::to_integer<int>(a.Pixels[pixel * 4 + channel]);
					const int right = std::to_integer<int>(b.Pixels[pixel * 4 + channel]);
					delta = std::max(delta, left > right ? left - right : right - left);
				}
				changed += delta > 4 ? 1U : 0U;
			}
			return changed;
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		// The view meets chunks at five levels of detail (0 to 4: 4 x 4 chunks of 32 cells under LodBias 0.25); the painted
		// splat map changes the image clearly from the first layer alone; a second capture repeats the first exactly.
		TEST_CASE("Golden: Terrain")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const TerrainCapture splatted = CaptureTerrain(gpu, true);
			REQUIRE(splatted.Lods.size() == 16);
			const std::set<uint32_t> levels(splatted.Lods.begin(), splatted.Lods.end());
			CHECK(levels == std::set<uint32_t>{ 0, 1, 2, 3, 4 });
			const size_t changed = CountChangedPixels(splatted.Picture, CaptureTerrain(gpu, false).Picture);
			CAPTURE(changed);
			CHECK(changed > 5000);
			CHECK(CaptureTerrain(gpu, true).Picture.Pixels == splatted.Picture.Pixels);
			ENGINE_CHECK_GOLDEN(TerrainGoldenScene, splatted.Picture, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
