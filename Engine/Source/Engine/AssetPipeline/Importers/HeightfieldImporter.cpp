#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/HeightfieldImporter.h"

#include "Engine/AssetPipeline/Private/ImportErrors.h"
#include "Engine/Core/BinaryReader.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <utility>

// The heightfield importer and the heightmap conversion (HeightfieldImporter.h; Docs/Decisions/0037-m26-contract.md decision
// 20), stream T-A. The conversion reads a 16-bit PNG through the vendored stb_image's 16-bit path (whose PNG decoding the
// DecodePng fuzz target covers) and a RAW by a size check and a copy.

namespace Engine {

	namespace Utils {

		constexpr std::array<std::string_view, 1> HeightfieldExtensions = { ".heightfield" };

		// The eight bytes every PNG file starts with (PNG specification, section 5.2): stb_image decodes other formats too,
		// and a heightmap is a PNG.
		constexpr std::array<uint8_t, 8> HeightmapPngSignature = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

		struct StbHeightmapDeleter
		{
			void operator()(stbi_us* pixels) const { stbi_image_free(pixels); }
		};

		// stb_image's reason for its last failure on this thread (its failure reason is thread-local).
		[[nodiscard]] static std::string_view GetHeightmapStbFailureReason()
		{
			const char* reason = stbi_failure_reason();
			return reason != nullptr ? std::string_view(reason) : std::string_view("unknown reason");
		}

		[[nodiscard]] static Error MakeSideOutOfRange(uint64_t side, std::string_view what)
		{
			return Error(ErrorCode::InvalidArgument, std::format("the {} is {} samples a side; a heightfield is {} to {}", what, side, MinHeightfieldResolution, MaxHeightfieldResolution))
				.WithHint("resample the heightmap to at most 1025 x 1025 samples (1025, 513 or 257 suit the terrain's 32-cell chunks)");
		}

		// The largest n with n * n <= value.
		[[nodiscard]] static uint64_t FloorSquareRoot(uint64_t value)
		{
			uint64_t root = static_cast<uint64_t>(std::sqrt(static_cast<double>(value)));
			while (root * root > value)
				--root;
			while ((root + 1) * (root + 1) <= value)
				++root;
			return root;
		}

		[[nodiscard]] static Result<HeightfieldData> ConvertRaw16(std::span<const std::byte> source)
		{
			const uint64_t samples = source.size() / sizeof(uint16_t);
			const uint64_t side = FloorSquareRoot(samples);
			if (source.size() % sizeof(uint16_t) != 0 || side * side != samples)
			{
				return std::unexpected(Error(ErrorCode::InvalidArgument,
					std::format("the RAW heightmap is {} bytes, not 2 N^2 for a square of N x N 16-bit samples", source.size()))
						.WithHint("write the heightmap as N x N little-endian 16-bit samples, row 0 first"));
			}
			if (side < MinHeightfieldResolution || side > MaxHeightfieldResolution)
				return std::unexpected(MakeSideOutOfRange(side, "RAW heightmap"));
			BinaryReader reader(source);
			HeightfieldData heightfield;
			heightfield.Resolution = static_cast<uint32_t>(side);
			ENGINE_TRY_ASSIGN(heightfield.Heights, reader.ReadArray<uint16_t>(static_cast<size_t>(samples)));
			return heightfield;
		}

		[[nodiscard]] static Result<HeightfieldData> ConvertPng16(std::span<const std::byte> source)
		{
			if (source.size() < HeightmapPngSignature.size()
				|| !std::equal(HeightmapPngSignature.begin(), HeightmapPngSignature.end(), source.begin(), [](uint8_t expected, std::byte actual)
			{
				return static_cast<std::byte>(expected) == actual;
			}))
			{
				return MakeError(ErrorCode::Parse, "not a PNG file ({} bytes without the PNG signature)", source.size());
			}
			if (source.size() > static_cast<size_t>(INT_MAX))
				return MakeError(ErrorCode::Parse, "a PNG file of {} bytes is too large to decode", source.size());

			const auto* data = reinterpret_cast<const stbi_uc*>(source.data());
			const int length = static_cast<int>(source.size());
			int width = 0;
			int height = 0;
			int channels = 0;
			if (stbi_info_from_memory(data, length, &width, &height, &channels) == 0)
				return MakeError(ErrorCode::Parse, "corrupt PNG header: {}", GetHeightmapStbFailureReason());
			// The header's size is checked before anything is decoded, so a hostile header cannot make stb_image allocate more
			// than a 1025 x 1025 image of four 16-bit channels.
			if (stbi_is_16_bit_from_memory(data, length) == 0)
			{
				return std::unexpected(Error(ErrorCode::InvalidArgument,
					std::format("the PNG heightmap ({} x {}) has 8-bit samples; a heightmap needs 16-bit ones", width, height))
						.WithHint("export the heightmap as a 16-bit greyscale PNG, or as a RAW of 16-bit samples: 256 levels terrace a terrain"));
			}
			if (width != height)
			{
				return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("the PNG heightmap is {} x {} samples, not square", width, height))
						.WithHint("crop or resample the heightmap to N x N samples"));
			}
			if (width < static_cast<int>(MinHeightfieldResolution) || width > static_cast<int>(MaxHeightfieldResolution))
				return std::unexpected(MakeSideOutOfRange(static_cast<uint64_t>(std::max(width, 0)), "PNG heightmap"));

			int decodedWidth = 0;
			int decodedHeight = 0;
			int decodedChannels = 0;
			// Desired channels 0: the image's own channels, of which the first is the height (a grey image's only one).
			const std::unique_ptr<stbi_us, StbHeightmapDeleter> pixels(
				stbi_load_16_from_memory(data, length, &decodedWidth, &decodedHeight, &decodedChannels, 0));
			if (pixels == nullptr)
				return MakeError(ErrorCode::Parse, "corrupt PNG: {}", GetHeightmapStbFailureReason());
			if (decodedWidth != width || decodedHeight != height || decodedChannels < 1 || decodedChannels > 4)
			{
				return MakeError(ErrorCode::Parse, "corrupt PNG: its header declares {} x {} samples but it decodes to {} x {} with {} channels", width, height,
					decodedWidth, decodedHeight, decodedChannels);
			}

			HeightfieldData heightfield;
			heightfield.Resolution = static_cast<uint32_t>(width);
			const size_t sampleCount = static_cast<size_t>(width) * static_cast<size_t>(height);
			heightfield.Heights.resize(sampleCount);
			const size_t stride = static_cast<size_t>(decodedChannels);
			for (size_t index = 0; index < sampleCount; ++index)
				heightfield.Heights[index] = pixels.get()[index * stride];
			return heightfield;
		}

	}

	Result<HeightfieldData> ConvertHeightmap(std::span<const std::byte> source, HeightmapSourceKind kind)
	{
		switch (kind)
		{
			case HeightmapSourceKind::Png16: return Utils::ConvertPng16(source);
			case HeightmapSourceKind::Raw16: return Utils::ConvertRaw16(source);
		}
		return MakeError(ErrorCode::InvalidArgument, "heightmap source kind {} is not a 16-bit PNG or a RAW", std::to_underlying(kind));
	}

	std::span<const std::string_view> HeightfieldImporter::GetExtensions() const
	{
		return Utils::HeightfieldExtensions;
	}

	Result<ImportResult> HeightfieldImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();
		Result<HeightfieldData> heightfield = ReadHeightfieldSource(context.GetSourceBytes());
		if (!heightfield.has_value())
			return std::unexpected(Utils::MakeImportFailed(heightfield.error(), sourcePath, std::format("while importing heightfield '{}'", sourcePath)));

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{
			.Handle = metadata.Handle, .Type = AssetType::Heightfield, .SubAssetKey = {}, .Cooked = CookHeightfield(*heightfield, Version) });
		// A heightfield reads only its own source and references no asset (ADR 0028's phase-3 ledger), set explicitly as every
		// importer's dependencies are.
		result.Dependencies = {};
		return result;
	}

}
