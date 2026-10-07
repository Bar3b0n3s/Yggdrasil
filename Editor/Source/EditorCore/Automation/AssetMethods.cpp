#include "EditorPCH.h"
#include "EditorCore/Automation/AssetMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/AssetDeleteCommand.h"
#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/Commands/AssetMoveCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "EditorCore/Private/PrefabInstances.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetReference.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/AssetPipeline/Importers/GltfImporter.h"
#include "Engine/AssetPipeline/Importers/MaterialImporter.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/MergePatch.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <filesystem>
#include <set>

namespace Engine {

	namespace {

		constexpr uint32_t MaxListLimit = 1000;
		constexpr std::string_view NotNativeHint = "use asset.getImportSettings and asset.setImportSettings for an imported asset";

		Buffer TextToBuffer(std::string_view text)
		{
			const std::span<const std::byte> bytes = AsBytes(text);
			return Buffer(bytes.begin(), bytes.end());
		}

		// What asset.info and asset.setImportSettings need of the importer `importerId` the editor registers: its version and
		// the registry name of its settings struct. The editor registers the built-in importers (RegisterBuiltinImporters);
		// they are stateless, so a registry built here answers exactly as the editor's.
		struct ImporterFacts
		{
			uint32_t Version = 0;
			std::string SettingsTypeName{};
		};

		std::optional<ImporterFacts> FindImporterFacts(std::string_view importerId)
		{
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			const IAssetImporter* importer = importers.FindById(importerId);
			if (importer == nullptr)
				return std::nullopt;
			return ImporterFacts{ .Version = importer->GetVersion(), .SettingsTypeName = std::string(importer->GetSettingsTypeName()) };
		}

		// The registered record of the main asset `handle` that a method needing its .meta reads (`what`: "import settings",
		// "properties"). Errors at `pointer`: InvalidArgument for a built-in, a sub-asset or a dependency file; NotFound for
		// an unknown handle.
		Result<const AssetRecord*> FindMainAssetRecord(const EditorAssetManager& assets, AssetHandle handle, std::string_view pointer, std::string_view what)
		{
			const AssetRegistry& registry = assets.GetRegistry();
			if (assets.GetBuiltins().Find(handle) != nullptr || IsBuiltinAssetHandle(handle))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is a built-in asset: it has no {}", assets.GetReferencePath(handle), what), "built-in assets cannot change"));
			}
			if (const std::optional<AssetRegistry::Location> location = registry.Locate(handle); location.has_value() && !location->SubAssetKey.empty())
			{
				const std::string source = Utils::ToProjectRelative(location->Record->SourcePath);
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is a sub-asset: its source '{}' has the {}", assets.GetReferencePath(handle), source, what),
					std::format("pass \"{}\"", source)));
			}
			const AssetRecord* record = registry.Find(handle);
			if (record == nullptr)
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, pointer, std::format("no asset has the handle {}", handle.ToString())));
			if (record->Metadata.Kind == AssetMetaKind::Dependency)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is a dependency file of {}, not an asset", Utils::ToProjectRelative(record->SourcePath),
						Utils::MakeAssetSummary(assets, record->Metadata.Owner).Path),
					"pass the asset that owns it"));
			}
			return record;
		}

		// The registry JSON of `material` as asset.getProperties reports it: every field, without "Format" and "Version".
		Result<Json> MaterialToValues(const MaterialData& material, const TypeRegistry& registry)
		{
			ENGINE_TRY_ASSIGN(Json document, MaterialToJson(material, registry));
			document.erase("Format");
			document.erase("Version");
			return document;
		}

		// The material `document` (a canonical .material document) with the merge patch `values` applied (null resets a field),
		// read strictly through the registry. Enum names are accepted in any ASCII case (§13.4). Errors: InvalidArgument at
		// `pointer` for values that are not an object or that set "Format" or "Version"; Validation located under `pointer`.
		Result<MaterialData> PatchMaterial(const Json& document, const Json& values, const TypeRegistry& registry, std::string_view pointer)
		{
			if (!values.is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "the values must be an object of material fields",
					"for example {\"BaseColor\": [0.9, 0.15, 0.15, 1], \"Roughness\": 0.45}"));
			}
			for (const std::string_view key : { std::string_view("Format"), std::string_view("Version") })
			{
				if (values.contains(key))
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("{}/{}", pointer, key),
						std::format("'{}' is part of the file format, not a material property", key)));
				}
			}
			Json patch = values;
			const StructInfo* type = registry.FindStruct<MaterialData>();
			ENGINE_ASSERT(type != nullptr, "The Material struct is registered by the engine context (RegisterAssetTypes)");
			Utils::CanonicalizeEnumSpellings(patch, type->GetType());
			MaterialLoadReport report;
			Result<MaterialData> material = MaterialFromJson(ApplyMergePatch(document, patch), registry, report, true);
			if (!material)
				return std::unexpected(Utils::PrefixPointers(material.error(), pointer));
			return material;
		}

		// The text of a new native asset of `type` at `path`: a material from `values` over the defaults, an empty scene
		// named after the file stem, or a prefab with one root entity named after it. Errors: those of PatchMaterial and of
		// the serializers.
		Result<std::string> MakeNativeAssetText(EditorContext& editor, AssetCreateType type, const VfsPath& path, const VariantValue& values)
		{
			const std::string stem(path.GetStem());
			const TypeRegistry& registry = editor.GetTypeRegistry();
			switch (type)
			{
				case AssetCreateType::Material:
				{
					MaterialData material;
					if (!values.Get().is_null())
					{
						ENGINE_TRY_ASSIGN(const Json defaults, MaterialToJson(material, registry));
						ENGINE_TRY_ASSIGN(material, PatchMaterial(defaults, values.Get(), registry, "/values"));
					}
					return MaterialToText(material, registry);
				}
				case AssetCreateType::Scene:
				{
					const Scope<Scene> scene = editor.CreateScene(stem);
					return SceneSerializer::SaveToString(*scene);
				}
				case AssetCreateType::Prefab:
				{
					const Scope<Scene> scratch = editor.CreateScene(stem);
					const Entity root = scratch->CreateEntity(stem);
					ENGINE_TRY_ASSIGN(const Prefab prefab, Prefab::CreateFromEntity(root, stem));
					return prefab.SaveToString();
				}
				case AssetCreateType::SoundEffect:
				case AssetCreateType::Folder:
					break;
			}
			return MakeError(ErrorCode::Unsupported, "assets of this type have no content to create");
		}

		// The file extension asset.create requires for `type` (empty for a folder).
		std::string_view GetCreateExtension(AssetCreateType type)
		{
			switch (type)
			{
				case AssetCreateType::Material:    return ".material";
				case AssetCreateType::Scene:       return ".scene";
				case AssetCreateType::Prefab:      return ".prefab";
				case AssetCreateType::SoundEffect: return ".sfx";
				case AssetCreateType::Folder:      return {};
			}
			return {};
		}

		// The settings patch `settings` spelled canonically for the importer `importerId` (enum names in any ASCII case,
		// §13.4), validated to be an object. Errors: InvalidArgument at `pointer` for a value that is not an object.
		Result<Json> PrepareSettingsPatch(const TypeRegistry& registry, std::string_view importerId, const VariantValue& settings, std::string_view pointer)
		{
			if (!settings.Get().is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "the settings must be an object (an RFC 7386 merge patch)",
					"for example {\"Usage\": \"NormalMap\"}"));
			}
			Json patch = settings.Get();
			if (const std::optional<ImporterFacts> facts = FindImporterFacts(importerId); facts.has_value() && !facts->SettingsTypeName.empty())
			{
				if (const StructInfo* type = registry.FindStruct(facts->SettingsTypeName); type != nullptr)
					Utils::CanonicalizeEnumSpellings(patch, type->GetType());
			}
			return patch;
		}

		// EditorAssetManager::MergeImportSettings with its errors located under `pointer`.
		Result<VariantValue> MergeSettings(const EditorAssetManager& assets, std::string_view importerId, const VariantValue& base, const Json& patch,
			std::string_view pointer)
		{
			Result<VariantValue> merged = assets.MergeImportSettings(importerId, base, patch);
			if (!merged)
			{
				if (merged.error().GetCode() == ErrorCode::InvalidArgument)
					return std::unexpected(Utils::LocateAtParam(merged.error(), pointer));
				return std::unexpected(Utils::PrefixPointers(merged.error(), pointer));
			}
			return merged;
		}

		// The decimal offset of a list cursor ("" is 0). Errors: InvalidArgument at "/cursor" for anything else, or an offset
		// beyond `total`.
		Result<size_t> ParseListCursor(std::string_view cursor, size_t total)
		{
			if (cursor.empty())
				return size_t{ 0 };
			size_t offset = 0;
			const auto [end, error] = std::from_chars(cursor.data(), cursor.data() + cursor.size(), offset);
			if (error != std::errc() || end != cursor.data() + cursor.size() || offset > total)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/cursor", std::format("'{}' is not a cursor of this listing", cursor),
					"pass \"\" for the start, or the nextCursor of the previous call"));
			}
			return offset;
		}

		// Whether the open scene instantiates `prefab`, so a new version of it must update the instances (§5.5).
		bool IsInstancedInOpenScene(EditorContext& editor, AssetHandle prefab)
		{
			return editor.HasScene() && !Utils::FindPrefabInstances(editor.GetScene(), std::span<const AssetHandle>(&prefab, 1)).empty();
		}

		std::vector<AssetSummary> MakeSubAssetSummaries(const EditorAssetManager& assets, std::span<const SubAssetEntry> subAssets)
		{
			std::vector<AssetSummary> summaries;
			summaries.reserve(subAssets.size());
			for (const SubAssetEntry& subAsset : subAssets)
				summaries.push_back(Utils::MakeAssetSummary(assets, subAsset.Handle));
			return summaries;
		}

		std::vector<AssetDiagnosticInfo> MakeDiagnosticInfos(std::span<const AssetDiagnostic> diagnostics)
		{
			std::vector<AssetDiagnosticInfo> infos;
			infos.reserve(diagnostics.size());
			for (const AssetDiagnostic& diagnostic : diagnostics)
				infos.push_back(Utils::MakeAssetDiagnosticInfo(diagnostic));
			return infos;
		}

		// asset.import's operation: the import of the copied asset, resolved once its job completed and its result is
		// published.
		class AssetImportOperation final : public PendingOperation
		{
		public:
			AssetImportOperation(EditorContext& editor, JobHandle<AssetImportOutcome> import, AssetImportResult result)
				: m_Editor(&editor), m_Import(std::move(import)), m_Result(std::move(result))
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				if (!m_Import.IsReady())
					return std::nullopt;
				EditorAssetManager& assets = m_Editor->GetAssets();
				assets.WaitIdle();
				Result<AssetImportOutcome> outcome = m_Import.Take();
				if (!outcome)
					return std::unexpected(std::move(outcome).error().WithContext(std::format("importing '{}'", m_Result.Asset.Path)));
				m_Result.Asset = Utils::MakeAssetSummary(assets, outcome->Handle);
				m_Result.SubAssets = MakeSubAssetSummaries(assets, outcome->SubAssets);
				m_Result.Diagnostics = MakeDiagnosticInfos(outcome->Diagnostics);
				return context.SerializeResult(m_Result);
			}

			[[nodiscard]] std::string GetPhase() const override { return std::format("Automation:asset.import {}", m_Result.Asset.Path); }
		private:
			EditorContext* m_Editor = nullptr; // documented back-reference: the editor outlives its requests
			JobHandle<AssetImportOutcome> m_Import;
			AssetImportResult m_Result;
		};

		// asset.reimport's operation: the reimport, then (for a Prefab the open scene instantiates) the update of its
		// instances as an undo step of its own (§5.5).
		class AssetReimportOperation final : public PendingOperation
		{
		public:
			AssetReimportOperation(EditorContext& editor, AssetHandle handle, JobHandle<AssetImportOutcome> reimport)
				: m_Editor(&editor), m_Handle(handle), m_Reimport(std::move(reimport))
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				if (!m_Reimport.IsReady())
					return std::nullopt;
				EditorAssetManager& assets = m_Editor->GetAssets();
				assets.WaitIdle();
				Result<AssetImportOutcome> outcome = m_Reimport.Take();
				if (!outcome)
					return std::unexpected(std::move(outcome).error().WithContext(std::format("reimporting '{}'", assets.GetReferencePath(m_Handle))));
				if (IsInstancedInOpenScene(*m_Editor, m_Handle))
				{
					const AssetHandle prefabs[] = { m_Handle };
					Result<Scope<Command>> update = m_Editor->CreatePrefabUpdateCommand(prefabs);
					if (!update)
						return std::unexpected(std::move(update).error());
					if (*update != nullptr)
					{
						Result<uint64_t> executed = m_Editor->Execute(std::move(*update));
						if (!executed)
							return std::unexpected(std::move(executed).error());
					}
				}
				AssetReimportResult result;
				result.Asset = Utils::MakeAssetSummary(assets, m_Handle);
				result.SubAssets = MakeSubAssetSummaries(assets, outcome->SubAssets);
				result.Diagnostics = MakeDiagnosticInfos(outcome->Diagnostics);
				return context.SerializeResult(result);
			}

			[[nodiscard]] std::string GetPhase() const override { return std::format("Automation:asset.reimport {}", m_Handle.ToString()); }
		private:
			EditorContext* m_Editor = nullptr; // documented back-reference: the editor outlives its requests
			AssetHandle m_Handle{};
			JobHandle<AssetImportOutcome> m_Reimport;
		};

		// One file asset.import copies: where it comes from and where it goes.
		struct ImportCopy
		{
			Buffer Bytes{};
			VfsPath Destination{};
		};

		// Refuses a native file that is a symbolic link or another non-regular file, or whose canonical path leaves
		// `canonicalDirectory` (asset.import never reads outside the source's folder). Errors: NotFound for a missing file;
		// PermissionDenied for the refusals; Io when the file system cannot answer.
		Status CheckNativeImportFile(const std::filesystem::path& file, const std::filesystem::path& canonicalDirectory, std::string_view pointer)
		{
			const std::string text = FileSystem::PathToUtf8(file);
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::symlink_status(file, error);
			if (status.type() == std::filesystem::file_type::not_found)
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, pointer, std::format("'{}' does not exist", text)));
			if (error)
				return MakeError(ErrorCode::Io, "cannot inspect '{}': {}", text, error.message());
			if (std::filesystem::is_symlink(status) || status.type() != std::filesystem::file_type::regular)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::PermissionDenied, pointer,
					std::format("'{}' is a symbolic link, a reparse point or not a regular file: asset.import copies only regular files", text),
					"copy the file itself next to the source"));
			}
			const std::filesystem::path canonical = std::filesystem::weakly_canonical(file, error);
			if (error)
				return MakeError(ErrorCode::Io, "cannot resolve '{}': {}", text, error.message());
			const std::filesystem::path relative = canonical.lexically_relative(canonicalDirectory);
			if (relative.empty() || *relative.begin() == std::filesystem::path(".."))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::PermissionDenied, pointer,
					std::format("'{}' lies outside the source's folder '{}'", text, FileSystem::PathToUtf8(canonicalDirectory)),
					"keep the glTF's buffers and images in its folder or below it"));
			}
			return {};
		}

		// Reads the source of asset.import and the files of its dependency closure (the decoded relative URIs of a glTF,
		// GltfImporter::ListExternalUris), checking each native file with CheckNativeImportFile. The first copy is the source.
		Result<std::vector<ImportCopy>> ReadImportFiles(EditorMethodContext& context, const AssetImportParams& params, const VfsPath& destination)
		{
			const VirtualFileSystem& vfs = context.GetEditor().GetVfs();
			const std::filesystem::path native = FileSystem::PathFromUtf8(params.Source);
			const bool isNative = params.Source.find("://") == std::string::npos && native.is_absolute();
			std::vector<ImportCopy> copies;

			// The source and a reader for the files beside it.
			std::filesystem::path canonicalDirectory;
			VfsPath projectSource; // the source in the project; empty for a native source
			if (isNative)
			{
				std::error_code error;
				canonicalDirectory = std::filesystem::weakly_canonical(native.parent_path(), error);
				if (error)
					return MakeError(ErrorCode::Io, "cannot resolve the folder of '{}': {}", params.Source, error.message());
				ENGINE_TRY(CheckNativeImportFile(native, canonicalDirectory, "/source"));
				ENGINE_TRY_ASSIGN(Buffer bytes, FileSystem::ReadFile(native));
				copies.push_back(ImportCopy{ .Bytes = std::move(bytes), .Destination = destination });
			}
			else
			{
				ENGINE_TRY_ASSIGN(VfsPath source, context.ResolveProjectPath(params.Source, "/source"));
				Result<Buffer> bytes = vfs.ReadFile(source);
				if (!bytes)
				{
					if (bytes.error().GetCode() == ErrorCode::NotFound)
						return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/source", std::format("'{}' does not exist", params.Source)));
					return std::unexpected(Utils::ToEditorFileError(std::move(bytes).error()));
				}
				copies.push_back(ImportCopy{ .Bytes = std::move(*bytes), .Destination = destination });
				projectSource = std::move(source);
			}

			// Only a glTF has a closure; extensions match importers ignoring ASCII case (IAssetImporter::CanImport).
			if (!GltfImporter().CanImport(destination.GetExtension()))
				return copies;
			ENGINE_TRY_ASSIGN(const std::vector<std::string> uris, WithContext(GltfImporter::ListExternalUris(copies.front().Bytes), std::format("reading the dependency closure of '{}'", params.Source)));
			for (const std::string& uri : uris)
			{
				ENGINE_TRY_ASSIGN(VfsPath target, WithContext(destination.GetParent().Join(uri), std::format("copying '{}'", uri)));
				if (isNative)
				{
					const std::filesystem::path file = native.parent_path() / FileSystem::PathFromUtf8(uri);
					ENGINE_TRY(WithContext(CheckNativeImportFile(file, canonicalDirectory, "/source"), std::format("in the closure of '{}'", params.Source)));
					ENGINE_TRY_ASSIGN(Buffer bytes, FileSystem::ReadFile(file));
					copies.push_back(ImportCopy{ .Bytes = std::move(bytes), .Destination = std::move(target) });
					continue;
				}
				ENGINE_TRY_ASSIGN(const VfsPath file, WithContext(projectSource.GetParent().Join(uri), std::format("in the closure of '{}'", params.Source)));
				Result<Buffer> bytes = vfs.ReadFile(file);
				if (!bytes)
					return std::unexpected(Utils::ToEditorFileError(std::move(bytes).error()).WithContext(std::format("in the closure of '{}'", params.Source)));
				copies.push_back(ImportCopy{ .Bytes = std::move(*bytes), .Destination = std::move(target) });
			}
			return copies;
		}

	}

	namespace Automation {

		Result<AssetListResult> AssetList(EditorMethodContext& context, const AssetListParams& params)
		{
			if (params.Limit < 1 || params.Limit > MaxListLimit)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/limit", std::format("limit must be 1 to {}", MaxListLimit)));
			ENGINE_TRY_ASSIGN(const VfsPath directory, Utils::ResolveAssetsPath(context, params.Dir, "/dir", {}, true));
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY(Utils::RefreshAssets(editor));
			Result<FileInfo> info = editor.GetVfs().GetInfo(directory);
			if (!info || !info->IsDirectory)
			{
				return std::unexpected(Utils::MakeParamError(info ? ErrorCode::InvalidArgument : ErrorCode::NotFound, "/dir",
					std::format("'{}' is not a folder of the project", params.Dir), "asset.list {dir: \"Assets\"} lists every asset"));
			}

			// Sorted by source path, a source's sub-assets right after it; dependency files are never listed.
			const EditorAssetManager& assets = editor.GetAssets();
			std::vector<AssetHandle> matches;
			for (const AssetRecord* record : assets.GetRegistry().GetRecords())
			{
				if (record->Metadata.Kind == AssetMetaKind::Dependency)
					continue;
				const VfsPath& source = record->SourcePath;
				const bool inside = params.Recursive ? source.IsUnder(directory) && source != directory : source.GetParent() == directory;
				if (!inside)
					continue;
				if (params.Type == AssetType::None || params.Type == record->Metadata.Type)
					matches.push_back(record->Metadata.Handle);
				if (!params.SubAssets)
					continue;
				for (const SubAssetEntry& subAsset : record->Metadata.SubAssets)
				{
					if (params.Type == AssetType::None || params.Type == subAsset.Type)
						matches.push_back(subAsset.Handle);
				}
			}

			ENGINE_TRY_ASSIGN(const size_t offset, ParseListCursor(params.Cursor, matches.size()));
			const size_t end = std::min(matches.size(), offset + params.Limit);
			AssetListResult result;
			result.Total = ToAutomationCounter(matches.size());
			for (size_t index = offset; index < end; ++index)
				result.Assets.push_back(Utils::MakeAssetSummary(assets, matches[index]));
			if (end < matches.size())
				result.NextCursor = std::to_string(end);
			return result;
		}

		Result<AssetInfoResult> AssetInfo(EditorMethodContext& context, const AssetInfoParams& params)
		{
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = context.GetEditor().GetAssets();
			AssetInfoResult result;
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			result.State = assets.GetState(handle);
			result.Version = ToAutomationCounter(assets.GetVersion(handle));
			for (const AssetHandle dependency : assets.GetDependencyGraph().GetDependencies(handle))
				result.Dependencies.push_back(Utils::MakeAssetSummary(assets, dependency));
			for (const AssetHandle dependent : assets.GetDependencyGraph().GetDependents(handle))
				result.Dependents.push_back(Utils::MakeAssetSummary(assets, dependent));

			if (const BuiltinAssetEntry* builtin = assets.GetBuiltins().Find(handle); builtin != nullptr)
			{
				result.Source = builtin->Path;
				result.Importer = builtin->Importer;
				result.Settings = builtin->Settings;
				if (const std::optional<ImporterFacts> facts = FindImporterFacts(builtin->Importer); facts.has_value())
					result.ImporterVersion = facts->Version;
			}
			else if (const std::optional<AssetRegistry::Location> location = assets.GetRegistry().Locate(handle); location.has_value())
			{
				const AssetRecord& record = *location->Record;
				result.Source = Utils::ToProjectRelative(record.SourcePath);
				result.Importer = record.Metadata.Importer;
				result.Settings = record.Metadata.Settings;
				const std::optional<ImporterFacts> facts = FindImporterFacts(record.Metadata.Importer);
				result.ImporterVersion = facts.has_value() ? facts->Version : record.Metadata.ImporterVersion;
				if (location->SubAssetKey.empty())
				{
					result.SubAssets = MakeSubAssetSummaries(assets, record.Metadata.SubAssets);
					for (const AssetRecord* dependency : assets.GetRegistry().GetDependencyRecords(handle))
						result.DependencyFiles.push_back(Utils::ToProjectRelative(dependency->SourcePath));
					std::sort(result.DependencyFiles.begin(), result.DependencyFiles.end());
				}
			}

			for (const AssetDiagnostic& diagnostic : assets.GetDiagnostics())
			{
				const bool aboutSource = !diagnostic.Asset.IsValid() && !result.Source.empty() && diagnostic.Path == result.Source;
				if (diagnostic.Asset == handle || aboutSource)
					result.Diagnostics.push_back(Utils::MakeAssetDiagnosticInfo(diagnostic));
			}
			return result;
		}

		Result<Scope<PendingOperation>> AssetImport(EditorMethodContext& context, const AssetImportParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const VfsPath directory, Utils::ResolveAssetsPath(context, params.DestDir, "/destDir", {}, true));
			if (!params.Settings.Get().is_null() && !params.Settings.Get().is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/settings", "the settings must be an object of import settings",
					"for example {\"Scale\": 0.01}"));
			}
			if (params.Source.empty())
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/source", "the source must not be empty"));
			ENGINE_TRY(Utils::RefreshAssets(editor));

			// Every file is read and checked before anything is written: a rejected URI refuses the whole import.
			const std::string fileName = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(params.Source).filename());
			ENGINE_TRY_ASSIGN(const VfsPath destination, WithContext(directory.Join(fileName), std::format("importing '{}'", params.Source)));
			ENGINE_TRY_ASSIGN(std::vector<ImportCopy> copies, ReadImportFiles(context, params, destination));

			// The asset's .meta: the importer that takes the extension, its settings merged with the given ones.
			EditorAssetManager& assets = editor.GetAssets();
			Result<AssetMetadata> created = assets.CreateMetadata(destination);
			if (!created)
			{
				if (created.error().GetCode() != ErrorCode::NotFound)
					return std::unexpected(std::move(created).error());
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/source", created.error().GetMessageText(), created.error().GetHint()));
			}
			AssetMetadata metadata = std::move(*created);
			if (!params.Settings.Get().is_null())
			{
				ENGINE_TRY_ASSIGN(const Json patch, PrepareSettingsPatch(editor.GetTypeRegistry(), metadata.Importer, params.Settings, "/settings"));
				ENGINE_TRY_ASSIGN(metadata.Settings, MergeSettings(assets, metadata.Importer, metadata.Settings, patch, "/settings"));
			}
			VirtualFileSystem& vfs = editor.GetVfs();
			std::vector<AssetFileEdit> edits;
			std::set<VfsPath> directories;
			const auto addDirectories = [&vfs, &edits, &directories](const VfsPath& folder)
			{
				std::vector<AssetFileEdit> missing;
				Utils::AddMissingDirectoryEdits(vfs, folder, missing);
				for (AssetFileEdit& edit : missing)
				{
					if (directories.insert(edit.Path).second)
						edits.push_back(std::move(edit));
				}
			};
			AssetImportResult result;
			for (size_t index = 0; index < copies.size(); ++index)
			{
				ImportCopy& copy = copies[index];
				ENGINE_TRY(Utils::CheckAssetPathFree(vfs, copy.Destination, "/destDir"));
				addDirectories(copy.Destination.GetParent());
				// The source's .meta, then a dependency meta per closure file (§6.4), owned by the source.
				const AssetMetadata meta = index == 0 ? metadata : assets.CreateDependencyMetadata(metadata.Handle);
				const std::string metaText = SerializeAssetMetadata(meta);
				ENGINE_TRY_ASSIGN(const VfsPath metaPath, GetMetaPath(copy.Destination));
				result.CopiedFiles.push_back(Utils::ToProjectRelative(copy.Destination));
				result.CopiedFiles.push_back(Utils::ToProjectRelative(metaPath));
				edits.push_back(AssetFileEdit{ .Path = copy.Destination, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = std::move(copy.Bytes) });
				edits.push_back(AssetFileEdit{ .Path = metaPath, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = TextToBuffer(metaText) });
			}
			std::sort(result.CopiedFiles.begin(), result.CopiedFiles.end());

			ENGINE_TRY_ASSIGN(const uint64_t undoIndex,
				editor.Execute(CreateScope<AssetEditCommand>(std::format("Import Asset '{}'", fileName), std::move(edits))));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			result.Asset = AssetSummary{ .Id = metadata.Handle.ToString(), .Path = Utils::ToProjectRelative(destination), .Type = metadata.Type };
			ENGINE_INFO("asset.import copied '{}' and {} file(s) of its closure to '{}'", params.Source, copies.size() - 1, Utils::ToProjectRelative(directory));
			return Scope<PendingOperation>(CreateScope<AssetImportOperation>(editor, assets.ReimportAsync(metadata.Handle), std::move(result)));
		}

		Result<Scope<PendingOperation>> AssetReimport(EditorMethodContext& context, const AssetReimportParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			EditorAssetManager& assets = editor.GetAssets();
			ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMainAssetRecord(assets, handle, "/asset", "source to import"));
			static_cast<void>(record);
			return Scope<PendingOperation>(CreateScope<AssetReimportOperation>(editor, handle, assets.ReimportAsync(handle)));
		}

		Result<AssetCreateResult> AssetCreate(EditorMethodContext& context, const AssetCreateParams& params)
		{
			EditorContext& editor = context.GetEditor();
			if (params.Type == AssetCreateType::SoundEffect)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::Unsupported, "/type", "sound effect assets are not supported by this editor yet",
					"create a Material, Scene, Prefab or Folder"));
			}
			if (params.Type != AssetCreateType::Material && context.HasParam("values") && !params.Values.Get().is_null())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/values", "only a Material takes values",
					"leave values out for scenes, prefabs and folders"));
			}
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveAssetsPath(context, params.Path, "/path", GetCreateExtension(params.Type)));
			ENGINE_TRY(Utils::RefreshAssets(editor));
			VirtualFileSystem& vfs = editor.GetVfs();
			const std::string relative = Utils::ToProjectRelative(path);
			const std::string stem(path.GetStem());

			AssetCreateResult result;
			result.Path = relative;
			std::vector<AssetFileEdit> edits;
			std::string label;
			if (params.Type == AssetCreateType::Folder)
			{
				if (vfs.Exists(path))
					return std::unexpected(Utils::MakeParamError(ErrorCode::AlreadyExists, "/path", std::format("'{}' already exists", relative)));
				Utils::AddMissingDirectoryEdits(vfs, path, edits);
				label = std::format("Create Folder '{}'", path.GetFileName());
			}
			else
			{
				ENGINE_TRY(Utils::CheckAssetPathFree(vfs, path, "/path"));
				ENGINE_TRY_ASSIGN(const std::string text, MakeNativeAssetText(editor, params.Type, path, params.Values));
				ENGINE_TRY_ASSIGN(const AssetMetadata metadata, editor.GetAssets().CreateMetadata(path));
				const std::string metaText = SerializeAssetMetadata(metadata);
				ENGINE_TRY_ASSIGN(const VfsPath metaPath, GetMetaPath(path));
				Utils::AddMissingDirectoryEdits(vfs, path.GetParent(), edits);
				edits.push_back(AssetFileEdit{ .Path = path, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = TextToBuffer(text) });
				edits.push_back(AssetFileEdit{ .Path = metaPath, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = TextToBuffer(metaText) });
				result.Asset = AssetSummary{ .Id = metadata.Handle.ToString(), .Path = relative, .Type = metadata.Type };
				label = std::format("Create {} '{}'", AssetTypeToString(metadata.Type), stem);
			}
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(CreateScope<AssetEditCommand>(std::move(label), std::move(edits))));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<AssetGetPropertiesResult> AssetGetProperties(EditorMethodContext& context, const AssetGetPropertiesParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = editor.GetAssets();
			Result<const AssetRecord*> record = FindMainAssetRecord(assets, handle, "/asset", "properties");
			if (!record || (*record)->Metadata.Importer != MaterialImporter::Id)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/asset",
					std::format("'{}' is not a native asset: it has no properties of its own", assets.GetReferencePath(handle)), std::string(NotNativeHint)));
			}
			Result<std::string> text = editor.GetVfs().ReadText((*record)->SourcePath);
			if (!text)
				return std::unexpected(Utils::ToEditorFileError(std::move(text).error()));
			MaterialLoadReport report;
			ENGINE_TRY_ASSIGN(const MaterialData material, WithContext(MaterialFromText(*text, editor.GetTypeRegistry(), report), std::format("reading '{}'", Utils::ToProjectRelative((*record)->SourcePath))));
			AssetGetPropertiesResult result;
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			ENGINE_TRY_ASSIGN(Json values, MaterialToValues(material, editor.GetTypeRegistry()));
			result.Values = VariantValue(std::move(values));
			return result;
		}

		Result<AssetSetPropertiesResult> AssetSetProperties(EditorMethodContext& context, const AssetSetPropertiesParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = editor.GetAssets();
			Result<const AssetRecord*> found = FindMainAssetRecord(assets, handle, "/asset", "properties");
			if (!found || (*found)->Metadata.Importer != MaterialImporter::Id)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/asset",
					std::format("'{}' is not a native asset: it has no properties of its own", assets.GetReferencePath(handle)), std::string(NotNativeHint)));
			}
			const VfsPath source = (*found)->SourcePath;
			const TypeRegistry& registry = editor.GetTypeRegistry();
			Result<std::string> text = editor.GetVfs().ReadText(source);
			if (!text)
				return std::unexpected(Utils::ToEditorFileError(std::move(text).error()));
			MaterialLoadReport report;
			ENGINE_TRY_ASSIGN(const MaterialData current, WithContext(MaterialFromText(*text, registry, report), std::format("reading '{}'", Utils::ToProjectRelative(source))));
			ENGINE_TRY_ASSIGN(const Json document, MaterialToJson(current, registry));
			ENGINE_TRY_ASSIGN(const MaterialData patched, PatchMaterial(document, params.Values.Get(), registry, "/values"));
			ENGINE_TRY_ASSIGN(const std::string patchedText, MaterialToText(patched, registry));

			AssetSetPropertiesResult result;
			if (patchedText != *text)
			{
				ENGINE_TRY_ASSIGN(Scope<AssetEditCommand> command, AssetEditCommand::CreateForWrite(editor, source, AsBytes(patchedText), std::format("Set Material '{}'", source.GetStem())));
				ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(std::move(command)));
				result.UndoIndex = ToAutomationCounter(undoIndex);
			}
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			ENGINE_TRY_ASSIGN(Json values, MaterialToValues(patched, registry));
			result.Values = VariantValue(std::move(values));
			return result;
		}

		Result<AssetGetImportSettingsResult> AssetGetImportSettings(EditorMethodContext& context, const AssetGetImportSettingsParams& params)
		{
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = context.GetEditor().GetAssets();
			ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMainAssetRecord(assets, handle, "/asset", "import settings"));
			AssetGetImportSettingsResult result;
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			result.Importer = record->Metadata.Importer;
			result.Settings = record->Metadata.Settings;
			return result;
		}

		Result<AssetSetImportSettingsResult> AssetSetImportSettings(EditorMethodContext& context, const AssetSetImportSettingsParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			EditorAssetManager& assets = editor.GetAssets();
			ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMainAssetRecord(assets, handle, "/asset", "import settings"));
			const AssetMetadata metadata = record->Metadata;
			const VfsPath metaPath = record->MetaPath;
			ENGINE_TRY_ASSIGN(const Json patch, PrepareSettingsPatch(editor.GetTypeRegistry(), metadata.Importer, params.Settings, "/settings"));
			AssetMetadata changed = metadata;
			ENGINE_TRY_ASSIGN(changed.Settings, MergeSettings(assets, metadata.Importer, metadata.Settings, patch, "/settings"));

			AssetSetImportSettingsResult result;
			result.Importer = metadata.Importer;
			result.Settings = changed.Settings;
			if (changed.Settings == metadata.Settings)
			{
				result.Asset = Utils::MakeAssetSummary(assets, handle);
				return result;
			}
			const std::string text = SerializeAssetMetadata(changed);
			const std::string label = std::format("Set Import Settings '{}'", Utils::ToProjectRelative(record->SourcePath));
			ENGINE_TRY_ASSIGN(Scope<AssetEditCommand> command, AssetEditCommand::CreateForWrite(editor, metaPath, AsBytes(text), label));

			if (!IsInstancedInOpenScene(editor, handle))
			{
				// The write schedules the reimport; the result does not wait for it.
				ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(std::move(command)));
				result.UndoIndex = ToAutomationCounter(undoIndex);
				result.Asset = Utils::MakeAssetSummary(assets, handle);
				return result;
			}

			// §5.5 "Update": the instances follow the new settings in the same undo step. The overrides are recorded against the
			// version the instances were built from, the settings written, the asset reimported now, and the instances rebuilt.
			LoadReport report;
			ENGINE_TRY_ASSIGN(const Prefab current, LoadPrefabAsset(assets, handle, editor.GetTypeRegistry(), report));
			EditorTransaction transaction(editor, label);
			ENGINE_TRY(Utils::RecordPrefabOverrides(editor, handle, current));
			ENGINE_TRY(editor.Execute(std::move(command)));
			ENGINE_TRY_ASSIGN(const AssetImportOutcome outcome, assets.Reimport(handle));
			static_cast<void>(outcome);
			const AssetHandle prefabs[] = { handle };
			ENGINE_TRY_ASSIGN(Scope<Command> update, editor.CreatePrefabUpdateCommand(prefabs));
			if (update != nullptr)
				ENGINE_TRY(editor.Execute(std::move(update)));
			result.UndoIndex = ToAutomationCounter(transaction.Commit());
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			return result;
		}

		Result<AssetMoveResult> AssetMove(EditorMethodContext& context, const AssetMoveParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = editor.GetAssets();
			ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMainAssetRecord(assets, handle, "/asset", "file to move"));
			const std::string_view extension = record->SourcePath.GetExtension();
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveAssetsPath(context, params.Path, "/path", extension));
			ENGINE_TRY_ASSIGN(Scope<AssetMoveCommand> command, AssetMoveCommand::Create(editor, handle, path));

			AssetMoveResult result;
			for (const AssetFileMove& move : command->GetMoves())
				result.MovedFiles.push_back(Utils::ToProjectRelative(move.To));
			std::sort(result.MovedFiles.begin(), result.MovedFiles.end());
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(std::move(command)));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			return result;
		}

		Result<AssetDeleteResult> AssetDelete(EditorMethodContext& context, const AssetDeleteParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = editor.GetAssets();
			ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMainAssetRecord(assets, handle, "/asset", "file to delete"));
			static_cast<void>(record);
			AssetDeleteResult result;
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			ENGINE_TRY_ASSIGN(const VfsPath trash, AssetDeleteCommand::ChooseTrashDirectory(editor, handle));
			ENGINE_TRY_ASSIGN(Scope<AssetDeleteCommand> command, AssetDeleteCommand::Create(editor, handle));
			result.TrashDirectory = Utils::ToProjectRelative(trash);
			for (const AssetFileMove& move : command->GetMoves())
				result.TrashedFiles.push_back(Utils::ToProjectRelative(move.From));
			std::sort(result.TrashedFiles.begin(), result.TrashedFiles.end());
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(std::move(command)));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

	}

	void RegisterAssetMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<AssetState>("AssetState", "Where an asset is in its life (§7.2).")
			.Entry(AssetState::Unloaded, "Unloaded", "Registered, not loaded.")
			.Entry(AssetState::Loading, "Loading", "A load or the import it needs is running.")
			.Entry(AssetState::Loaded, "Loaded", "The current version is loaded.")
			.Entry(AssetState::Failed, "Failed", "The last load failed; uses get the placeholder and a diagnostic is recorded.");

		registry.Enum<AssetCreateType>("AssetCreateType", "What asset.create makes.")
			.Entry(AssetCreateType::Material, "Material", "A .material file: a PBR material.")
			.Entry(AssetCreateType::Scene, "Scene", "A .scene file: an empty scene named after the file.")
			.Entry(AssetCreateType::Prefab, "Prefab", "A .prefab file with one root entity named after the file.")
			.Entry(AssetCreateType::SoundEffect, "SoundEffect", "A .sfx sound effect preset; not supported by this editor yet.")
			.Entry(AssetCreateType::Folder, "Folder", "A folder below Assets/.");

		registry.Struct<AssetDiagnosticInfo>("AssetDiagnosticInfo", "A diagnostic of an asset.")
			.Field("severity", &AssetDiagnosticInfo::Severity, "How serious it is.")
			.Field("code", &AssetDiagnosticInfo::Code, "The diagnostic code, such as ASSET_IMPORT_FAILED.")
			.Field("path", &AssetDiagnosticInfo::Path, "The file concerned: project-relative, or an engine path.")
			.Field("message", &AssetDiagnosticInfo::Message, "What is wrong.")
			.Field("hint", &AssetDiagnosticInfo::Hint, "How to fix it; empty when there is no hint.");

		registry.Struct<AssetListParams>("AssetListParams", "The params of asset.list.")
			.Field("dir", &AssetListParams::Dir, "The project-relative folder to list, Assets or below it.")
			.Field("type", &AssetListParams::Type, "Only assets of this type; None lists every type.")
			.Field("recursive", &AssetListParams::Recursive, "Also list the folders below dir.")
			.Field("subAssets", &AssetListParams::SubAssets, "Also list sub-assets (a glTF's meshes, materials and textures) after their source.")
			.Field("limit", &AssetListParams::Limit, "The most assets to return.", { .Min = 1.0, .Max = static_cast<double>(MaxListLimit) })
			.Field("cursor", &AssetListParams::Cursor, "\"\" for the start, or the nextCursor of the previous call.");

		registry.Struct<AssetListResult>("AssetListResult", "One page of assets.")
			.Field("assets", &AssetListResult::Assets, "The assets, sorted by path, sub-assets after their source.")
			.Field("nextCursor", &AssetListResult::NextCursor, "The cursor of the next page; empty when the listing is complete.")
			.Field("total", &AssetListResult::Total, "The number of matching assets over every page.");

		registry.Struct<AssetInfoParams>("AssetInfoParams", "The params of asset.info.")
			.Field("asset", &AssetInfoParams::Asset, "The asset: 16 hex digits, a path such as \"Assets/Materials/Red.material\" or an engine path.");

		registry.Struct<AssetInfoResult>("AssetInfoResult", "An asset's metadata, dependencies and diagnostics.")
			.Field("asset", &AssetInfoResult::Asset, "The asset.")
			.Field("source", &AssetInfoResult::Source, "Its source file (project-relative; a sub-asset's is its source's), or an engine path.")
			.Field("importer", &AssetInfoResult::Importer, "The importer named in its .meta; empty for procedural built-ins.")
			.Field("importerVersion", &AssetInfoResult::ImporterVersion, "The registered importer's version.")
			.Field("settings", &AssetInfoResult::Settings, "The import settings of its .meta; null without settings.")
			.Field("subAssets", &AssetInfoResult::SubAssets, "The sub-assets of a source; empty for a sub-asset.")
			.Field("dependencies", &AssetInfoResult::Dependencies, "The assets it references, sorted by id.")
			.Field("dependents", &AssetInfoResult::Dependents, "The assets that reference it, sorted by id.")
			.Field("dependencyFiles", &AssetInfoResult::DependencyFiles, "Its dependency files (a glTF's buffers and images), project-relative, sorted.")
			.Field("diagnostics", &AssetInfoResult::Diagnostics, "Its diagnostics.")
			.Field("state", &AssetInfoResult::State, "Whether it is loaded.")
			.Field("version", &AssetInfoResult::Version, "Its version: 0 until loaded, +1 for every reload.");

		registry.Struct<AssetImportParams>("AssetImportParams", "The params of asset.import.")
			.Field("source", &AssetImportParams::Source, "The file to import: an absolute native path, or a project path.")
			.Field("destDir", &AssetImportParams::DestDir, "The project-relative folder below Assets/ to copy it into; created when missing.")
			.Field("settings", &AssetImportParams::Settings, "Import settings merged over the importer's defaults; null for the defaults.");

		registry.Struct<AssetImportResult>("AssetImportResult", "The imported asset.")
			.Field("asset", &AssetImportResult::Asset, "The imported asset.")
			.Field("subAssets", &AssetImportResult::SubAssets, "Its sub-assets.")
			.Field("copiedFiles", &AssetImportResult::CopiedFiles, "The files copied with their .meta files, project-relative, sorted.")
			.Field("diagnostics", &AssetImportResult::Diagnostics, "The importer's warnings.")
			.Field("undoIndex", &AssetImportResult::UndoIndex, "The undo index of the copy; 0 in a batch.");

		registry.Struct<AssetReimportParams>("AssetReimportParams", "The params of asset.reimport.")
			.Field("asset", &AssetReimportParams::Asset, "The asset to import again (a source, not a sub-asset).");

		registry.Struct<AssetReimportResult>("AssetReimportResult", "The reimported asset.")
			.Field("asset", &AssetReimportResult::Asset, "The reimported asset.")
			.Field("subAssets", &AssetReimportResult::SubAssets, "Its sub-assets.")
			.Field("diagnostics", &AssetReimportResult::Diagnostics, "The importer's warnings.");

		registry.Struct<AssetCreateParams>("AssetCreateParams", "The params of asset.create.")
			.Field("type", &AssetCreateParams::Type, "What to create.")
			.Field("path", &AssetCreateParams::Path, "The project-relative path below Assets/, with the type's extension (.material, .scene, .prefab).")
			.Field("values", &AssetCreateParams::Values, "A Material's fields over the defaults (registry names, such as Roughness); absent for other types.");

		registry.Struct<AssetCreateResult>("AssetCreateResult", "The created asset.")
			.Field("asset", &AssetCreateResult::Asset, "The created asset; an empty id and type None for a folder.")
			.Field("path", &AssetCreateResult::Path, "The project-relative path created.")
			.Field("undoIndex", &AssetCreateResult::UndoIndex, "The command's undo index; 0 in a dry run or a batch.");

		registry.Struct<AssetGetPropertiesParams>("AssetGetPropertiesParams", "The params of asset.getProperties.")
			.Field("asset", &AssetGetPropertiesParams::Asset, "The native asset (a material).");

		registry.Struct<AssetGetPropertiesResult>("AssetGetPropertiesResult", "A native asset's properties.")
			.Field("asset", &AssetGetPropertiesResult::Asset, "The asset.")
			.Field("values", &AssetGetPropertiesResult::Values, "Every property, by registry name.");

		registry.Struct<AssetSetPropertiesParams>("AssetSetPropertiesParams", "The params of asset.setProperties.")
			.Field("asset", &AssetSetPropertiesParams::Asset, "The native asset (a material).")
			.Field("values", &AssetSetPropertiesParams::Values, "An RFC 7386 merge patch of its properties: members replace, null resets a field.");

		registry.Struct<AssetSetPropertiesResult>("AssetSetPropertiesResult", "The properties after the patch.")
			.Field("asset", &AssetSetPropertiesResult::Asset, "The asset.")
			.Field("values", &AssetSetPropertiesResult::Values, "Every property after the patch.")
			.Field("undoIndex", &AssetSetPropertiesResult::UndoIndex, "The command's undo index; 0 when nothing changed, in a dry run or a batch.");

		registry.Struct<AssetGetImportSettingsParams>("AssetGetImportSettingsParams", "The params of asset.getImportSettings.")
			.Field("asset", &AssetGetImportSettingsParams::Asset, "The asset (a source, not a sub-asset).");

		registry.Struct<AssetGetImportSettingsResult>("AssetGetImportSettingsResult", "An asset's import settings.")
			.Field("asset", &AssetGetImportSettingsResult::Asset, "The asset.")
			.Field("importer", &AssetGetImportSettingsResult::Importer, "The importer's id.")
			.Field("settings", &AssetGetImportSettingsResult::Settings, "Every field of the importer's settings; null without settings.");

		registry.Struct<AssetSetImportSettingsParams>("AssetSetImportSettingsParams", "The params of asset.setImportSettings.")
			.Field("asset", &AssetSetImportSettingsParams::Asset, "The asset (a source, not a sub-asset).")
			.Field("settings", &AssetSetImportSettingsParams::Settings, "An RFC 7386 merge patch of the import settings.");

		registry.Struct<AssetSetImportSettingsResult>("AssetSetImportSettingsResult", "The import settings after the patch.")
			.Field("asset", &AssetSetImportSettingsResult::Asset, "The asset.")
			.Field("importer", &AssetSetImportSettingsResult::Importer, "The importer's id.")
			.Field("settings", &AssetSetImportSettingsResult::Settings, "Every field after the patch.")
			.Field("undoIndex", &AssetSetImportSettingsResult::UndoIndex, "The undo index; 0 when nothing changed or in a batch.");

		registry.Struct<AssetMoveParams>("AssetMoveParams", "The params of asset.move.")
			.Field("asset", &AssetMoveParams::Asset, "The asset to move (a source, not a sub-asset).")
			.Field("path", &AssetMoveParams::Path, "Its new project-relative path below Assets/, with the same extension.");

		registry.Struct<AssetMoveResult>("AssetMoveResult", "The moved asset.")
			.Field("asset", &AssetMoveResult::Asset, "The asset at its new path, with its handle unchanged.")
			.Field("movedFiles", &AssetMoveResult::MovedFiles, "The new paths of every file moved, .meta files and dependency files included.")
			.Field("undoIndex", &AssetMoveResult::UndoIndex, "The command's undo index; 0 in a batch.");

		registry.Struct<AssetDeleteParams>("AssetDeleteParams", "The params of asset.delete.")
			.Field("asset", &AssetDeleteParams::Asset, "The asset to move to the trash (a source, not a sub-asset).");

		registry.Struct<AssetDeleteResult>("AssetDeleteResult", "The deleted asset.")
			.Field("asset", &AssetDeleteResult::Asset, "The asset as it was.")
			.Field("trashDirectory", &AssetDeleteResult::TrashDirectory, "Where its files went: \"Library/Trash/<entry>\".")
			.Field("trashedFiles", &AssetDeleteResult::TrashedFiles, "The old paths of every file trashed, .meta files and dependency files included.")
			.Field("undoIndex", &AssetDeleteResult::UndoIndex, "The command's undo index; 0 in a batch.");
	}

	void RegisterAssetMethods(MethodRegistry& methods)
	{
		Json listExample = Json::object();
		listExample["dir"] = "Assets/Materials";
		listExample["type"] = "Material";
		methods.Add(
			{
				.Name = "asset.list",
				.Description = "Lists the project's assets below a folder, optionally of one type, sorted by path with each glTF's sub-assets "
							   "after it; paged with limit and cursor.",
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "List the materials.", .Params = listExample } },
			},
			&Automation::AssetList);

		Json infoExample = Json::object();
		infoExample["asset"] = "Assets/Models/Track.glb";
		methods.Add(
			{
				.Name = "asset.info",
				.Description = "Reports an asset's source, importer, import settings, sub-assets, dependencies, dependents, dependency files, "
							   "diagnostics, load state and version.",
				.RequiredParams = { "asset" },
				.AllowedInBatch = true,
				.Examples = { { .Description = "Inspect a glTF model.", .Params = infoExample } },
			},
			&Automation::AssetInfo);

		Json importExample = Json::object();
		importExample["source"] = "C:/Downloads/Kart/Kart.gltf";
		importExample["destDir"] = "Assets/Models";
		methods.AddPending<EditorMethodContext, AssetImportParams, AssetImportResult>(
			{
				.Name = "asset.import",
				.Description = "Copies a file (and a glTF's buffers and images) into a folder below Assets/ with its .meta files as one "
							   "undoable command, then imports it; resolves when the import completed.",
				.RequiredParams = { "source", "destDir" },
				.ExposeAsTool = true,
				.Mutates = true,
				.TimeoutSeconds = 300,
				.Examples = { { .Description = "Import a glTF model with its textures.", .Params = importExample } },
			},
			&Automation::AssetImport);

		Json reimportExample = Json::object();
		reimportExample["asset"] = "Assets/Models/Track.glb";
		methods.AddPending<EditorMethodContext, AssetReimportParams, AssetReimportResult>(
			{
				.Name = "asset.reimport",
				.Description = "Imports an asset again, bypassing the cache, and updates the open scene's instances of a changed prefab; "
							   "resolves when the import completed.",
				.RequiredParams = { "asset" },
				.Mutates = true,
				.TimeoutSeconds = 300,
				.Examples = { { .Description = "Import a model again.", .Params = reimportExample } },
			},
			&Automation::AssetReimport);

		Json createExample = Json::object();
		createExample["type"] = "Material";
		createExample["path"] = "Assets/Materials/Red.material";
		createExample["values"] = Json::object();
		createExample["values"]["BaseColor"] = Json::array({ 0.9, 0.15, 0.15, 1.0 });
		createExample["values"]["Roughness"] = 0.45;
		methods.Add(
			{
				.Name = "asset.create",
				.Description = "Creates a material (with values over the defaults), an empty scene, a one-entity prefab or a folder below "
							   "Assets/, with its .meta, as one undoable command.",
				.RequiredParams = { "type", "path" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Create a red material.", .Params = createExample } },
			},
			&Automation::AssetCreate);

		Json getPropertiesExample = Json::object();
		getPropertiesExample["asset"] = "Assets/Materials/Red.material";
		methods.Add(
			{
				.Name = "asset.getProperties",
				.Description = "Returns every property of a native asset (a material) by registry name.",
				.RequiredParams = { "asset" },
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read a material.", .Params = getPropertiesExample } },
			},
			&Automation::AssetGetProperties);

		Json setPropertiesExample = Json::object();
		setPropertiesExample["asset"] = "Assets/Materials/Red.material";
		setPropertiesExample["values"] = Json::object();
		setPropertiesExample["values"]["Metallic"] = 1.0;
		methods.Add(
			{
				.Name = "asset.setProperties",
				.Description = "Applies an RFC 7386 merge patch to a native asset's properties (a material) and writes it, as one undoable "
							   "command.",
				.RequiredParams = { "asset", "values" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Make a material metallic.", .Params = setPropertiesExample } },
			},
			&Automation::AssetSetProperties);

		Json getSettingsExample = Json::object();
		getSettingsExample["asset"] = "Assets/Textures/Wood.png";
		methods.Add(
			{
				.Name = "asset.getImportSettings",
				.Description = "Returns the importer and every import setting of an asset's .meta.",
				.RequiredParams = { "asset" },
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read a texture's import settings.", .Params = getSettingsExample } },
			},
			&Automation::AssetGetImportSettings);

		Json setSettingsExample = Json::object();
		setSettingsExample["asset"] = "Assets/Textures/WoodNormal.png";
		setSettingsExample["settings"] = Json::object();
		setSettingsExample["settings"]["Usage"] = "NormalMap";
		methods.Add(
			{
				.Name = "asset.setImportSettings",
				.Description = "Applies an RFC 7386 merge patch to an asset's import settings as one undoable command; the asset reimports. "
							   "For a glTF the open scene instantiates, the instances follow in the same undo step.",
				.RequiredParams = { "asset", "settings" },
				.Mutates = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Import a texture as a normal map.", .Params = setSettingsExample } },
			},
			&Automation::AssetSetImportSettings);

		Json moveExample = Json::object();
		moveExample["asset"] = "Assets/Red.material";
		moveExample["path"] = "Assets/Materials/Red.material";
		methods.Add(
			{
				.Name = "asset.move",
				.Description = "Moves an asset with its .meta (and a glTF's dependency files) to another path below Assets/ as one undoable "
							   "command; its handle and every reference to it stay valid.",
				.RequiredParams = { "asset", "path" },
				.ExposeAsTool = true,
				.Mutates = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Move a material into the Materials folder.", .Params = moveExample } },
			},
			&Automation::AssetMove);

		Json deleteExample = Json::object();
		deleteExample["asset"] = "Assets/Materials/Old.material";
		methods.Add(
			{
				.Name = "asset.delete",
				.Description = "Moves an asset with its .meta (and a glTF's dependency files) to Library/Trash as one undoable command; never "
							   "a permanent delete.",
				.RequiredParams = { "asset" },
				.ExposeAsTool = true,
				.Mutates = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Delete an unused material.", .Params = deleteExample } },
			},
			&Automation::AssetDelete);
	}

}
