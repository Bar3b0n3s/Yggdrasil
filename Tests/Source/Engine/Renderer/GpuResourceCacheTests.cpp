#include "TestsPCH.h"

#include "Engine/Renderer/GpuResourceCache.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/BuiltinTextures.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/Asset/LutData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureCompression.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/RingBufferSink.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Shared/MaterialConstants.h"
#include "Support/AssetTestFixture.h"
#include "Support/BufferCreationFails.h"
#include "Support/BufferReadback.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/RenderReference.h"
#include "Support/TestOptions.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Engine {

	// A 2x1 RGBA8 texture whose two texels are `value`.
	static AssetRef<Asset> MakeTexture(uint8_t value)
	{
		TextureData texture;
		texture.Format = TextureFormat::RGBA8Unorm;
		texture.Width = 2;
		texture.Height = 1;
		texture.Mips.push_back({ .Width = 2, .Height = 1, .Offset = 0, .Size = 8 });
		texture.Pixels.assign(8, std::byte{ value });
		return CreateRef<TextureData>(std::move(texture));
	}

	// A black environment with a 4² skybox of three mips, the full-size specular cube and IrradianceSH9[0] = (1, 2, 3).
	static AssetRef<Asset> MakeEnvironment()
	{
		EnvironmentData environment;
		environment.Skybox = { .FaceSize = 4, .MipCount = 3, .Texels = Buffer(ComputeCubeMapByteSize(4, 3), std::byte{ 0 }) };
		environment.Specular = { .FaceSize = EnvironmentData::SpecularFaceSize,
			.MipCount = EnvironmentData::SpecularMipCount,
			.Texels = Buffer(ComputeCubeMapByteSize(EnvironmentData::SpecularFaceSize, EnvironmentData::SpecularMipCount), std::byte{ 0 }) };
		environment.IrradianceSH9[0] = glm::vec3(1.0f, 2.0f, 3.0f);
		return CreateRef<EnvironmentData>(std::move(environment));
	}

	// A font with one glyph, 'A', filling a 4x4 atlas that is inside the outline everywhere.
	static AssetRef<Asset> MakeFont()
	{
		FontData font;
		font.Ascent = 0.8f;
		font.Descent = -0.2f;
		font.AtlasWidth = 4;
		font.AtlasHeight = 4;
		font.AtlasPixels.assign(16, std::byte{ 255 });
		font.Glyphs.push_back({ .Codepoint = 'A',
			.Advance = 0.6f,
			.PlaneMin = glm::vec2(0.0f, 0.0f),
			.PlaneMax = glm::vec2(0.5f, 0.7f),
			.AtlasMin = glm::vec2(0.0f),
			.AtlasMax = glm::vec2(1.0f) });
		return CreateRef<FontData>(std::move(font));
	}

	TEST_SUITE("Renderer")
	{
		// M23 acceptance (R-TEXC-BC, decision 15, stream X): a BC7 texture uploads as BC7 on a device with the
		// textureCompressionBC feature, and on a device without it through the decode fallback as RGBA8, whose texels are
		// DecompressTextureBlocks' (Asset/TextureCompression.h), never the placeholder. BC1 (sRGB) and BC4, which Auto never
		// chooses but the payload, the upload and the fallback handle (decision 13), take their own formats: BC1Srgb's mirror
		// is SRGBA8 and BC4's is R8 holding the decoded red channel alone, which samples as (r, 0, 0, 1) as BC4 does. The
		// TextureFormat values are decision 13's.
		TEST_CASE("GpuResourceCache: a device without BC support uploads a BC texture through the decode fallback"
			* doctest::test_suite(Test::GpuSuite))
		{
			Buffer rgba;
			for (uint32_t texel = 0; texel < 8 * 8; ++texel)
			{
				for (const uint32_t value : { texel * 4, 255 - texel * 2, (texel * 37) % 256, 255U })
					rgba.push_back(static_cast<std::byte>(value & 0xFFU));
			}
			struct BlockCase
			{
				TextureFormat Format = TextureFormat::RGBA8Unorm;
				TextureBlockCodec Codec = TextureBlockCodec::BC7;
				nvrhi::Format Native = nvrhi::Format::UNKNOWN;   // the mirror's format with BC support
				nvrhi::Format Fallback = nvrhi::Format::UNKNOWN; // and through the decode fallback
			};
			const std::array<BlockCase, 3> cases = { {
				{ .Format = TextureFormat::BC7Unorm, .Codec = TextureBlockCodec::BC7, .Native = nvrhi::Format::BC7_UNORM, .Fallback = nvrhi::Format::RGBA8_UNORM },
				{ .Format = TextureFormat::BC1Srgb, .Codec = TextureBlockCodec::BC1, .Native = nvrhi::Format::BC1_UNORM_SRGB, .Fallback = nvrhi::Format::SRGBA8_UNORM },
				{ .Format = TextureFormat::BC4Unorm, .Codec = TextureBlockCodec::BC4, .Native = nvrhi::Format::BC4_UNORM, .Fallback = nvrhi::Format::R8_UNORM },
			} };
			std::vector<AssetRef<TextureData>> textures;
			std::vector<Buffer> fallbackTexels;
			for (const BlockCase& block : cases)
			{
				const Result<Buffer> blocks = CompressTextureBlocks(block.Codec, 8, 8, rgba);
				REQUIRE(blocks.has_value());
				const Result<Buffer> decoded = DecompressTextureBlocks(block.Codec, 8, 8, *blocks);
				REQUIRE(decoded.has_value());
				Buffer expected = *decoded;
				if (block.Codec == TextureBlockCodec::BC4)
				{
					expected.resize(decoded->size() / 4);
					for (size_t texel = 0; texel < expected.size(); ++texel)
						expected[texel] = (*decoded)[texel * 4];
				}
				fallbackTexels.push_back(std::move(expected));
				auto texture = CreateRef<TextureData>();
				texture->Format = block.Format;
				texture->Width = 8;
				texture->Height = 8;
				texture->Mips = { { .Width = 8, .Height = 8, .Offset = 0, .Size = blocks->size() } };
				texture->Pixels = *blocks;
				REQUIRE_MESSAGE(ValidateTextureData(*texture).has_value(), ValidateTextureData(*texture).error().ToString());
				textures.push_back(std::move(texture));
			}

			for (const bool withBc : { true, false })
			{
				CAPTURE(withBc);
				Test::HeadlessGpuFixture gpu({ .DisableTextureCompressionBC = !withBc });
				ENGINE_REQUIRE_GPU(gpu);
				CHECK(gpu.GetDevice().GetInfo().TextureCompressionBC == withBc);
				Test::InMemoryAssetManager assets;
				{
					// The cache names its path once, at its first block-compressed texture, however many follow.
					Test::ExpectLog path(LogLevel::Info, withBc ? "upload as their BC blocks" : "the decode fallback");
					GpuResourceCache cache(gpu.GetDevice(), assets);
					Readback readback(gpu.GetDevice());
					for (size_t index = 0; index < cases.size(); ++index)
					{
						const BlockCase& block = cases[index];
						CAPTURE(TextureFormatToString(block.Format));
						const UUID handle(0x7E57BC7000000001ULL + index);
						assets.Publish(handle, textures[index]);
						const GpuTexture& mirror = cache.GetTexture(handle);
						REQUIRE(mirror.Texture != nullptr);
						CHECK_FALSE(mirror.IsPlaceholder);
						CHECK(mirror.Format == block.Format);
						CHECK(mirror.Texture->getDesc().format == (withBc ? block.Native : block.Fallback));
						if (!withBc)
						{
							const Result<Image> image = readback.ReadTexture(*mirror.Texture);
							REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
							CHECK(image->Pixels == fallbackTexels[index]);
						}
					}
					CHECK(path.GetMatchCount() == 1);
				}
				gpu.GetDevice().RunGarbageCollection();
			}
		}

		// M23 (decision 10, stream X): MaterialFlagNormalMapTwoChannel follows the bound normal map's TextureFormat. A material
		// first drawn while its normal map is RGBA8 (its placeholder during an import, or Compression None) gets constants
		// without the flag; when only the texture changes to BC5 (a new version), the mirror writes constants with the flag and
		// takes a new generation, and a change back to RGBA8 clears it, on a device with BC and through the decode fallback
		// alike (whose RGBA8 mirror of a BC5 texture holds (x, y, 0, 255), so z is reconstructed either way). BC5Unorm is
		// decision 13's value.
		TEST_CASE("GpuResourceCache: a material's two-channel normal flag follows its normal map's format when only the texture changes"
			* doctest::test_suite(Test::GpuSuite))
		{
			constexpr auto BC5Unorm = TextureFormat::BC5Unorm;
			Buffer rgba;
			for (uint32_t texel = 0; texel < 4 * 4; ++texel)
			{
				for (const uint32_t value : { 128 + texel, 128 - texel, 255U, 255U })
					rgba.push_back(static_cast<std::byte>(value & 0xFFU));
			}
			const Result<Buffer> blocks = CompressTextureBlocks(TextureBlockCodec::BC5, 4, 4, rgba);
			REQUIRE(blocks.has_value());
			auto bc5 = CreateRef<TextureData>();
			bc5->Format = BC5Unorm;
			bc5->Width = 4;
			bc5->Height = 4;
			bc5->Mips = { { .Width = 4, .Height = 4, .Offset = 0, .Size = blocks->size() } };
			bc5->Pixels = *blocks;
			REQUIRE_MESSAGE(ValidateTextureData(*bc5).has_value(), ValidateTextureData(*bc5).error().ToString());
			auto rgba8 = CreateRef<TextureData>();
			rgba8->Format = TextureFormat::RGBA8Unorm;
			rgba8->Width = 4;
			rgba8->Height = 4;
			rgba8->Mips = { { .Width = 4, .Height = 4, .Offset = 0, .Size = rgba.size() } };
			rgba8->Pixels = rgba;

			for (const bool withBc : { true, false })
			{
				CAPTURE(withBc);
				Test::HeadlessGpuFixture gpu({ .DisableTextureCompressionBC = !withBc });
				ENGINE_REQUIRE_GPU(gpu);
				Test::InMemoryAssetManager assets;
				const AssetHandle normalMap(0x7E57BC5000000001ULL);
				const AssetHandle material(0x7E57BC5000000002ULL);
				assets.Publish(normalMap, rgba8);
				auto data = CreateRef<MaterialData>();
				data->NormalMap = TypedAssetHandle<AssetType::Texture>(normalMap);
				assets.Publish(material, data);
				{
					GpuResourceCache cache(gpu.GetDevice(), assets);
					const GpuMaterial& first = cache.GetMaterial(material);
					CHECK((first.Flags & MaterialFlagNormalMapTwoChannel) == 0);
					const uint64_t firstGeneration = first.Generation;
					const nvrhi::BufferHandle firstConstants = first.Constants;

					assets.Publish(normalMap, bc5);
					const GpuMaterial& compressed = cache.GetMaterial(material);
					CHECK((compressed.Flags & MaterialFlagNormalMapTwoChannel) != 0);
					CHECK(compressed.Generation != firstGeneration);
					CHECK(compressed.Constants != firstConstants);

					assets.Publish(normalMap, rgba8);
					CHECK((cache.GetMaterial(material).Flags & MaterialFlagNormalMapTwoChannel) == 0);
				}
				gpu.GetDevice().RunGarbageCollection();
			}
		}

		// M23 (decisions 7, 10 and 15, stream X): a quad emitting a BC7 sRGB texture and lit through a BC5 normal map, on a
		// device with BC and through the decode fallback. The cache's BC mirrors (BC7_UNORM_SRGB, BC5_UNORM) and the fallback's
		// decompressed ones (SRGBA8_UNORM, RGBA8_UNORM) are asset mirrors of the placement rule's place 3, released with the
		// cache in a device loss and uploaded again by the next Get, so the image after the recovery is byte-identical; the
		// material keeps its two-channel normal flag through the loss. The two devices' images agree within a few levels: the
		// GPU's BC decoders and CMP_Core's agree within the formats' tolerance, and the shader reconstructs the normal's z
		// from x and y either way.
		TEST_CASE("GpuResourceCache: a device loss drops the BC and fallback mirrors, and the next Get uploads them again"
			* doctest::test_suite(Test::GpuSuite))
		{
			Buffer colorTexels;
			Buffer normalTexels;
			for (uint32_t y = 0; y < 8; ++y)
			{
				for (uint32_t x = 0; x < 8; ++x)
				{
					for (const uint32_t value : { 32 + x * 28, 40 + y * 24, 200 - (x + y) * 10, 255U })
						colorTexels.push_back(static_cast<std::byte>(value & 0xFFU));
					// A bumpy normal map: x and y lean by column and row, z is what a unit vector leaves.
					for (const uint32_t value : { 96 + x * 9, 160 - y * 9, 255U, 255U })
						normalTexels.push_back(static_cast<std::byte>(value & 0xFFU));
				}
			}
			const auto makeTexture = [](TextureFormat format, TextureBlockCodec codec, const Buffer& texels)
			{
				Result<Buffer> blocks = CompressTextureBlocks(codec, 8, 8, texels);
				REQUIRE_MESSAGE(blocks.has_value(), blocks.error().ToString());
				auto texture = CreateRef<TextureData>();
				texture->Format = format;
				texture->Width = 8;
				texture->Height = 8;
				texture->Mips = { { .Width = 8, .Height = 8, .Offset = 0, .Size = blocks->size() } };
				texture->Pixels = std::move(*blocks);
				REQUIRE_MESSAGE(ValidateTextureData(*texture).has_value(), ValidateTextureData(*texture).error().ToString());
				return texture;
			};
			const AssetRef<TextureData> emissive = makeTexture(TextureFormat::BC7Srgb, TextureBlockCodec::BC7, colorTexels);
			const AssetRef<TextureData> normal = makeTexture(TextureFormat::BC5Unorm, TextureBlockCodec::BC5, normalTexels);
			const UUID emissiveHandle(0x7E57BC7000000011ULL);
			const UUID normalHandle(0x7E57BC5000000011ULL);

			std::array<std::vector<std::byte>, 2> images{};
			for (const bool withBc : { true, false })
			{
				CAPTURE(withBc);
				Test::HeadlessGpuFixture gpu({ .DisableTextureCompressionBC = !withBc });
				ENGINE_REQUIRE_GPU(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				REQUIRE(device.GetInfo().TextureCompressionBC == withBc);
				Test::SceneRendererIntegrationFixture setup(gpu);
				setup.Assets().Publish(emissiveHandle, emissive);
				setup.Assets().Publish(normalHandle, normal);
				auto material = CreateRef<MaterialData>();
				material->BaseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
				material->Roughness = 0.6f;
				material->Emissive = glm::vec3(1.0f);
				material->EmissiveStrength = 0.25f;
				material->EmissiveMap = TypedAssetHandle<AssetType::Texture>(emissiveHandle);
				material->NormalMap = TypedAssetHandle<AssetType::Texture>(normalHandle);
				setup.Assets().Publish(UUID(8911), material);

				uint64_t frame = 40;
				Test::DeviceLossTest test;
				test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
				test.Use = [&device, &setup, &frame]() -> Result<std::vector<std::byte>>
				{
					RenderSnapshot snapshot = setup.Snapshot();
					snapshot.FrameIndex = ++frame;
					snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional,
						.Intensity = 3.0f,
						.Direction = glm::normalize(glm::vec3(0.3f, -0.4f, -1.0f)) });
					ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
					list->open();
					const Status rendered = setup.Renderer().Render(*list, snapshot);
					list->close();
					setup.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
					ENGINE_TRY(rendered);
					Readback readback(device);
					ENGINE_TRY_ASSIGN(const Image pixels, readback.ReadTexture(*setup.Renderer().GetFinalTexture()));
					return pixels.Pixels;
				};
				const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
				REQUIRE_MESSAGE(report.has_value(), report.error().ToString());

				// After the recovery: the same mirrors' formats, never a placeholder, and the material's flag.
				const GpuTexture& emissiveMirror = setup.Cache().GetTexture(emissiveHandle);
				const GpuTexture& normalMirror = setup.Cache().GetTexture(normalHandle);
				REQUIRE(emissiveMirror.Texture != nullptr);
				REQUIRE(normalMirror.Texture != nullptr);
				CHECK_FALSE(emissiveMirror.IsPlaceholder);
				CHECK_FALSE(normalMirror.IsPlaceholder);
				CHECK(emissiveMirror.Texture->getDesc().format == (withBc ? nvrhi::Format::BC7_UNORM_SRGB : nvrhi::Format::SRGBA8_UNORM));
				CHECK(normalMirror.Texture->getDesc().format == (withBc ? nvrhi::Format::BC5_UNORM : nvrhi::Format::RGBA8_UNORM));
				CHECK((setup.Cache().GetMaterial(UUID(8911)).Flags & MaterialFlagNormalMapTwoChannel) != 0);
				images[withBc ? 0 : 1] = report->After;
			}

			REQUIRE(images[0].size() == images[1].size());
			REQUIRE_FALSE(images[0].empty());
			int largest = 0;
			for (size_t index = 0; index < images[0].size(); ++index)
				largest = std::max(largest, std::abs(std::to_integer<int>(images[0][index]) - std::to_integer<int>(images[1][index])));
			CHECK(largest <= 3);
			// The quad is lit and emits, so the comparison is not of two black images.
			CHECK(std::ranges::any_of(images[0], [](std::byte value)
			{
				return std::to_integer<int>(value) > 32;
			}));
		}

		// M23 acceptance (R-TEXC-HDR, decisions 14 and 15, stream Y): a BC6H skybox cube uploads as BC6H_UFLOAT on a device with
		// BC, and on a device without it through the decode fallback as RGBA16_FLOAT, whose texels are DecompressTextureBlocks'
		// for every face (+X to -Z: a face read from the wrong layer fails) and close to the source's. Each face is a distinct
		// constant colour, which BC6H holds within a few per cent. BC6HUfloat is CubeMapFormat's frozen value.
		TEST_CASE("GpuResourceCache: a BC6H environment uploads as BC6H with BC support and through the decode fallback without it"
			* doctest::test_suite(Test::GpuSuite))
		{
			constexpr uint32_t FaceSize = 8;
			constexpr uint32_t MipCount = 4;
			const std::array<glm::dvec3, CubeMapData::FaceCount> faceColors = { glm::dvec3(4.0, 0.5, 0.25), glm::dvec3(0.25, 4.0, 0.5),
				glm::dvec3(0.5, 0.25, 4.0), glm::dvec3(2.0, 2.0, 0.125), glm::dvec3(0.125, 2.0, 2.0), glm::dvec3(1.0, 0.0625, 1.0) };
			// Mip 0 of each face as RGBA16F texels, and every level's blocks in CubeMapData's order (level by level, the six
			// faces of each in layer order).
			std::array<Buffer, CubeMapData::FaceCount> sources{};
			std::array<Buffer, CubeMapData::FaceCount> faceBlocks{};
			EnvironmentData environment;
			environment.Skybox = { .FaceSize = FaceSize, .MipCount = MipCount, .Format = CubeMapFormat::BC6HUfloat, .Texels = {} };
			for (uint32_t level = 0; level < MipCount; ++level)
			{
				const uint32_t size = std::max(1U, FaceSize >> level);
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					Buffer texels;
					for (uint32_t texel = 0; texel < size * size; ++texel)
					{
						for (const double value : { faceColors[face].x, faceColors[face].y, faceColors[face].z, 1.0 })
						{
							const uint16_t half = Test::DoubleToHalf(value);
							texels.push_back(static_cast<std::byte>(half & 0xFFU));
							texels.push_back(static_cast<std::byte>(half >> 8));
						}
					}
					const Result<Buffer> blocks = CompressTextureBlocks(TextureBlockCodec::BC6H, size, size, texels);
					REQUIRE_MESSAGE(blocks.has_value(), blocks.error().ToString());
					if (level == 0)
					{
						sources[face] = texels;
						faceBlocks[face] = *blocks;
					}
					environment.Skybox.Texels.insert(environment.Skybox.Texels.end(), blocks->begin(), blocks->end());
				}
			}
			environment.Specular = { .FaceSize = EnvironmentData::SpecularFaceSize,
				.MipCount = EnvironmentData::SpecularMipCount,
				.Format = CubeMapFormat::Rgba16Float,
				.Texels = Buffer(ComputeCubeMapByteSize(EnvironmentData::SpecularFaceSize, EnvironmentData::SpecularMipCount), std::byte{ 0 }) };
			REQUIRE_MESSAGE(ValidateEnvironmentData(environment).has_value(), ValidateEnvironmentData(environment).error().ToString());
			const auto asset = CreateRef<EnvironmentData>(std::move(environment));
			// The binary16 value at byte `index` of RGBA16F texels.
			const auto readHalf = [](std::span<const std::byte> texels, size_t index)
			{
				return Test::HalfToDouble(static_cast<uint16_t>(std::to_integer<uint32_t>(texels[index]) | (std::to_integer<uint32_t>(texels[index + 1]) << 8)));
			};

			for (const bool withBc : { true, false })
			{
				CAPTURE(withBc);
				Test::HeadlessGpuFixture gpu({ .DisableTextureCompressionBC = !withBc });
				ENGINE_REQUIRE_GPU(gpu);
				Test::InMemoryAssetManager assets;
				assets.Publish(AssetHandle(0x7E57BC6000000001ULL), asset);
				{
					GpuResourceCache cache(gpu.GetDevice(), assets);
					const GpuEnvironment* mirror = cache.GetEnvironment(AssetHandle(0x7E57BC6000000001ULL));
					REQUIRE(mirror != nullptr);
					REQUIRE(mirror->Skybox != nullptr);
					CHECK(mirror->SkyboxMipCount == MipCount);
					CHECK(mirror->Skybox->getDesc().format == (withBc ? nvrhi::Format::BC6H_UFLOAT : nvrhi::Format::RGBA16_FLOAT));
					if (!withBc)
					{
						Readback readback(gpu.GetDevice());
						for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
						{
							CAPTURE(face);
							const Result<Image> image = readback.ReadTexture(*mirror->Skybox, 0, face);
							REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
							const Result<Buffer> expected = DecompressTextureBlocks(TextureBlockCodec::BC6H, FaceSize, FaceSize, faceBlocks[face]);
							REQUIRE(expected.has_value());
							CHECK(image->Pixels == *expected);
							for (size_t channel = 0; channel < 3; ++channel)
							{
								const double source = readHalf(sources[face], channel * 2);
								CHECK(std::abs(readHalf(image->Pixels, channel * 2) - source) <= 0.05 * source + 1.0 / 1024.0);
							}
						}
					}
				}
				gpu.GetDevice().RunGarbageCollection();
			}
		}

		// M23 (R-TEXC-HDR, decisions 7, 14 and 15, stream Y): BC6H environment cubes are asset mirrors of the placement rule's
		// place 3. A skybox drawn from a BC6H environment matches the one drawn from its RGBA16F source within BC6H's error,
		// on a device with BC (BC6H_UFLOAT) and through the decode fallback without it (RGBA16_FLOAT), and a device loss
		// releases the mirror with the cache, whose next Get uploads the BC6H cube again, so the render after the recovery is
		// byte-identical. Each face is a distinct constant colour below 1, which the Linear tonemapper keeps.
		TEST_CASE("GpuResourceCache: a BC6H environment's skybox renders like its RGBA16F source, and after a device loss"
			* doctest::test_suite(Test::GpuSuite))
		{
			constexpr uint32_t FaceSize = 16;
			constexpr uint32_t MipCount = 5;
			const std::array<glm::dvec3, CubeMapData::FaceCount> faceColors = { glm::dvec3(0.8, 0.2, 0.1), glm::dvec3(0.1, 0.7, 0.2),
				glm::dvec3(0.2, 0.3, 0.9), glm::dvec3(0.9, 0.9, 0.2), glm::dvec3(0.1, 0.6, 0.6), glm::dvec3(0.7, 0.1, 0.7) };
			EnvironmentData source;
			source.Skybox = { .FaceSize = FaceSize, .MipCount = MipCount, .Format = CubeMapFormat::Rgba16Float, .Texels = {} };
			EnvironmentData compressed;
			compressed.Skybox = { .FaceSize = FaceSize, .MipCount = MipCount, .Format = CubeMapFormat::BC6HUfloat, .Texels = {} };
			for (uint32_t level = 0; level < MipCount; ++level)
			{
				const uint32_t size = std::max(1U, FaceSize >> level);
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					Buffer texels;
					for (uint32_t texel = 0; texel < size * size; ++texel)
					{
						for (const double value : { faceColors[face].x, faceColors[face].y, faceColors[face].z, 1.0 })
						{
							const uint16_t half = Test::DoubleToHalf(value);
							texels.push_back(static_cast<std::byte>(half & 0xFFU));
							texels.push_back(static_cast<std::byte>(half >> 8));
						}
					}
					const Result<Buffer> blocks = CompressTextureBlocks(TextureBlockCodec::BC6H, size, size, texels);
					REQUIRE_MESSAGE(blocks.has_value(), blocks.error().ToString());
					source.Skybox.Texels.insert(source.Skybox.Texels.end(), texels.begin(), texels.end());
					compressed.Skybox.Texels.insert(compressed.Skybox.Texels.end(), blocks->begin(), blocks->end());
				}
			}
			for (EnvironmentData* environment : { &source, &compressed })
			{
				environment->Specular = { .FaceSize = EnvironmentData::SpecularFaceSize,
					.MipCount = EnvironmentData::SpecularMipCount,
					.Format = CubeMapFormat::Rgba16Float,
					.Texels = Buffer(ComputeCubeMapByteSize(EnvironmentData::SpecularFaceSize, EnvironmentData::SpecularMipCount), std::byte{ 0 }) };
				REQUIRE_MESSAGE(ValidateEnvironmentData(*environment).has_value(), ValidateEnvironmentData(*environment).error().ToString());
			}
			const AssetHandle sourceHandle(0x7E57BC6000000011ULL);
			const AssetHandle compressedHandle(0x7E57BC6000000012ULL);
			const auto sourceAsset = CreateRef<EnvironmentData>(std::move(source));
			const auto compressedAsset = CreateRef<EnvironmentData>(std::move(compressed));

			for (const bool withBc : { true, false })
			{
				CAPTURE(withBc);
				Test::HeadlessGpuFixture gpu({ .DisableTextureCompressionBC = !withBc });
				ENGINE_REQUIRE_GPU(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				{
					Test::SceneRendererIntegrationFixture setup(gpu);
					setup.Assets().Publish(sourceHandle, sourceAsset);
					setup.Assets().Publish(compressedHandle, compressedAsset);
					uint64_t frame = 60;
					// The skybox of `environment` over the whole view (no meshes), read back.
					const auto render = [&device, &setup, &frame](AssetHandle environment) -> Result<std::vector<std::byte>>
					{
						RenderSnapshot snapshot = setup.Snapshot();
						snapshot.FrameIndex = ++frame;
						snapshot.Meshes.clear();
						snapshot.Camera.ClearToSkybox = true;
						snapshot.Environment = RenderEnvironment{ .Environment = environment, .Intensity = 1.0f, .ShowSkybox = true };
						ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
						list->open();
						const Status rendered = setup.Renderer().Render(*list, snapshot);
						list->close();
						setup.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
						ENGINE_TRY(rendered);
						Readback readback(device);
						ENGINE_TRY_ASSIGN(const Image pixels, readback.ReadTexture(*setup.Renderer().GetFinalTexture()));
						return pixels.Pixels;
					};
					Test::DeviceLossTest test;
					test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
					test.Use = [&render, compressedHandle]() -> Result<std::vector<std::byte>>
					{
						return render(compressedHandle);
					};
					const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
					REQUIRE_MESSAGE(report.has_value(), report.error().ToString());

					const GpuEnvironment* mirror = setup.Cache().GetEnvironment(compressedHandle);
					REQUIRE(mirror != nullptr);
					REQUIRE(mirror->Skybox != nullptr);
					CHECK(mirror->Skybox->getDesc().format == (withBc ? nvrhi::Format::BC6H_UFLOAT : nvrhi::Format::RGBA16_FLOAT));
					// The RGBA16F source's render, after the loss test, which compares the GPU objects alive before and after.
					const Result<std::vector<std::byte>> reference = render(sourceHandle);
					REQUIRE_MESSAGE(reference.has_value(), reference.error().ToString());
					REQUIRE(report->After.size() == reference->size());
					int largest = 0;
					for (size_t index = 0; index < reference->size(); ++index)
						largest = std::max(largest, std::abs(std::to_integer<int>(report->After[index]) - std::to_integer<int>((*reference)[index])));
					CHECK(largest <= 4);
					// The view shows a lit sky, so the comparison is not of two black images.
					CHECK(std::ranges::any_of(*reference, [](std::byte value)
					{
						return std::to_integer<int>(value) > 32;
					}));
				}
				device.RunGarbageCollection();
			}
		}

		TEST_CASE("GpuResourceCache: uploads a mesh and a texture through the M5 upload paths"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				const GpuMesh& cube = cache.GetMesh(BuiltinAssetHandles::CubeMesh);
				CHECK(cube.VertexBuffer != nullptr);
				CHECK(cube.IndexBuffer != nullptr);
				CHECK(cube.VertexCount == 24);
				CHECK(cube.IndexCount == 36);
				CHECK(cube.Submeshes.size() == 1);
				CHECK(cube.Version == 1);
				CHECK_FALSE(cube.IsPlaceholder);

				const GpuTexture& checker = cache.GetTexture(BuiltinAssetHandles::CheckerTexture);
				REQUIRE(checker.Texture != nullptr);
				CHECK_FALSE(checker.IsPlaceholder);
				// The texels read back exactly (mip 0), whichever upload path the device took.
				Readback readback(gpu.GetDevice());
				Result<Image> image = readback.ReadTexture(*checker.Texture);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				const TextureData expected = GenerateBuiltinTexture(BuiltinTexture::Checker);
				CHECK(std::ranges::equal(image->Pixels, GetMipPixels(expected, 0)));

				// The same handle at the same version is served from the cache.
				CHECK(&cache.GetMesh(BuiltinAssetHandles::CubeMesh) == &cube);
				CHECK(cache.GetStats().MeshCount == 1);
				CHECK(cache.GetStats().TextureCount == 1);
				cache.Clear();
				CHECK(cache.GetStats().MeshCount == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: an injected texture OOM yields the placeholder and a diagnostic"
			* doctest::test_suite(Test::GpuSuite))
		{
			// Roadmap M5 acceptance "=oom-texture yields a placeholder plus diagnostic", deferred to GpuResourceCache (ADR 0009
			// decision 8): every sampled texture creation fails, the mirror is the placeholder's (null when even that fails) and
			// the asset manager records ASSET_UPLOAD_FAILED, logged once; nothing crashes.
			Test::HeadlessGpuFixture gpu({ .SynchronizationValidation = true, .InjectFault = GpuFault::OomTexture, .FramesInFlight = 2 });
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				Test::ExpectLog expected(LogLevel::Error, std::string(AssetUploadFailedCode));
				const GpuTexture& texture = cache.GetTexture(BuiltinAssetHandles::CheckerTexture);
				CHECK(texture.IsPlaceholder);
				static_cast<void>(cache.GetTexture(BuiltinAssetHandles::CheckerTexture));
				CHECK(expected.GetMatchCount() == 1);
				CHECK(cache.GetStats().UploadFailures >= 1);
				CHECK(std::ranges::any_of(assets.GetManager().GetDiagnostics(), [](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Code == AssetUploadFailedCode && diagnostic.Asset == BuiltinAssetHandles::CheckerTexture;
				}));
				// Meshes still upload: the fault hits sampled textures only.
				CHECK_FALSE(cache.GetMesh(BuiltinAssetHandles::CubeMesh).IsPlaceholder);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: a new asset version re-uploads and CollectStale releases the old mirror"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			assets.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(8, 8, 1));
			assets.OpenProject(false);
			const AssetHandle wood = assets.GetManager().Resolve("Assets/Wood.png").value_or(AssetHandle());
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				const nvrhi::TextureHandle first = cache.GetTexture(wood).Texture;
				REQUIRE(first != nullptr);
				CHECK(cache.GetTexture(wood).Version == 1);
				assets.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(8, 8, 2));
				REQUIRE(assets.GetManager().Refresh().has_value());
				const GpuTexture& second = cache.GetTexture(wood);
				CHECK(second.Version == 2);
				CHECK(second.Texture != first);
				cache.CollectStale();
				CHECK(cache.GetStats().TextureCount == 1);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: null and missing handles share the placeholder's mirror"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				// Null: the placeholder itself, without a diagnostic.
				const GpuMesh& none = cache.GetMesh(AssetHandle());
				CHECK(none.VertexBuffer != nullptr);
				CHECK(none.VertexCount == 24);
				CHECK_FALSE(none.IsPlaceholder);
				CHECK(&cache.GetMesh(BuiltinAssetHandles::CubeMesh) == &none);
				CHECK(assets.GetDiagnostics().empty());

				// Missing: an entry of its own that shares the placeholder's GPU objects; the manager reports it once.
				const AssetHandle unknown(0x7e57'0000'0000'0001);
				Test::ExpectLog missing(LogLevel::Error, std::string(AssetMissingCode));
				const GpuTexture& stand = cache.GetTexture(unknown);
				CHECK(stand.IsPlaceholder);
				CHECK(stand.Version == 0);
				REQUIRE(stand.Texture != nullptr);
				CHECK(stand.Texture == cache.GetTexture(BuiltinAssetHandles::MissingTexture).Texture);
				// The placeholder's whole mip chain was uploaded; its base level reads back exactly.
				CHECK(stand.Texture->getDesc().mipLevels == 7);
				Readback readback(gpu.GetDevice());
				Result<Image> image = readback.ReadTexture(*stand.Texture);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				const TextureData expected = GenerateBuiltinTexture(BuiltinTexture::Missing);
				CHECK(std::ranges::equal(image->Pixels, GetMipPixels(expected, 0)));
				CHECK(&cache.GetTexture(unknown) == &stand);
				CHECK(missing.GetMatchCount() == 1);
				// A mesh asked for as a texture is of another type: the texture placeholder again.
				Test::ExpectLog mismatch(LogLevel::Error, std::string(AssetTypeMismatchCode));
				CHECK(cache.GetTexture(BuiltinAssetHandles::SphereMesh).IsPlaceholder);
				CHECK(cache.GetStats().TextureCount == 3);
				CHECK(cache.GetStats().UploadFailures == 0);

				// Once the asset exists, its own version is uploaded and the stand-in becomes stale.
				assets.Publish(unknown, MakeTexture(40));
				const GpuTexture& real = cache.GetTexture(unknown);
				CHECK_FALSE(real.IsPlaceholder);
				CHECK(real.Version == 1);
				CHECK(real.Texture != cache.GetTexture(BuiltinAssetHandles::MissingTexture).Texture);
				cache.CollectStale();
				CHECK(cache.GetStats().TextureCount == 3);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: a failed upload is remembered per version and reported once per asset"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu({ .SynchronizationValidation = true, .InjectFault = GpuFault::OomTexture, .FramesInFlight = 2 });
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			const AssetHandle texture(0x7e57'0000'0000'0004);
			assets.Publish(texture, MakeTexture(30));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Test::ExpectLog failed(LogLevel::Error, std::string(AssetUploadFailedCode));
				// The asset and then its placeholder fail; the asset's diagnostic says that nothing can be drawn.
				const GpuTexture& first = cache.GetTexture(texture);
				CHECK(first.IsPlaceholder);
				CHECK(first.Texture == nullptr);
				CHECK(first.Version == 1);
				CHECK(cache.GetStats().UploadFailures == 2);
				CHECK(failed.GetMatchCount() == 1);
				const auto findDiagnostic = [&assets](AssetHandle handle) -> const AssetDiagnostic*
				{
					const std::span<const AssetDiagnostic> diagnostics = assets.GetDiagnostics();
					const auto found = std::ranges::find_if(diagnostics, [handle](const AssetDiagnostic& diagnostic)
					{
						return diagnostic.Code == AssetUploadFailedCode && diagnostic.Asset == handle;
					});
					return found != diagnostics.end() ? &*found : nullptr;
				};
				const AssetDiagnostic* diagnostic = findDiagnostic(texture);
				REQUIRE(diagnostic != nullptr);
				CHECK(diagnostic->Severity == DiagnosticSeverity::Error);
				CHECK(diagnostic->Message.contains("could not be uploaded either"));
				CHECK(findDiagnostic(BuiltinAssetHandles::MissingTexture) == nullptr);

				// The same version is not uploaded again.
				CHECK(&cache.GetTexture(texture) == &first);
				CHECK(cache.GetStats().UploadFailures == 2);

				// Asked for directly, the placeholder reports its own failure, once.
				const GpuTexture& placeholder = cache.GetTexture(BuiltinAssetHandles::MissingTexture);
				CHECK(placeholder.Texture == nullptr);
				CHECK(placeholder.IsPlaceholder);
				CHECK(failed.GetMatchCount() == 2);
				CHECK(findDiagnostic(BuiltinAssetHandles::MissingTexture) != nullptr);
				CHECK(cache.GetStats().UploadFailures == 2);

				// A new version tries again; the asset manager logs the asset's diagnostic only once.
				assets.Publish(texture, MakeTexture(31));
				CHECK(cache.GetTexture(texture).Version == 2);
				CHECK(cache.GetStats().UploadFailures == 3);
				CHECK(failed.GetMatchCount() == 2);

				// Buffers are not textures: meshes still upload.
				CHECK(cache.GetMesh(BuiltinAssetHandles::SphereMesh).VertexBuffer != nullptr);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: CollectStale releases old versions, then unused mirrors on request"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			const AssetHandle texture(0x7e57'0000'0000'0002);
			const AssetHandle mesh(0x7e57'0000'0000'0003);
			assets.Publish(texture, MakeTexture(10));
			MeshData plane = GenerateBuiltinMesh(BuiltinMesh::Plane);
			plane.Slots.front().DefaultMaterial = BuiltinAssetHandles::ErrorMaterial;
			assets.Publish(mesh, CreateRef<MeshData>(std::move(plane)));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				const GpuMesh& uploaded = cache.GetMesh(mesh);
				CHECK_FALSE(uploaded.IsPlaceholder);
				CHECK(uploaded.VertexCount == 121);
				CHECK(uploaded.IndexCount == 600);
				REQUIRE(uploaded.Submeshes.size() == 1);
				CHECK(uploaded.Submeshes.front().IndexCount == 600);
				REQUIRE(uploaded.DefaultMaterials.size() == 1);
				CHECK(uploaded.DefaultMaterials.front() == BuiltinAssetHandles::ErrorMaterial);
				CHECK(uploaded.Bounds.GetSize().x == 1.0f);

				const nvrhi::TextureHandle first = cache.GetTexture(texture).Texture;
				REQUIRE(first != nullptr);
				Readback readback(gpu.GetDevice());
				assets.Publish(texture, MakeTexture(20));
				const GpuTexture& second = cache.GetTexture(texture);
				CHECK(second.Version == 2);
				REQUIRE(second.Texture != nullptr);
				CHECK(second.Texture != first);
				Result<Image> image = readback.ReadTexture(*second.Texture);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				CHECK(std::ranges::all_of(image->Pixels, [](std::byte value)
				{
					return value == std::byte{ 20 };
				}));
				CHECK(cache.GetStats().TextureCount == 2);
				cache.CollectStale();
				CHECK(cache.GetStats().TextureCount == 1);
				CHECK(cache.GetStats().MeshCount == 1);

				// Only what was used since the previous CollectStale survives a release of unused mirrors.
				static_cast<void>(cache.GetMesh(mesh));
				cache.CollectStale(true);
				CHECK(cache.GetStats().MeshCount == 1);
				CHECK(cache.GetStats().TextureCount == 0);
				cache.CollectStale(true);
				CHECK(cache.GetStats().MeshCount == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M8 (Docs/Decisions/0013-m8-decisions.md decision 7): materials and environments, the unload and hot-reload
		// acceptance tests.

		TEST_CASE("GpuResourceCache: live counts return to baseline after unloading a scene" * doctest::test_suite(Test::GpuSuite))
		{
			// A scene's GPU state is a mesh, a material with its texture, an environment and a text's font atlas, mirrored by
			// the cache and the TextRenderer and bound by a SceneRenderer. Unloading it (the view's renderer destroyed, then
			// the hosts' collections of SceneRenderer.h, "Stale mirrors") returns every live count to its baseline: the
			// counts before the first load plus exactly the pass-owned vertex buffers, which grow once and stay by design
			// (here the TextRenderer's; the scene draws no debug lines). A second load must then come back to exactly that.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::InMemoryAssetManager assets;
			constexpr AssetHandle TextureHandle{ 0x9001 };
			constexpr AssetHandle MaterialHandle{ 0x9002 };
			constexpr AssetHandle EnvironmentHandle{ 0x9003 };
			constexpr AssetHandle FontHandle{ 0x9004 };
			MaterialData material;
			material.BaseColorMap = TypedAssetHandle<AssetType::Texture>(TextureHandle);
			assets.Publish(TextureHandle, MakeTexture(200));
			assets.Publish(MaterialHandle, CreateRef<MaterialData>(std::move(material)));
			assets.Publish(EnvironmentHandle, MakeEnvironment());
			assets.Publish(FontHandle, MakeFont());

			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.Position = glm::vec3(0.0f, 0.0f, 5.0f);
			snapshot.Camera.View = glm::translate(glm::mat4(1.0f), -snapshot.Camera.Position);
			snapshot.Camera.ViewportWidth = 32;
			snapshot.Camera.ViewportHeight = 32;
			snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 1000.0f, 32, 32);
			snapshot.Meshes.push_back(MeshDrawItem{ .Mesh = BuiltinAssetHandles::SphereMesh, .Materials = { MaterialHandle } });
			snapshot.Environment.Environment = EnvironmentHandle;
			snapshot.Texts.push_back(TextItem{ .Text = "A", .Font = FontHandle });
			{
				GpuResourceCache cache(device, assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				// Loads the scene into a new view, checks it is mirrored, and unloads it; returns the live count while loaded.
				const auto loadAndUnload = [&]() -> uint64_t
				{
					uint64_t loaded = 0;
					{
						Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(device, **pipelines, cache, assets, { .Width = 32, .Height = 32 });
						REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
						Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
						REQUIRE(commandList.has_value());
						(*commandList)->open();
						const Status rendered = (*renderer)->Render(**commandList, snapshot);
						(*commandList)->close();
						(*renderer)->OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**commandList));
						REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
						CHECK(cache.GetStats().MaterialCount == 1);
						CHECK(cache.GetStats().EnvironmentCount == 1);
						device.RunGarbageCollection();
						loaded = device.GetResourceTracker().GetTotalLiveCount();
					}
					// Unload: the view went with its renderer; two collections release everything not used since (§8.14
					// item 2), and the device's garbage collection destroys what NVRHI deferred.
					for (int collection = 0; collection < 2; ++collection)
					{
						cache.CollectStale(true);
						(*pipelines)->CollectStale(assets, true);
					}
					CHECK(cache.GetStats().MeshCount == 0);
					CHECK(cache.GetStats().TextureCount == 0);
					CHECK(cache.GetStats().MaterialCount == 0);
					CHECK(cache.GetStats().EnvironmentCount == 0);
					device.WaitForIdle();
					device.RunGarbageCollection();
					return loaded;
				};

				// The live count of every type.
				const auto countLive = [&device]()
				{
					std::array<uint64_t, GpuResourceTypeCount> counts{};
					for (size_t type = 0; type < GpuResourceTypeCount; ++type)
						counts[type] = device.GetResourceTracker().GetLiveCount(static_cast<GpuResourceType>(type));
					return counts;
				};

				device.WaitForIdle();
				device.RunGarbageCollection();
				const std::array<uint64_t, GpuResourceTypeCount> beforeLoad = countLive();
				// Each load runs outside its CHECK: the helper REQUIREs a successful render.
				const uint64_t loadedFirst = loadAndUnload();
				const std::array<uint64_t, GpuResourceTypeCount> afterFirstUnload = countLive();
				const std::string firstUnloadCounts = device.GetResourceTracker().DescribeLiveCounts();
				INFO("live after the first unload: ", firstUnloadCounts);
				CHECK(loadedFirst > device.GetResourceTracker().GetTotalLiveCount());
				for (size_t type = 0; type < GpuResourceTypeCount; ++type)
				{
					CAPTURE(type);
					const uint64_t grown = static_cast<GpuResourceType>(type) == GpuResourceType::GpuBuffer ? 1 : 0;
					CHECK(afterFirstUnload[type] == beforeLoad[type] + grown);
				}
				const uint64_t baseline = device.GetResourceTracker().GetTotalLiveCount();
				const uint64_t loadedAgain = loadAndUnload();
				CHECK(loadedAgain > baseline);
				const std::string secondUnloadCounts = device.GetResourceTracker().DescribeLiveCounts();
				INFO("live after the second unload: ", secondUnloadCounts);
				CHECK(device.GetResourceTracker().GetTotalLiveCount() == baseline);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("HotReload: texture change re-uploads" * doctest::test_suite(Test::GpuSuite))
		{
			// An external change of a texture through the editor's asset manager (polled, reimported, swapped, §7.5) gives the
			// cache a new version: the next GetTexture uploads it and a material using it is rebuilt with the new texture. The
			// PNG imports as BC7 (Compression Auto, M23), which Readback cannot read, so the device has no BC support and the
			// cache uploads the decoded texels (the decode fallback), which it can.
			Test::HeadlessGpuFixture gpu({ .DisableTextureCompressionBC = true });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 1));
			fixture.OpenProject(true);
			const AssetHandle texture = fixture.GetManager().Resolve("Assets/Wood.png").value_or(AssetHandle());
			REQUIRE(texture.IsValid());
			// A material references its textures by handle (§6.5); the scan registers it.
			fixture.WriteProjectText("Assets/Wood.material",
				std::format(R"({{ "Format": "Material", "Version": 1, "BaseColorMap": "{}" }})", texture.ToString()));
			const Result<AssetRefreshReport> refreshed = fixture.GetManager().Refresh();
			REQUIRE_MESSAGE(refreshed.has_value(), refreshed.error().ToString());
			const AssetHandle material = fixture.GetManager().Resolve("Assets/Wood.material").value_or(AssetHandle());
			REQUIRE(material.IsValid());
			{
				GpuResourceCache cache(device, fixture.GetManager());
				Readback readback(device);
				const nvrhi::TextureHandle before = cache.GetTexture(texture).Texture;
				REQUIRE(before != nullptr);
				const Result<Image> beforeImage = readback.ReadTexture(*before);
				REQUIRE(beforeImage.has_value());
				const GpuMaterial& materialBefore = cache.GetMaterial(material);
				CHECK(materialBefore.BaseColorMap == before);
				const uint64_t generationBefore = materialBefore.Generation;

				fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 2));
				for (const double seconds : { 0.0, 0.5, 1.0, 1.5 })
				{
					fixture.GetManager().Update(seconds);
					static_cast<void>(fixture.GetMainThreadQueue().Drain());
				}
				fixture.GetManager().WaitIdle();
				REQUIRE(fixture.GetManager().GetVersion(texture) == 2);

				const GpuTexture& after = cache.GetTexture(texture);
				CHECK(after.Version == 2);
				REQUIRE(after.Texture != nullptr);
				CHECK(after.Texture != before);
				const Result<Image> afterImage = readback.ReadTexture(*after.Texture);
				REQUIRE(afterImage.has_value());
				CHECK(afterImage->Pixels != beforeImage->Pixels);
				// The material's own version is unchanged, but its texture's is not: the mirror is rebuilt with the new texture.
				const nvrhi::TextureHandle afterTexture = after.Texture;
				const GpuMaterial& materialAfter = cache.GetMaterial(material);
				CHECK(materialAfter.Generation != generationBefore);
				CHECK(materialAfter.BaseColorMap == afterTexture);
				// The replaced version is released; the new one stays with the three slot defaults the material binds (White for
				// metallic-roughness and occlusion, FlatNormal, Black).
				cache.CollectStale();
				CHECK(cache.GetStats().TextureCount == 4);
				CHECK(cache.GetMaterial(material).BaseColorMap == afterTexture);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: a material mirror binds White, FlatNormal and Black to its empty slots" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			MaterialData material;
			material.AlphaMode = AlphaMode::Mask;
			material.AlphaCutoff = 0.25f;
			material.DoubleSided = true;
			assets.Publish(AssetHandle(0x9101), CreateRef<MaterialData>(std::move(material)));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				const nvrhi::TextureHandle white = cache.GetTexture(BuiltinAssetHandles::WhiteTexture).Texture;
				const nvrhi::TextureHandle flatNormal = cache.GetTexture(BuiltinAssetHandles::FlatNormalTexture).Texture;
				const nvrhi::TextureHandle black = cache.GetTexture(BuiltinAssetHandles::BlackTexture).Texture;
				const GpuMaterial& mirrored = cache.GetMaterial(AssetHandle(0x9101));
				CHECK(mirrored.BaseColorMap == white);
				CHECK(mirrored.MetallicRoughnessMap == white);
				CHECK(mirrored.OcclusionMap == white);
				CHECK(mirrored.NormalMap == flatNormal);
				CHECK(mirrored.EmissiveMap == black);
				CHECK(mirrored.AlphaMode == AlphaMode::Mask);
				CHECK(mirrored.AlphaCutoff == 0.25f);
				CHECK(mirrored.DoubleSided);
				CHECK_FALSE(mirrored.IsPlaceholder);
				CHECK(mirrored.Flags == 0); // the flags its constants were written with: no emissive map

				// A missing material mirrors the Error material (GetOrPlaceholder reports it once); a null handle the Default
				// material.
				{
					Test::ExpectLog missing(LogLevel::Error, "ASSET_MISSING");
					CHECK(cache.GetMaterial(AssetHandle(0x9199)).IsPlaceholder);
				}
				CHECK_FALSE(cache.GetMaterial(AssetHandle()).IsPlaceholder);

				// A new version rebuilds the mirror under a new generation, with the new version's flags.
				const uint64_t generation = cache.GetMaterial(AssetHandle(0x9101)).Generation;
				auto emissive = CreateRef<MaterialData>();
				emissive->EmissiveMap = TypedAssetHandle<AssetType::Texture>(BuiltinAssetHandles::WhiteTexture);
				assets.Publish(AssetHandle(0x9101), emissive);
				const GpuMaterial& rebuiltMirror = cache.GetMaterial(AssetHandle(0x9101));
				CHECK(rebuiltMirror.Flags == MaterialFlagEmissiveMap);
				const uint64_t rebuilt = rebuiltMirror.Generation;
				CHECK(rebuilt != generation);
				// A mirror released by CollectStale and built again never gets an earlier generation back (the renderer's
				// binding sets key on it).
				cache.CollectStale(true);
				cache.CollectStale(true);
				CHECK(cache.GetStats().MaterialCount == 0);
				const uint64_t again = cache.GetMaterial(AssetHandle(0x9101)).Generation;
				CHECK(again != generation);
				CHECK(again != rebuilt);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: an environment mirrors both cubes, and a null or unloadable one is null" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			assets.Publish(AssetHandle(0x9201), MakeEnvironment());
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				const GpuEnvironment* mirrored = cache.GetEnvironment(AssetHandle(0x9201));
				REQUIRE(mirrored != nullptr);
				REQUIRE(mirrored->Skybox != nullptr);
				REQUIRE(mirrored->Specular != nullptr);
				CHECK(mirrored->Skybox->getDesc().dimension == nvrhi::TextureDimension::TextureCube);
				CHECK(mirrored->Skybox->getDesc().format == nvrhi::Format::RGBA16_FLOAT);
				CHECK(mirrored->SkyboxMipCount == 3);
				CHECK(mirrored->SpecularMipCount == EnvironmentData::SpecularMipCount);
				CHECK(mirrored->IrradianceSH9[0] == glm::vec3(1.0f, 2.0f, 3.0f));
				CHECK(cache.GetStats().EnvironmentCount == 1);
				CHECK(cache.GetEnvironment(AssetHandle()) == nullptr);
				// An unknown handle: the cache reports the load's NotFound itself, once (GpuResourceCache.h).
				{
					Test::ExpectLog missing(LogLevel::Error, "ASSET_MISSING");
					CHECK(cache.GetEnvironment(AssetHandle(0x9299)) == nullptr);
					CHECK(cache.GetEnvironment(AssetHandle(0x9299)) == nullptr);
					CHECK(missing.GetMatchCount() == 1);
				}
				const std::span<const AssetDiagnostic> diagnostics = assets.GetDiagnostics();
				CHECK(std::ranges::count_if(diagnostics, [](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Code == AssetMissingCode && diagnostic.Asset == AssetHandle(0x9299);
				}) == 1);
				// An asset of another type is not an environment.
				{
					Test::ExpectLog mismatch(LogLevel::Error, "ASSET_TYPE_MISMATCH");
					CHECK(cache.GetEnvironment(BuiltinAssetHandles::CubeMesh) == nullptr);
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16; stream Q-B).
		TEST_CASE("GpuResourceCache: an irradiance volume mirrors its probes' coefficients, and a null, unloadable or inconsistent one is null"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			// 2 x 2 x 3 probes whose coefficient k of probe i is (i, k, i + k).
			IrradianceVolumeData volume;
			volume.CountX = 2;
			volume.CountY = 2;
			volume.CountZ = 3;
			volume.Size = glm::vec3(4.0f, 2.0f, 6.0f);
			for (uint32_t probe = 0; probe < 12; ++probe)
			{
				IrradianceProbe coefficients;
				for (uint32_t index = 0; index < 9; ++index)
					coefficients.SH9[index] = glm::vec3(static_cast<float>(probe), static_cast<float>(index), static_cast<float>(probe + index));
				volume.Probes.push_back(coefficients);
			}
			IrradianceVolumeData inconsistent = volume;
			inconsistent.Probes.pop_back();
			assets.Publish(AssetHandle(0x9301), CreateRef<IrradianceVolumeData>(std::move(volume)));
			assets.Publish(AssetHandle(0x9302), CreateRef<IrradianceVolumeData>(std::move(inconsistent)));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				const GpuIrradianceVolume* mirrored = cache.GetIrradianceVolume(AssetHandle(0x9301));
				REQUIRE(mirrored != nullptr);
				CHECK(mirrored->CountX == 2);
				CHECK(mirrored->CountY == 2);
				CHECK(mirrored->CountZ == 3);
				REQUIRE(mirrored->Probes != nullptr);
				const uint64_t byteSize = 12 * 9 * sizeof(glm::vec4);
				CHECK(mirrored->Probes->getDesc().byteSize == byteSize);
				CHECK(mirrored->Probes->getDesc().structStride == sizeof(glm::vec4));
				const Result<std::vector<std::byte>> bytes = Test::ReadBufferBytes(gpu.GetDevice(), *mirrored->Probes, byteSize);
				REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
				std::vector<glm::vec4> read(12 * 9);
				std::memcpy(read.data(), bytes->data(), byteSize);
				for (uint32_t probe = 0; probe < 12; ++probe)
				{
					for (uint32_t index = 0; index < 9; ++index)
					{
						CAPTURE(probe);
						CAPTURE(index);
						CHECK(read[probe * 9 + index]
							== glm::vec4(static_cast<float>(probe), static_cast<float>(index), static_cast<float>(probe + index), 0.0f));
					}
				}
				CHECK(cache.GetIrradianceVolume(AssetHandle(0x9301)) == mirrored);
				CHECK(cache.GetStats().IrradianceVolumeCount == 1);
				CHECK(cache.GetIrradianceVolume(AssetHandle()) == nullptr);
				// An unknown handle, an asset of another type and a volume whose probes disagree with its counts: each reported
				// once (GpuResourceCache.h), with GetOrPlaceholder's codes.
				{
					Test::ExpectLog missing(LogLevel::Error, "ASSET_MISSING");
					CHECK(cache.GetIrradianceVolume(AssetHandle(0x9399)) == nullptr);
					CHECK(cache.GetIrradianceVolume(AssetHandle(0x9399)) == nullptr);
					CHECK(missing.GetMatchCount() == 1);
				}
				{
					Test::ExpectLog mismatch(LogLevel::Error, "ASSET_TYPE_MISMATCH");
					CHECK(cache.GetIrradianceVolume(BuiltinAssetHandles::CubeMesh) == nullptr);
				}
				{
					Test::ExpectLog failed(LogLevel::Error, "ASSET_UPLOAD_FAILED");
					CHECK(cache.GetIrradianceVolume(AssetHandle(0x9302)) == nullptr);
					CHECK(cache.GetIrradianceVolume(AssetHandle(0x9302)) == nullptr);
					CHECK(failed.GetMatchCount() == 1);
				}
				// Collected like the other mirrors: unused for a period, it is released; a device loss drops it, and the next
				// Get uploads it again.
				cache.CollectStale();
				CHECK(cache.GetIrradianceVolume(AssetHandle(0x9301)) != nullptr);
				cache.CollectStale(true);
				cache.CollectStale(true);
				CHECK(cache.GetStats().IrradianceVolumeCount == 0);
				CHECK(cache.GetIrradianceVolume(AssetHandle(0x9301)) != nullptr);
				cache.ReleaseGpuObjects();
				CHECK(cache.GetStats().IrradianceVolumeCount == 0);
				CHECK(cache.GetIrradianceVolume(AssetHandle(0x9301)) != nullptr);
				CHECK(cache.GetStats().IrradianceVolumeCount == 1);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: a device loss drops every mirror, and the next Get uploads it again"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, decision 27): the asset mirrors are uploaded again from the loaded assets, and a material's
			// mirror gets a generation never used before, so no binding set from before the loss is found again.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GpuResourceCache& cache = setup.Cache();
			const uint64_t generation = cache.GetMaterial(UUID(8911)).Generation;
			CHECK(cache.GetMesh(UUID(8910)).VertexBuffer != nullptr);
			CHECK(cache.GetStats().MeshCount == 1);

			cache.ReleaseGpuObjects();
			CHECK(cache.GetStats().MeshCount == 0);
			CHECK(cache.GetStats().MaterialCount == 0);
			const GpuMesh& mesh = cache.GetMesh(UUID(8910));
			CHECK(mesh.VertexBuffer != nullptr);
			CHECK_FALSE(mesh.IsPlaceholder);
			CHECK(cache.GetMaterial(UUID(8911)).Generation > generation);
		}

		TEST_CASE("GpuResourceCache: Release leaves none of its mirrors' GPU objects alive" * doctest::test_suite(Test::GpuSuite))
		{
			// The placement rule's place 3 (C18.3, decision 27): once the cache released, the recovery finds no tracked object
			// of it alive, so nothing the cache's destruction would free outlives its Release: meshes, textures, materials'
			// constants and environments' cubes, placeholders included.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::InMemoryAssetManager assets;
			assets.Publish(AssetHandle(0x9301), MakeTexture(60));
			assets.Publish(AssetHandle(0x9302), MakeEnvironment());
			const auto countLive = [&device]()
			{
				device.WaitForIdle();
				device.RunGarbageCollection();
				std::array<uint64_t, GpuResourceTypeCount> counts{};
				for (size_t type = 0; type < GpuResourceTypeCount; ++type)
					counts[type] = device.GetResourceTracker().GetLiveCount(static_cast<GpuResourceType>(type));
				return counts;
			};
			Scope<GpuResourceCache> cache = CreateScope<GpuResourceCache>(device, assets);
			CHECK(cache->GetMesh(AssetHandle()).VertexBuffer != nullptr);
			CHECK(cache->GetTexture(AssetHandle(0x9301)).Texture != nullptr);
			CHECK(cache->GetMaterial(AssetHandle()).Constants != nullptr);
			CHECK(cache->GetEnvironment(AssetHandle(0x9302)) != nullptr);
			const std::array<uint64_t, GpuResourceTypeCount> loaded = countLive();

			cache->ReleaseGpuObjects();
			const GpuResourceCacheStats stats = cache->GetStats();
			CHECK(stats.MeshCount == 0);
			CHECK(stats.TextureCount == 0);
			CHECK(stats.MaterialCount == 0);
			CHECK(stats.EnvironmentCount == 0);
			const std::array<uint64_t, GpuResourceTypeCount> released = countLive();
			const std::string releasedCounts = device.GetResourceTracker().DescribeLiveCounts();
			INFO("live after the release: ", releasedCounts);
			CHECK(std::accumulate(released.begin(), released.end(), uint64_t{ 0 }) < std::accumulate(loaded.begin(), loaded.end(), uint64_t{ 0 }));
			cache.reset();
			const std::array<uint64_t, GpuResourceTypeCount> destroyed = countLive();
			const std::string destroyedCounts = device.GetResourceTracker().DescribeLiveCounts();
			INFO("live after the destruction: ", destroyedCounts);
			CHECK(released == destroyed);
		}

		TEST_CASE("GpuResourceCache: an upload that fails on a lost device is neither counted nor reported, and is tried again after the release"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, decision 28): a Gpu error on a lost device is the loss, never the asset's out-of-memory, so the cache
			// records no ASSET_UPLOAD_FAILED (an undeclared Error would fail this case) and keeps no failure for the recovered
			// device. The injected texture OOM stands in for the lost device's refused creations; on the device again, the same
			// failure is the asset's own, counted and reported, and Release keeps the count.
			Test::HeadlessGpuFixture gpu({ .SynchronizationValidation = true, .InjectFault = GpuFault::OomTexture, .FramesInFlight = 2 });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::InMemoryAssetManager assets;
			const AssetHandle texture(0x7e57'0000'0000'0031);
			const AssetHandle environment(0x7e57'0000'0000'0032);
			const AssetHandle material(0x7e57'0000'0000'0033);
			assets.Publish(texture, MakeTexture(70));
			assets.Publish(environment, MakeEnvironment());
			assets.Publish(material, CreateRef<MaterialData>());
			{
				GpuResourceCache cache(device, assets);
				device.GetDiagnostics().SetDeviceLost(true);
				REQUIRE(device.IsLost());
				const GpuTexture& lost = cache.GetTexture(texture);
				CHECK(lost.IsPlaceholder);
				CHECK(lost.Texture == nullptr);
				CHECK(cache.GetEnvironment(environment) == nullptr);
				// The material's constants buffer, refused like the textures.
				CHECK(cache.GetMaterial(material).Constants == nullptr);
				CHECK(cache.GetStats().UploadFailures == 0);
				CHECK(assets.GetDiagnostics().empty());
				// The recovery: the device exists again, and the cache released its stand-ins.
				device.GetDiagnostics().ClearDeviceLost();
				cache.ReleaseGpuObjects();
				CHECK(cache.GetStats().TextureCount == 0);
				CHECK(cache.GetStats().EnvironmentCount == 0);
				CHECK(cache.GetStats().MaterialCount == 0);

				Test::ExpectLog failed(LogLevel::Error, std::string(AssetUploadFailedCode));
				CHECK(cache.GetTexture(texture).IsPlaceholder);
				CHECK(cache.GetEnvironment(environment) == nullptr);
				CHECK(cache.GetStats().UploadFailures == 3); // the texture, its placeholder and the environment
				CHECK(failed.GetMatchCount() == 2);
				CHECK(std::ranges::count_if(assets.GetDiagnostics(), [](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Code == AssetUploadFailedCode;
				}) == 2);
				cache.ReleaseGpuObjects();
				CHECK(cache.GetStats().UploadFailures == 3);

				// On the device again the material's constants upload; its slots' textures fail with the injected
				// out-of-memory as the texture did, reported under the declared ASSET_UPLOAD_FAILED.
				CHECK(cache.GetMaterial(material).Constants != nullptr);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: a mesh whose buffer creation fails leaves no command list alive, and the next device loss recovers"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3; Docs/Decisions/0029-m18-contract.md decision 39): an upload's command list is executed whatever its
			// buffers' creations returned. Closed and never executed, it kept itself and the vertex buffer it wrote alive, an
			// out-of-memory upload (§8.14 item 7: a placeholder and a diagnostic) poisoned every later recovery, whose live
			// check refused it. The index buffers fail as out-of-memory without reaching Vulkan (Test::BufferCreationFails).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::InMemoryAssetManager assets;
			GpuResourceCache cache(device, assets);
			const auto countLive = [&device]()
			{
				device.WaitForIdle();
				device.RunGarbageCollection();
				std::array<uint64_t, GpuResourceTypeCount> counts{};
				for (size_t type = 0; type < GpuResourceTypeCount; ++type)
					counts[type] = device.GetResourceTracker().GetLiveCount(static_cast<GpuResourceType>(type));
				return counts;
			};
			const std::array<uint64_t, GpuResourceTypeCount> before = countLive();
			{
				Test::ExpectLog failed(LogLevel::Error, std::string(AssetUploadFailedCode));
				const Test::BufferCreationFails failing([](const VkBufferCreateInfo& info)
				{
					return (info.usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0;
				});
				const GpuMesh& mesh = cache.GetMesh(BuiltinAssetHandles::CubeMesh);
				CHECK(mesh.IsPlaceholder);
				CHECK(mesh.IndexBuffer == nullptr);
				CHECK(cache.GetStats().UploadFailures > 0);
			}
			const std::array<uint64_t, GpuResourceTypeCount> after = countLive();
			INFO("live after the failed upload: ", device.GetResourceTracker().DescribeLiveCounts());
			CHECK(after == before);

			device.GetDiagnostics().SetDeviceLost(true);
			device.ReportDeviceLost("GpuResourceCacheTests");
			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			const GpuMesh& uploaded = cache.GetMesh(BuiltinAssetHandles::CubeMesh);
			CHECK_FALSE(uploaded.IsPlaceholder);
			CHECK(uploaded.IndexBuffer != nullptr);
			device.RunGarbageCollection();
		}

		TEST_CASE("GpuResourceCache: TrimUnused releases the mirrors of replaced versions and unloaded assets and keeps the current ones"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, Docs/Decisions/0029-m18-contract.md decision 43): "unreferenced uploads" are trimmed when a
			// request ran out of GPU memory: a replaced version and an asset the manager no longer holds, never the mirror of a
			// loaded, current asset, which the next Render would upload again at once, nor a placeholder. The device's trim
			// reaches the cache through its registration.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			const AssetHandle kept(0x7e57'0000'0000'0011);
			const AssetHandle replaced(0x7e57'0000'0000'0012);
			const AssetHandle removed(0x7e57'0000'0000'0013);
			assets.Publish(kept, MakeTexture(10));
			assets.Publish(replaced, MakeTexture(20));
			assets.Publish(removed, MakeTexture(30));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				REQUIRE(cache.GetTexture(kept).Texture != nullptr);
				REQUIRE(cache.GetTexture(replaced).Texture != nullptr);
				REQUIRE(cache.GetTexture(removed).Texture != nullptr);
				static_cast<void>(cache.GetMesh(AssetHandle())); // the placeholder (Cube) mirror
				assets.Publish(replaced, MakeTexture(21));
				REQUIRE(cache.GetTexture(replaced).Version == 2);
				assets.Remove(removed);
				REQUIRE(cache.GetStats().TextureCount == 4);

				const nvrhi::TextureHandle current = cache.GetTexture(kept).Texture;
				cache.TrimUnused();
				CHECK(cache.GetStats().TextureCount == 2); // the kept asset and the replaced asset's current version
				CHECK(cache.GetStats().MeshCount == 1);    // the placeholder stays
				CHECK(cache.GetTexture(kept).Texture == current);

				// A dropped mirror is uploaded again by its next Get.
				assets.Publish(removed, MakeTexture(31));
				CHECK_FALSE(cache.GetTexture(removed).IsPlaceholder);
				CHECK(cache.GetStats().TextureCount == 3);

				// The device's trim reaches the cache through its registration: the mirror of an asset unloaded again goes, and
				// what is current stays.
				assets.Remove(removed);
				gpu.GetDevice().TrimUnused();
				CHECK(cache.GetStats().TextureCount == 2);
				CHECK(cache.GetTexture(kept).Texture == current);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M23 (C23.1, C-MESH2; Docs/Decisions/0034-m23-contract.md decision 9, stream M): a mesh without MeshData version 2's
		// attribute stream binds the cache's one shared default stream (TexCoord1 (0, 0), opaque white), which holds as many
		// entries as the largest such mesh has vertices; a mesh with the stream uploads its own. Clear drops the shared stream
		// with the mirrors.
		TEST_CASE("GpuResourceCache: meshes without the attribute stream share one default stream, grown to the largest of them"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			// A strip of `quads` unit quads along X, with the attribute stream when `painted`.
			const auto makeStrip = [](uint32_t quads, bool painted)
			{
				auto mesh = CreateRef<MeshData>();
				for (uint32_t quad = 0; quad < quads; ++quad)
				{
					const auto base = static_cast<uint32_t>(mesh->Vertices.size());
					const float left = static_cast<float>(quad);
					for (const glm::vec2 corner : { glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1) })
					{
						const glm::vec3 position(left + corner.x, corner.y, 0.0f);
						mesh->Vertices.push_back({ .Position = position, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = corner });
						mesh->Bounds.Extend(position);
						if (painted)
							mesh->Attributes.push_back({ .TexCoord1 = corner, .Color = { corner.x, corner.y, 0.5f, 1.0f } });
					}
					mesh->Indices.insert(mesh->Indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
				}
				mesh->Submeshes = { { .IndexCount = static_cast<uint32_t>(mesh->Indices.size()), .Bounds = mesh->Bounds } };
				mesh->Slots = { {} };
				REQUIRE_MESSAGE(ValidateMeshData(*mesh).has_value(), ValidateMeshData(*mesh).error().ToString());
				return mesh;
			};
			const AssetHandle small(0x5A7E000000000011ULL);
			const AssetHandle large(0x5A7E000000000012ULL);
			const AssetHandle smallAgain(0x5A7E000000000013ULL);
			const AssetHandle painted(0x5A7E000000000014ULL);
			assets.Publish(small, makeStrip(1, false));
			assets.Publish(large, makeStrip(2, false));
			assets.Publish(smallAgain, makeStrip(1, false));
			assets.Publish(painted, makeStrip(1, true));
			constexpr uint64_t Entry = sizeof(MeshVertexAttributes);
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				const nvrhi::BufferHandle first = cache.GetMesh(small).AttributeBuffer;
				REQUIRE(first != nullptr);
				CHECK(first->getDesc().byteSize == 4 * Entry);
				CHECK(first->getDesc().isVertexBuffer);
				// A larger mesh grows the shared stream; the smaller mesh keeps the one it took, and later meshes share the new one.
				const nvrhi::BufferHandle grown = cache.GetMesh(large).AttributeBuffer;
				REQUIRE(grown != nullptr);
				CHECK(grown != first);
				CHECK(grown->getDesc().byteSize == 8 * Entry);
				CHECK(cache.GetMesh(small).AttributeBuffer == first);
				CHECK(cache.GetMesh(smallAgain).AttributeBuffer == grown);
				// A mesh with the stream has its own buffer of one entry per vertex.
				const GpuMesh& own = cache.GetMesh(painted);
				REQUIRE(own.AttributeBuffer != nullptr);
				CHECK(own.AttributeBuffer != grown);
				CHECK(own.AttributeBuffer->getDesc().byteSize == 4 * Entry);
				// The placeholder (Cube, 24 vertices, no stream) grows it again.
				CHECK(cache.GetMesh(AssetHandle()).AttributeBuffer->getDesc().byteSize == 24 * Entry);

				// A stream whose creation runs out of memory fails the mesh's upload, as its other buffers' would: the
				// placeholder stands in, the failure is reported, and the shared stream is kept.
				const AssetHandle failing(0x5A7E000000000015ULL);
				assets.Publish(failing, makeStrip(1, true));
				{
					Test::ExpectLog failed(LogLevel::Error, std::string(AssetUploadFailedCode));
					const Test::BufferCreationFails failure([](const VkBufferCreateInfo& info)
					{
						return info.size == 4 * sizeof(MeshVertexAttributes);
					});
					const GpuMesh& placeholder = cache.GetMesh(failing);
					CHECK(placeholder.IsPlaceholder);
					CHECK(placeholder.AttributeBuffer != nullptr);
					CHECK(cache.GetStats().UploadFailures == 1);
				}
				CHECK(cache.GetMesh(smallAgain).AttributeBuffer == grown);

				// Clear drops the shared stream with the mirrors: the next mesh without one starts a stream of its own size.
				cache.Clear();
				CHECK(cache.GetMesh(small).AttributeBuffer->getDesc().byteSize == 4 * Entry);

				// A collection keeps the shared stream while a mesh binds it, and drops it with the last such mesh.
				const nvrhi::BufferHandle shared = cache.GetMesh(small).AttributeBuffer;
				cache.CollectStale(true);
				CHECK(cache.GetMesh(small).AttributeBuffer == shared);
				CHECK(cache.GetMesh(large).AttributeBuffer->getDesc().byteSize == 8 * Entry);
				cache.CollectStale(true);
				cache.CollectStale(true);
				CHECK(cache.GetStats().MeshCount == 0);
				// Released: the next mesh starts a stream of its own size, not the 8-entry one the collection would otherwise keep.
				CHECK(cache.GetMesh(small).AttributeBuffer->getDesc().byteSize == 4 * Entry);

				// TrimUnused (an out-of-memory trim) keeps the shared stream while a remaining mesh binds it and releases it with
				// the last one. It keeps the mirrors of loaded assets, so the meshes' assets are unloaded first.
				cache.Clear();
				CHECK(cache.GetMesh(smallAgain).AttributeBuffer->getDesc().byteSize == 4 * Entry);
				const nvrhi::BufferHandle current = cache.GetMesh(large).AttributeBuffer;
				REQUIRE(current->getDesc().byteSize == 8 * Entry);
				assets.Remove(smallAgain);
				cache.TrimUnused();
				CHECK(cache.GetStats().MeshCount == 1);
				CHECK(cache.GetMesh(small).AttributeBuffer == current);
				assets.Remove(small);
				assets.Remove(large);
				cache.TrimUnused();
				CHECK(cache.GetStats().MeshCount == 0);
				assets.Publish(small, makeStrip(1, false));
				CHECK(cache.GetMesh(small).AttributeBuffer->getDesc().byteSize == 4 * Entry);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M23 (decision 7, the recreation protocol's place 3; stream M): the attribute streams are GpuResourceCache mirrors,
		// so a device loss drops them, the shared default stream included, and the next GetMesh uploads them again; the
		// render after the recovery equals the one before.
		TEST_CASE("GpuResourceCache: a device loss drops the attribute streams, and the next GetMesh uploads them again"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::SceneRendererIntegrationFixture setup(gpu);
			// The fixture's quad with the attribute stream.
			auto painted = CreateRef<MeshData>();
			for (const glm::vec3& position : { glm::vec3(-1, -1, 0), glm::vec3(1, -1, 0), glm::vec3(1, 1, 0), glm::vec3(-1, 1, 0) })
			{
				const float shade = static_cast<float>(painted->Vertices.size()) * 0.25f;
				painted->Vertices.push_back({ .Position = position, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 0 } });
				painted->Attributes.push_back({ .TexCoord1 = glm::vec2(0.5f), .Color = glm::vec4(shade, 1.0f, 0.5f, 1.0f) });
			}
			painted->Indices = { 0, 1, 2, 0, 2, 3 };
			painted->Bounds = { { -1, -1, 0 }, { 1, 1, 0 } };
			painted->Submeshes = { { .IndexCount = 6, .Bounds = painted->Bounds } };
			painted->Slots = { {} };
			const UUID paintedHandle(0x5A7E000000000021ULL);
			setup.Assets().Publish(paintedHandle, painted);

			uint64_t frame = 60;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &setup, &frame, paintedHandle]() -> Result<std::vector<std::byte>>
			{
				// The fixture's quad (the default stream) on the left, the painted one on the right.
				RenderSnapshot snapshot = setup.Snapshot();
				snapshot.FrameIndex = ++frame;
				snapshot.Meshes.front().World = glm::translate(glm::mat4(1.0f), glm::vec3(-1.1f, 0.0f, -4.0f));
				MeshDrawItem right = snapshot.Meshes.front();
				right.World = glm::translate(glm::mat4(1.0f), glm::vec3(1.1f, 0.0f, -4.0f));
				right.Mesh = paintedHandle;
				right.Entity = UUID(40);
				right.PickId = 2;
				snapshot.Meshes.push_back(right);
				// Unsaturated, so the painted quad's colours show: no ambient light, a dim sun.
				snapshot.Environment.FallbackColor = glm::vec3(0.0f);
				snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional, .Intensity = 0.4f, .Direction = glm::vec3(0.0f, 0.0f, -1.0f) });
				ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
				list->open();
				const Status rendered = setup.Renderer().Render(*list, snapshot);
				list->close();
				setup.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
				ENGINE_TRY(rendered);
				Readback readback(device);
				ENGINE_TRY_ASSIGN(const Image pixels, readback.ReadTexture(*setup.Renderer().GetFinalTexture()));
				return pixels.Pixels;
			};
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());

			// Both meshes have their streams again, the painted one its own, the plain one the shared default.
			const nvrhi::BufferHandle plain = setup.Cache().GetMesh(UUID(8910)).AttributeBuffer;
			const GpuMesh& mirror = setup.Cache().GetMesh(paintedHandle);
			REQUIRE(plain != nullptr);
			REQUIRE(mirror.AttributeBuffer != nullptr);
			CHECK_FALSE(mirror.IsPlaceholder);
			CHECK(mirror.AttributeBuffer != plain);
			// The painted quad's colours show: the two halves of the image differ.
			const std::vector<std::byte>& image = report->After;
			const auto pixel = [&image](uint32_t x, uint32_t y)
			{
				return std::to_integer<int>(image[(static_cast<size_t>(y) * 64 + x) * 4]) + std::to_integer<int>(image[(static_cast<size_t>(y) * 64 + x) * 4 + 1]) * 256
					+ std::to_integer<int>(image[(static_cast<size_t>(y) * 64 + x) * 4 + 2]) * 65536;
			};
			CHECK(pixel(18, 32) != pixel(46, 32));
		}

		// M23 (C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27, stream C): a mesh with a skin and morph
		// targets uploads them for the Skinning pass (its skin one MeshSkinVertex per vertex, its morph deltas target by
		// target, both raw buffers, and its vertices readable as a raw buffer too), with their counts and the deform bounds the
		// Prepare stage culls and resolves weights with; a mesh with neither has none of them.
		TEST_CASE("GpuResourceCache: a skinned and morphed mesh uploads its skin, its morph deltas and its deform bounds"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			// A quad skinned to two joints (its left column to joint 0, its right to joint 1), with two morph targets.
			auto skinned = CreateRef<MeshData>();
			for (const glm::vec2 corner : { glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1) })
			{
				const glm::vec3 position(corner, 0.0f);
				skinned->Vertices.push_back({ .Position = position, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = corner });
				skinned->Bounds.Extend(position);
				skinned->Skin.push_back({ .Joints = { static_cast<uint16_t>(corner.x), 0, 0, 0 }, .Weights = { 1, 0, 0, 0 } });
			}
			skinned->SkinJointCount = 2;
			skinned->Indices = { 0, 1, 2, 0, 2, 3 };
			skinned->Submeshes = { { .IndexCount = 6, .Bounds = skinned->Bounds } };
			skinned->Slots = { {} };
			for (const float lift : { 0.25f, -0.5f })
			{
				MeshMorphTarget target{ .Name = std::format("Lift{}", lift), .DefaultWeight = lift };
				for (uint32_t vertex = 0; vertex < 4; ++vertex)
					target.Deltas.push_back({ .Position = { 0.0f, 0.0f, lift * static_cast<float>(vertex) } });
				skinned->MorphTargets.push_back(std::move(target));
			}
			REQUIRE_MESSAGE(ValidateMeshData(*skinned).has_value(), ValidateMeshData(*skinned).error().ToString());
			const AssetHandle skinnedHandle(0x5A7E000000000021ULL);
			const AssetHandle plainHandle(0x5A7E000000000022ULL);
			assets.Publish(skinnedHandle, skinned);
			auto plain = CreateRef<MeshData>(*skinned);
			plain->Skin.clear();
			plain->SkinJointCount = 0;
			plain->MorphTargets.clear();
			assets.Publish(plainHandle, plain);
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				const GpuMesh mirror = cache.GetMesh(skinnedHandle);
				REQUIRE_FALSE(mirror.IsPlaceholder);
				REQUIRE(mirror.SkinBuffer != nullptr);
				REQUIRE(mirror.MorphBuffer != nullptr);
				CHECK(mirror.SkinJointCount == 2);
				CHECK(mirror.MorphTargetCount == 2);
				CHECK(mirror.VertexBuffer->getDesc().canHaveRawViews);
				CHECK(mirror.SkinBuffer->getDesc().canHaveRawViews);
				CHECK(mirror.MorphBuffer->getDesc().canHaveRawViews);
				CHECK(mirror.DeformBounds == ComputeMeshDeformBounds(*skinned));
				CHECK(mirror.DeformBounds.DefaultWeights == std::vector<float>{ 0.25f, -0.5f });
				// The buffers hold the skin and the deltas, target by target, byte for byte.
				const Result<std::vector<std::byte>> skin = Test::ReadBufferBytes(gpu.GetDevice(), *mirror.SkinBuffer, 4 * sizeof(MeshSkinVertex));
				REQUIRE_MESSAGE(skin.has_value(), skin.error().ToString());
				CHECK(std::memcmp(skin->data(), skinned->Skin.data(), skin->size()) == 0);
				const Result<std::vector<std::byte>> morphs = Test::ReadBufferBytes(gpu.GetDevice(), *mirror.MorphBuffer, 8 * sizeof(MeshMorphDelta));
				REQUIRE_MESSAGE(morphs.has_value(), morphs.error().ToString());
				CHECK(std::memcmp(morphs->data(), skinned->MorphTargets[0].Deltas.data(), 4 * sizeof(MeshMorphDelta)) == 0);
				CHECK(std::memcmp(morphs->data() + 4 * sizeof(MeshMorphDelta), skinned->MorphTargets[1].Deltas.data(), 4 * sizeof(MeshMorphDelta)) == 0);

				const GpuMesh& rigid = cache.GetMesh(plainHandle);
				CHECK(rigid.SkinBuffer == nullptr);
				CHECK(rigid.MorphBuffer == nullptr);
				CHECK(rigid.SkinJointCount == 0);
				CHECK(rigid.MorphTargetCount == 0);
				CHECK(rigid.DeformBounds == MeshDeformBounds{});
				CHECK_FALSE(rigid.VertexBuffer->getDesc().canHaveRawViews);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// W23.3's review: the Skinning pass addresses a deformable mesh's bytes with 32-bit offsets, so a mesh whose vertices
		// or morph deltas would exceed 4 GiB is refused before any buffer is made: the placeholder draws and the failure is
		// reported. The size comes from the counts alone, so a mesh built in memory with 1,000 morph targets over 120,000
		// vertices (4.3 GB of deltas, none of them held) reaches the refusal without the memory a real one would take.
		TEST_CASE("GpuResourceCache: a deformable mesh beyond the Skinning pass's 4 GiB is refused, and its placeholder draws"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			auto huge = CreateRef<MeshData>();
			huge->Vertices.resize(120000);
			huge->Indices = { 0, 1, 2 };
			huge->Bounds = { glm::vec3(0.0f), glm::vec3(0.0f) };
			huge->Submeshes = { { .IndexCount = 3, .Bounds = huge->Bounds } };
			huge->Slots = { {} };
			huge->MorphTargets.resize(1000);
			REQUIRE(static_cast<uint64_t>(huge->MorphTargets.size()) * huge->Vertices.size() * sizeof(MeshMorphDelta) > std::numeric_limits<uint32_t>::max());
			const AssetHandle hugeHandle(0x5A7E000000000023ULL);
			assets.Publish(hugeHandle, huge);
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				const Test::ExpectLog refused(LogLevel::Error, std::string(AssetUploadFailedCode));
				const GpuMesh& mirror = cache.GetMesh(hugeHandle);
				CHECK(mirror.IsPlaceholder);
				CHECK(mirror.MorphBuffer == nullptr);
				CHECK(cache.GetStats().UploadFailures == 1);
				const std::span<const AssetDiagnostic> diagnostics = assets.GetDiagnostics();
				const auto found = std::ranges::find_if(diagnostics, [hugeHandle](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Code == AssetUploadFailedCode && diagnostic.Asset == hugeHandle;
				});
				REQUIRE(found != diagnostics.end());
				CHECK(found->Message.contains("4 GiB"));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M26 (C26.1, R-LUT; Docs/Decisions/0037-m26-contract.md decision 15, stream X-B): GetLut keeps a LUT as GetEnvironment
		// keeps an environment: a Size^3 RGBA16_FLOAT 3D texture with the table's domain per (handle, version), no placeholder,
		// each failure reported once, released by CollectStale, TrimUnused and the recreation protocol's Release.
		TEST_CASE("GpuResourceCache: a LUT's mirror is its table as a 3D texture per version, and a null or unloadable LUT has none"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			const AssetHandle handle(0x9401);
			LutData lut = GenerateBuiltinLut(BuiltinLut::Identity);
			lut.DomainMin = glm::vec3(0.0f, -1.0f, 0.25f);
			lut.DomainMax = glm::vec3(2.0f, 1.0f, 0.75f);
			assets.Publish(handle, CreateRef<LutData>(lut));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				CHECK(cache.GetLut(AssetHandle()) == nullptr);
				const GpuLut* mirror = cache.GetLut(handle);
				REQUIRE(mirror != nullptr);
				REQUIRE(mirror->Texture != nullptr);
				const nvrhi::TextureDesc& desc = mirror->Texture->getDesc();
				CHECK(desc.dimension == nvrhi::TextureDimension::Texture3D);
				CHECK(desc.width == 33);
				CHECK(desc.height == 33);
				CHECK(desc.depth == 33);
				CHECK(desc.mipLevels == 1);
				CHECK(desc.format == nvrhi::Format::RGBA16_FLOAT);
				CHECK(mirror->Size == 33);
				CHECK(mirror->DomainMin == lut.DomainMin);
				CHECK(mirror->DomainMax == lut.DomainMax);
				CHECK(mirror->Version == assets.GetVersion(handle));
				CHECK(cache.GetLut(handle) == mirror);
				CHECK(cache.GetStats().LutCount == 1);
				// The built-in identity LUT, procedural, is served like any LUT.
				const GpuLut* identity = cache.GetLut(BuiltinAssetHandles::IdentityLut);
				REQUIRE(identity != nullptr);
				CHECK(identity->Size == 33);
				CHECK(identity->DomainMin == glm::vec3(0.0f));
				CHECK(identity->DomainMax == glm::vec3(1.0f));
				CHECK(cache.GetStats().LutCount == 2);

				// A new version is a new mirror; the replaced one is stale.
				LutData small = lut;
				small.Size = 2;
				small.Values = { glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 1.0f, 0.0f),
					glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 1.0f), glm::vec3(1.0f) };
				assets.Publish(handle, CreateRef<LutData>(small));
				const GpuLut* replaced = cache.GetLut(handle);
				REQUIRE(replaced != nullptr);
				CHECK(replaced->Size == 2);
				CHECK(replaced->Texture->getDesc().depth == 2);
				CHECK(cache.GetStats().LutCount == 3);
				cache.CollectStale();
				CHECK(cache.GetStats().LutCount == 2);

				// An unknown handle and an asset of another type: no mirror, each reported once (GpuResourceCache.h).
				{
					Test::ExpectLog missing(LogLevel::Error, "ASSET_MISSING");
					CHECK(cache.GetLut(AssetHandle(0x9499)) == nullptr);
					CHECK(cache.GetLut(AssetHandle(0x9499)) == nullptr);
					CHECK(missing.GetMatchCount() == 1);
				}
				CHECK(std::ranges::count_if(assets.GetDiagnostics(), [](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Code == AssetMissingCode && diagnostic.Asset == AssetHandle(0x9499);
				}) == 1);
				{
					Test::ExpectLog mismatch(LogLevel::Error, "ASSET_TYPE_MISMATCH");
					CHECK(cache.GetLut(BuiltinAssetHandles::CubeMesh) == nullptr);
				}

				// Unused mirrors go at a scene unload's collection; the recreation protocol's Release drops every one, and the
				// next GetLut uploads it again.
				CHECK(cache.GetLut(handle) != nullptr);
				cache.CollectStale(true);
				cache.CollectStale(true);
				CHECK(cache.GetStats().LutCount == 0);
				CHECK(cache.GetLut(handle) != nullptr);
				cache.ReleaseGpuObjects();
				CHECK(cache.GetStats().LutCount == 0);
				const GpuLut* again = cache.GetLut(handle);
				REQUIRE(again != nullptr);
				CHECK(again->Texture != nullptr);
				CHECK(cache.GetStats().LutCount == 1);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22, stream T-C): GetHeightfield keeps a heightfield
		// as GetEnvironment keeps an environment: its samples as a Resolution^2 R16_UNORM texture per (handle, version), sample
		// (x, z) at texel (x, z), no placeholder, each failure reported once, released by CollectStale, TrimUnused and the
		// recreation protocol's Release.
		TEST_CASE("GpuResourceCache: a heightfield's mirror is its samples as an R16 texture per version, and a null or unloadable one has none"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			const AssetHandle handle(0x9501);
			// Every sample distinct and its bytes asymmetric, so a transposed or byte-swapped upload shows.
			HeightfieldData heightfield = MakeFlatHeightfield(5);
			for (uint32_t z = 0; z < 5; ++z)
			{
				for (uint32_t x = 0; x < 5; ++x)
					heightfield.Heights[x + 5 * static_cast<size_t>(z)] = static_cast<uint16_t>(x * 4000 + z * 13 + 1);
			}
			assets.Publish(handle, CreateRef<HeightfieldData>(heightfield));
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				CHECK(cache.GetHeightfield(AssetHandle()) == nullptr);
				const GpuHeightfield* mirror = cache.GetHeightfield(handle);
				REQUIRE(mirror != nullptr);
				REQUIRE(mirror->Heights != nullptr);
				const nvrhi::TextureDesc& desc = mirror->Heights->getDesc();
				CHECK(desc.dimension == nvrhi::TextureDimension::Texture2D);
				CHECK(desc.width == 5);
				CHECK(desc.height == 5);
				CHECK(desc.mipLevels == 1);
				CHECK(desc.format == nvrhi::Format::R16_UNORM);
				CHECK(mirror->Resolution == 5);
				CHECK(mirror->Version == assets.GetVersion(handle));
				CHECK(cache.GetHeightfield(handle) == mirror);
				CHECK(cache.GetStats().HeightfieldCount == 1);
				{
					Readback readback(gpu.GetDevice());
					const Result<Image> texels = readback.ReadTexture(*mirror->Heights);
					REQUIRE_MESSAGE(texels.has_value(), texels.error().ToString());
					for (uint32_t z = 0; z < 5; ++z)
					{
						const std::span<const std::byte> row = texels->GetRow(z);
						for (uint32_t x = 0; x < 5; ++x)
						{
							uint16_t sample = 0;
							std::memcpy(&sample, row.data() + 2 * static_cast<size_t>(x), sizeof(sample));
							CHECK(sample == heightfield.Heights[x + 5 * static_cast<size_t>(z)]);
						}
					}
				}

				// A new version is a new mirror; the replaced one is stale.
				assets.Publish(handle, CreateRef<HeightfieldData>(MakeFlatHeightfield(9, 100)));
				const GpuHeightfield* replaced = cache.GetHeightfield(handle);
				REQUIRE(replaced != nullptr);
				CHECK(replaced->Resolution == 9);
				CHECK(replaced->Heights->getDesc().width == 9);
				CHECK(cache.GetStats().HeightfieldCount == 2);
				cache.CollectStale();
				CHECK(cache.GetStats().HeightfieldCount == 1);

				// An unknown handle and an asset of another type: no mirror, each reported once (GpuResourceCache.h).
				{
					Test::ExpectLog missing(LogLevel::Error, "ASSET_MISSING");
					CHECK(cache.GetHeightfield(AssetHandle(0x9599)) == nullptr);
					CHECK(cache.GetHeightfield(AssetHandle(0x9599)) == nullptr);
					CHECK(missing.GetMatchCount() == 1);
				}
				CHECK(std::ranges::count_if(assets.GetDiagnostics(), [](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Code == AssetMissingCode && diagnostic.Asset == AssetHandle(0x9599);
				}) == 1);
				{
					Test::ExpectLog mismatch(LogLevel::Error, "ASSET_TYPE_MISMATCH");
					CHECK(cache.GetHeightfield(BuiltinAssetHandles::CubeMesh) == nullptr);
				}
				// A heightfield whose samples disagree with its resolution (no loader makes one) is refused, never read past, and
				// reported once.
				const AssetHandle inconsistent(0x9502);
				HeightfieldData shortHeightfield = MakeFlatHeightfield(5);
				shortHeightfield.Heights.pop_back();
				assets.Publish(inconsistent, CreateRef<HeightfieldData>(std::move(shortHeightfield)));
				{
					Test::ExpectLog failed(LogLevel::Error, "ASSET_UPLOAD_FAILED");
					CHECK(cache.GetHeightfield(inconsistent) == nullptr);
					CHECK(cache.GetHeightfield(inconsistent) == nullptr);
					CHECK(failed.GetMatchCount() == 1);
				}

				// Unused mirrors go at a scene unload's collection; TrimUnused drops one whose asset was unloaded; the recreation
				// protocol's Release drops every one, and the next GetHeightfield uploads it again.
				CHECK(cache.GetHeightfield(handle) != nullptr);
				cache.CollectStale(true);
				cache.CollectStale(true);
				CHECK(cache.GetStats().HeightfieldCount == 0);
				CHECK(cache.GetHeightfield(handle) != nullptr);
				cache.ReleaseGpuObjects();
				CHECK(cache.GetStats().HeightfieldCount == 0);
				const GpuHeightfield* again = cache.GetHeightfield(handle);
				REQUIRE(again != nullptr);
				CHECK(again->Heights != nullptr);
				CHECK(cache.GetStats().HeightfieldCount == 1);
				assets.Remove(handle);
				cache.TrimUnused();
				CHECK(cache.GetStats().HeightfieldCount == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
