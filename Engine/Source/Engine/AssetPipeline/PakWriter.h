#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/PakFormat.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine {

	// One entry to write (PakFormat.h): a cooked asset (Handle, its AssetType name as Type, its readable reference as Path,
	// a complete cooked artifact as Data) or a plain file (null Handle, Type "File", a relative VFS path, the file's bytes).
	struct PakWriterEntry
	{
		AssetHandle Handle{};
		std::string Type{};
		std::string Path{};
		Buffer Data{};
	};

	// Builds a pak (Architecture §14.1, §7.6), used by the exporter (M7) for Engine.pak and Game.pak. Deterministic: the same
	// entries and metadata give byte-identical paks whatever the order of Add ("Pak: writer is deterministic"): entries are
	// laid out sorted by (Handle, Path), each at the next 16-byte boundary with zero padding, followed by the canonical TOC.
	// Not thread-safe (one owner).
	class PakWriter
	{
	public:
		PakWriter() = default;

		// Adds `entry`. Errors: InvalidArgument for a Type that is neither an AssetType name other than None nor "File", a
		// null handle on a cooked asset or a handle on a plain file, an empty path, a plain-file path VfsPath rejects, or
		// cooked-asset Data that ReadCookedArtifact rejects or whose type differs from Type; AlreadyExists for a repeated path
		// or cooked-asset handle.
		[[nodiscard]] Status Add(PakWriterEntry entry);

		// The TOC's "Metadata" (a JSON object; null writes {}). Asserts an object or null.
		void SetMetadata(VariantValue metadata);

		[[nodiscard]] size_t GetEntryCount() const { return m_Entries.size(); }

		// The complete pak.
		[[nodiscard]] Buffer Build() const;

		// Build(), written to `path` atomically (FileSystem::WriteFileAtomic, no backup). Errors: those of the write.
		[[nodiscard]] Status WriteToFile(const std::filesystem::path& path) const;
	private:
		std::vector<PakWriterEntry> m_Entries;
		VariantValue m_Metadata;
	};

}
