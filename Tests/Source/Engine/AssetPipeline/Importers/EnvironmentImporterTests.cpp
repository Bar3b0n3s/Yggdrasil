#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/EnvironmentImporter.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/TextureCompression.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Support/AssetTestFixture.h"
#include "Support/Fuzz.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// EnvironmentImporter (Architecture §7.4, §8.6; Docs/Decisions/0013-m8-decisions.md decision 9): Radiance .hdr and, since
// version 2, OpenEXR .exr (Docs/Decisions/0034-m23-contract.md decision 14) through the GPU bake, Unsupported without a
// baker; the Compression setting stores the cubes as BC6H or RGBA16F.

namespace Engine {

	namespace {

		// The committed Studio HDRI (Resources/Environments, Poly Haven CC0, 1k).
		Buffer ReadStudio()
		{
			Result<Buffer> hdr = FileSystem::ReadFile(Test::GetRepositoryRoot() / "Resources/Environments/Studio.hdr");
			REQUIRE_MESSAGE(hdr.has_value(), hdr.error().ToString());
			return std::move(*hdr);
		}

		// A fixture of Tests/Data/Assets/Environments.
		Buffer ReadEnvironmentFixture(std::string_view name)
		{
			Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath(std::format("Assets/Environments/{}", name)));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			return std::move(*bytes);
		}

		// Imports `bytes` as project://Assets/Environments/<name> with `settings` and `baker` (null: none).
		Result<ImportResult> ImportEnvironment(Test::AssetTestFixture& fixture, std::string_view name, std::span<const std::byte> bytes,
			const Json& settings, IEnvironmentBaker* baker)
		{
			const std::string relative = std::string("Assets/Environments/") + std::string(name);
			fixture.WriteProjectFile(relative, bytes);
			const Buffer source = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0xe0e7000000000001ull);
			metadata.Type = AssetType::Environment;
			metadata.Importer = std::string(EnvironmentImporter::Id);
			metadata.ImporterVersion = EnvironmentImporter::Version;
			metadata.Settings = VariantValue(settings);
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath(relative),
				.SourceBytes = source,
				.Settings = VariantValue(settings),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = baker,
				.ScriptDiagnostics = nullptr,
			});
			return EnvironmentImporter().Import(context, metadata);
		}

		Json DefaultSettings()
		{
			Json settings = Json::object();
			settings["ClampLuminance"] = false;
			settings["MaxLuminance"] = 1000.0;
			return settings;
		}

		// A flat (uncompressed) Radiance RGBE image of `width` x `height` texels of radiance 1.
		Buffer MakeFlatHdr(uint32_t width, uint32_t height)
		{
			const std::string header = std::format("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {} +X {}\n", height, width);
			const std::span<const std::byte> headerBytes = AsBytes(header);
			Buffer bytes(headerBytes.begin(), headerBytes.end());
			// 1.0 = (128 / 256) * 2^1: mantissa bytes 128, exponent byte 128 + 1.
			constexpr std::array<std::byte, 4> One = { std::byte{ 128 }, std::byte{ 128 }, std::byte{ 128 }, std::byte{ 129 } };
			for (uint32_t index = 0; index < width * height; ++index)
				bytes.insert(bytes.end(), One.begin(), One.end());
			return bytes;
		}

		// A baker that checks the decoded input it receives (the equirectangular shape and finite, non-negative texels) and
		// bakes nothing: the fuzz case's oracle for what the decoders hand on.
		class InputCheckingBaker final : public IEnvironmentBaker
		{
		public:
			Result<EnvironmentData> Bake(const EnvironmentBakeInput& input) override
			{
				CHECK(input.Width == 2 * input.Height);
				CHECK(input.Height > 0);
				CHECK(input.Texels.size() == static_cast<size_t>(input.Width) * input.Height * 3);
				CHECK(std::ranges::all_of(input.Texels, [](float texel)
				{
					return std::isfinite(texel) && texel >= 0.0f;
				}));
				++m_Bakes;
				return std::unexpected(Error(ErrorCode::Unsupported, "the fuzz case bakes nothing"));
			}

			[[nodiscard]] uint64_t GetBakeCount() const { return m_Bakes; }
		private:
			uint64_t m_Bakes = 0;
		};

		// A baker that keeps the decoded input it receives and bakes nothing.
		class CapturingBaker final : public IEnvironmentBaker
		{
		public:
			Result<EnvironmentData> Bake(const EnvironmentBakeInput& input) override
			{
				m_Inputs.push_back(input);
				return std::unexpected(Error(ErrorCode::Unsupported, "the test bakes nothing"));
			}

			[[nodiscard]] const std::vector<EnvironmentBakeInput>& GetInputs() const { return m_Inputs; }
		private:
			std::vector<EnvironmentBakeInput> m_Inputs{};
		};

		// The image the importer hands its baker for `bytes` imported as `name` (a baker that bakes nothing), or the
		// import's error when the decoder refused the source.
		Result<EnvironmentBakeInput> Decode(Test::AssetTestFixture& fixture, std::string_view name, std::span<const std::byte> bytes)
		{
			CapturingBaker baker;
			const Result<ImportResult> imported = ImportEnvironment(fixture, name, bytes, DefaultSettings(), &baker);
			REQUIRE_FALSE(imported.has_value());
			if (baker.GetInputs().empty())
				return std::unexpected(Error(imported.error()));
			REQUIRE(baker.GetInputs().size() == 1);
			CHECK(imported.error().GetCode() == ErrorCode::Unsupported);
			return baker.GetInputs().front();
		}

		// --- OpenEXR images the tests write (the OpenEXR file layout, as Tests/Data/Generate/MakeFormatFixtures.py writes
		// Gradient.exr: magic number, version, header attributes, offset table, chunks) ---------------------------------------

		constexpr int ExrUint = 0;
		constexpr int ExrHalf = 1;
		constexpr int ExrFloat = 2;
		constexpr uint8_t ExrNone = 0;
		constexpr uint8_t ExrRle = 1;
		// A tiled image's level modes (the tiledesc's low nibble; its high nibble is the rounding mode).
		constexpr uint8_t ExrOneLevel = 0;
		constexpr uint8_t ExrMipmapLevels = 1;
		constexpr uint8_t ExrRipmapLevels = 2;

		struct TestExrChannel
		{
			std::string Name{};
			int PixelType = ExrHalf;
			int32_t XSampling = 1;
			int32_t YSampling = 1;
		};

		struct TestExr
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			std::vector<TestExrChannel> Channels{}; // in the file's order (OpenEXR sorts them by name)
			// The value of channel `channel` at texel (x, y) of the top level (lower levels repeat its top-left texels).
			std::function<double(uint32_t x, uint32_t y, size_t channel)> Value{};
			uint8_t Compression = ExrNone; // NONE and RLE are written; any other value goes into the header only
			bool Tiled = false;
			uint32_t TileWidth = 0;
			uint32_t TileHeight = 0;
			uint8_t LevelMode = ExrOneLevel; // tiled: one level, mipmap levels (l, l) or ripmap levels (x, y)
			bool RoundUp = false;            // tiled: the level count and level sizes rounded up instead of down
			uint8_t LineOrder = 0;           // 0 increasing y, 1 decreasing y (the chunks' order in the file)
			int32_t MinX = 0;                // the data window's origin
			int32_t MinY = 0;
			uint8_t ExtraVersionFlags = 0; // 0x08 deep, 0x10 multi-part
			bool HeaderOnly = false;       // no offset table and no chunks
			// When set, a chunkCount attribute of the image's chunk count plus this.
			std::optional<int32_t> ChunkCountDelta{};
		};

		// A written image and the file offset of each chunk, in offset table order.
		struct TestExrFile
		{
			Buffer Bytes{};
			std::vector<size_t> Chunks{};
		};

		void AppendU32(Buffer& out, uint32_t value)
		{
			for (int shift = 0; shift < 32; shift += 8)
				out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
		}

		void AppendI32(Buffer& out, int32_t value)
		{
			AppendU32(out, static_cast<uint32_t>(value));
		}

		void AppendU64(Buffer& out, uint64_t value)
		{
			for (int shift = 0; shift < 64; shift += 8)
				out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
		}

		void AppendName(Buffer& out, std::string_view name)
		{
			const std::span<const std::byte> bytes = AsBytes(name);
			out.insert(out.end(), bytes.begin(), bytes.end());
			out.push_back(std::byte{ 0 });
		}

		void AppendAttribute(Buffer& out, std::string_view name, std::string_view type, const Buffer& value)
		{
			AppendName(out, name);
			AppendName(out, type);
			AppendU32(out, static_cast<uint32_t>(value.size()));
			out.insert(out.end(), value.begin(), value.end());
		}

		void SetI32(Buffer& bytes, size_t offset, int32_t value)
		{
			for (size_t index = 0; index < 4; ++index)
				bytes[offset + index] = static_cast<std::byte>((static_cast<uint32_t>(value) >> (8 * index)) & 0xFFU);
		}

		// One texel value of `pixelType`: a binary16 (round to nearest even), a binary32 or a uint32.
		void AppendValue(Buffer& out, int pixelType, double value)
		{
			if (pixelType == ExrHalf)
			{
				const uint16_t half = Test::DoubleToHalf(value);
				out.push_back(static_cast<std::byte>(half & 0xFFU));
				out.push_back(static_cast<std::byte>(half >> 8));
			}
			else if (pixelType == ExrFloat)
				AppendU32(out, std::bit_cast<uint32_t>(static_cast<float>(value)));
			else
				AppendU32(out, static_cast<uint32_t>(value));
		}

		// OpenEXR's RLE chunk data: the bytes interleaved (even offsets, then odd), delta-coded from the second on, then runs
		// of three to 128 equal bytes (a count byte n - 1 and the byte) and literal stretches of up to 127 bytes (a count byte
		// -n and the bytes).
		Buffer EncodeRle(const Buffer& raw)
		{
			Buffer predicted;
			for (size_t index = 0; index < raw.size(); index += 2)
				predicted.push_back(raw[index]);
			for (size_t index = 1; index < raw.size(); index += 2)
				predicted.push_back(raw[index]);
			for (size_t index = predicted.size(); index-- > 1;)
				predicted[index] = static_cast<std::byte>((std::to_integer<int>(predicted[index]) - std::to_integer<int>(predicted[index - 1]) + 128) & 0xFF);
			Buffer out;
			size_t position = 0;
			while (position < predicted.size())
			{
				size_t run = 1;
				while (position + run < predicted.size() && run < 128 && predicted[position + run] == predicted[position])
					++run;
				if (run >= 3)
				{
					out.push_back(static_cast<std::byte>(run - 1));
					out.push_back(predicted[position]);
					position += run;
					continue;
				}
				size_t literal = 0;
				while (position + literal < predicted.size() && literal < 127)
				{
					const size_t at = position + literal;
					if (at + 2 < predicted.size() && predicted[at] == predicted[at + 1] && predicted[at] == predicted[at + 2])
						break;
					++literal;
				}
				out.push_back(static_cast<std::byte>(static_cast<uint8_t>(-static_cast<int>(literal))));
				out.insert(out.end(), predicted.begin() + static_cast<std::ptrdiff_t>(position), predicted.begin() + static_cast<std::ptrdiff_t>(position + literal));
				position += literal;
			}
			return out;
		}

		// The pixel data of the rectangle at (x0, y0) of `width` x `height` texels of level (`levelX`, `levelY`): each row's
		// channels in turn.
		Buffer EncodePixels(const TestExr& exr, uint32_t levelX, uint32_t levelY, uint32_t x0, uint32_t y0, uint32_t width, uint32_t height)
		{
			Buffer raw;
			for (uint32_t y = y0; y < y0 + height; ++y)
			{
				for (size_t channel = 0; channel < exr.Channels.size(); ++channel)
				{
					for (uint32_t x = x0; x < x0 + width; ++x)
						AppendValue(raw, exr.Channels[channel].PixelType, exr.Value(std::min(x << levelX, exr.Width - 1), std::min(y << levelY, exr.Height - 1), channel));
				}
			}
			if (exr.Compression != ExrRle)
				return raw;
			Buffer encoded = EncodeRle(raw);
			REQUIRE(encoded.size() != raw.size()); // an equal size reads as stored uncompressed
			return encoded;
		}

		// OpenEXR's level size: the top level's edge halved `level` times, rounded down or up, at least 1.
		uint32_t GetTestExrLevelSize(uint32_t topLevel, uint32_t level, bool roundUp)
		{
			const uint32_t scale = 1U << level;
			const uint32_t size = topLevel / scale + (roundUp && topLevel % scale != 0 ? 1U : 0U);
			return std::max(size, 1U);
		}

		// OpenEXR's level count along an edge: floor(log2(size)) + 1 rounding down, ceil(log2(size)) + 1 rounding up.
		uint32_t GetTestExrLevelCount(uint32_t size, bool roundUp)
		{
			return static_cast<uint32_t>(std::bit_width(size)) + (roundUp && !std::has_single_bit(size) ? 1U : 0U);
		}

		// Each chunk of `exr` with its place in the offset table (scanline blocks of one line; tiles in the levels' order,
		// ripmap levels x fastest, and row by row within a level).
		std::vector<std::pair<size_t, Buffer>> WriteTestExrChunks(const TestExr& exr)
		{
			std::vector<std::pair<size_t, Buffer>> chunks;
			if (!exr.Tiled)
			{
				for (uint32_t y = 0; y < exr.Height; ++y)
				{
					Buffer chunk;
					AppendI32(chunk, exr.MinY + static_cast<int32_t>(y));
					const Buffer data = EncodePixels(exr, 0, 0, 0, y, exr.Width, 1);
					AppendU32(chunk, static_cast<uint32_t>(data.size()));
					chunk.insert(chunk.end(), data.begin(), data.end());
					chunks.emplace_back(y, std::move(chunk));
				}
				if (exr.LineOrder == 1)
					std::ranges::reverse(chunks);
				return chunks;
			}
			std::vector<std::pair<uint32_t, uint32_t>> levels;
			if (exr.LevelMode == ExrMipmapLevels)
			{
				for (uint32_t level = 0; level < GetTestExrLevelCount(std::max(exr.Width, exr.Height), exr.RoundUp); ++level)
					levels.emplace_back(level, level);
			}
			else if (exr.LevelMode == ExrRipmapLevels)
			{
				for (uint32_t levelY = 0; levelY < GetTestExrLevelCount(exr.Height, exr.RoundUp); ++levelY)
				{
					for (uint32_t levelX = 0; levelX < GetTestExrLevelCount(exr.Width, exr.RoundUp); ++levelX)
						levels.emplace_back(levelX, levelY);
				}
			}
			else
				levels.emplace_back(0, 0);
			for (const auto& [levelX, levelY] : levels)
			{
				const uint32_t width = GetTestExrLevelSize(exr.Width, levelX, exr.RoundUp);
				const uint32_t height = GetTestExrLevelSize(exr.Height, levelY, exr.RoundUp);
				for (uint32_t tileY = 0; tileY * exr.TileHeight < height; ++tileY)
				{
					for (uint32_t tileX = 0; tileX * exr.TileWidth < width; ++tileX)
					{
						const uint32_t x0 = tileX * exr.TileWidth;
						const uint32_t y0 = tileY * exr.TileHeight;
						Buffer chunk;
						for (const uint32_t value : { tileX, tileY, levelX, levelY })
							AppendU32(chunk, value);
						const Buffer data = EncodePixels(exr, levelX, levelY, x0, y0, std::min(exr.TileWidth, width - x0), std::min(exr.TileHeight, height - y0));
						AppendU32(chunk, static_cast<uint32_t>(data.size()));
						chunk.insert(chunk.end(), data.begin(), data.end());
						chunks.emplace_back(chunks.size(), std::move(chunk));
					}
				}
			}
			return chunks;
		}

		TestExrFile WriteTestExr(const TestExr& exr)
		{
			// Each chunk with its place in the offset table (none for a header alone).
			const std::vector<std::pair<size_t, Buffer>> chunks = exr.HeaderOnly ? std::vector<std::pair<size_t, Buffer>>() : WriteTestExrChunks(exr);
			Buffer header;
			Buffer channels;
			for (const TestExrChannel& channel : exr.Channels)
			{
				AppendName(channels, channel.Name);
				AppendI32(channels, channel.PixelType);
				AppendU32(channels, 0); // pLinear and three reserved bytes
				AppendI32(channels, channel.XSampling);
				AppendI32(channels, channel.YSampling);
			}
			channels.push_back(std::byte{ 0 });
			Buffer window;
			for (const int32_t value : { exr.MinX, exr.MinY, exr.MinX + static_cast<int32_t>(exr.Width) - 1, exr.MinY + static_cast<int32_t>(exr.Height) - 1 })
				AppendI32(window, value);
			Buffer floatOne;
			AppendU32(floatOne, std::bit_cast<uint32_t>(1.0f));
			Buffer center;
			AppendU64(center, 0);
			AppendAttribute(header, "channels", "chlist", channels);
			AppendAttribute(header, "compression", "compression", { static_cast<std::byte>(exr.Compression) });
			AppendAttribute(header, "dataWindow", "box2i", window);
			AppendAttribute(header, "displayWindow", "box2i", window);
			AppendAttribute(header, "lineOrder", "lineOrder", { static_cast<std::byte>(exr.LineOrder) });
			AppendAttribute(header, "pixelAspectRatio", "float", floatOne);
			AppendAttribute(header, "screenWindowCenter", "v2f", center);
			AppendAttribute(header, "screenWindowWidth", "float", floatOne);
			if (exr.Tiled)
			{
				Buffer tiles;
				AppendU32(tiles, exr.TileWidth);
				AppendU32(tiles, exr.TileHeight);
				tiles.push_back(static_cast<std::byte>(exr.LevelMode | (exr.RoundUp ? 0x10 : 0x00)));
				AppendAttribute(header, "tiles", "tiledesc", tiles);
			}
			if (exr.ChunkCountDelta.has_value())
			{
				Buffer count;
				AppendI32(count, static_cast<int32_t>(chunks.size()) + *exr.ChunkCountDelta);
				AppendAttribute(header, "chunkCount", "int", count);
			}
			header.push_back(std::byte{ 0 });

			TestExrFile file;
			for (const int magic : { 0x76, 0x2f, 0x31, 0x01, 2 })
				file.Bytes.push_back(static_cast<std::byte>(magic));
			file.Bytes.push_back(static_cast<std::byte>((exr.Tiled ? 0x02 : 0x00) | exr.ExtraVersionFlags));
			file.Bytes.push_back(std::byte{ 0 });
			file.Bytes.push_back(std::byte{ 0 });
			file.Bytes.insert(file.Bytes.end(), header.begin(), header.end());
			if (exr.HeaderOnly)
				return file;

			std::vector<uint64_t> offsets(chunks.size());
			uint64_t offset = file.Bytes.size() + 8 * chunks.size();
			for (const auto& [place, chunk] : chunks)
			{
				offsets[place] = offset;
				offset += chunk.size();
			}
			for (const uint64_t entry : offsets)
				AppendU64(file.Bytes, entry);
			file.Chunks.reserve(offsets.size());
			for (const uint64_t entry : offsets)
				file.Chunks.push_back(static_cast<size_t>(entry));
			for (const auto& [place, chunk] : chunks)
				file.Bytes.insert(file.Bytes.end(), chunk.begin(), chunk.end());
			return file;
		}

		// An image whose R, G and B channels hold `reference`'s texels (A, when present, 1).
		TestExr MakeExrOf(const EnvironmentBakeInput& reference, std::vector<TestExrChannel> channels)
		{
			TestExr exr;
			exr.Width = reference.Width;
			exr.Height = reference.Height;
			exr.Channels = std::move(channels);
			std::vector<int> components;
			for (const TestExrChannel& channel : exr.Channels)
				components.push_back(channel.Name == "R" ? 0 : channel.Name == "G" ? 1
						: channel.Name == "B"                                      ? 2
																				   : -1);
			exr.Value = [texels = reference.Texels, width = reference.Width, components](uint32_t x, uint32_t y, size_t channel) -> double
			{
				const int component = components[channel];
				return component < 0 ? 1.0 : static_cast<double>(texels[(static_cast<size_t>(y) * width + x) * 3 + static_cast<size_t>(component)]);
			};
			return exr;
		}

		std::vector<TestExrChannel> MakeChannels(std::initializer_list<std::string_view> names, int pixelType)
		{
			std::vector<TestExrChannel> channels;
			for (const std::string_view name : names)
				channels.push_back({ .Name = std::string(name), .PixelType = pixelType, .XSampling = 1, .YSampling = 1 });
			return channels;
		}

		// A small 2:1 image of half-float B, G and R channels of radiance 0.5.
		TestExr MakeSmallExr()
		{
			TestExr exr;
			exr.Width = 8;
			exr.Height = 4;
			exr.Channels = MakeChannels({ "B", "G", "R" }, ExrHalf);
			exr.Value = [](uint32_t /*x*/, uint32_t /*y*/, size_t /*channel*/) -> double
			{
				return 0.5;
			};
			return exr;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("EnvironmentImporter: claims .hdr and .exr, runs on the main thread and is registered")
		{
			const EnvironmentImporter importer;
			CHECK(importer.CanImport(".hdr"));
			CHECK(importer.CanImport(".EXR"));
			CHECK(importer.RequiresMainThread());
			CHECK(importer.GetMainType() == AssetType::Environment);
			CHECK(EnvironmentImporter::Version == 2);
			ImporterRegistry registry;
			RegisterBuiltinImporters(registry);
			CHECK(registry.FindForExtension(".hdr") == registry.FindById(EnvironmentImporter::Id));
			CHECK(registry.FindForExtension(".exr") == registry.FindById(EnvironmentImporter::Id));
			Test::AssetTestFixture fixture;
			const StructInfo* settings = fixture.GetRegistry().FindStruct("EnvironmentImportSettings");
			REQUIRE(settings != nullptr);
			// M23: Compression after MaxLuminance, a TextureCompression, Auto by default, so earlier settings and the built-ins
			// read as Auto.
			std::vector<std::string> fields;
			for (const Scope<FieldInfo>& field : settings->GetFields())
				fields.push_back(field->GetName());
			CHECK(fields == std::vector<std::string>{ "ClampLuminance", "MaxLuminance", "Compression" });
			const FieldInfo* compression = settings->FindField("Compression");
			REQUIRE(compression != nullptr);
			CHECK(compression->GetType().GetKind() == FieldType::Enum);
			CHECK(compression->GetType().GetEnum() == fixture.GetRegistry().FindEnum<TextureCompression>());
			CHECK(compression->GetType().GetEnum() != nullptr);
			CHECK(EnvironmentImportSettings{}.Compression == TextureCompression::Auto);
		}

		TEST_CASE("EnvironmentImporter: without a baker the import is Unsupported with the GPU hint")
		{
			Test::AssetTestFixture fixture;
			const Result<ImportResult> imported = ImportEnvironment(fixture, "Studio.hdr", ReadStudio(), DefaultSettings(), nullptr);
			REQUIRE_FALSE(imported.has_value());
			CHECK(imported.error().GetCode() == ErrorCode::Unsupported);
			CHECK(imported.error().ToString().contains("start the editor with a GPU once to bake this environment"));
			// An OpenEXR source needs the same bake.
			const Result<ImportResult> exr = ImportEnvironment(fixture, "Gradient.exr", ReadEnvironmentFixture("Gradient.exr"), DefaultSettings(), nullptr);
			REQUIRE_FALSE(exr.has_value());
			CHECK(exr.error().GetCode() == ErrorCode::Unsupported);
			CHECK(exr.error().GetHint() == EnvironmentImporter::GpuHint);
		}

		// M23 rewrote version 1's case ".exr is refused with the Poly Haven hint and broken .hdr files fail precisely": a .exr
		// is decoded now, so its part checks that a .exr that is not an OpenEXR image fails precisely.
		TEST_CASE("EnvironmentImporter: broken .hdr files and a .exr that is not OpenEXR fail precisely")
		{
			Test::AssetTestFixture fixture;
			const Result<ImportResult> exr = ImportEnvironment(fixture, "Sky.exr", AsBytes("v/1\x01"), DefaultSettings(), nullptr);
			REQUIRE_FALSE(exr.has_value());
			CHECK(exr.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(exr.error().ToString().contains("not an OpenEXR image"));
			// A Radiance image under the .exr extension is not read as one.
			const Result<ImportResult> renamed = ImportEnvironment(fixture, "Renamed.exr", MakeFlatHdr(8, 4), DefaultSettings(), nullptr);
			REQUIRE_FALSE(renamed.has_value());
			CHECK(renamed.error().GetCode() == ErrorCode::ImportFailed);

			const Result<ImportResult> garbage = ImportEnvironment(fixture, "Broken.hdr", AsBytes("#?RADIANCE\nnot an image"), DefaultSettings(), nullptr);
			REQUIRE_FALSE(garbage.has_value());
			CHECK(garbage.error().GetCode() == ErrorCode::ImportFailed);

			// An image that is not a Radiance HDR (stbi_loadf would decode a PNG as linear floats), and one that is not 2:1.
			const Result<ImportResult> png = ImportEnvironment(fixture, "Renamed.hdr", Test::MakeTestPng(8, 4), DefaultSettings(), nullptr);
			REQUIRE_FALSE(png.has_value());
			CHECK(png.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(png.error().ToString().contains("not a Radiance HDR image"));
			const Result<ImportResult> square = ImportEnvironment(fixture, "Square.hdr", MakeFlatHdr(8, 8), DefaultSettings(), nullptr);
			REQUIRE_FALSE(square.has_value());
			CHECK(square.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(square.error().ToString().contains("8x8"));
			// Wider than 8,192 texels (decision 21, B): refused from the header alone, before any texel is decoded, so a
			// header without pixel data is enough.
			const Result<ImportResult> wide = ImportEnvironment(fixture, "Wide.hdr",
				AsBytes(std::string_view("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 8193 +X 16386\n")), DefaultSettings(), nullptr);
			REQUIRE_FALSE(wide.has_value());
			CHECK(wide.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(wide.error().ToString().contains("16386x8193"));
			CHECK(wide.error().GetHint() == "use the 8k or a smaller variant of the HDRI");

			// Settings outside their ranges are Validation errors of the settings, before the source is decoded.
			Json clamped = DefaultSettings();
			clamped["ClampLuminance"] = true;
			clamped["MaxLuminance"] = 0.5;
			const Result<ImportResult> invalid = ImportEnvironment(fixture, "Clamped.hdr", MakeFlatHdr(8, 4), clamped, nullptr);
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::Validation);
		}

		// M23 (A-EXR, decision 14): the generated Gradient.exr (half-float scanlines, uncompressed) and GradientZip.exr (ZIP)
		// hold Gradient.hdr's texels as stb_image decodes them, each exact in a binary16, so the decoders hand the baker the
		// same image. So do the layouts the test writes from those texels: float channels beside an alpha channel, RLE,
		// tiles of one level with partial tiles at the edges, mipmapped tiles (the top level is read), and decreasing line
		// order with a data window away from the origin. The generated block fixtures (W23.2's review) hold the same
		// texels each repeated over 4 x 4 texels (128 x 64) in several blocks of every compression the importer reads: 64
		// one-line blocks of NONE, RLE and ZIPS, four 16-line blocks of ZIP and PXR24 and two 32-line blocks of PIZ, so a
		// wrong block height for any of them (GetExrLinesPerBlock) refuses its fixture.
		TEST_CASE("EnvironmentImporter: OpenEXR images decode to the texels Gradient.hdr decodes to")
		{
			Test::AssetTestFixture fixture;
			const Result<EnvironmentBakeInput> reference = Decode(fixture, "Gradient.hdr", ReadEnvironmentFixture("Gradient.hdr"));
			REQUIRE_MESSAGE(reference.has_value(), reference.error().ToString());
			REQUIRE(reference->Width == 32);
			REQUIRE(reference->Height == 16);
			const auto checkSame = [&reference](const Result<EnvironmentBakeInput>& decoded)
			{
				REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
				CHECK(decoded->Width == reference->Width);
				CHECK(decoded->Height == reference->Height);
				CHECK(decoded->Texels == reference->Texels);
			};
			for (const std::string_view name : { "Gradient.exr", "GradientZip.exr" })
			{
				CAPTURE(name);
				checkSame(Decode(fixture, name, ReadEnvironmentFixture(name)));
			}
			constexpr uint32_t BlockScale = 4;
			std::vector<float> scaled;
			for (uint32_t y = 0; y < reference->Height * BlockScale; ++y)
			{
				for (uint32_t x = 0; x < reference->Width * BlockScale; ++x)
				{
					const size_t texel = (static_cast<size_t>(y / BlockScale) * reference->Width + x / BlockScale) * 3;
					scaled.insert(scaled.end(), reference->Texels.begin() + static_cast<std::ptrdiff_t>(texel), reference->Texels.begin() + static_cast<std::ptrdiff_t>(texel + 3));
				}
			}
			for (const std::string_view name : { "GradientBlocksNone.exr", "GradientBlocksRle.exr", "GradientBlocksZips.exr", "GradientBlocksZip.exr",
					 "GradientBlocksPiz.exr", "GradientBlocksPxr24.exr" })
			{
				CAPTURE(name);
				const Result<EnvironmentBakeInput> decoded = Decode(fixture, name, ReadEnvironmentFixture(name));
				CHECK_MESSAGE(decoded.has_value(), (decoded.has_value() ? std::string() : decoded.error().ToString()));
				if (!decoded.has_value())
					continue;
				CHECK(decoded->Width == reference->Width * BlockScale);
				CHECK(decoded->Height == reference->Height * BlockScale);
				CHECK(decoded->Texels == scaled);
			}

			TestExr floats = MakeExrOf(*reference, MakeChannels({ "A", "B", "G", "R" }, ExrFloat));
			TestExr rle = MakeExrOf(*reference, MakeChannels({ "B", "G", "R" }, ExrHalf));
			rle.Compression = ExrRle;
			TestExr tiled = MakeExrOf(*reference, MakeChannels({ "B", "G", "R" }, ExrHalf));
			tiled.Tiled = true;
			tiled.TileWidth = 12; // 32 x 16 in 3 x 2 tiles, the last column and row partial
			tiled.TileHeight = 10;
			TestExr mipmapped = MakeExrOf(*reference, MakeChannels({ "B", "G", "R" }, ExrFloat));
			mipmapped.Tiled = true;
			mipmapped.LevelMode = ExrMipmapLevels;
			mipmapped.TileWidth = 8;
			mipmapped.TileHeight = 8;
			mipmapped.Compression = ExrRle;
			TestExr decreasing = MakeExrOf(*reference, MakeChannels({ "B", "G", "R" }, ExrFloat));
			decreasing.LineOrder = 1;
			decreasing.MinX = -5;
			decreasing.MinY = 7;
			const std::array<std::pair<std::string_view, const TestExr*>, 5> variants = { { { "Floats.exr", &floats },
				{ "Rle.exr", &rle },
				{ "Tiled.exr", &tiled },
				{ "Mipmapped.exr", &mipmapped },
				{ "Decreasing.exr", &decreasing } } };
			for (const auto& [name, exr] : variants)
			{
				CAPTURE(name);
				checkSame(Decode(fixture, name, WriteTestExr(*exr).Bytes));
			}
		}

		TEST_CASE("EnvironmentImporter: OpenEXR uint channels read as the nearest floats")
		{
			TestExr exr;
			exr.Width = 8;
			exr.Height = 4;
			exr.Channels = MakeChannels({ "B", "G", "R" }, ExrUint);
			// B: 2^24 + 1 rounds to 2^24; G: the column; R: 70,000 + the row.
			exr.Value = [](uint32_t x, uint32_t y, size_t channel) -> double
			{
				return channel == 0 ? 16777217.0 : channel == 1 ? static_cast<double>(x)
																: 70000.0 + y;
			};
			Test::AssetTestFixture fixture;
			const Result<EnvironmentBakeInput> decoded = Decode(fixture, "Uint.exr", WriteTestExr(exr).Bytes);
			REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
			REQUIRE(decoded->Texels.size() == 8 * 4 * 3);
			for (uint32_t y = 0; y < 4; ++y)
			{
				for (uint32_t x = 0; x < 8; ++x)
				{
					const size_t texel = (static_cast<size_t>(y) * 8 + x) * 3;
					CHECK(decoded->Texels[texel] == 70000.0f + static_cast<float>(y));
					CHECK(decoded->Texels[texel + 1] == static_cast<float>(x));
					CHECK(decoded->Texels[texel + 2] == 16777216.0f);
				}
			}
		}

		// The tile layouts OpenEXR defines, each checked chunk by chunk before tinyexr decodes (GetExrTileLevels): one level,
		// mipmap levels and ripmap levels (x fastest), with level counts and sizes rounded down or up, on an image whose
		// edges are no powers of two (so rounding up adds levels and texels) in partial tiles; the top level is read. One
		// image also declares its (correct) chunkCount.
		TEST_CASE("EnvironmentImporter: OpenEXR tiles of every level mode and rounding decode their top level")
		{
			TestExr exr;
			exr.Width = 40;
			exr.Height = 20;
			exr.Channels = MakeChannels({ "B", "G", "R" }, ExrHalf);
			exr.Tiled = true;
			exr.TileWidth = 16;
			exr.TileHeight = 8;
			// Exact in a binary16: multiples of 1/64 below 16.
			exr.Value = [](uint32_t x, uint32_t y, size_t channel) -> double
			{
				return (static_cast<double>(x) + 40.0 * y) / 64.0 + 0.25 * static_cast<double>(channel);
			};
			Test::AssetTestFixture fixture;
			for (const uint8_t levelMode : { ExrOneLevel, ExrMipmapLevels, ExrRipmapLevels })
			{
				for (const bool roundUp : { false, true })
				{
					CAPTURE(static_cast<int>(levelMode));
					CAPTURE(roundUp);
					TestExr layout = exr;
					layout.LevelMode = levelMode;
					layout.RoundUp = roundUp;
					if (levelMode == ExrRipmapLevels && roundUp)
						layout.ChunkCountDelta = 0;
					layout.Compression = roundUp ? ExrRle : ExrNone;
					const Result<EnvironmentBakeInput> decoded = Decode(fixture, "Tiles.exr", WriteTestExr(layout).Bytes);
					REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
					REQUIRE(decoded->Width == 40);
					REQUIRE(decoded->Height == 20);
					REQUIRE(decoded->Texels.size() == 40 * 20 * 3);
					for (uint32_t y = 0; y < 20; ++y)
					{
						for (uint32_t x = 0; x < 40; ++x)
						{
							const size_t texel = (static_cast<size_t>(y) * 40 + x) * 3;
							// R, G and B are the file's channels 2, 1 and 0.
							CHECK(decoded->Texels[texel] == static_cast<float>(exr.Value(x, y, 2)));
							CHECK(decoded->Texels[texel + 1] == static_cast<float>(exr.Value(x, y, 1)));
							CHECK(decoded->Texels[texel + 2] == static_cast<float>(exr.Value(x, y, 0)));
						}
					}
				}
			}
		}

		TEST_CASE("EnvironmentImporter: unsupported OpenEXR images are refused naming what is unsupported")
		{
			Test::AssetTestFixture fixture;
			const auto refuse = [&fixture](const TestExr& exr, std::string_view named)
			{
				CAPTURE(named);
				const Result<EnvironmentBakeInput> decoded = Decode(fixture, "Unsupported.exr", WriteTestExr(exr).Bytes);
				REQUIRE_FALSE(decoded.has_value());
				CHECK(decoded.error().GetCode() == ErrorCode::ImportFailed);
				CHECK(decoded.error().ToString().contains(named));
			};
			TestExr multiPart = MakeSmallExr();
			multiPart.ExtraVersionFlags = 0x10;
			refuse(multiPart, "multi-part");
			TestExr deep = MakeSmallExr();
			deep.ExtraVersionFlags = 0x08;
			refuse(deep, "deep");
			// tinyexr refuses DWA without naming it; the importer names every compression it does not read.
			for (const auto& [compression, name] : std::to_array<std::pair<uint8_t, std::string_view>>({ { 6, "B44" }, { 7, "B44A" }, { 8, "DWAA" }, { 9, "DWAB" } }))
			{
				TestExr compressed = MakeSmallExr();
				compressed.Compression = compression;
				compressed.HeaderOnly = true;
				refuse(compressed, name);
			}
			TestExr noBlue = MakeSmallExr();
			noBlue.Channels = MakeChannels({ "G", "R", "Y" }, ExrHalf);
			refuse(noBlue, "blue");
			TestExr subsampled = MakeSmallExr();
			subsampled.Channels[2].XSampling = 2;
			subsampled.HeaderOnly = true;
			refuse(subsampled, "samples its channel 'R'");
			// Nine float channels of an 8,192 x 4,096 image are 1,152 MiB decoded: refused from the header, before the offset
			// table is read or anything allocated.
			TestExr channels;
			channels.Width = 8192;
			channels.Height = 4096;
			channels.Channels = MakeChannels({ "A", "B", "G", "R", "U", "V", "W", "X", "Y" }, ExrFloat);
			channels.HeaderOnly = true;
			refuse(channels, "MiB");
		}

		// The checks every source shares (an equirectangular size, finite and non-negative texels), and the chunk checks that
		// keep tinyexr from returning memory it never wrote or writing outside its tiles: a scanline block that repeats
		// another's lines, a tile at a negative coordinate and a repeated tile are refused before any pixel is decoded.
		TEST_CASE("EnvironmentImporter: broken OpenEXR files fail precisely and never decode unwritten texels")
		{
			Test::AssetTestFixture fixture;
			const auto fail = [&fixture](std::span<const std::byte> bytes) -> Error
			{
				const Result<EnvironmentBakeInput> decoded = Decode(fixture, "Broken.exr", bytes);
				REQUIRE_FALSE(decoded.has_value());
				CHECK(decoded.error().GetCode() == ErrorCode::ImportFailed);
				return decoded.error();
			};
			CHECK(fail({}).ToString().contains("empty"));

			TestExr square = MakeSmallExr();
			square.Height = 8;
			CHECK(fail(WriteTestExr(square).Bytes).ToString().contains("8x8"));
			TestExr wide = MakeSmallExr();
			wide.Width = 16386;
			wide.Height = 8193;
			wide.HeaderOnly = true;
			const Error tooWide = fail(WriteTestExr(wide).Bytes);
			CHECK(tooWide.ToString().contains("16386x8193"));
			CHECK(tooWide.GetHint() == "use the 8k or a smaller variant of the HDRI");

			TestExr infinite = MakeSmallExr();
			infinite.Value = [](uint32_t x, uint32_t y, size_t channel) -> double
			{
				return x == 3 && y == 2 && channel == 1 ? std::numeric_limits<double>::infinity() : 0.5;
			};
			CHECK(fail(WriteTestExr(infinite).Bytes).ToString().contains("texel (3, 2)"));
			TestExr negative = MakeSmallExr();
			negative.Value = [](uint32_t x, uint32_t y, size_t /*channel*/) -> double
			{
				return x == 5 && y == 1 ? -1.0 : 0.5;
			};
			CHECK(fail(WriteTestExr(negative).Bytes).ToString().contains("negative"));

			TestExrFile repeatedLine = WriteTestExr(MakeSmallExr());
			SetI32(repeatedLine.Bytes, repeatedLine.Chunks[2], 0);
			CHECK(fail(repeatedLine.Bytes).ToString().contains("line 0"));

			TestExr tiled = MakeSmallExr();
			tiled.Tiled = true;
			tiled.TileWidth = 4;
			tiled.TileHeight = 4;
			TestExrFile negativeTile = WriteTestExr(tiled);
			SetI32(negativeTile.Bytes, negativeTile.Chunks[1], -1);
			CHECK(fail(negativeTile.Bytes).ToString().contains("tile (-1, 0)"));
			TestExrFile repeatedTile = WriteTestExr(tiled);
			SetI32(repeatedTile.Bytes, repeatedTile.Chunks[1], 0);
			CHECK(fail(repeatedTile.Bytes).ToString().contains("tile (0, 0)"));

			// A chunkCount attribute other than the image's chunk count: tinyexr would read that many scanline blocks.
			TestExr fewerBlocks = MakeSmallExr();
			fewerBlocks.ChunkCountDelta = -1;
			CHECK(fail(WriteTestExr(fewerBlocks).Bytes).ToString().contains("chunkCount"));
			TestExr moreTiles = tiled;
			moreTiles.ChunkCountDelta = 1;
			CHECK(fail(WriteTestExr(moreTiles).Bytes).ToString().contains("chunkCount"));

			// Truncated in the pixel data, and in the offset table.
			Buffer truncated = WriteTestExr(MakeSmallExr()).Bytes;
			truncated.resize(truncated.size() - 10);
			static_cast<void>(fail(truncated));
			const TestExrFile whole = WriteTestExr(MakeSmallExr());
			const Buffer table(whole.Bytes.begin(), whole.Bytes.begin() + static_cast<std::ptrdiff_t>(whole.Chunks.front() - 16));
			CHECK(fail(table).ToString().contains("offset table"));
		}

		TEST_CASE("EnvironmentImporter: 200 seeded mutations of a Radiance .hdr never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The seeds: the Gradient.hdr fixture (run-length encoded) and a small flat RGBE image, decoded from memory through
			// stbi_loadf (§8.6 step 1). A decoded image reaches the baker, which checks it; a broken one is ImportFailed.
			Test::AssetTestFixture fixture;
			const std::array<Buffer, 2> seeds = { ReadEnvironmentFixture("Gradient.hdr"), MakeFlatHdr(16, 8) };
			const Json settings = DefaultSettings();
			const VfsPath sourcePath = fixture.ProjectPath("Assets/Environments/Fuzz.hdr");
			InputCheckingBaker baker;
			const Test::FuzzRun run = Test::RunFuzzCase(0x4D2, 200, [&fixture, &seeds, &settings, &sourcePath, &baker](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = seeds[static_cast<size_t>(random.RangeInt(0, 1))];
				Test::MutateBytes(mutated, random);
				AssetMetadata metadata;
				metadata.Handle = AssetHandle(0xe0e7000000000002ull);
				metadata.Type = AssetType::Environment;
				metadata.Importer = std::string(EnvironmentImporter::Id);
				metadata.ImporterVersion = EnvironmentImporter::Version;
				metadata.Settings = VariantValue(settings);
				ImportContext context({
					.Vfs = &fixture.GetVfs(),
					.SourcePath = sourcePath,
					.SourceBytes = mutated,
					.Settings = VariantValue(settings),
					.Registry = &fixture.GetRegistry(),
					.Assets = {},
					.EnvironmentBaker = &baker,
					.ScriptDiagnostics = nullptr,
				});
				const uint64_t bakes = baker.GetBakeCount();
				const Result<ImportResult> imported = EnvironmentImporter().Import(context, metadata);
				REQUIRE_FALSE(imported.has_value());
				// The decoded image reached the baker, which refused to bake it.
				if (baker.GetBakeCount() != bakes)
				{
					CHECK(imported.error().GetCode() == ErrorCode::Unsupported);
					return true;
				}
				CHECK(imported.error().GetCode() == ErrorCode::ImportFailed);
				return false;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}

		// M23 (decision 19): the OpenEXR reader's fuzz target, EnvironmentImporterExr. The seeds: the generated Gradient.exr
		// (uncompressed scanlines), GradientZip.exr (ZIP), GradientBlocksZips.exr (ZIPS), GradientBlocksPiz.exr (PIZ) and
		// GradientBlocksPxr24.exr (PXR24), and the test's RLE image of mipmapped tiles, so header, offset table, chunk,
		// inflate, Huffman, wavelet, byte-plane, RLE and tile paths all see mutations. A decoded image reaches the baker,
		// which checks it; a refused one is ImportFailed.
		TEST_CASE("EnvironmentImporter: 200 seeded mutations of an OpenEXR file never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			Test::AssetTestFixture fixture;
			const Result<EnvironmentBakeInput> reference = Decode(fixture, "Gradient.hdr", ReadEnvironmentFixture("Gradient.hdr"));
			REQUIRE(reference.has_value());
			TestExr tiled = MakeExrOf(*reference, MakeChannels({ "B", "G", "R" }, ExrHalf));
			tiled.Tiled = true;
			tiled.LevelMode = ExrMipmapLevels;
			tiled.TileWidth = 16;
			tiled.TileHeight = 8;
			tiled.Compression = ExrRle;
			const std::array<Buffer, 6> seeds = { ReadEnvironmentFixture("Gradient.exr"), ReadEnvironmentFixture("GradientZip.exr"),
				ReadEnvironmentFixture("GradientBlocksZips.exr"), ReadEnvironmentFixture("GradientBlocksPiz.exr"), ReadEnvironmentFixture("GradientBlocksPxr24.exr"),
				WriteTestExr(tiled).Bytes };
			const Json settings = DefaultSettings();
			const VfsPath sourcePath = fixture.ProjectPath("Assets/Environments/Fuzz.exr");
			InputCheckingBaker baker;
			const Test::FuzzRun run = Test::RunFuzzCase(0xE4F2, 200, [&fixture, &seeds, &settings, &sourcePath, &baker](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = seeds[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(seeds.size()) - 1))];
				Test::MutateBytes(mutated, random);
				AssetMetadata metadata;
				metadata.Handle = AssetHandle(0xe0e7000000000003ull);
				metadata.Type = AssetType::Environment;
				metadata.Importer = std::string(EnvironmentImporter::Id);
				metadata.ImporterVersion = EnvironmentImporter::Version;
				metadata.Settings = VariantValue(settings);
				ImportContext context({
					.Vfs = &fixture.GetVfs(),
					.SourcePath = sourcePath,
					.SourceBytes = mutated,
					.Settings = VariantValue(settings),
					.Registry = &fixture.GetRegistry(),
					.Assets = {},
					.EnvironmentBaker = &baker,
					.ScriptDiagnostics = nullptr,
				});
				const uint64_t bakes = baker.GetBakeCount();
				const Result<ImportResult> imported = EnvironmentImporter().Import(context, metadata);
				REQUIRE_FALSE(imported.has_value());
				if (baker.GetBakeCount() != bakes)
				{
					CHECK(imported.error().GetCode() == ErrorCode::Unsupported);
					return true;
				}
				CHECK(imported.error().GetCode() == ErrorCode::ImportFailed);
				return false;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}

		TEST_CASE("EnvironmentImporter: bakes and cooks the Studio HDRI deterministically" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
				Test::AssetTestFixture fixture;
				const Buffer studio = ReadStudio();
				const Result<ImportResult> first = ImportEnvironment(fixture, "Studio.hdr", studio, DefaultSettings(), baker->get());
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				REQUIRE(first->Artifacts.size() == 1);
				CHECK(first->Artifacts.front().Type == AssetType::Environment);
				const Result<AssetRef<EnvironmentData>> environment = LoadCookedEnvironment(first->Artifacts.front().Cooked);
				REQUIRE_MESSAGE(environment.has_value(), environment.error().ToString());
				CHECK((*environment)->Skybox.FaceSize == 256); // 1k HDRI: min(1024, 1024 / 4)
				CHECK((*environment)->Specular.FaceSize == EnvironmentData::SpecularFaceSize);
				// "Importers: importing twice gives identical hashes" holds for the GPU bake on one device, and the BC6H
				// compression of Auto, the default, is deterministic too.
				CHECK((*environment)->Skybox.Format == CubeMapFormat::BC6HUfloat);
				const Result<ImportResult> second = ImportEnvironment(fixture, "Studio.hdr", studio, DefaultSettings(), baker->get());
				REQUIRE(second.has_value());
				CHECK(second->Artifacts.front().Cooked == first->Artifacts.front().Cooked);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("EnvironmentImporter: the generated flat-RGBE Gradient.hdr bakes a sky brighter above than below" * doctest::test_suite(Test::GpuSuite))
		{
			// Tests/Data/Assets/Environments/Gradient.hdr (MakeFormatFixtures.py): 32 x 16 flat RGBE scanlines, a blue sky over a
			// dark brown ground, the Formats.scene environment of Projects/FeatureTest.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
				Test::AssetTestFixture fixture;
				const Result<ImportResult> imported = ImportEnvironment(fixture, "Gradient.hdr", ReadEnvironmentFixture("Gradient.hdr"), DefaultSettings(), baker->get());
				REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
				const Result<AssetRef<EnvironmentData>> environment = LoadCookedEnvironment(imported->Artifacts.front().Cooked);
				REQUIRE_MESSAGE(environment.has_value(), environment.error().ToString());
				CHECK((*environment)->Skybox.FaceSize == 8); // min(1024, 32 / 4)
				// The band 0 coefficient is the mean radiance; the (1, -1) coefficient grows with +Y, the sky's side.
				for (int channel = 0; channel < 3; ++channel)
				{
					CAPTURE(channel);
					CHECK((*environment)->IrradianceSH9[0][channel] > 0.0f);
					CHECK((*environment)->IrradianceSH9[1][channel] > 0.0f);
				}
				CHECK((*environment)->IrradianceSH9[1].z > (*environment)->IrradianceSH9[1].x);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M23 (C23.1, A-EXR and R-TEXC-HDR; Docs/Decisions/0034-m23-contract.md decision 14, stream Y): EnvironmentImporter
		// version 2 reads OpenEXR through tinyexr: Gradient.exr, the half-float scanline copy of Gradient.hdr's texels that
		// stream Y's generator writes, bakes the environment Gradient.hdr bakes (Compression None: RGBA16F cubes); with
		// Compression Auto, the default, both cubes are stored as BC6H.
		TEST_CASE("EnvironmentImporter: Gradient.exr bakes as Gradient.hdr does, and Auto stores BC6H cubes"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
				Test::AssetTestFixture fixture;
				Json uncompressed = DefaultSettings();
				uncompressed["Compression"] = "None";
				Result<Buffer> hdr = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Environments/Gradient.hdr"));
				Result<Buffer> exr = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Environments/Gradient.exr"));
				REQUIRE_MESSAGE(hdr.has_value(), hdr.error().ToString());
				REQUIRE_MESSAGE(exr.has_value(), exr.error().ToString());
				const Result<ImportResult> fromHdr = ImportEnvironment(fixture, "Gradient.hdr", *hdr, uncompressed, baker->get());
				const Result<ImportResult> fromExr = ImportEnvironment(fixture, "Gradient.exr", *exr, uncompressed, baker->get());
				REQUIRE_MESSAGE(fromHdr.has_value(), fromHdr.error().ToString());
				REQUIRE_MESSAGE(fromExr.has_value(), fromExr.error().ToString());
				const Result<AssetRef<EnvironmentData>> hdrEnvironment = LoadCookedEnvironment(fromHdr->Artifacts.front().Cooked);
				const Result<AssetRef<EnvironmentData>> exrEnvironment = LoadCookedEnvironment(fromExr->Artifacts.front().Cooked);
				REQUIRE(hdrEnvironment.has_value());
				REQUIRE(exrEnvironment.has_value());
				CHECK((*exrEnvironment)->Skybox.FaceSize == (*hdrEnvironment)->Skybox.FaceSize);
				for (size_t coefficient = 0; coefficient < 9; ++coefficient)
				{
					for (int channel = 0; channel < 3; ++channel)
						CHECK((*exrEnvironment)->IrradianceSH9[coefficient][channel] == doctest::Approx((*hdrEnvironment)->IrradianceSH9[coefficient][channel]).epsilon(1e-3));
				}

				const Result<ImportResult> compressed = ImportEnvironment(fixture, "Gradient.exr", *exr, DefaultSettings(), baker->get());
				REQUIRE_MESSAGE(compressed.has_value(), compressed.error().ToString());
				const Result<AssetRef<EnvironmentData>> bc6h = LoadCookedEnvironment(compressed->Artifacts.front().Cooked);
				REQUIRE_MESSAGE(bc6h.has_value(), bc6h.error().ToString());
				CHECK((*bc6h)->Skybox.Format == CubeMapFormat::BC6HUfloat);
				CHECK((*bc6h)->Specular.Format == CubeMapFormat::BC6HUfloat);
				// Stream Y: the uncompressed bake keeps RGBA16F cubes, the compressed one holds each face level of them as BC6H
				// blocks (decoding within BC6H's error of the RGBA16F bake), and the SH9 is the bake's either way.
				CHECK((*exrEnvironment)->Skybox.Format == CubeMapFormat::Rgba16Float);
				CHECK((*exrEnvironment)->Specular.Format == CubeMapFormat::Rgba16Float);
				CHECK((*bc6h)->IrradianceSH9 == (*exrEnvironment)->IrradianceSH9);
				const CubeMapData& source = (*exrEnvironment)->Skybox;
				const CubeMapData& blocks = (*bc6h)->Skybox;
				const Result<Buffer> face = DecompressTextureBlocks(TextureBlockCodec::BC6H, blocks.FaceSize, blocks.FaceSize,
					std::span<const std::byte>(blocks.Texels).first(ComputeCubeMapFaceByteSize(blocks.FaceSize, 0, CubeMapFormat::BC6HUfloat)));
				REQUIRE_MESSAGE(face.has_value(), face.error().ToString());
				const size_t faceBytes = ComputeCubeMapFaceByteSize(source.FaceSize, 0, CubeMapFormat::Rgba16Float);
				REQUIRE(face->size() == faceBytes);
				for (size_t offset = 0; offset < faceBytes; offset += CubeMapData::BytesPerTexel)
				{
					for (size_t channel = 0; channel < 3; ++channel)
					{
						const auto half = [offset, channel](std::span<const std::byte> texels)
						{
							return Test::HalfToDouble(static_cast<uint16_t>(std::to_integer<uint32_t>(texels[offset + channel * 2])
								| (std::to_integer<uint32_t>(texels[offset + channel * 2 + 1]) << 8)));
						};
						const double expected = half(source.Texels);
						CHECK(std::abs(half(*face) - expected) <= 0.05 * expected + 1.0 / 256.0);
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("EnvironmentImporter: an import whose bake meets a device loss is not remembered, and imports after the recovery"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18's milestone review (Docs/Decisions/0029-m18-contract.md decision 28): the editor's asset manager with the
			// Renderer's baker on a lost device. The bake fails with Gpu, which is the loss: no ASSET_IMPORT_FAILED diagnostic
			// and no remembered failure, so the Load after the recovery bakes and loads the environment.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
				Test::AssetTestFixture fixture(Test::AssetTestFixtureOptions{ .EnvironmentBaker = baker->get() });
				fixture.WriteProjectFile("Assets/Environments/Gradient.hdr", ReadEnvironmentFixture("Gradient.hdr"));
				fixture.OpenProject(false);
				const AssetHandle handle = fixture.GetManager().Resolve("Assets/Environments/Gradient.hdr").value_or(AssetHandle());
				REQUIRE(handle.IsValid());
				const auto countFailures = [&fixture, handle]()
				{
					return std::ranges::count_if(fixture.GetManager().GetDiagnostics(), [handle](const AssetDiagnostic& diagnostic)
					{
						return diagnostic.Asset == handle && diagnostic.Code == AssetImportFailedCode;
					});
				};

				device.GetDiagnostics().SetDeviceLost(true);
				device.ReportDeviceLost("EnvironmentImporter test");
				CHECK((*baker)->IsDeviceLost());
				const Result<AssetRef<Asset>> lost = fixture.GetManager().Load(handle);
				REQUIRE_FALSE(lost.has_value());
				CHECK(lost.error().GetCode() == ErrorCode::Gpu);
				CHECK(fixture.GetManager().GetState(handle) != AssetState::Failed);
				CHECK(countFailures() == 0);

				const Status recovered = device.Recover(nullptr);
				REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
				CHECK_FALSE((*baker)->IsDeviceLost());
				const Result<AssetRef<Asset>> loaded = fixture.GetManager().Load(handle);
				REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
				CHECK(fixture.GetManager().GetState(handle) == AssetState::Loaded);
				CHECK(countFailures() == 0);
			}
			device.RunGarbageCollection();
		}
	}

}
