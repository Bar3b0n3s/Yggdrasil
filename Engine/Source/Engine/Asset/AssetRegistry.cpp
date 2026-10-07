#include "EnginePCH.h"
#include "Engine/Asset/AssetRegistry.h"

#include "Engine/Asset/AssetReference.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <algorithm>
#include <format>
#include <set>
#include <tuple>

namespace Engine {

	namespace {

		// A readable .meta whose source exists with exactly its spelling: a registration candidate of the scan.
		struct ScanCandidate
		{
			VfsPath MetaPath{};
			VfsPath SourcePath{};
			AssetMetadata Metadata{};
		};

	}

	namespace Utils {

		static constexpr std::string_view MetaExtension = ".meta";
		static constexpr std::string_view AssetsFolder = "Assets";

		static char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		static std::string FoldAsciiCase(std::string_view text)
		{
			std::string folded(text);
			std::ranges::transform(folded, folded.begin(), ToLowerAscii);
			return folded;
		}

		static bool IsMetaFile(const VfsPath& path)
		{
			const std::string_view name = path.GetFileName();
			return name.size() > MetaExtension.size() && name.ends_with(MetaExtension);
		}

		// The project-relative spelling diagnostics use ("Assets/Models/Track.glb").
		static std::string RelativeText(const VfsPath& path)
		{
			return std::string(path.GetPath());
		}

		static const AssetImporterDescription* FindImporterForExtension(std::span<const AssetImporterDescription> importers,
			std::string_view extension)
		{
			if (extension.empty())
				return nullptr;
			const std::string folded = FoldAsciiCase(extension);
			const auto found = std::ranges::find_if(importers, [&folded](const AssetImporterDescription& importer)
			{
				return std::ranges::find(importer.Extensions, folded) != importer.Extensions.end();
			});
			return found == importers.end() ? nullptr : &*found;
		}

		[[maybe_unused]] static bool AreKnownLocationsSorted(std::span<const AssetKnownLocation> knownLocations)
		{
			return std::ranges::adjacent_find(knownLocations, [](const AssetKnownLocation& left, const AssetKnownLocation& right)
			{
				return !(left.Handle < right.Handle);
			}) == knownLocations.end();
		}

		static const AssetKnownLocation* FindKnownLocation(std::span<const AssetKnownLocation> knownLocations, AssetHandle handle)
		{
			const auto found = std::ranges::lower_bound(knownLocations, handle, std::less<>(), &AssetKnownLocation::Handle);
			return found != knownLocations.end() && found->Handle == handle ? &*found : nullptr;
		}

		static Result<AssetMetadata> ReadMetadata(const VirtualFileSystem& vfs, const VfsPath& metaPath, std::string_view metaText)
		{
			ENGINE_TRY_ASSIGN(const std::string text, vfs.ReadText(metaPath));
			return ParseAssetMetadata(text, metaText);
		}

		static bool DiagnosticLess(const AssetDiagnostic& left, const AssetDiagnostic& right)
		{
			return std::tie(left.Path, left.Code, left.Subject) < std::tie(right.Path, right.Code, right.Subject);
		}

		// The metadata rules a registered record must satisfy (the parser enforces the same for .meta files).
		static Status ValidateRecord(const AssetRecord& record)
		{
			const AssetMetadata& metadata = record.Metadata;
			const Result<VfsPath> metaPath = GetMetaPath(record.SourcePath);
			if (!metaPath.has_value() || *metaPath != record.MetaPath)
			{
				return MakeError(ErrorCode::InvalidArgument, "the record of '{}' must keep its .meta beside it, not at '{}'", record.SourcePath.ToString(),
					record.MetaPath.ToString());
			}
			if (!metadata.Handle.IsValid())
				return MakeError(ErrorCode::InvalidArgument, "the record of '{}' has the null handle", record.SourcePath.ToString());
			if (metadata.Kind == AssetMetaKind::Dependency)
			{
				if (!metadata.Owner.IsValid() || metadata.Type != AssetType::None || !metadata.SubAssets.empty())
					return MakeError(ErrorCode::InvalidArgument, "the dependency record of '{}' needs an owner and no asset members", record.SourcePath.ToString());
				return {};
			}
			if (metadata.Type == AssetType::None || metadata.Importer.empty() || metadata.Owner.IsValid())
				return MakeError(ErrorCode::InvalidArgument, "the asset record of '{}' needs a type and an importer and no owner", record.SourcePath.ToString());
			for (size_t index = 0; index < metadata.SubAssets.size(); ++index)
			{
				const SubAssetEntry& entry = metadata.SubAssets[index];
				if (entry.Key.empty() || entry.Type == AssetType::None || entry.Handle != DeriveSubAssetHandle(metadata.Handle, entry.Key)
					|| (index > 0 && !(metadata.SubAssets[index - 1].Key < entry.Key)))
				{
					return MakeError(ErrorCode::InvalidArgument, "the asset record of '{}' has an invalid sub-asset entry '{}'", record.SourcePath.ToString(),
						entry.Key);
				}
			}
			return {};
		}

	}

	Result<AssetScanResult> AssetRegistry::Scan(const VirtualFileSystem& vfs, const VfsPath& root,
		std::span<const AssetImporterDescription> importers, std::span<const AssetKnownLocation> knownLocations)
	{
		ENGINE_CORE_ASSERT(Utils::AreKnownLocationsSorted(knownLocations), "Scan: knownLocations must be sorted by handle, each handle once");
		ENGINE_TRY_ASSIGN(const std::vector<VfsEntry> entries, vfs.List(root, true));

		// The listing is sorted by path, so everything below follows the canonical order.
		std::vector<VfsPath> metaPaths;
		std::vector<VfsPath> sources;
		for (const VfsEntry& entry : entries)
		{
			if (entry.Info.IsDirectory)
				continue;
			if (Utils::IsMetaFile(entry.Path))
				metaPaths.push_back(entry.Path);
			else
				sources.push_back(entry.Path);
		}
		const std::set<VfsPath> sourceSet(sources.begin(), sources.end());
		// No directory holds two entries that differ only in ASCII case (§4.10), so folded paths are unique.
		std::map<std::string, VfsPath> sourcesByFoldedPath;
		for (const VfsPath& source : sources)
			sourcesByFoldedPath.emplace(Utils::FoldAsciiCase(source.GetPath()), source);

		AssetScanResult result;
		std::vector<ScanFixSource> fixes;
		std::set<VfsPath> covered; // sources that have a .meta of some spelling, readable or not
		std::vector<ScanCandidate> candidates;

		for (const VfsPath& metaPath : metaPaths)
		{
			++result.MetaCount;
			const std::string metaText = Utils::RelativeText(metaPath);
			ENGINE_TRY_ASSIGN(const VfsPath sourcePath, GetSourcePathOfMeta(metaPath));
			const bool exact = sourceSet.contains(sourcePath);
			const VfsPath* caseVariant = nullptr; // the source's own spelling when only the letter case differs
			if (exact)
			{
				covered.insert(sourcePath);
			}
			else if (const auto variant = sourcesByFoldedPath.find(Utils::FoldAsciiCase(sourcePath.GetPath())); variant != sourcesByFoldedPath.end())
			{
				caseVariant = &variant->second;
				covered.insert(variant->second);
			}

			Result<AssetMetadata> metadata = Utils::ReadMetadata(vfs, metaPath, metaText);
			if (!metadata.has_value())
			{
				// Left alone: a replacement .meta would give the asset a new handle and break every reference to it.
				result.Diagnostics.push_back({
					.Severity = DiagnosticSeverity::Error,
					.Code = std::string(AssetImportFailedCode),
					.Asset = {},
					.Path = metaText,
					.Message = std::format("the .meta cannot be read: {}", metadata.error().ToString()),
					.Hint = "correct the .meta by hand; replacing it would give the asset a new handle and break every reference to it",
					.Subject = {},
					.AutoFixable = false,
				});
				continue;
			}

			if (exact)
			{
				candidates.push_back({ .MetaPath = metaPath, .SourcePath = sourcePath, .Metadata = std::move(*metadata) });
			}
			else if (caseVariant != nullptr)
			{
				const std::string sourceText = Utils::RelativeText(*caseVariant);
				ENGINE_TRY_ASSIGN(const VfsPath fixedMetaPath, GetMetaPath(*caseVariant));
				result.Diagnostics.push_back({
					.Severity = DiagnosticSeverity::Error,
					.Code = std::string(PathCaseMismatchCode),
					.Asset = metadata->Handle,
					.Path = metaText,
					.Message = std::format("the .meta matches the source '{}' only when letter case is ignored", sourceText),
					.Hint = std::format("rename the .meta to '{}' (project.validate {{fix: true}})", fixedMetaPath.GetFileName()),
					.Subject = sourceText,
					.AutoFixable = true,
				});
				fixes.push_back({
					.Kind = AssetScanFixKind::RenameMetaToSourceCase,
					.Code = std::string(PathCaseMismatchCode),
					.Path = metaText,
					.Subject = sourceText,
					.MetaPath = metaPath,
					.SourcePath = *caseVariant,
					.Metadata = {},
				});
			}
			else
			{
				result.Diagnostics.push_back({
					.Severity = DiagnosticSeverity::Warning,
					.Code = std::string(AssetOrphanMetaCode),
					.Asset = metadata->Handle,
					.Path = metaText,
					.Message = std::format("the .meta has no source file '{}'", Utils::RelativeText(sourcePath)),
					.Hint = "restore the source file, or move the .meta to the trash (project.validate {fix: true})",
					.Subject = {},
					.AutoFixable = true,
				});
				fixes.push_back({
					.Kind = AssetScanFixKind::TrashMeta,
					.Code = std::string(AssetOrphanMetaCode),
					.Path = metaText,
					.Subject = {},
					.MetaPath = metaPath,
					.SourcePath = sourcePath,
					.Metadata = {},
				});
			}
		}

		// The keeper rule for handles that several .meta files carry (see the header): the registered path, then the last
		// known location, then the shortest path, then byte order. Never the order the copies happen to sort in.
		std::map<AssetHandle, std::vector<size_t>> candidatesByHandle;
		for (size_t index = 0; index < candidates.size(); ++index)
			candidatesByHandle[candidates[index].Metadata.Handle].push_back(index);

		std::vector<size_t> keepers;
		std::vector<std::pair<size_t, size_t>> duplicates; // (duplicate, keeper)
		for (const auto& [handle, indices] : candidatesByHandle)
		{
			size_t keeper = indices.front();
			if (indices.size() > 1)
			{
				const auto findAt = [&candidates, &indices](const VfsPath& path) -> std::optional<size_t>
				{
					const auto found = std::ranges::find_if(indices, [&candidates, &path](size_t index)
					{
						return candidates[index].SourcePath == path;
					});
					return found == indices.end() ? std::nullopt : std::optional<size_t>(*found);
				};
				std::optional<size_t> chosen;
				if (const auto registered = m_Records.find(handle); registered != m_Records.end())
					chosen = findAt(registered->second.SourcePath);
				if (!chosen.has_value())
				{
					if (const AssetKnownLocation* known = Utils::FindKnownLocation(knownLocations, handle))
						chosen = findAt(known->SourcePath);
				}
				if (!chosen.has_value())
				{
					chosen = *std::ranges::min_element(indices, [&candidates](size_t left, size_t right)
					{
						const std::string_view leftPath = candidates[left].SourcePath.GetPath();
						const std::string_view rightPath = candidates[right].SourcePath.GetPath();
						return std::tuple(leftPath.size(), leftPath) < std::tuple(rightPath.size(), rightPath);
					});
				}
				keeper = *chosen;
				for (const size_t index : indices)
				{
					if (index != keeper)
						duplicates.emplace_back(index, keeper);
				}
			}
			keepers.push_back(keeper);
		}

		const auto reportDuplicate = [&result, &fixes](const ScanCandidate& duplicate, const VfsPath& keeperMetaPath)
		{
			const std::string metaText = Utils::RelativeText(duplicate.MetaPath);
			const std::string keeperText = Utils::RelativeText(keeperMetaPath);
			result.Diagnostics.push_back({
				.Severity = DiagnosticSeverity::Error,
				.Code = std::string(AssetDuplicateHandleCode),
				.Asset = duplicate.Metadata.Handle,
				.Path = metaText,
				.Message = std::format("duplicate handle {} (also in {})", duplicate.Metadata.Handle.ToString(), keeperText),
				.Hint = "project.validate {fix: true} gives the copy a fresh handle",
				.Subject = keeperText,
				.AutoFixable = true,
			});
			fixes.push_back({
				.Kind = AssetScanFixKind::AssignNewHandle,
				.Code = std::string(AssetDuplicateHandleCode),
				.Path = metaText,
				.Subject = keeperText,
				.MetaPath = duplicate.MetaPath,
				.SourcePath = duplicate.SourcePath,
				.Metadata = duplicate.Metadata,
			});
		};
		for (const auto& [duplicate, keeper] : duplicates)
		{
			covered.insert(candidates[duplicate].SourcePath);
			reportDuplicate(candidates[duplicate], candidates[keeper].MetaPath);
		}

		// Rebuild the content from the keepers, in path order.
		std::ranges::sort(keepers, [&candidates](size_t left, size_t right)
		{
			return candidates[left].MetaPath < candidates[right].MetaPath;
		});
		m_Records.clear();
		m_SourcePaths.clear();
		m_SubAssets.clear();
		for (const size_t index : keepers)
		{
			ScanCandidate& candidate = candidates[index];
			AssetRecord record{ .Metadata = candidate.Metadata, .SourcePath = candidate.SourcePath, .MetaPath = candidate.MetaPath };
			if (const Status insertable = CheckInsertable(record, AssetHandle()); !insertable.has_value())
			{
				// A handle that another record already holds as a sub-asset handle (or the reverse): as rare as a hash
				// collision, and fixed the same way as a copied .meta.
				const auto holderOf = [this](AssetHandle handle) -> const AssetRecord*
				{
					if (const AssetRecord* holder = Find(handle))
						return holder;
					const auto subAsset = m_SubAssets.find(handle);
					return subAsset != m_SubAssets.end() ? Find(subAsset->second) : nullptr;
				};
				const AssetRecord* other = holderOf(record.Metadata.Handle);
				for (const SubAssetEntry& entry : record.Metadata.SubAssets)
				{
					if (other == nullptr)
						other = holderOf(entry.Handle);
				}
				reportDuplicate(candidate, other != nullptr ? other->MetaPath : candidate.MetaPath);
				continue;
			}
			Insert(std::move(record));
		}

		// Type/importer mismatches (registered all the same: the handle stays valid) and orphan dependencies.
		for (const auto& [handle, record] : m_Records)
		{
			const std::string metaText = Utils::RelativeText(record.MetaPath);
			if (record.Metadata.Kind == AssetMetaKind::Asset)
			{
				const AssetImporterDescription* importer = Utils::FindImporterForExtension(importers, record.SourcePath.GetExtension());
				if (importer != nullptr && (importer->Id != record.Metadata.Importer || importer->MainType != record.Metadata.Type))
				{
					result.Diagnostics.push_back({
						.Severity = DiagnosticSeverity::Error,
						.Code = std::string(AssetTypeMismatchCode),
						.Asset = handle,
						.Path = metaText,
						.Message = std::format("the .meta names importer '{}' and type {}, but '{}' files are imported by '{}' as {}",
							record.Metadata.Importer, AssetTypeToString(record.Metadata.Type), record.SourcePath.GetExtension(), importer->Id,
							AssetTypeToString(importer->MainType)),
						.Hint = std::format("rewrite the .meta for importer '{}', keeping its Handle, with that importer's default settings "
											"(project.validate {{fix: true}})",
							importer->Id),
						.Subject = {},
						.AutoFixable = true,
					});
					// The .meta of the importer that takes the extension, keeping the handle: the old importer's settings and
					// sub-assets mean nothing to the new one (its defaults apply, and its next import lists its sub-assets).
					AssetMetadata rewritten = record.Metadata;
					rewritten.Type = importer->MainType;
					rewritten.Importer = importer->Id;
					rewritten.ImporterVersion = importer->Version;
					rewritten.Settings = VariantValue();
					rewritten.SubAssets.clear();
					fixes.push_back({
						.Kind = AssetScanFixKind::RewriteImporter,
						.Code = std::string(AssetTypeMismatchCode),
						.Path = metaText,
						.Subject = {},
						.MetaPath = record.MetaPath,
						.SourcePath = record.SourcePath,
						.Metadata = std::move(rewritten),
					});
				}
				continue;
			}

			const auto owner = m_Records.find(record.Metadata.Owner);
			if (owner == m_Records.end() || owner->second.Metadata.Kind != AssetMetaKind::Asset)
			{
				result.Diagnostics.push_back({
					.Severity = DiagnosticSeverity::Warning,
					.Code = std::string(AssetOrphanDependencyCode),
					.Asset = handle,
					.Path = metaText,
					.Message = std::format("the dependency's owner {} is not registered", record.Metadata.Owner.ToString()),
					.Hint = "restore the owning asset, or move the dependency's .meta to the trash (project.validate {fix: true})",
					.Subject = {},
					.AutoFixable = true,
				});
				fixes.push_back({
					.Kind = AssetScanFixKind::TrashMeta,
					.Code = std::string(AssetOrphanDependencyCode),
					.Path = metaText,
					.Subject = {},
					.MetaPath = record.MetaPath,
					.SourcePath = record.SourcePath,
					.Metadata = {},
				});
			}
		}

		for (const VfsPath& source : sources)
		{
			if (!covered.contains(source) && Utils::FindImporterForExtension(importers, source.GetExtension()) != nullptr)
				result.SourcesWithoutMeta.push_back(source);
		}

		std::ranges::stable_sort(result.Diagnostics, Utils::DiagnosticLess);
		m_ScanDiagnostics = result.Diagnostics;
		m_ScanFixes = std::move(fixes);
		return result;
	}

	const AssetRecord* AssetRegistry::Find(AssetHandle handle) const
	{
		const auto found = m_Records.find(handle);
		return found == m_Records.end() ? nullptr : &found->second;
	}

	const AssetRecord* AssetRegistry::FindBySourcePath(const VfsPath& sourcePath) const
	{
		const auto found = m_SourcePaths.find(sourcePath);
		return found == m_SourcePaths.end() ? nullptr : Find(found->second);
	}

	std::optional<AssetRegistry::Location> AssetRegistry::Locate(AssetHandle handle) const
	{
		if (const AssetRecord* record = Find(handle))
		{
			if (record->Metadata.Kind != AssetMetaKind::Asset)
				return std::nullopt;
			return Location{ .Record = record, .Type = record->Metadata.Type, .SubAssetKey = {} };
		}
		const auto subAsset = m_SubAssets.find(handle);
		if (subAsset == m_SubAssets.end())
			return std::nullopt;
		const AssetRecord* source = Find(subAsset->second);
		ENGINE_CORE_ASSERT(source != nullptr, "Sub-asset {} has no source record", handle.ToString());
		if (source == nullptr)
			return std::nullopt;
		const auto entry = std::ranges::find(source->Metadata.SubAssets, handle, &SubAssetEntry::Handle);
		if (entry == source->Metadata.SubAssets.end())
			return std::nullopt;
		return Location{ .Record = source, .Type = entry->Type, .SubAssetKey = entry->Key };
	}

	std::optional<AssetHandle> AssetRegistry::Resolve(std::string_view reference) const
	{
		const Result<AssetReference> parsed = ParseAssetReference(reference);
		if (!parsed.has_value())
			return std::nullopt;
		switch (parsed->Kind)
		{
			case AssetReferenceKind::Handle:
				return Locate(parsed->Handle).has_value() ? std::optional<AssetHandle>(parsed->Handle) : std::nullopt;
			case AssetReferenceKind::ProjectPath:
			{
				const AssetRecord* record = FindBySourcePath(parsed->Path);
				if (record == nullptr || record->Metadata.Kind != AssetMetaKind::Asset)
					return std::nullopt;
				if (parsed->SubAssetKey.empty())
					return record->Metadata.Handle;
				const SubAssetEntry* entry = record->Metadata.FindSubAsset(parsed->SubAssetKey);
				return entry != nullptr ? std::optional<AssetHandle>(entry->Handle) : std::nullopt;
			}
			case AssetReferenceKind::EnginePath:
				return std::nullopt;
		}
		return std::nullopt;
	}

	std::string AssetRegistry::GetReferencePath(AssetHandle handle) const
	{
		const std::optional<Location> location = Locate(handle);
		if (!location.has_value())
			return {};
		return FormatAssetReference({
			.Kind = AssetReferenceKind::ProjectPath,
			.Handle = {},
			.Path = location->Record->SourcePath,
			.SubAssetKey = location->SubAssetKey,
		});
	}

	std::vector<AssetHandle> AssetRegistry::GetHandles() const
	{
		std::vector<AssetHandle> handles;
		handles.reserve(m_Records.size() + m_SubAssets.size());
		for (const auto& [handle, record] : m_Records)
		{
			if (record.Metadata.Kind == AssetMetaKind::Asset)
				handles.push_back(handle);
		}
		for (const auto& entry : m_SubAssets)
			handles.push_back(entry.first);
		std::ranges::sort(handles);
		return handles;
	}

	std::vector<const AssetRecord*> AssetRegistry::GetRecords() const
	{
		std::vector<const AssetRecord*> records;
		records.reserve(m_SourcePaths.size());
		for (const auto& entry : m_SourcePaths)
			records.push_back(Find(entry.second));
		return records;
	}

	std::vector<const AssetRecord*> AssetRegistry::GetDependencyRecords(AssetHandle owner) const
	{
		std::vector<const AssetRecord*> records;
		for (const auto& entry : m_SourcePaths)
		{
			const AssetRecord* record = Find(entry.second);
			if (record->Metadata.Kind == AssetMetaKind::Dependency && record->Metadata.Owner == owner)
				records.push_back(record);
		}
		return records;
	}

	std::vector<AssetKnownLocation> AssetRegistry::GetKnownLocations() const
	{
		std::vector<AssetKnownLocation> locations;
		locations.reserve(m_Records.size());
		for (const auto& [handle, record] : m_Records)
			locations.push_back({ .Handle = handle, .SourcePath = record.SourcePath });
		return locations;
	}

	Status AssetRegistry::Add(AssetRecord record)
	{
		ENGINE_TRY(CheckInsertable(record, AssetHandle()));
		Insert(std::move(record));
		return {};
	}

	Status AssetRegistry::Update(AssetRecord record)
	{
		const AssetHandle handle = record.Metadata.Handle;
		if (Find(handle) == nullptr)
			return MakeError(ErrorCode::NotFound, "no registered .meta has handle {}", handle.ToString());
		ENGINE_TRY(CheckInsertable(record, handle));
		Erase(handle);
		Insert(std::move(record));
		return {};
	}

	Status AssetRegistry::Remove(AssetHandle handle)
	{
		if (Find(handle) == nullptr)
			return MakeError(ErrorCode::NotFound, "no registered .meta has handle {}", handle.ToString());
		Erase(handle);
		return {};
	}

	Status AssetRegistry::Rename(AssetHandle handle, const VfsPath& newSourcePath)
	{
		const auto found = m_Records.find(handle);
		if (found == m_Records.end())
			return MakeError(ErrorCode::NotFound, "no registered .meta has handle {}", handle.ToString());
		AssetRecord& record = found->second;
		if (record.SourcePath == newSourcePath)
			return {};
		if (m_SourcePaths.contains(newSourcePath))
			return MakeError(ErrorCode::AlreadyExists, "'{}' is already registered", newSourcePath.ToString());
		Result<VfsPath> metaPath = GetMetaPath(newSourcePath);
		if (!metaPath.has_value())
			return std::unexpected(std::move(metaPath).error());
		m_SourcePaths.erase(record.SourcePath);
		record.SourcePath = newSourcePath;
		record.MetaPath = std::move(*metaPath);
		m_SourcePaths.emplace(newSourcePath, handle);
		return {};
	}

	Result<const AssetRecord*> AssetRegistry::FindMovableRecord(AssetHandle handle) const
	{
		const AssetRecord* record = Find(handle);
		if (record == nullptr)
		{
			if (m_SubAssets.contains(handle))
				return MakeError(ErrorCode::InvalidArgument, "{} is a sub-asset, which moves with its source file", handle.ToString());
			return MakeError(ErrorCode::NotFound, "no registered asset has handle {}", handle.ToString());
		}
		if (record->Metadata.Kind != AssetMetaKind::Asset)
		{
			return MakeError(ErrorCode::InvalidArgument, "{} ('{}') is a dependency of {}, which moves with its owner", handle.ToString(),
				record->SourcePath.ToString(), record->Metadata.Owner.ToString());
		}
		return record;
	}

	Result<std::vector<AssetFileMove>> AssetRegistry::PlanMove(AssetHandle handle, const VfsPath& newSourcePath) const
	{
		ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMovableRecord(handle));
		ENGINE_TRY_ASSIGN(const VfsPath assetsRoot, VfsPath::Create(record->SourcePath.GetScheme(), Utils::AssetsFolder));
		if (!newSourcePath.IsUnder(assetsRoot) || newSourcePath == assetsRoot)
		{
			return MakeError(ErrorCode::InvalidArgument, "'{}' is outside the Assets folder '{}'", newSourcePath.ToString(),
				assetsRoot.ToString());
		}
		if (newSourcePath == record->SourcePath)
			return MakeError(ErrorCode::InvalidArgument, "'{}' is already at '{}'", handle.ToString(), newSourcePath.ToString());
		ENGINE_TRY_ASSIGN(const VfsPath newMetaPath, GetMetaPath(newSourcePath));
		if (m_SourcePaths.contains(newSourcePath))
			return MakeError(ErrorCode::AlreadyExists, "'{}' is already registered", newSourcePath.ToString());

		std::vector<AssetFileMove> moves = {
			{ .From = record->SourcePath, .To = newSourcePath },
			{ .From = record->MetaPath, .To = newMetaPath },
		};

		// The dependency files keep their paths relative to the source's directory, so its relative URIs stay valid.
		const VfsPath sourceDirectory = record->SourcePath.GetParent();
		const VfsPath newDirectory = newSourcePath.GetParent();
		const size_t prefixLength = sourceDirectory.GetPath().empty() ? 0 : sourceDirectory.GetPath().size() + 1;
		for (const AssetRecord* dependency : GetDependencyRecords(handle))
		{
			if (!dependency->SourcePath.IsUnder(sourceDirectory))
			{
				return MakeError(ErrorCode::InvalidArgument, "the dependency '{}' of '{}' lies outside the source's folder, so it cannot move with it",
					dependency->SourcePath.ToString(), record->SourcePath.ToString());
			}
			ENGINE_TRY_ASSIGN(const VfsPath newDependencyPath, newDirectory.Join(dependency->SourcePath.GetPath().substr(prefixLength)));
			if (newDependencyPath == dependency->SourcePath)
				continue;
			if (m_SourcePaths.contains(newDependencyPath))
				return MakeError(ErrorCode::AlreadyExists, "'{}' is already registered", newDependencyPath.ToString());
			ENGINE_TRY_ASSIGN(const VfsPath newDependencyMetaPath, GetMetaPath(newDependencyPath));
			moves.push_back({ .From = dependency->SourcePath, .To = newDependencyPath });
			moves.push_back({ .From = dependency->MetaPath, .To = newDependencyMetaPath });
		}
		return moves;
	}

	Result<std::vector<AssetFileMove>> AssetRegistry::PlanTrash(AssetHandle handle, const VfsPath& trashDirectory) const
	{
		ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMovableRecord(handle));
		if (trashDirectory.GetScheme() != record->SourcePath.GetScheme())
		{
			return MakeError(ErrorCode::InvalidArgument, "the trash '{}' is not in the scheme of '{}'", trashDirectory.ToString(),
				record->SourcePath.ToString());
		}

		std::vector<VfsPath> files = { record->SourcePath, record->MetaPath };
		for (const AssetRecord* dependency : GetDependencyRecords(handle))
		{
			files.push_back(dependency->SourcePath);
			files.push_back(dependency->MetaPath);
		}
		std::vector<AssetFileMove> moves;
		moves.reserve(files.size());
		for (const VfsPath& file : files)
		{
			ENGINE_TRY_ASSIGN(VfsPath destination, trashDirectory.Join(file.GetPath()));
			moves.push_back({ .From = file, .To = std::move(destination) });
		}
		return moves;
	}

	Result<AssetScanFix> AssetRegistry::PlanFix(const AssetDiagnostic& diagnostic, AssetHandle newHandle) const
	{
		const auto reported = std::ranges::find_if(m_ScanDiagnostics, [&diagnostic](const AssetDiagnostic& candidate)
		{
			return candidate.Code == diagnostic.Code && candidate.Path == diagnostic.Path && candidate.Subject == diagnostic.Subject;
		});
		if (reported == m_ScanDiagnostics.end())
			return MakeError(ErrorCode::NotFound, "the last scan did not report {} for '{}'", diagnostic.Code, diagnostic.Path);
		const auto fix = std::ranges::find_if(m_ScanFixes, [&diagnostic](const ScanFixSource& candidate)
		{
			return candidate.Code == diagnostic.Code && candidate.Path == diagnostic.Path && candidate.Subject == diagnostic.Subject;
		});
		if (!reported->AutoFixable || fix == m_ScanFixes.end())
			return MakeError(ErrorCode::InvalidArgument, "{} for '{}' cannot be fixed automatically", diagnostic.Code, diagnostic.Path);

		AssetScanFix plan{ .Kind = fix->Kind, .MetaPath = fix->MetaPath, .NewMetaPath = {}, .NewMetaText = {} };
		switch (fix->Kind)
		{
			case AssetScanFixKind::AssignNewHandle:
			{
				if (!newHandle.IsValid())
					return MakeError(ErrorCode::InvalidArgument, "a fresh handle for '{}' must not be the null handle", diagnostic.Path);
				AssetMetadata metadata = fix->Metadata;
				metadata.Handle = newHandle;
				for (SubAssetEntry& entry : metadata.SubAssets)
					entry.Handle = DeriveSubAssetHandle(newHandle, entry.Key);
				plan.NewMetaText = SerializeAssetMetadata(metadata);
				break;
			}
			case AssetScanFixKind::TrashMeta:
				break;
			case AssetScanFixKind::RenameMetaToSourceCase:
			{
				ENGINE_TRY_ASSIGN(plan.NewMetaPath, GetMetaPath(fix->SourcePath));
				break;
			}
			case AssetScanFixKind::RewriteImporter:
				plan.NewMetaText = SerializeAssetMetadata(fix->Metadata);
				break;
		}
		return plan;
	}

	Status AssetRegistry::CheckInsertable(const AssetRecord& record, AssetHandle ignored) const
	{
		ENGINE_TRY(Utils::ValidateRecord(record));
		// Whether `handle` is taken by a record other than `ignored` (as a .meta handle or a sub-asset handle).
		const auto isTaken = [this, ignored](AssetHandle handle)
		{
			if (handle != ignored && m_Records.contains(handle))
				return true;
			const auto subAsset = m_SubAssets.find(handle);
			return subAsset != m_SubAssets.end() && subAsset->second != ignored;
		};
		const AssetHandle handle = record.Metadata.Handle;
		if (isTaken(handle))
			return MakeError(ErrorCode::AlreadyExists, "handle {} of '{}' is already registered", handle.ToString(), record.SourcePath.ToString());
		const auto path = m_SourcePaths.find(record.SourcePath);
		if (path != m_SourcePaths.end() && path->second != ignored)
			return MakeError(ErrorCode::AlreadyExists, "'{}' is already registered", record.SourcePath.ToString());
		for (const SubAssetEntry& entry : record.Metadata.SubAssets)
		{
			if (isTaken(entry.Handle) || entry.Handle == handle)
			{
				return MakeError(ErrorCode::AlreadyExists, "sub-asset handle {} ('{}') of '{}' is already registered", entry.Handle.ToString(),
					entry.Key, record.SourcePath.ToString());
			}
		}
		return {};
	}

	void AssetRegistry::Insert(AssetRecord record)
	{
		const AssetHandle handle = record.Metadata.Handle;
		m_SourcePaths[record.SourcePath] = handle;
		for (const SubAssetEntry& entry : record.Metadata.SubAssets)
			m_SubAssets[entry.Handle] = handle;
		m_Records[handle] = std::move(record);
	}

	void AssetRegistry::Erase(AssetHandle handle)
	{
		const auto found = m_Records.find(handle);
		if (found == m_Records.end())
			return;
		for (const SubAssetEntry& entry : found->second.Metadata.SubAssets)
			m_SubAssets.erase(entry.Handle);
		m_SourcePaths.erase(found->second.SourcePath);
		m_Records.erase(found);
	}

}
