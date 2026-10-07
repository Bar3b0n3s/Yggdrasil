#include "EnginePCH.h"
#include "Engine/Asset/AssetMetadata.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <set>

namespace Engine {

	namespace Utils {

		static constexpr std::string_view MetaExtension = ".meta";

		static constexpr std::array<std::string_view, 8> AssetMetaKeys = { "Format", "Version", "Handle", "Type", "Importer",
			"ImporterVersion", "Settings", "SubAssets" };
		static constexpr std::array<std::string_view, 5> DependencyMetaKeys = { "Format", "Version", "Handle", "Type", "Owner" };
		static constexpr std::array<std::string_view, 3> SubAssetKeys = { "Key", "Handle", "Type" };

		// Fails on the first member of `object` that is not in `knownKeys`, located at that member.
		static Status RejectUnknownMembers(const JsonReader& object, std::span<const std::string_view> knownKeys, std::string_view what)
		{
			ENGINE_TRY_ASSIGN(const std::vector<std::string> unknown, object.FindUnknownMembers(knownKeys));
			if (unknown.empty())
				return {};
			ENGINE_TRY_ASSIGN(const JsonReader member, object.GetMember(unknown.front()));
			return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, std::format("unknown member '{}' in {}", unknown.front(), what))
					.WithHint("a .meta is written by the editor; remove the member, or delete the .meta to let the editor write a new one"));
		}

		// A handle member that must be present and not null.
		static Result<AssetHandle> ReadHandleMember(const JsonReader& object, std::string_view key)
		{
			ENGINE_TRY_ASSIGN(const JsonReader member, object.GetMember(key));
			ENGINE_TRY_ASSIGN(const UUID handle, member.ReadUUID());
			if (!handle.IsValid())
				return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, std::format("'{}' must not be the null handle", key)));
			return handle;
		}

		static Result<AssetType> ReadAssetType(const JsonReader& member)
		{
			ENGINE_TRY_ASSIGN(const std::string name, member.ReadString());
			const std::optional<AssetType> type = AssetTypeFromString(name);
			if (!type.has_value() || *type == AssetType::None)
				return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, std::format("unknown asset type '{}'", name)));
			return *type;
		}

		static Result<std::vector<SubAssetEntry>> ReadSubAssets(const JsonReader& member, AssetHandle source)
		{
			ENGINE_TRY_ASSIGN(const size_t count, member.GetArraySize());
			std::vector<SubAssetEntry> entries;
			entries.reserve(count);
			for (size_t index = 0; index < count; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader element, member.GetElement(index));
				ENGINE_TRY(element.ExpectType(JsonType::Object));
				ENGINE_TRY(RejectUnknownMembers(element, SubAssetKeys, "a sub-asset entry"));
				ENGINE_TRY_ASSIGN(const JsonReader keyMember, element.GetMember("Key"));
				ENGINE_TRY_ASSIGN(std::string key, keyMember.ReadString());
				if (key.empty())
					return std::unexpected(keyMember.MakeLocatedError(ErrorCode::Validation, "a sub-asset key must not be empty"));
				if (!entries.empty() && key <= entries.back().Key)
				{
					return std::unexpected(keyMember.MakeLocatedError(ErrorCode::Validation,
						key == entries.back().Key ? std::format("sub-asset key '{}' is repeated", key)
												  : std::format("sub-asset key '{}' is out of order: SubAssets is sorted by Key", key)));
				}
				ENGINE_TRY_ASSIGN(const JsonReader handleMember, element.GetMember("Handle"));
				ENGINE_TRY_ASSIGN(const UUID handle, handleMember.ReadUUID());
				const AssetHandle expected = DeriveSubAssetHandle(source, key);
				if (handle != expected)
				{
					return std::unexpected(handleMember.MakeLocatedError(ErrorCode::Validation,
						std::format("sub-asset '{}' has handle {}, but its handle is {} (Hash64 of the source handle and the key)", key,
							handle.ToString(), expected.ToString())));
				}
				ENGINE_TRY_ASSIGN(const JsonReader typeMember, element.GetMember("Type"));
				ENGINE_TRY_ASSIGN(const AssetType type, ReadAssetType(typeMember));
				entries.push_back({ .Key = std::move(key), .Handle = handle, .Type = type });
			}
			return entries;
		}

		static Result<AssetMetadata> ParseMetadataDocument(std::string_view text)
		{
			ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
			const JsonReader root(document);
			ENGINE_TRY(root.ExpectType(JsonType::Object));
			ENGINE_TRY(root.ReadFormatHeader(AssetMetadata::FormatName, AssetMetadata::CurrentVersion, AssetMetadata::CurrentVersion));

			AssetMetadata metadata;
			ENGINE_TRY_ASSIGN(metadata.Handle, ReadHandleMember(root, "Handle"));
			ENGINE_TRY_ASSIGN(const JsonReader typeMember, root.GetMember("Type"));
			ENGINE_TRY_ASSIGN(const std::string typeName, typeMember.ReadString());

			if (typeName == AssetMetadata::DependencyTypeName)
			{
				metadata.Kind = AssetMetaKind::Dependency;
				ENGINE_TRY(RejectUnknownMembers(root, DependencyMetaKeys, "a dependency meta"));
				ENGINE_TRY_ASSIGN(metadata.Owner, ReadHandleMember(root, "Owner"));
				return metadata;
			}

			metadata.Kind = AssetMetaKind::Asset;
			ENGINE_TRY_ASSIGN(metadata.Type, ReadAssetType(typeMember));
			ENGINE_TRY(RejectUnknownMembers(root, AssetMetaKeys, "an asset meta"));
			ENGINE_TRY_ASSIGN(const JsonReader importerMember, root.GetMember("Importer"));
			ENGINE_TRY_ASSIGN(metadata.Importer, importerMember.ReadString());
			if (metadata.Importer.empty())
				return std::unexpected(importerMember.MakeLocatedError(ErrorCode::Validation, "an asset meta names its importer"));
			ENGINE_TRY_ASSIGN(metadata.ImporterVersion, root.ReadMember<uint32_t>("ImporterVersion"));
			ENGINE_TRY_ASSIGN(const JsonReader settings, root.GetMember("Settings"));
			if (!settings.IsNull())
			{
				ENGINE_TRY(settings.ExpectType(JsonType::Object));
				metadata.Settings = VariantValue(settings.GetValue());
			}
			ENGINE_TRY_ASSIGN(const JsonReader subAssets, root.GetMember("SubAssets"));
			ENGINE_TRY_ASSIGN(metadata.SubAssets, ReadSubAssets(subAssets, metadata.Handle));
			return metadata;
		}

		[[maybe_unused]] static bool IsValidMetadata(const AssetMetadata& metadata)
		{
			if (!metadata.Handle.IsValid())
				return false;
			if (metadata.Kind == AssetMetaKind::Dependency)
			{
				return metadata.Owner.IsValid() && metadata.Type == AssetType::None && metadata.Importer.empty() && metadata.Settings.IsNull()
					&& metadata.SubAssets.empty();
			}
			if (metadata.Type == AssetType::None || metadata.Importer.empty() || metadata.Owner.IsValid())
				return false;
			if (!metadata.Settings.IsNull() && !metadata.Settings.Get().is_object())
				return false;
			for (size_t index = 0; index < metadata.SubAssets.size(); ++index)
			{
				const SubAssetEntry& entry = metadata.SubAssets[index];
				if (entry.Key.empty() || entry.Type == AssetType::None || entry.Handle != DeriveSubAssetHandle(metadata.Handle, entry.Key))
					return false;
				if (index > 0 && metadata.SubAssets[index - 1].Key >= entry.Key)
					return false;
			}
			return true;
		}

	}

	const SubAssetEntry* AssetMetadata::FindSubAsset(std::string_view key) const
	{
		const auto found = std::ranges::find_if(SubAssets, [key](const SubAssetEntry& entry)
		{
			return entry.Key == key;
		});
		return found == SubAssets.end() ? nullptr : &*found;
	}

	Result<VfsPath> GetMetaPath(const VfsPath& source)
	{
		if (source.IsEmpty() || source.IsRoot())
			return MakeError(ErrorCode::InvalidArgument, "'{}' is not a file, so it has no .meta", source.ToString());
		if (source.GetExtension() == Utils::MetaExtension)
			return MakeError(ErrorCode::InvalidArgument, "'{}' is a .meta itself, which has no .meta", source.ToString());
		return VfsPath::Create(source.GetScheme(), std::format("{}{}", source.GetPath(), Utils::MetaExtension));
	}

	Result<VfsPath> GetSourcePathOfMeta(const VfsPath& metaPath)
	{
		const std::string_view name = metaPath.GetFileName();
		if (!name.ends_with(Utils::MetaExtension) || name.size() == Utils::MetaExtension.size())
			return MakeError(ErrorCode::InvalidArgument, "'{}' is not a .meta of a source file", metaPath.ToString());
		const std::string_view path = metaPath.GetPath();
		return VfsPath::Create(metaPath.GetScheme(), path.substr(0, path.size() - Utils::MetaExtension.size()));
	}

	Result<AssetMetadata> ParseAssetMetadata(std::string_view text, std::string_view metaPath)
	{
		Result<AssetMetadata> metadata = Utils::ParseMetadataDocument(text);
		if (!metadata.has_value() && !metaPath.empty())
		{
			ErrorLocation location;
			location.File = std::string(metaPath);
			return std::unexpected(std::move(metadata).error().WithLocation(std::move(location)));
		}
		return metadata;
	}

	std::string SerializeAssetMetadata(const AssetMetadata& metadata)
	{
		ENGINE_CORE_ASSERT(Utils::IsValidMetadata(metadata), "SerializeAssetMetadata of an invalid meta (handle {})", metadata.Handle.ToString());

		JsonWriter writer(JsonStyle::Pretty);
		writer.BeginObject();
		writer.WriteKey("Format");
		writer.WriteString(AssetMetadata::FormatName);
		writer.WriteKey("Version");
		writer.WriteUInt(AssetMetadata::CurrentVersion);
		writer.WriteKey("Handle");
		writer.WriteUUID(metadata.Handle);
		writer.WriteKey("Type");
		if (metadata.Kind == AssetMetaKind::Dependency)
		{
			writer.WriteString(AssetMetadata::DependencyTypeName);
			writer.WriteKey("Owner");
			writer.WriteUUID(metadata.Owner);
		}
		else
		{
			writer.WriteString(AssetTypeToString(metadata.Type));
			writer.WriteKey("Importer");
			writer.WriteString(metadata.Importer);
			writer.WriteKey("ImporterVersion");
			writer.WriteUInt(metadata.ImporterVersion);
			writer.WriteKey("Settings");
			writer.WriteJson(metadata.Settings.Get());
			writer.WriteKey("SubAssets");
			writer.BeginArray();
			for (const SubAssetEntry& entry : metadata.SubAssets)
			{
				writer.BeginObject();
				writer.WriteKey("Key");
				writer.WriteString(entry.Key);
				writer.WriteKey("Handle");
				writer.WriteUUID(entry.Handle);
				writer.WriteKey("Type");
				writer.WriteString(AssetTypeToString(entry.Type));
				writer.EndObject();
			}
			writer.EndArray();
		}
		writer.EndObject();

		Result<std::string> text = writer.Finish();
		ENGINE_CORE_ASSERT(text.has_value(), "SerializeAssetMetadata: {}", text.has_value() ? std::string() : text.error().ToString());
		return text.has_value() ? std::move(*text) : std::string();
	}

}
