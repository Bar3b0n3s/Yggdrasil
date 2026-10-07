#include "EnginePCH.h"
#include "Engine/Asset/CookedFormat.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Hash.h"

#include <cstring>
#include <format>

namespace Engine {

	Buffer WriteCookedArtifact(AssetType type, uint16_t formatVersion, uint32_t importerVersion, std::span<const std::byte> payload)
	{
		ENGINE_CORE_ASSERT(type != AssetType::None, "A cooked artifact has a type");
		BinaryWriter writer;
		writer.WriteBytes(std::as_bytes(std::span(CookedHeader::Magic)));
		writer.WriteU16(formatVersion);
		writer.WriteU16(std::to_underlying(type));
		writer.WriteU32(importerVersion);
		writer.WriteU32(0);
		writer.WriteU64(payload.size());
		writer.WriteU64(XXH64(payload));
		writer.WriteBytes(payload);
		return writer.TakeBuffer();
	}

	Result<CookedArtifactView> ReadCookedArtifact(std::span<const std::byte> bytes)
	{
		if (bytes.size() < CookedHeader::Size)
			return MakeError(ErrorCode::Parse, "a cooked artifact is at least {} bytes, got {}", CookedHeader::Size, bytes.size());

		BinaryReader reader(bytes);
		ENGINE_TRY_ASSIGN(const std::span<const std::byte> magic, reader.ReadBytes(CookedHeader::Magic.size()));
		if (std::memcmp(magic.data(), CookedHeader::Magic.data(), CookedHeader::Magic.size()) != 0)
			return MakeError(ErrorCode::Parse, "not a cooked artifact: the header does not start with \"ECKD\"");

		CookedArtifactView view;
		ENGINE_TRY_ASSIGN(view.Header.FormatVersion, reader.ReadU16());
		ENGINE_TRY_ASSIGN(const uint16_t type, reader.ReadU16());
		ENGINE_TRY_ASSIGN(view.Header.ImporterVersion, reader.ReadU32());
		ENGINE_TRY_ASSIGN(view.Header.Flags, reader.ReadU32());
		ENGINE_TRY_ASSIGN(view.Header.PayloadSize, reader.ReadU64());
		ENGINE_TRY_ASSIGN(view.Header.PayloadHash, reader.ReadU64());

		if (view.Header.Flags != 0)
			return MakeError(ErrorCode::Parse, "cooked artifact flags are reserved and must be 0, got {:#x}", view.Header.Flags);
		if (view.Header.PayloadSize != reader.GetRemaining())
		{
			return MakeError(ErrorCode::Parse, "the header names a payload of {} bytes, but {} bytes follow it", view.Header.PayloadSize,
				reader.GetRemaining());
		}
		if (type == std::to_underlying(AssetType::None) || type > std::to_underlying(AssetType::Replay))
			return MakeError(ErrorCode::Validation, "cooked artifact of unknown asset type {}", type);
		view.Header.Type = static_cast<AssetType>(type);

		ENGINE_TRY_ASSIGN(view.Payload, reader.ReadBytes(reader.GetRemaining()));
		if (XXH64(view.Payload) != view.Header.PayloadHash)
			return MakeError(ErrorCode::Validation, "payload hash mismatch: the artifact is corrupted");
		return view;
	}

	Result<CookedArtifactView> ReadCookedArtifact(std::span<const std::byte> bytes, AssetType expectedType, uint16_t formatVersion)
	{
		ENGINE_TRY_ASSIGN(CookedArtifactView view, ReadCookedArtifact(bytes));
		if (view.Header.Type != expectedType)
		{
			return MakeError(ErrorCode::Validation, "the cooked artifact holds a {}, not a {}", AssetTypeToString(view.Header.Type),
				AssetTypeToString(expectedType));
		}
		if (view.Header.FormatVersion != formatVersion)
		{
			return std::unexpected(Error(ErrorCode::UnsupportedVersion,
				std::format("the cooked {} has payload format version {}; this build reads version {}", AssetTypeToString(expectedType),
					view.Header.FormatVersion, formatVersion))
					.WithHint("reimport the asset (the cache rebuilds it) or re-export the game"));
		}
		return view;
	}

}
