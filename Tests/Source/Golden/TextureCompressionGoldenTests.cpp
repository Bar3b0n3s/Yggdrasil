#include "TestsPCH.h"

#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestData.h"
#include "Support/Utf8Path.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <functional>
#include <string>
#include <string_view>

// The textured golden (M23 W23.2; Docs/Decisions/0034-m23-contract.md decisions 13 and 17): the golden scaffold
// Scaffold/GoldenTextured.jsonl (IdSeed 2103) writes Assets/Scenes/GoldenTextured/TextureCompression.scene, four textures
// each imported twice, with Compression Auto (block-compressed by usage) and None (RGBA8), on quads side by side before an
// orthographic camera: a colour photo (BC7Srgb), an alpha-masked RGBA texture (BC7Srgb), a normal map (BC5, its z
// reconstructed) and a metallic-roughness map (Linear, BC7Unorm). It pins how compressed textures look next to their
// uncompressed twins, so a change of the encoder, its settings or the decode shows in one reviewed image.

namespace Engine {

	namespace {

		constexpr std::string_view TextureCompressionGoldenScene = "TextureCompression";

		// One of the scene's four pairs: the materials <Name>Auto and <Name>None in Assets/Materials/GoldenTextured.
		struct TexturePair
		{
			std::string_view Name{};
			// Whether every pixel of the Auto quad stays within 24/255 of its render with the None texture: true for colour
			// and linear data (measured at most 7/255 and 1/255). The alpha-masked quad's cutoff edge moves with BC7's alpha,
			// and the normal map's horizon texels change further, since the z that BC5's two channels reconstruct is most
			// sensitive to x and y there (measured at most 145/255 and 113/255).
			bool Bounded = false;
		};

		constexpr std::array<TexturePair, 4> TexturePairs = { {
			{ .Name = "Color", .Bounded = true },
			{ .Name = "Mask", .Bounded = false },
			{ .Name = "Normal", .Bounded = false },
			{ .Name = "Linear", .Bounded = true },
		} };

		// The handle of the material `name` of the golden scaffold, from its committed .meta.
		[[nodiscard]] AssetHandle ReadGoldenMaterial(std::string_view name)
		{
			const std::filesystem::path meta = Test::GetRepositoryRoot() / "Projects" / "FeatureTest" / "Assets" / "Materials" / "GoldenTextured"
				/ Test::PathFromUtf8(std::format("{}.material.meta", name));
			const Result<std::string> text = FileSystem::ReadText(meta);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const Result<AssetMetadata> metadata = ParseAssetMetadata(*text);
			REQUIRE_MESSAGE(metadata.has_value(), metadata.error().ToString());
			REQUIRE(metadata->Handle.IsValid());
			return metadata->Handle;
		}

		// The scene rendered as every golden is, then `edit`.
		[[nodiscard]] Image CaptureTextureCompression(Test::HeadlessGpuFixture& gpu, const std::function<void(RenderSnapshot&)>& edit = {},
			const std::function<void(const RenderSnapshot&)>& inspect = {})
		{
			const Test::GoldenSceneOptions options{ .EditSnapshot = [&edit](RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Post.FxaaEnabled);
				if (edit)
					edit(snapshot);
			},
				.InspectSnapshot = inspect };
			const Result<Image> image = Test::RenderGoldenScene(gpu, TextureCompressionGoldenScene, options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			REQUIRE(image->IsValid());
			CHECK(image->Width == 640);
			CHECK(image->Height == 360);
			return *image;
		}

		// How two renders of one size differ: the pixels whose largest channel difference exceeds 0 and 24, and that
		// largest difference.
		struct ImageDifference
		{
			size_t Changed = 0;
			size_t Large = 0;
			int Largest = 0;
		};

		[[nodiscard]] ImageDifference CompareRenders(const Image& first, const Image& second)
		{
			REQUIRE(first.Pixels.size() == second.Pixels.size());
			ImageDifference difference;
			for (size_t pixel = 0; pixel < first.Pixels.size() / 4; ++pixel)
			{
				int delta = 0;
				for (size_t channel = 0; channel < 3; ++channel)
				{
					const int a = std::to_integer<int>(first.Pixels[pixel * 4 + channel]);
					const int b = std::to_integer<int>(second.Pixels[pixel * 4 + channel]);
					delta = std::max(delta, std::abs(a - b));
				}
				difference.Changed += delta > 0 ? 1U : 0U;
				difference.Large += delta > 24 ? 1U : 0U;
				difference.Largest = std::max(difference.Largest, delta);
			}
			return difference;
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		// Each pair's Auto quad samples its block-compressed texels: drawing it with its None twin's material changes some of
		// its pixels, those of colour and linear data only within the golden comparison's per-pixel bound, and no more
		// pixels than the one quad covers (the other quads keep their materials); a second capture repeats the first
		// exactly.
		TEST_CASE("Golden: TextureCompression")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			std::array<AssetHandle, TexturePairs.size()> compressed{};
			std::array<AssetHandle, TexturePairs.size()> uncompressed{};
			for (size_t pair = 0; pair < TexturePairs.size(); ++pair)
			{
				compressed[pair] = ReadGoldenMaterial(std::format("{}Auto", TexturePairs[pair].Name));
				uncompressed[pair] = ReadGoldenMaterial(std::format("{}None", TexturePairs[pair].Name));
			}

			const Image golden = CaptureTextureCompression(gpu, {}, [&compressed, &uncompressed](const RenderSnapshot& snapshot)
			{
				REQUIRE(snapshot.Meshes.size() == 2 * TexturePairs.size());
				for (size_t pair = 0; pair < TexturePairs.size(); ++pair)
				{
					CAPTURE(TexturePairs[pair].Name);
					const auto draws = [&snapshot](AssetHandle material)
					{
						return std::ranges::count_if(snapshot.Meshes, [material](const MeshDrawItem& mesh)
						{
							return mesh.Materials.size() == 1 && mesh.Materials.front() == material;
						});
					};
					CHECK(draws(compressed[pair]) == 1);
					CHECK(draws(uncompressed[pair]) == 1);
				}
			});

			for (size_t pair = 0; pair < TexturePairs.size(); ++pair)
			{
				CAPTURE(TexturePairs[pair].Name);
				const Image twin = CaptureTextureCompression(gpu, [&compressed, &uncompressed, pair](RenderSnapshot& snapshot)
				{
					for (MeshDrawItem& mesh : snapshot.Meshes)
					{
						if (mesh.Materials.size() == 1 && mesh.Materials.front() == compressed[pair])
							mesh.Materials.front() = uncompressed[pair];
					}
				});
				const ImageDifference difference = CompareRenders(golden, twin);
				CAPTURE(difference.Changed);
				CAPTURE(difference.Large);
				CAPTURE(difference.Largest);
				MESSAGE(std::format("{}: {} pixels changed, {} by more than 24, at most {}", TexturePairs[pair].Name, difference.Changed, difference.Large,
					difference.Largest));
				CHECK(difference.Changed > 0);
				// One quad is 130 x 130 pixels; FXAA may reach one pixel beyond its edges.
				CHECK(difference.Changed <= 132U * 132U);
				if (TexturePairs[pair].Bounded)
					CHECK(difference.Large == 0);
			}

			CHECK(CaptureTextureCompression(gpu).Pixels == golden.Pixels);
			ENGINE_CHECK_GOLDEN(TextureCompressionGoldenScene, golden, gpu.GetDevice().GetInfo().DeviceClass);
		}
	}

}
