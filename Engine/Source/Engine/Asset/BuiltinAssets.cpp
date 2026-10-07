#include "EnginePCH.h"
#include "Engine/Asset/BuiltinAssets.h"

#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/BuiltinTextures.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace Engine {

	namespace {

		// One compiled-in entry of GetProceduralBuiltinEntries.
		struct ProceduralEntry
		{
			AssetHandle Handle{};
			std::string_view Path{};
			AssetType Type = AssetType::None;
		};

		constexpr std::array<ProceduralEntry, 14> ProceduralEntries = { {
			{ BuiltinAssetHandles::CubeMesh, "engine://Meshes/Cube", AssetType::Mesh },
			{ BuiltinAssetHandles::SphereMesh, "engine://Meshes/Sphere", AssetType::Mesh },
			{ BuiltinAssetHandles::PlaneMesh, "engine://Meshes/Plane", AssetType::Mesh },
			{ BuiltinAssetHandles::QuadMesh, "engine://Meshes/Quad", AssetType::Mesh },
			{ BuiltinAssetHandles::CylinderMesh, "engine://Meshes/Cylinder", AssetType::Mesh },
			{ BuiltinAssetHandles::CapsuleMesh, "engine://Meshes/Capsule", AssetType::Mesh },
			{ BuiltinAssetHandles::ConeMesh, "engine://Meshes/Cone", AssetType::Mesh },
			{ BuiltinAssetHandles::DefaultMaterial, "engine://Materials/Default", AssetType::Material },
			{ BuiltinAssetHandles::ErrorMaterial, "engine://Materials/Error", AssetType::Material },
			{ BuiltinAssetHandles::WhiteTexture, "engine://Textures/White", AssetType::Texture },
			{ BuiltinAssetHandles::BlackTexture, "engine://Textures/Black", AssetType::Texture },
			{ BuiltinAssetHandles::FlatNormalTexture, "engine://Textures/FlatNormal", AssetType::Texture },
			{ BuiltinAssetHandles::CheckerTexture, "engine://Textures/Checker", AssetType::Texture },
			{ BuiltinAssetHandles::MissingTexture, "engine://Textures/Missing", AssetType::Texture },
		} };

		// The generator of each procedural handle.
		constexpr std::array<std::pair<AssetHandle, BuiltinMesh>, 7> ProceduralMeshes = { {
			{ BuiltinAssetHandles::CubeMesh, BuiltinMesh::Cube },
			{ BuiltinAssetHandles::SphereMesh, BuiltinMesh::Sphere },
			{ BuiltinAssetHandles::PlaneMesh, BuiltinMesh::Plane },
			{ BuiltinAssetHandles::QuadMesh, BuiltinMesh::Quad },
			{ BuiltinAssetHandles::CylinderMesh, BuiltinMesh::Cylinder },
			{ BuiltinAssetHandles::CapsuleMesh, BuiltinMesh::Capsule },
			{ BuiltinAssetHandles::ConeMesh, BuiltinMesh::Cone },
		} };
		constexpr std::array<std::pair<AssetHandle, BuiltinMaterial>, 2> ProceduralMaterials = { {
			{ BuiltinAssetHandles::DefaultMaterial, BuiltinMaterial::Default },
			{ BuiltinAssetHandles::ErrorMaterial, BuiltinMaterial::Error },
		} };
		constexpr std::array<std::pair<AssetHandle, BuiltinTexture>, 5> ProceduralTextures = { {
			{ BuiltinAssetHandles::WhiteTexture, BuiltinTexture::White },
			{ BuiltinAssetHandles::BlackTexture, BuiltinTexture::Black },
			{ BuiltinAssetHandles::FlatNormalTexture, BuiltinTexture::FlatNormal },
			{ BuiltinAssetHandles::CheckerTexture, BuiltinTexture::Checker },
			{ BuiltinAssetHandles::MissingTexture, BuiltinTexture::Missing },
		} };

		// The members of the catalogue and of its entries, in their canonical order.
		constexpr std::array<std::string_view, 3> RootMembers = { "Format", "Version", "Assets" };
		constexpr std::array<std::string_view, 8> EntryMembers = { "Handle", "Path", "Type", "Source", "File", "Importer", "Settings", "Generator" };

		constexpr std::string_view EnginePathPrefix = "engine://";

	}

	namespace Utils {

		static std::string_view SourceToString(BuiltinAssetSource source)
		{
			switch (source)
			{
				case BuiltinAssetSource::Procedural: return "Procedural";
				case BuiltinAssetSource::File:       return "File";
				case BuiltinAssetSource::Generated:  return "Generated";
			}
			ENGINE_CORE_ASSERT(false, "Unknown BuiltinAssetSource {}", std::to_underlying(source));
			return "Procedural";
		}

		static std::optional<BuiltinAssetSource> SourceFromString(std::string_view name)
		{
			for (const BuiltinAssetSource source : { BuiltinAssetSource::Procedural, BuiltinAssetSource::File, BuiltinAssetSource::Generated })
			{
				if (SourceToString(source) == name)
					return source;
			}
			return std::nullopt;
		}

		// Fails, located at the first one, when `object` has a member outside `known`: the catalogue is engine-written, so
		// an unknown member is a typo, never data to keep.
		static Status RejectUnknownMembers(const JsonReader& object, std::span<const std::string_view> known)
		{
			ENGINE_TRY_ASSIGN(const std::vector<std::string> unknown, object.FindUnknownMembers(known));
			if (!unknown.empty())
				return std::unexpected(object.MakeLocatedError(ErrorCode::Validation, std::format("unknown member '{}'", unknown.front())));
			return {};
		}

		// "engine://<path>" naming a file, without a sub-asset key.
		static Status ValidateEnginePath(const JsonReader& member, std::string_view path)
		{
			if (!path.starts_with(EnginePathPrefix))
				return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, std::format("'{}' is not an engine path (engine://...)", path)));
			if (path.contains('#'))
				return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, std::format("'{}' names a sub-asset; built-ins are main assets", path)));
			const Result<VfsPath> parsed = VfsPath::Parse(path);
			if (!parsed)
				return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, std::format("'{}': {}", path, parsed.error().GetMessageText())));
			if (parsed->IsRoot())
				return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, std::format("'{}' names no asset", path)));
			return {};
		}

		static Status ExpectEmpty(const JsonReader& entry, std::string_view member, const std::string& value, std::string_view source)
		{
			if (value.empty())
				return {};
			ENGINE_TRY_ASSIGN(const JsonReader reader, entry.GetMember(member));
			return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, std::format("a {} entry leaves '{}' empty", source, member)));
		}

		static Status ExpectNotEmpty(const JsonReader& entry, std::string_view member, const std::string& value, std::string_view source)
		{
			if (!value.empty())
				return {};
			ENGINE_TRY_ASSIGN(const JsonReader reader, entry.GetMember(member));
			return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, std::format("a {} entry needs a non-empty '{}'", source, member)));
		}

		// The members of one entry, each checked on its own; the rules that tie them to the source follow in ReadEntry.
		static Result<BuiltinAssetEntry> ReadEntryMembers(const JsonReader& element)
		{
			BuiltinAssetEntry entry;
			ENGINE_TRY_ASSIGN(const JsonReader handle, element.GetMember("Handle"));
			ENGINE_TRY_ASSIGN(entry.Handle, handle.ReadUUID());
			if (!IsBuiltinAssetHandle(entry.Handle))
			{
				return std::unexpected(handle.MakeLocatedError(ErrorCode::Validation,
					std::format("handle {} is outside the reserved built-in range {:016x} to {:016x}", entry.Handle, FirstBuiltinAssetHandle,
						LastBuiltinAssetHandle)));
			}

			ENGINE_TRY_ASSIGN(const JsonReader path, element.GetMember("Path"));
			ENGINE_TRY_ASSIGN(entry.Path, path.ReadString());
			ENGINE_TRY(ValidateEnginePath(path, entry.Path));

			ENGINE_TRY_ASSIGN(const JsonReader type, element.GetMember("Type"));
			ENGINE_TRY_ASSIGN(const std::string typeName, type.ReadString());
			const std::optional<AssetType> assetType = AssetTypeFromString(typeName);
			if (!assetType || *assetType == AssetType::None)
				return std::unexpected(type.MakeLocatedError(ErrorCode::Validation, std::format("'{}' is not an asset type", typeName)));
			entry.Type = *assetType;

			ENGINE_TRY_ASSIGN(const JsonReader source, element.GetMember("Source"));
			ENGINE_TRY_ASSIGN(const std::string sourceName, source.ReadString());
			const std::optional<BuiltinAssetSource> assetSource = SourceFromString(sourceName);
			if (!assetSource)
			{
				return std::unexpected(source.MakeLocatedError(ErrorCode::Validation,
					std::format("unknown source '{}' (expected Procedural, File or Generated)", sourceName)));
			}
			entry.Source = *assetSource;

			ENGINE_TRY_ASSIGN(entry.File, element.ReadMember<std::string>("File"));
			ENGINE_TRY_ASSIGN(entry.Importer, element.ReadMember<std::string>("Importer"));
			ENGINE_TRY_ASSIGN(entry.Generator, element.ReadMember<std::string>("Generator"));

			// {} is the importer's defaults, held as null so that equal entries compare equal (BuiltinAssets.h).
			ENGINE_TRY_ASSIGN(const JsonReader settings, element.GetMember("Settings"));
			ENGINE_TRY(settings.ExpectType(JsonType::Object));
			if (!settings.GetValue().empty())
			{
				const Result<std::string> written = JsonWriter::Write(settings.GetValue(), JsonStyle::Minified);
				if (!written)
					return std::unexpected(settings.MakeLocatedError(ErrorCode::Validation, written.error().GetMessageText()));
				entry.Settings = VariantValue(settings.GetValue());
			}
			return entry;
		}

		static Result<BuiltinAssetEntry> ReadEntry(const JsonReader& element)
		{
			ENGINE_TRY(element.ExpectType(JsonType::Object));
			ENGINE_TRY(RejectUnknownMembers(element, EntryMembers));
			ENGINE_TRY_ASSIGN(BuiltinAssetEntry entry, ReadEntryMembers(element));

			const std::string_view source = SourceToString(entry.Source);
			switch (entry.Source)
			{
				case BuiltinAssetSource::Procedural:
				{
					ENGINE_TRY(ExpectEmpty(element, "File", entry.File, source));
					ENGINE_TRY(ExpectEmpty(element, "Importer", entry.Importer, source));
					ENGINE_TRY(ExpectEmpty(element, "Generator", entry.Generator, source));
					const std::span<const BuiltinAssetEntry> procedural = GetProceduralBuiltinEntries();
					const auto match = std::ranges::find(procedural, entry.Handle, &BuiltinAssetEntry::Handle);
					if (match == procedural.end() || match->Path != entry.Path || match->Type != entry.Type)
					{
						return std::unexpected(element.MakeLocatedError(ErrorCode::Validation,
							std::format("the engine cannot generate a procedural {} '{}' with handle {}", AssetTypeToString(entry.Type), entry.Path,
								entry.Handle)));
					}
					break;
				}
				case BuiltinAssetSource::File:
				{
					ENGINE_TRY(ExpectNotEmpty(element, "File", entry.File, source));
					ENGINE_TRY(ExpectNotEmpty(element, "Importer", entry.Importer, source));
					ENGINE_TRY(ExpectEmpty(element, "Generator", entry.Generator, source));
					const Status file = VfsPath::ValidateRelativePath(entry.File);
					if (!file)
					{
						ENGINE_TRY_ASSIGN(const JsonReader member, element.GetMember("File"));
						return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, file.error().GetMessageText()));
					}
					break;
				}
				case BuiltinAssetSource::Generated:
				{
					ENGINE_TRY(ExpectNotEmpty(element, "Generator", entry.Generator, source));
					ENGINE_TRY(ExpectEmpty(element, "File", entry.File, source));
					ENGINE_TRY(ExpectEmpty(element, "Importer", entry.Importer, source));
					break;
				}
			}
			if (entry.Source != BuiltinAssetSource::File && !entry.Settings.IsNull())
			{
				ENGINE_TRY_ASSIGN(const JsonReader member, element.GetMember("Settings"));
				return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, std::format("a {} entry has no settings: write {{}}", source)));
			}
			return entry;
		}

		template<typename Kind, size_t Count>
		static std::optional<Kind> FindProcedural(const std::array<std::pair<AssetHandle, Kind>, Count>& table, AssetHandle handle)
		{
			for (const auto& [entryHandle, kind] : table)
			{
				if (entryHandle == handle)
					return kind;
			}
			return std::nullopt;
		}

	}

	std::span<const BuiltinAssetEntry> GetProceduralBuiltinEntries()
	{
		static const std::vector<BuiltinAssetEntry> Entries = []()
		{
			std::vector<BuiltinAssetEntry> entries;
			entries.reserve(ProceduralEntries.size());
			for (const ProceduralEntry& entry : ProceduralEntries)
			{
				entries.push_back({
					.Handle = entry.Handle,
					.Path = std::string(entry.Path),
					.Type = entry.Type,
					.Source = BuiltinAssetSource::Procedural,
					.File = {},
					.Importer = {},
					.Settings = VariantValue(),
					.Generator = {},
				});
			}
			return entries;
		}();
		return Entries;
	}

	Result<BuiltinAssetCatalog> BuiltinAssetCatalog::Parse(std::string_view text)
	{
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		const JsonReader root(document);
		ENGINE_TRY(root.ExpectType(JsonType::Object));
		ENGINE_TRY(root.ReadFormatHeader(FormatName, CurrentVersion, CurrentVersion));
		ENGINE_TRY(Utils::RejectUnknownMembers(root, RootMembers));

		ENGINE_TRY_ASSIGN(const JsonReader assets, root.GetMember("Assets"));
		ENGINE_TRY_ASSIGN(const size_t count, assets.GetArraySize());
		BuiltinAssetCatalog catalog;
		catalog.m_Entries.reserve(count);
		std::set<std::string, std::less<>> paths;
		for (size_t index = 0; index < count; ++index)
		{
			ENGINE_TRY_ASSIGN(const JsonReader element, assets.GetElement(index));
			ENGINE_TRY_ASSIGN(BuiltinAssetEntry entry, Utils::ReadEntry(element));
			if (!catalog.m_Entries.empty())
			{
				const AssetHandle previous = catalog.m_Entries.back().Handle;
				if (entry.Handle == previous)
					return std::unexpected(element.MakeLocatedError(ErrorCode::Validation, std::format("handle {} is repeated", entry.Handle)));
				if (entry.Handle < previous)
				{
					return std::unexpected(element.MakeLocatedError(ErrorCode::Validation,
						std::format("the entries are not sorted by handle: {} follows {}", entry.Handle, previous)));
				}
			}
			if (!paths.insert(entry.Path).second)
				return std::unexpected(element.MakeLocatedError(ErrorCode::Validation, std::format("path '{}' is repeated", entry.Path)));
			catalog.m_Entries.push_back(std::move(entry));
		}
		return catalog;
	}

	Result<BuiltinAssetCatalog> BuiltinAssetCatalog::Load(const VirtualFileSystem& vfs)
	{
		ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Create("engine", FileName));
		ENGINE_TRY_ASSIGN(const std::string text, WithContext(vfs.ReadText(path), "while reading the built-in asset catalogue"));
		return WithContext(Parse(text), std::format("in '{}'", path.ToString()));
	}

	std::string BuiltinAssetCatalog::ToText() const
	{
		JsonWriter writer(JsonStyle::Pretty);
		writer.BeginObject();
		writer.WriteKey("Format");
		writer.WriteString(FormatName);
		writer.WriteKey("Version");
		writer.WriteUInt(CurrentVersion);
		writer.WriteKey("Assets");
		writer.BeginArray();
		for (const BuiltinAssetEntry& entry : m_Entries)
		{
			writer.BeginObject();
			writer.WriteKey("Handle");
			writer.WriteUUID(entry.Handle);
			writer.WriteKey("Path");
			writer.WriteString(entry.Path);
			writer.WriteKey("Type");
			writer.WriteString(AssetTypeToString(entry.Type));
			writer.WriteKey("Source");
			writer.WriteString(Utils::SourceToString(entry.Source));
			writer.WriteKey("File");
			writer.WriteString(entry.File);
			writer.WriteKey("Importer");
			writer.WriteString(entry.Importer);
			writer.WriteKey("Settings");
			if (entry.Settings.IsNull())
			{
				writer.BeginObject();
				writer.EndObject();
			}
			else
			{
				writer.WriteJson(entry.Settings.Get());
			}
			writer.WriteKey("Generator");
			writer.WriteString(entry.Generator);
			writer.EndObject();
		}
		writer.EndArray();
		writer.EndObject();
		// Parse accepted only entries that can be written (strings, settings), so the writer cannot report a data problem.
		Result<std::string> text = writer.Finish();
		ENGINE_CORE_VERIFY(text.has_value(), "BuiltinAssetCatalog::ToText could not write an entry that Parse accepted");
		return text.has_value() ? std::move(*text) : std::string();
	}

	const BuiltinAssetEntry* BuiltinAssetCatalog::Find(AssetHandle handle) const
	{
		const auto entry = std::ranges::lower_bound(m_Entries, handle, {}, &BuiltinAssetEntry::Handle);
		return entry != m_Entries.end() && entry->Handle == handle ? &*entry : nullptr;
	}

	const BuiltinAssetEntry* BuiltinAssetCatalog::FindByPath(std::string_view path) const
	{
		const auto entry = std::ranges::find(m_Entries, path, &BuiltinAssetEntry::Path);
		return entry != m_Entries.end() ? &*entry : nullptr;
	}

	Result<AssetRef<Asset>> CreateProceduralBuiltinAsset(AssetHandle handle)
	{
		if (const std::optional<BuiltinMesh> mesh = Utils::FindProcedural(ProceduralMeshes, handle))
			return AssetRef<Asset>(CreateRef<MeshData>(GenerateBuiltinMesh(*mesh)));
		if (const std::optional<BuiltinTexture> texture = Utils::FindProcedural(ProceduralTextures, handle))
			return AssetRef<Asset>(CreateRef<TextureData>(GenerateBuiltinTexture(*texture)));
		if (const std::optional<BuiltinMaterial> material = Utils::FindProcedural(ProceduralMaterials, handle))
			return AssetRef<Asset>(CreateRef<MaterialData>(CreateBuiltinMaterial(*material)));
		return MakeError(ErrorCode::NotFound, "{} is not a procedural built-in asset", handle);
	}

	MaterialData CreateBuiltinMaterial(BuiltinMaterial material)
	{
		MaterialData data;
		switch (material)
		{
			case BuiltinMaterial::Default:
				return data;
			case BuiltinMaterial::Error:
			{
				// Magenta in both the base colour and the emission, rough, so it reads as flat magenta under any lighting.
				data.BaseColor = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
				data.Emissive = glm::vec3(1.0f, 0.0f, 1.0f);
				data.Roughness = 1.0f;
				return data;
			}
		}
		ENGINE_CORE_ASSERT(false, "Unknown BuiltinMaterial {}", std::to_underlying(material));
		return data;
	}

}
