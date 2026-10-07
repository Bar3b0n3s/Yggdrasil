#include "EnginePCH.h"
#include "Engine/AssetPipeline/IAssetImporter.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <algorithm>
#include <format>
#include <iterator>

namespace Engine {

	namespace Utils {

		static constexpr std::string_view EngineScheme = "engine";
		static constexpr std::string_view AssetsFolder = "Assets";

		static char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		static bool EqualsIgnoringAsciiCase(std::string_view left, std::string_view right)
		{
			return std::ranges::equal(left, right, [](char a, char b)
			{
				return ToLowerAscii(a) == ToLowerAscii(b);
			});
		}

		// Whether `path` lies inside the asset root of `source`'s scheme: anywhere under engine:// for a built-in, below the
		// Assets folder otherwise (project://Assets).
		static bool IsInsideAssetRoot(const VfsPath& source, const VfsPath& path)
		{
			if (path.GetScheme() != source.GetScheme())
				return false;
			if (source.GetScheme() == EngineScheme)
				return !path.IsRoot();
			const Result<VfsPath> root = VfsPath::Create(source.GetScheme(), AssetsFolder);
			return root.has_value() && path.IsUnder(*root) && path != *root;
		}

	}

	ImportContext::ImportContext(Specification specification)
		: m_Specification(std::move(specification))
	{
	}

	Result<Buffer> ImportContext::ReadDependency(const VfsPath& path)
	{
		if (!Utils::IsInsideAssetRoot(m_Specification.SourcePath, path))
		{
			const bool isEngine = m_Specification.SourcePath.GetScheme() == Utils::EngineScheme;
			return std::unexpected(Error(ErrorCode::InvalidArgument,
				std::format("'{}' cannot read '{}': an import reads only files {}", m_Specification.SourcePath.ToString(), path.ToString(),
					isEngine ? "under engine://" : std::format("under {}://{}", m_Specification.SourcePath.GetScheme(), Utils::AssetsFolder)))
					.WithHint("keep the files an asset references inside the project's Assets folder"));
		}

		Result<Buffer> bytes = m_Specification.Vfs->ReadFile(path);
		if (!bytes.has_value())
			return std::unexpected(std::move(bytes).error().WithContext(std::format("referenced by '{}'", m_Specification.SourcePath.ToString())));

		const uint64_t hash = XXH64(*bytes);
		const auto existing = std::ranges::find_if(m_Reads, [&path](const ImportDependencyRead& read)
		{
			return read.Path == path;
		});
		if (existing != m_Reads.end())
			existing->Hash = hash;
		else
			m_Reads.push_back({ .Path = path, .Hash = hash });
		return bytes;
	}

	std::optional<ImportAssetLookupEntry> ImportContext::FindAsset(const VfsPath& path)
	{
		const std::span<const ImportAssetLookupEntry> assets = m_Specification.Assets;
		const auto found = std::ranges::lower_bound(assets, path, std::less<>(), &ImportAssetLookupEntry::SourcePath);
		const ImportAssetLookupEntry* entry = found != assets.end() && found->SourcePath == path ? &*found : nullptr;

		auto existing = std::ranges::find_if(m_Lookups, [&path](const ImportAssetLookup& lookup)
		{
			return lookup.Path == path;
		});
		if (existing == m_Lookups.end())
		{
			m_Lookups.push_back({ .Path = path, .Found = std::nullopt });
			existing = std::prev(m_Lookups.end());
		}
		if (entry == nullptr)
		{
			existing->Found.reset();
			return std::nullopt;
		}
		existing->Found.emplace(*entry);
		return *entry;
	}

	std::vector<ImportDependencyRead> ImportContext::GetDependencyReads() const
	{
		std::vector<ImportDependencyRead> reads = m_Reads;
		std::ranges::sort(reads, std::less<>(), &ImportDependencyRead::Path);
		return reads;
	}

	std::vector<ImportAssetLookup> ImportContext::GetLookups() const
	{
		std::vector<ImportAssetLookup> lookups = m_Lookups;
		std::ranges::sort(lookups, std::less<>(), &ImportAssetLookup::Path);
		return lookups;
	}

	Result<std::vector<VfsPath>> IAssetImporter::ListDependencyFiles(std::span<const std::byte> /*source*/, const VfsPath& /*sourcePath*/) const
	{
		return std::vector<VfsPath>();
	}

	bool IAssetImporter::CanImport(std::string_view extension) const
	{
		if (extension.empty())
			return false;
		return std::ranges::any_of(GetExtensions(), [extension](std::string_view candidate)
		{
			return Utils::EqualsIgnoringAsciiCase(candidate, extension);
		});
	}

}
