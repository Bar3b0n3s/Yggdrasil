#include "EnginePCH.h"
#include "Engine/AssetPipeline/EngineAssetBaker.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/AssetPipeline/AssetCache.h"
#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace Engine {

	namespace {

		constexpr std::string_view EngineCacheScheme = "enginecache";
		constexpr std::string_view EngineResourcesScheme = "engine";
		// The hint of every entry this run cannot bake (EngineAssetBaker.h).
		constexpr std::string_view BakeElsewhereHint = "run Editor --headless --bake-engine-assets on a machine with a GPU";

		// What baking one File or Generated entry takes, with its cache key.
		struct PendingBake
		{
			AssetHandle Handle{};
			AssetType Type = AssetType::None;
			std::string Path{};
			uint64_t Key = 0;
			// File entries: the importer, the source and its complete settings.
			const IAssetImporter* Importer = nullptr;
			VfsPath SourcePath{};
			Buffer Source{};
			Json Settings{};
			// Generated entries: the generator.
			const EngineAssetGenerator* Generator = nullptr;
		};

	}

	namespace Utils {

		static VfsPath GetEngineCacheRoot()
		{
			// A scheme root is a valid path.
			Result<VfsPath> root = VfsPath::Create(EngineCacheScheme, {});
			ENGINE_CORE_VERIFY(root.has_value(), "enginecache:// is a valid path");
			return std::move(*root);
		}

		static Status ValidateSpecification(const EngineBakeSpecification& specification)
		{
			ENGINE_CORE_ASSERT(specification.Vfs != nullptr && specification.Importers != nullptr && specification.Registry != nullptr
					&& specification.Jobs != nullptr,
				"EngineBakeSpecification needs its VFS, importers, type registry and job system");
			if (!specification.Vfs->IsMounted(EngineResourcesScheme) || !specification.Vfs->IsMounted(EngineCacheScheme))
				return MakeError(ErrorCode::InvalidArgument, "baking engine assets needs engine:// and enginecache:// mounted");
			return {};
		}

		// The generator `id` of the specification, or nullptr.
		static const EngineAssetGenerator* FindGenerator(std::span<const EngineAssetGenerator> generators, std::string_view id)
		{
			const auto found = std::ranges::find(generators, id, &EngineAssetGenerator::Id);
			return found != generators.end() ? &*found : nullptr;
		}

		// An Unsupported error ("not available here") with the hint of EngineAssetBaker.h.
		static Error MakeUnsupported(std::string message)
		{
			return Error(ErrorCode::Unsupported, std::move(message)).WithHint(std::string(BakeElsewhereHint));
		}

		// The importer's default settings with the entry's Settings merged over them (like asset.setImportSettings), as the
		// complete canonical object; JSON null for an importer without settings. Errors: Validation for settings the
		// settings struct rejects (unknown members included) or settings on an importer that has none; NotFound for an
		// unregistered settings type.
		static Result<Json> MergeEntrySettings(const IAssetImporter& importer, const TypeRegistry& registry, const BuiltinAssetEntry& entry)
		{
			const std::string_view typeName = importer.GetSettingsTypeName();
			const bool hasPatch = !entry.Settings.IsNull() && !(entry.Settings.Get().is_object() && entry.Settings.Get().empty());
			if (typeName.empty())
			{
				if (hasPatch)
					return MakeError(ErrorCode::Validation, "the importer '{}' has no settings, but the entry sets some", importer.GetId());
				return Json(nullptr);
			}
			const StructInfo* settingsType = registry.FindStruct(typeName);
			if (settingsType == nullptr)
				return MakeError(ErrorCode::NotFound, "the settings type '{}' of the importer '{}' is not registered", typeName, importer.GetId());
			ObjectPtr settings = settingsType->CreateDefault();
			if (hasPatch)
			{
				const ReadContext strict{ .Schemas = nullptr, .Strict = true, .Diagnostics = nullptr };
				ENGINE_TRY(settingsType->ApplyMergePatch(settings.get(), JsonReader(entry.Settings.Get()), strict));
			}
			return settingsType->ToJson(settings.get());
		}

		// What baking `entry` takes. Errors: NotFound for a Procedural entry; Unsupported when this build or run lacks the
		// entry's importer or generator; the errors of reading the source and merging the settings.
		static Result<PendingBake> PrepareBake(const EngineBakeSpecification& specification, const BuiltinAssetEntry& entry)
		{
			PendingBake pending;
			pending.Handle = entry.Handle;
			pending.Type = entry.Type;
			pending.Path = entry.Path;
			switch (entry.Source)
			{
				case BuiltinAssetSource::Procedural:
				{
					return MakeError(ErrorCode::NotFound, "'{}' is a procedural built-in: it is generated in code and never cached", entry.Path);
				}
				case BuiltinAssetSource::File:
				{
					pending.Importer = specification.Importers->FindById(entry.Importer);
					if (pending.Importer == nullptr)
						return std::unexpected(MakeUnsupported(std::format("this build has no importer '{}' for '{}'", entry.Importer, entry.Path)));
					ENGINE_TRY_ASSIGN(pending.SourcePath, VfsPath::Create(EngineResourcesScheme, entry.File));
					ENGINE_TRY_ASSIGN(pending.Source, specification.Vfs->ReadFile(pending.SourcePath));
					ENGINE_TRY_ASSIGN(pending.Settings, MergeEntrySettings(*pending.Importer, *specification.Registry, entry));
					pending.Key = AssetCache::ComputeKey(pending.Source, pending.Importer->GetId(), pending.Importer->GetVersion(), pending.Settings,
						EngineCookVersion);
					return pending;
				}
				case BuiltinAssetSource::Generated:
				{
					pending.Generator = FindGenerator(specification.Generators, entry.Generator);
					if (pending.Generator == nullptr)
						return std::unexpected(MakeUnsupported(std::format("this run has no generator '{}' for '{}'", entry.Generator, entry.Path)));
					ENGINE_CORE_ASSERT(pending.Generator->Version >= 1 && pending.Generator->Generate != nullptr,
						"The engine asset generator '{}' needs a version and a function", pending.Generator->Id);
					pending.Key = AssetCache::ComputeKey({}, pending.Generator->Id, pending.Generator->Version, Json::object(), EngineCookVersion);
					return pending;
				}
			}
			return MakeError(ErrorCode::InvalidArgument, "'{}' has an unknown source", entry.Path);
		}

		// The cached bake of `pending` when it is valid for the current inputs; nullopt when it must be baked (missing, or
		// discarded by the cache as corrupted). Errors: the cache's (Io).
		static Result<std::optional<CachedImport>> FindCurrentBake(const VirtualFileSystem& vfs, const AssetCache& cache, const PendingBake& pending)
		{
			ENGINE_TRY_ASSIGN(std::optional<CachedImport> cached, cache.Find(pending.Handle, pending.Key));
			// Built-ins look nothing up, but an importer may read files below engine://, which must still hash the same.
			if (cached.has_value() && !IsManifestCurrent(vfs, cached->Reads, cached->Lookups, {}))
				cached.reset();
			return cached;
		}

		// Checks that every artifact of a bake is a valid cooked artifact of its type and that the main one is the entry's.
		static Status ValidateBakedArtifacts(const PendingBake& pending, const ImportResult& result)
		{
			if (result.Artifacts.empty() || result.Artifacts.front().Handle != pending.Handle || result.Artifacts.front().Type != pending.Type)
			{
				return MakeError(ErrorCode::ImportFailed, "the bake did not produce the {} {} as its main artifact", AssetTypeToString(pending.Type),
					pending.Handle);
			}
			for (const ImportedArtifact& artifact : result.Artifacts)
			{
				ENGINE_TRY_ASSIGN(const CookedArtifactView cooked, ReadCookedArtifact(artifact.Cooked));
				if (cooked.Header.Type != artifact.Type)
				{
					return MakeError(ErrorCode::ImportFailed, "the artifact {} holds a {}, not a {}", artifact.Handle, AssetTypeToString(cooked.Header.Type),
						AssetTypeToString(artifact.Type));
				}
			}
			return {};
		}

		// Imports or generates one entry. Pure apart from the files its importer reads, so it runs on a job unless the importer
		// requires the main thread.
		static Result<CachedImport> RunBake(const PendingBake& pending, const VirtualFileSystem* vfs, const TypeRegistry* registry,
			IEnvironmentBaker* environmentBaker)
		{
			const std::string context = std::format("while baking '{}'", pending.Path);
			CachedImport baked;
			if (pending.Generator != nullptr)
			{
				ENGINE_TRY_ASSIGN(Buffer cooked, WithContext(pending.Generator->Generate(), context));
				baked.Import.Artifacts.push_back(ImportedArtifact{ .Handle = pending.Handle, .Type = pending.Type, .SubAssetKey = {}, .Cooked = std::move(cooked) });
			}
			else
			{
				ImportContext importContext(ImportContext::Specification{
					.Vfs = vfs,
					.SourcePath = pending.SourcePath,
					.SourceBytes = pending.Source,
					.Settings = VariantValue(pending.Settings),
					.Registry = registry,
					.Assets = {},
					.EnvironmentBaker = environmentBaker,
					.ScriptDiagnostics = nullptr,
				});
				// Built-ins have no .meta: the catalogue fixes the handle, the importer and the settings.
				AssetMetadata metadata;
				metadata.Handle = pending.Handle;
				metadata.Kind = AssetMetaKind::Asset;
				metadata.Type = pending.Type;
				metadata.Importer = std::string(pending.Importer->GetId());
				metadata.ImporterVersion = pending.Importer->GetVersion();
				metadata.Settings = VariantValue(pending.Settings);
				ENGINE_TRY_ASSIGN(baked.Import, WithContext(pending.Importer->Import(importContext, metadata), context));
				baked.Reads = importContext.GetDependencyReads();
				baked.Lookups = importContext.GetLookups();
			}
			ENGINE_TRY(WithContext(ValidateBakedArtifacts(pending, baked.Import), context));
			return baked;
		}

		// The diagnostic of an entry this run did not bake: Warning for what is not available here (Unsupported: a missing
		// importer or generator, an import that needs a GPU), Error for a failure.
		static AssetDiagnostic MakeSkippedDiagnostic(const BuiltinAssetEntry& entry, const Error& error)
		{
			const bool unavailable = error.GetCode() == ErrorCode::Unsupported;
			std::string hint = error.GetHint();
			if (unavailable && hint.empty())
				hint = std::string(BakeElsewhereHint);
			return AssetDiagnostic{
				.Severity = unavailable ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error,
				.Code = std::string(AssetImportFailedCode),
				.Asset = entry.Handle,
				.Path = entry.Path,
				.Message = unavailable ? std::format("not baked: {}", error.GetMessageText()) : Error(error).WithHint(std::string()).ToString(),
				.Hint = std::move(hint),
				.Subject = {},
				.AutoFixable = false,
			};
		}

		// The cooked artifacts of a bake, main first.
		static std::vector<Buffer> TakeArtifacts(CachedImport import)
		{
			std::vector<Buffer> artifacts;
			artifacts.reserve(import.Import.Artifacts.size());
			for (ImportedArtifact& artifact : import.Import.Artifacts)
				artifacts.push_back(std::move(artifact.Cooked));
			return artifacts;
		}

	}

	Result<EngineBakeReport> BakeEngineAssets(const EngineBakeSpecification& specification, const BuiltinAssetCatalog& catalog)
	{
		ENGINE_TRY(Utils::ValidateSpecification(specification));
		AssetCache cache(*specification.Vfs, Utils::GetEngineCacheRoot());

		// One stale entry being baked: on a job, or already baked here by an importer that requires the main thread.
		struct BakeTask
		{
			size_t Index = 0;
			uint64_t Key = 0;
			JobHandle<CachedImport> Job{};
			std::optional<Result<CachedImport>> Done{};
		};

		// Plan every entry on this thread, bake the stale ones as jobs, then store the results here in catalogue order, so the
		// cache's writes and the report are deterministic whatever order the jobs finish in.
		const std::span<const BuiltinAssetEntry> entries = catalog.GetEntries();
		std::vector<std::optional<AssetDiagnostic>> skipped(entries.size());
		std::vector<bool> upToDate(entries.size(), false);
		std::vector<BakeTask> tasks;
		// The first cache failure ends the run, but only after every submitted job is done: the jobs use the specification's
		// services, which the caller may release once this returns.
		std::optional<Error> failure;
		for (size_t index = 0; index < entries.size() && !failure.has_value(); ++index)
		{
			const BuiltinAssetEntry& entry = entries[index];
			if (entry.Source == BuiltinAssetSource::Procedural)
				continue;
			Result<PendingBake> pending = Utils::PrepareBake(specification, entry);
			if (!pending.has_value())
			{
				skipped[index] = Utils::MakeSkippedDiagnostic(entry, pending.error());
				continue;
			}
			Result<std::optional<CachedImport>> cached = Utils::FindCurrentBake(*specification.Vfs, cache, *pending);
			if (!cached.has_value())
			{
				failure = std::move(cached).error();
				continue;
			}
			if (cached->has_value())
			{
				upToDate[index] = true;
				continue;
			}

			BakeTask& task = tasks.emplace_back();
			task.Index = index;
			task.Key = pending->Key;
			if (pending->Importer != nullptr && pending->Importer->RequiresMainThread())
			{
				task.Done = Utils::RunBake(*pending, specification.Vfs, specification.Registry, specification.EnvironmentBaker);
				continue;
			}
			const VirtualFileSystem* vfs = specification.Vfs;
			const TypeRegistry* registry = specification.Registry;
			IEnvironmentBaker* environmentBaker = specification.EnvironmentBaker;
			task.Job = specification.Jobs->Submit([bake = std::move(*pending), vfs, registry, environmentBaker]() -> Result<CachedImport>
			{
				return Utils::RunBake(bake, vfs, registry, environmentBaker);
			});
		}

		std::vector<bool> baked(entries.size(), false);
		for (BakeTask& task : tasks)
		{
			const BuiltinAssetEntry& entry = entries[task.Index];
			Result<CachedImport> result = task.Done.has_value() ? std::move(*task.Done) : task.Job.Take();
			if (failure.has_value())
				continue;
			if (!result.has_value())
			{
				skipped[task.Index] = Utils::MakeSkippedDiagnostic(entry, result.error());
				continue;
			}
			Status stored = cache.Store(entry.Handle, task.Key, *result);
			if (!stored.has_value())
			{
				failure = std::move(stored).error();
				continue;
			}
			baked[task.Index] = true;
		}
		if (failure.has_value())
			return std::unexpected(std::move(*failure));

		EngineBakeReport report;
		for (size_t index = 0; index < entries.size(); ++index)
		{
			if (baked[index])
				report.Baked.push_back(entries[index].Handle);
			else if (upToDate[index])
				report.UpToDate.push_back(entries[index].Handle);
			else if (skipped[index].has_value())
				report.Skipped.push_back(std::move(*skipped[index]));
		}
		return report;
	}

	Result<std::vector<Buffer>> GetOrBakeEngineAsset(const EngineBakeSpecification& specification, const BuiltinAssetEntry& entry)
	{
		ENGINE_TRY(Utils::ValidateSpecification(specification));
		AssetCache cache(*specification.Vfs, Utils::GetEngineCacheRoot());
		ENGINE_TRY_ASSIGN(const PendingBake pending, Utils::PrepareBake(specification, entry));
		ENGINE_TRY_ASSIGN(std::optional<CachedImport> cached, Utils::FindCurrentBake(*specification.Vfs, cache, pending));
		if (cached.has_value())
			return Utils::TakeArtifacts(std::move(*cached));

		Result<CachedImport> baked = Utils::RunBake(pending, specification.Vfs, specification.Registry, specification.EnvironmentBaker);
		if (!baked.has_value())
		{
			// An import that needs what this run lacks (a GPU) can be baked by a run that has it.
			if (baked.error().GetCode() == ErrorCode::Unsupported && baked.error().GetHint().empty())
				return std::unexpected(std::move(baked).error().WithHint(std::string(BakeElsewhereHint)));
			return std::unexpected(std::move(baked).error());
		}
		// The cache saves the next run this bake; it is no precondition of this one. bin/EngineCache is shared by every editor
		// of the checkout (decision 13), and on Windows replacing an entry fails while another process reads it, so a failed
		// store leaves the built-in usable and the next first use bakes it again.
		if (Status stored = cache.Store(entry.Handle, pending.Key, *baked); !stored.has_value())
		{
			ENGINE_CORE_WARN("Engine cooked cache: the bake of {} '{}' could not be stored (it is baked again at its next first use): {}",
				AssetTypeToString(entry.Type), entry.Path, stored.error().ToString());
		}
		return Utils::TakeArtifacts(std::move(*baked));
	}

}
