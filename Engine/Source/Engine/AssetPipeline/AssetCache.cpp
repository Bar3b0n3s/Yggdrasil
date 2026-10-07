#include "EnginePCH.h"
#include "Engine/AssetPipeline/AssetCache.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <utility>

namespace Engine {

	struct AssetCache::State
	{
		VirtualFileSystem* Vfs = nullptr; // documented back-reference
		VfsPath Root;
	};

	namespace {

		constexpr std::string_view ArtifactExtension = ".bin";
		constexpr std::string_view ManifestExtension = ".import";

		// The members of a manifest and of its records, in canonical order (AssetCache.h).
		constexpr std::string_view ManifestMembers[] = { "Format", "Version", "Artifacts", "Dependencies", "Diagnostics", "Reads", "Lookups" };
		constexpr std::string_view ArtifactMembers[] = { "Handle", "Type", "Key" };
		constexpr std::string_view DiagnosticMembers[] = { "Severity", "Code", "Path", "Message", "Hint", "Subject" };
		constexpr std::string_view ReadMembers[] = { "Path", "XXH64" };
		constexpr std::string_view LookupMembers[] = { "Path", "Handle", "Type" };

		// One artifact record of a manifest: its bytes are in <root>/<Handle>/<key>.bin.
		struct ManifestArtifact
		{
			AssetHandle Handle{};
			AssetType Type = AssetType::None;
			std::string SubAssetKey{};
		};

		// A manifest as read, before its artifacts are.
		struct Manifest
		{
			std::vector<ManifestArtifact> Artifacts{};
			std::vector<AssetHandle> Dependencies{};
			std::vector<AssetDiagnostic> Diagnostics{};
			std::vector<ImportDependencyRead> Reads{};
			std::vector<ImportAssetLookup> Lookups{};
		};

	}

	namespace Utils {

		static std::string FormatHex(uint64_t value)
		{
			return std::format("{:016x}", value);
		}

		static std::string_view SeverityToString(DiagnosticSeverity severity)
		{
			return severity == DiagnosticSeverity::Error ? "Error" : "Warning";
		}

		// <root>/<handle>, and the files of one key in it.
		static Result<VfsPath> HandleDirectory(const VfsPath& root, AssetHandle handle)
		{
			return root.Join(FormatHex(handle.GetValue()));
		}

		static Result<VfsPath> KeyFile(const VfsPath& root, AssetHandle handle, uint64_t key, std::string_view extension)
		{
			return root.Join(std::format("{}/{}{}", FormatHex(handle.GetValue()), FormatHex(key), extension));
		}

		// A lookup's "Type": the AssetType name of an asset, "Dependency" for a dependency meta, null when nothing was found.
		static void WriteLookupType(JsonWriter& writer, const std::optional<ImportAssetLookupEntry>& found)
		{
			if (!found.has_value())
				writer.WriteNull();
			else if (found->Kind == AssetMetaKind::Dependency)
				writer.WriteString(AssetMetadata::DependencyTypeName);
			else
				writer.WriteString(AssetTypeToString(found->Type));
		}

		// The canonical manifest text of `import` (AssetCache.h). Errors: Validation when the writer rejects a value (a path
		// or message that is not valid UTF-8).
		static Result<std::string> WriteManifest(const CachedImport& import)
		{
			JsonWriter writer(JsonStyle::Pretty);
			writer.BeginObject();
			writer.WriteKey("Format");
			writer.WriteString(AssetCache::ManifestFormatName);
			writer.WriteKey("Version");
			writer.WriteUInt(AssetCache::ManifestVersion);

			writer.WriteKey("Artifacts");
			writer.BeginArray();
			for (const ImportedArtifact& artifact : import.Import.Artifacts)
			{
				writer.BeginObject();
				writer.WriteKey("Handle");
				writer.WriteUUID(artifact.Handle);
				writer.WriteKey("Type");
				writer.WriteString(AssetTypeToString(artifact.Type));
				writer.WriteKey("Key");
				writer.WriteString(artifact.SubAssetKey);
				writer.EndObject();
			}
			writer.EndArray();

			writer.WriteKey("Dependencies");
			writer.BeginArray();
			for (const AssetHandle dependency : import.Import.Dependencies)
				writer.WriteUUID(dependency);
			writer.EndArray();

			writer.WriteKey("Diagnostics");
			writer.BeginArray();
			for (const AssetDiagnostic& diagnostic : import.Import.Diagnostics)
			{
				writer.BeginObject();
				writer.WriteKey("Severity");
				writer.WriteString(SeverityToString(diagnostic.Severity));
				writer.WriteKey("Code");
				writer.WriteString(diagnostic.Code);
				writer.WriteKey("Path");
				writer.WriteString(diagnostic.Path);
				writer.WriteKey("Message");
				writer.WriteString(diagnostic.Message);
				writer.WriteKey("Hint");
				writer.WriteString(diagnostic.Hint);
				writer.WriteKey("Subject");
				writer.WriteString(diagnostic.Subject);
				writer.EndObject();
			}
			writer.EndArray();

			writer.WriteKey("Reads");
			writer.BeginArray();
			for (const ImportDependencyRead& read : import.Reads)
			{
				writer.BeginObject();
				writer.WriteKey("Path");
				writer.WriteString(read.Path.ToString());
				writer.WriteKey("XXH64");
				writer.WriteString(FormatHex(read.Hash));
				writer.EndObject();
			}
			writer.EndArray();

			writer.WriteKey("Lookups");
			writer.BeginArray();
			for (const ImportAssetLookup& lookup : import.Lookups)
			{
				writer.BeginObject();
				writer.WriteKey("Path");
				writer.WriteString(lookup.Path.ToString());
				writer.WriteKey("Handle");
				if (lookup.Found.has_value() && lookup.Found->Handle.IsValid())
					writer.WriteUUID(lookup.Found->Handle);
				else
					writer.WriteNull();
				writer.WriteKey("Type");
				WriteLookupType(writer, lookup.Found);
				writer.EndObject();
			}
			writer.EndArray();
			writer.EndObject();
			return writer.Finish();
		}

		static Status RejectUnknownMembers(const JsonReader& reader, std::span<const std::string_view> known)
		{
			ENGINE_TRY_ASSIGN(const std::vector<std::string> unknown, reader.FindUnknownMembers(known));
			if (!unknown.empty())
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, std::format("unknown member '{}'", unknown.front())));
			return {};
		}

		static Result<AssetHandle> ReadValidHandle(const JsonReader& reader)
		{
			ENGINE_TRY_ASSIGN(const AssetHandle handle, reader.ReadUUID());
			if (!handle.IsValid())
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, "expected a handle other than the null handle"));
			return handle;
		}

		static Result<uint64_t> ReadHex(const JsonReader& reader)
		{
			ENGINE_TRY_ASSIGN(const std::string text, reader.ReadString());
			const std::optional<UUID> value = UUID::FromString(text);
			if (!value.has_value())
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, "expected 16 hex digits"));
			return value->GetValue();
		}

		static Result<VfsPath> ReadPath(const JsonReader& reader)
		{
			ENGINE_TRY_ASSIGN(const std::string text, reader.ReadString());
			Result<VfsPath> path = VfsPath::Parse(text);
			if (!path.has_value())
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, path.error().GetMessageText()));
			return std::move(*path);
		}

		static Result<AssetType> ReadAssetType(const JsonReader& reader)
		{
			ENGINE_TRY_ASSIGN(const std::string name, reader.ReadString());
			const std::optional<AssetType> type = AssetTypeFromString(name);
			if (!type.has_value() || *type == AssetType::None)
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, std::format("unknown asset type '{}'", name)));
			return *type;
		}

		// The array member `key` of `reader`, each element read with `readElement(element) -> Result<T>`.
		template<typename T, typename ReadElement>
		static Result<std::vector<T>> ReadArrayMember(const JsonReader& reader, std::string_view key, ReadElement&& readElement)
		{
			ENGINE_TRY_ASSIGN(const JsonReader array, reader.GetMember(key));
			ENGINE_TRY_ASSIGN(const size_t count, array.GetArraySize());
			std::vector<T> elements;
			elements.reserve(count);
			for (size_t index = 0; index < count; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader element, array.GetElement(index));
				ENGINE_TRY_ASSIGN(T value, readElement(element));
				elements.push_back(std::move(value));
			}
			return elements;
		}

		static Result<ManifestArtifact> ReadManifestArtifact(const JsonReader& element)
		{
			ENGINE_TRY(element.ExpectType(JsonType::Object));
			ENGINE_TRY(RejectUnknownMembers(element, ArtifactMembers));
			ManifestArtifact artifact;
			ENGINE_TRY_ASSIGN(const JsonReader handle, element.GetMember("Handle"));
			ENGINE_TRY_ASSIGN(artifact.Handle, ReadValidHandle(handle));
			ENGINE_TRY_ASSIGN(const JsonReader type, element.GetMember("Type"));
			ENGINE_TRY_ASSIGN(artifact.Type, ReadAssetType(type));
			ENGINE_TRY_ASSIGN(artifact.SubAssetKey, element.ReadMember<std::string>("Key"));
			return artifact;
		}

		// One diagnostic of the import of `source`. The manifest does not store the diagnostic's asset: an import reports
		// about its own source.
		static Result<AssetDiagnostic> ReadManifestDiagnostic(const JsonReader& element, AssetHandle source)
		{
			ENGINE_TRY(element.ExpectType(JsonType::Object));
			ENGINE_TRY(RejectUnknownMembers(element, DiagnosticMembers));
			AssetDiagnostic diagnostic;
			diagnostic.Asset = source;
			ENGINE_TRY_ASSIGN(const JsonReader severity, element.GetMember("Severity"));
			ENGINE_TRY_ASSIGN(const std::string severityName, severity.ReadString());
			if (severityName == SeverityToString(DiagnosticSeverity::Error))
				diagnostic.Severity = DiagnosticSeverity::Error;
			else if (severityName == SeverityToString(DiagnosticSeverity::Warning))
				diagnostic.Severity = DiagnosticSeverity::Warning;
			else
				return std::unexpected(severity.MakeLocatedError(ErrorCode::Validation, std::format("unknown severity '{}'", severityName)));
			ENGINE_TRY_ASSIGN(diagnostic.Code, element.ReadMember<std::string>("Code"));
			ENGINE_TRY_ASSIGN(diagnostic.Path, element.ReadMember<std::string>("Path"));
			ENGINE_TRY_ASSIGN(diagnostic.Message, element.ReadMember<std::string>("Message"));
			ENGINE_TRY_ASSIGN(diagnostic.Hint, element.ReadMember<std::string>("Hint"));
			ENGINE_TRY_ASSIGN(diagnostic.Subject, element.ReadMember<std::string>("Subject"));
			return diagnostic;
		}

		static Result<ImportDependencyRead> ReadManifestRead(const JsonReader& element)
		{
			ENGINE_TRY(element.ExpectType(JsonType::Object));
			ENGINE_TRY(RejectUnknownMembers(element, ReadMembers));
			ImportDependencyRead read;
			ENGINE_TRY_ASSIGN(const JsonReader path, element.GetMember("Path"));
			ENGINE_TRY_ASSIGN(read.Path, ReadPath(path));
			ENGINE_TRY_ASSIGN(const JsonReader hash, element.GetMember("XXH64"));
			ENGINE_TRY_ASSIGN(read.Hash, ReadHex(hash));
			return read;
		}

		static Result<ImportAssetLookup> ReadManifestLookup(const JsonReader& element)
		{
			ENGINE_TRY(element.ExpectType(JsonType::Object));
			ENGINE_TRY(RejectUnknownMembers(element, LookupMembers));
			ImportAssetLookup lookup;
			ENGINE_TRY_ASSIGN(const JsonReader path, element.GetMember("Path"));
			ENGINE_TRY_ASSIGN(lookup.Path, ReadPath(path));
			ENGINE_TRY_ASSIGN(const JsonReader handle, element.GetMember("Handle"));
			ENGINE_TRY_ASSIGN(const JsonReader type, element.GetMember("Type"));
			if (handle.IsNull() != type.IsNull())
				return std::unexpected(element.MakeLocatedError(ErrorCode::Validation, "a lookup has either both a handle and a type or neither"));
			if (handle.IsNull())
				return lookup;

			// What FindAsset found at exactly this path; the owner of a dependency is not recorded.
			ImportAssetLookupEntry found;
			found.SourcePath = lookup.Path;
			ENGINE_TRY_ASSIGN(found.Handle, ReadValidHandle(handle));
			ENGINE_TRY_ASSIGN(const std::string typeName, type.ReadString());
			if (typeName == AssetMetadata::DependencyTypeName)
			{
				found.Kind = AssetMetaKind::Dependency;
			}
			else
			{
				found.Kind = AssetMetaKind::Asset;
				ENGINE_TRY_ASSIGN(found.Type, ReadAssetType(type));
			}
			lookup.Found = std::move(found);
			return lookup;
		}

		// Reads a manifest strictly: a cache entry is engine-written, so anything unexpected means corruption. `source` is the
		// handle the manifest is filed under, which must be its main artifact's.
		static Result<Manifest> ParseManifest(std::string_view text, AssetHandle source)
		{
			ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
			const JsonReader root(document);
			ENGINE_TRY(root.ExpectType(JsonType::Object));
			ENGINE_TRY(root.ReadFormatHeader(AssetCache::ManifestFormatName, AssetCache::ManifestVersion, AssetCache::ManifestVersion));
			ENGINE_TRY(RejectUnknownMembers(root, ManifestMembers));

			Manifest manifest;
			ENGINE_TRY_ASSIGN(manifest.Artifacts, ReadArrayMember<ManifestArtifact>(root, "Artifacts", &ReadManifestArtifact));
			if (manifest.Artifacts.empty() || manifest.Artifacts.front().Handle != source || !manifest.Artifacts.front().SubAssetKey.empty())
				return MakeError(ErrorCode::Validation, "the manifest's first artifact is not the main asset {}", source);
			std::set<AssetHandle> artifactHandles;
			for (const ManifestArtifact& artifact : manifest.Artifacts)
			{
				if (!artifactHandles.insert(artifact.Handle).second)
					return MakeError(ErrorCode::Validation, "the manifest names the artifact {} twice", artifact.Handle);
			}

			ENGINE_TRY_ASSIGN(manifest.Dependencies, ReadArrayMember<AssetHandle>(root, "Dependencies", &ReadValidHandle));
			const auto readDiagnostic = [source](const JsonReader& element)
			{
				return ReadManifestDiagnostic(element, source);
			};
			ENGINE_TRY_ASSIGN(manifest.Diagnostics, ReadArrayMember<AssetDiagnostic>(root, "Diagnostics", readDiagnostic));
			ENGINE_TRY_ASSIGN(manifest.Reads, ReadArrayMember<ImportDependencyRead>(root, "Reads", &ReadManifestRead));
			ENGINE_TRY_ASSIGN(manifest.Lookups, ReadArrayMember<ImportAssetLookup>(root, "Lookups", &ReadManifestLookup));
			return manifest;
		}

		// Removes every file of directory `directory` that is not one of the files of `keepKey` (both kinds). A missing
		// directory has nothing to remove.
		static Status RemoveOtherKeys(VirtualFileSystem& vfs, const VfsPath& directory, std::string_view keepKey)
		{
			Result<std::vector<VfsEntry>> listed = vfs.List(directory, false);
			if (!listed.has_value())
			{
				if (listed.error().GetCode() == ErrorCode::NotFound)
					return {};
				return std::unexpected(std::move(listed).error());
			}
			for (const VfsEntry& entry : *listed)
			{
				const std::string_view name = entry.Path.GetFileName();
				const bool kept = !entry.Info.IsDirectory
					&& (name == std::format("{}{}", keepKey, ArtifactExtension) || name == std::format("{}{}", keepKey, ManifestExtension));
				if (!kept)
					ENGINE_TRY(vfs.Remove(entry.Path));
			}
			return {};
		}

		// Removes `path` when it exists.
		static Status RemoveIfPresent(VirtualFileSystem& vfs, const VfsPath& path)
		{
			const Status removed = vfs.Remove(path);
			if (!removed.has_value() && removed.error().GetCode() != ErrorCode::NotFound)
				return removed;
			return {};
		}

		// The handles of the artifacts the manifest at `path` names, or none when it cannot be read (best effort: used only
		// to find older files to clean up).
		static std::vector<AssetHandle> ReadManifestArtifactHandles(const VirtualFileSystem& vfs, const VfsPath& path, AssetHandle source)
		{
			const Result<std::string> text = vfs.ReadText(path);
			if (!text.has_value())
				return {};
			const Result<Manifest> manifest = ParseManifest(*text, source);
			if (!manifest.has_value())
				return {};
			std::vector<AssetHandle> handles;
			for (const ManifestArtifact& artifact : manifest->Artifacts)
				handles.push_back(artifact.Handle);
			return handles;
		}

	}

	AssetCache::AssetCache(VirtualFileSystem& vfs, VfsPath root)
		: m_State(CreateScope<State>())
	{
		m_State->Vfs = &vfs;
		m_State->Root = std::move(root);
	}

	AssetCache::~AssetCache() = default;

	uint64_t AssetCache::ComputeKey(std::span<const std::byte> sourceBytes, std::string_view importerId, uint32_t importerVersion,
		const Json& canonicalSettings, uint32_t engineCookVersion)
	{
		Result<std::string> settings = JsonWriter::Write(canonicalSettings, JsonStyle::Minified);
		ENGINE_CORE_ASSERT(settings.has_value(), "Import settings must be canonical JSON: {}", settings.has_value() ? std::string() : settings.error().ToString());
		if (!settings.has_value())
		{
			// Only reachable with asserts off: settings the canonical writer rejects (a non-finite number, invalid UTF-8) still
			// get a deterministic key.
			settings = canonicalSettings.dump(-1, ' ', false, Json::error_handler_t::replace);
		}

		// Each part framed by its 8-byte little-endian length, so no two different inputs concatenate to the same bytes.
		XXH64Hasher hasher;
		const auto addBytes = [&hasher](std::span<const std::byte> bytes)
		{
			hasher.UpdateU64(bytes.size());
			hasher.Update(bytes);
		};
		const auto addU32 = [&hasher](uint32_t value)
		{
			hasher.UpdateU64(sizeof(value));
			const std::array<std::byte, sizeof(value)> bytes = {
				static_cast<std::byte>(value & 0xFF),
				static_cast<std::byte>((value >> 8) & 0xFF),
				static_cast<std::byte>((value >> 16) & 0xFF),
				static_cast<std::byte>((value >> 24) & 0xFF),
			};
			hasher.Update(bytes);
		};
		addBytes(sourceBytes);
		addBytes(AsBytes(importerId));
		addU32(importerVersion);
		addBytes(AsBytes(*settings));
		addU32(engineCookVersion);
		return hasher.Digest();
	}

	Result<std::optional<CachedImport>> AssetCache::Find(AssetHandle source, uint64_t key) const
	{
		VirtualFileSystem& vfs = *m_State->Vfs;
		const VfsPath& root = m_State->Root;
		ENGINE_TRY_ASSIGN(const VfsPath manifestPath, Utils::KeyFile(root, source, key, ManifestExtension));
		Result<std::string> text = vfs.ReadText(manifestPath);
		if (!text.has_value())
		{
			if (text.error().GetCode() == ErrorCode::NotFound)
				return std::optional<CachedImport>();
			if (text.error().GetCode() != ErrorCode::Validation)
				return std::unexpected(std::move(text).error().WithContext(std::format("while reading the cache entry of {}", source)));
			// Validation: not UTF-8 text, a corrupted manifest.
		}

		// A present entry: every part must be intact, or the whole entry is discarded and rebuilt (§7.5).
		const auto discard = [&vfs, &root, &manifestPath, source, key](const std::string& reason, std::span<const ManifestArtifact> artifacts)
			-> Result<std::optional<CachedImport>>
		{
			ENGINE_CORE_WARN("Asset cache: discarding the corrupted cache entry {} of asset {} in '{}' ({}); it is rebuilt", Utils::FormatHex(key),
				source, root.ToString(), reason);
			ENGINE_TRY(Utils::RemoveIfPresent(vfs, manifestPath));
			ENGINE_TRY_ASSIGN(const VfsPath mainArtifact, Utils::KeyFile(root, source, key, ArtifactExtension));
			ENGINE_TRY(Utils::RemoveIfPresent(vfs, mainArtifact));
			for (const ManifestArtifact& artifact : artifacts)
			{
				ENGINE_TRY_ASSIGN(const VfsPath artifactPath, Utils::KeyFile(root, artifact.Handle, key, ArtifactExtension));
				ENGINE_TRY(Utils::RemoveIfPresent(vfs, artifactPath));
			}
			return std::optional<CachedImport>();
		};
		if (!text.has_value())
			return discard(text.error().ToString(), {});
		Result<Manifest> manifest = Utils::ParseManifest(*text, source);
		if (!manifest.has_value())
			return discard(std::format("unreadable manifest: {}", manifest.error().ToString()), {});

		CachedImport import;
		import.Import.Artifacts.reserve(manifest->Artifacts.size());
		for (const ManifestArtifact& artifact : manifest->Artifacts)
		{
			ENGINE_TRY_ASSIGN(const VfsPath artifactPath, Utils::KeyFile(root, artifact.Handle, key, ArtifactExtension));
			Result<Buffer> bytes = vfs.ReadFile(artifactPath);
			if (!bytes.has_value())
			{
				if (bytes.error().GetCode() != ErrorCode::NotFound)
					return std::unexpected(std::move(bytes).error().WithContext(std::format("while reading the cache entry of {}", source)));
				return discard(std::format("missing artifact {}", artifact.Handle), manifest->Artifacts);
			}
			const Result<CookedArtifactView> cooked = ReadCookedArtifact(*bytes);
			if (!cooked.has_value())
				return discard(std::format("artifact {}: {}", artifact.Handle, cooked.error().ToString()), manifest->Artifacts);
			if (cooked->Header.Type != artifact.Type)
			{
				return discard(std::format("artifact {} holds a {}, the manifest names a {}", artifact.Handle, AssetTypeToString(cooked->Header.Type),
								   AssetTypeToString(artifact.Type)),
					manifest->Artifacts);
			}
			import.Import.Artifacts.push_back(ImportedArtifact{
				.Handle = artifact.Handle,
				.Type = artifact.Type,
				.SubAssetKey = artifact.SubAssetKey,
				.Cooked = std::move(*bytes),
			});
		}
		import.Import.Dependencies = std::move(manifest->Dependencies);
		import.Import.Diagnostics = std::move(manifest->Diagnostics);
		import.Reads = std::move(manifest->Reads);
		import.Lookups = std::move(manifest->Lookups);
		return std::optional<CachedImport>(std::move(import));
	}

	Result<std::optional<Buffer>> AssetCache::ReadArtifact(AssetHandle artifact, uint64_t key) const
	{
		VirtualFileSystem& vfs = *m_State->Vfs;
		ENGINE_TRY_ASSIGN(const VfsPath path, Utils::KeyFile(m_State->Root, artifact, key, ArtifactExtension));
		Result<Buffer> bytes = vfs.ReadFile(path);
		if (!bytes.has_value())
		{
			if (bytes.error().GetCode() == ErrorCode::NotFound)
				return std::optional<Buffer>();
			return std::unexpected(std::move(bytes).error().WithContext(std::format("while reading the cached artifact {}", artifact)));
		}
		const Result<CookedArtifactView> cooked = ReadCookedArtifact(*bytes);
		if (!cooked.has_value())
		{
			ENGINE_CORE_WARN("Asset cache: discarding the corrupted cached artifact {} ({}) in '{}': {}; it is rebuilt", artifact, Utils::FormatHex(key),
				m_State->Root.ToString(), cooked.error().ToString());
			ENGINE_TRY(Utils::RemoveIfPresent(vfs, path));
			return std::optional<Buffer>();
		}
		return std::optional<Buffer>(std::move(*bytes));
	}

	Status AssetCache::Store(AssetHandle source, uint64_t key, const CachedImport& import)
	{
		ENGINE_CORE_ASSERT(!import.Import.Artifacts.empty() && import.Import.Artifacts.front().Handle == source,
			"AssetCache::Store: the main artifact of an import is the source's own asset {}", source);
		VirtualFileSystem& vfs = *m_State->Vfs;
		const VfsPath& root = m_State->Root;
		const std::string keyText = Utils::FormatHex(key);
		const std::string context = std::format("while storing the cache entry of {} in '{}'", source, root.ToString());
		ENGINE_TRY_ASSIGN(const std::string manifest, WithContext(Utils::WriteManifest(import), context));

		// The handles whose older keys go: this import's, plus the artifacts of the source's older manifests (sub-assets a
		// reimport no longer produces).
		std::set<AssetHandle> handles;
		std::vector<std::pair<AssetHandle, std::string>> staleArtifacts; // (handle, older key) of dropped sub-assets
		ENGINE_TRY_ASSIGN(const VfsPath sourceDirectory, Utils::HandleDirectory(root, source));
		if (Result<std::vector<VfsEntry>> listed = vfs.List(sourceDirectory, false); listed.has_value())
		{
			for (const VfsEntry& entry : *listed)
			{
				if (entry.Info.IsDirectory || entry.Path.GetExtension() != ManifestExtension || entry.Path.GetStem() == keyText)
					continue;
				for (const AssetHandle handle : Utils::ReadManifestArtifactHandles(vfs, entry.Path, source))
					staleArtifacts.emplace_back(handle, std::string(entry.Path.GetStem()));
			}
		}

		// Every artifact first and the manifest last, each written atomically: an interrupted store leaves an entry without a
		// manifest, which Find reports as a miss.
		for (const ImportedArtifact& artifact : import.Import.Artifacts)
		{
			handles.insert(artifact.Handle);
			ENGINE_TRY_ASSIGN(const VfsPath directory, WithContext(Utils::HandleDirectory(root, artifact.Handle), context));
			ENGINE_TRY(WithContext(vfs.CreateDirectories(directory), context));
			ENGINE_TRY_ASSIGN(const VfsPath path, WithContext(Utils::KeyFile(root, artifact.Handle, key, ArtifactExtension), context));
			ENGINE_TRY(WithContext(vfs.WriteFileAtomic(path, artifact.Cooked), context));
		}
		ENGINE_TRY_ASSIGN(const VfsPath manifestPath, WithContext(Utils::KeyFile(root, source, key, ManifestExtension), context));
		ENGINE_TRY(WithContext(vfs.WriteFileAtomic(manifestPath, AsBytes(manifest)), context));

		// One entry per handle (§7.5).
		for (const AssetHandle handle : handles)
		{
			ENGINE_TRY_ASSIGN(const VfsPath directory, WithContext(Utils::HandleDirectory(root, handle), context));
			ENGINE_TRY(WithContext(Utils::RemoveOtherKeys(vfs, directory, keyText), context));
		}
		for (const auto& [handle, staleKey] : staleArtifacts)
		{
			if (handles.contains(handle))
				continue;
			ENGINE_TRY_ASSIGN(const VfsPath path, WithContext(root.Join(std::format("{}/{}{}", Utils::FormatHex(handle.GetValue()), staleKey, ArtifactExtension)), context));
			ENGINE_TRY(WithContext(Utils::RemoveIfPresent(vfs, path), context));
		}
		return {};
	}

	Status AssetCache::Remove(AssetHandle handle)
	{
		VirtualFileSystem& vfs = *m_State->Vfs;
		const VfsPath& root = m_State->Root;
		const std::string context = std::format("while removing the cache entries of {} from '{}'", handle, root.ToString());
		ENGINE_TRY_ASSIGN(const VfsPath directory, WithContext(Utils::HandleDirectory(root, handle), context));
		Result<std::vector<VfsEntry>> listed = vfs.List(directory, false);
		if (!listed.has_value())
		{
			if (listed.error().GetCode() == ErrorCode::NotFound)
				return {};
			return std::unexpected(std::move(listed).error().WithContext(context));
		}
		// The sub-asset artifacts of each manifest filed under `handle` go with it.
		for (const VfsEntry& entry : *listed)
		{
			if (entry.Info.IsDirectory || entry.Path.GetExtension() != ManifestExtension)
				continue;
			for (const AssetHandle artifact : Utils::ReadManifestArtifactHandles(vfs, entry.Path, handle))
			{
				if (artifact == handle)
					continue;
				ENGINE_TRY_ASSIGN(const VfsPath path, WithContext(root.Join(std::format("{}/{}{}", Utils::FormatHex(artifact.GetValue()), entry.Path.GetStem(), ArtifactExtension)), context));
				ENGINE_TRY(WithContext(Utils::RemoveIfPresent(vfs, path), context));
			}
		}
		return WithContext(Utils::RemoveIfPresent(vfs, directory), context);
	}

	const VfsPath& AssetCache::GetRoot() const
	{
		return m_State->Root;
	}

	bool IsManifestCurrent(const VirtualFileSystem& vfs, std::span<const ImportDependencyRead> reads, std::span<const ImportAssetLookup> lookups,
		std::span<const ImportAssetLookupEntry> assets)
	{
		for (const ImportDependencyRead& read : reads)
		{
			const Result<Buffer> bytes = vfs.ReadFile(read.Path);
			if (!bytes.has_value() || XXH64(*bytes) != read.Hash)
				return false;
		}
		for (const ImportAssetLookup& lookup : lookups)
		{
			const auto found = std::ranges::lower_bound(assets, lookup.Path, {}, &ImportAssetLookupEntry::SourcePath);
			const bool present = found != assets.end() && found->SourcePath == lookup.Path;
			if (present != lookup.Found.has_value())
				return false;
			// The same registered asset: handle, kind and type (a manifest does not record a dependency's owner).
			if (present && (found->Handle != lookup.Found->Handle || found->Kind != lookup.Found->Kind || found->Type != lookup.Found->Type))
				return false;
		}
		return true;
	}

}
