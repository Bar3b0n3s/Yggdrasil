#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/PakFormat.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// Reading paks (Architecture §14.1; the layout is PakFormat.h). Exported games open Data/Engine.pak and Data/Game.pak with
// it (RuntimeApp, M7), mount their plain files with PakMount and serve their cooked assets with RuntimeAssetManager.

namespace Engine {

	// One open pak. Immutable after Open and shared (Ref<const PakReader>) between the PakMount and the RuntimeAssetManager
	// that use it; every member is thread-safe, so entries are read from jobs.
	class PakReader
	{
	public:
		// Restricts construction to the factories; CreateRef still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class PakReader;
		};

		// Use Open or OpenMemory.
		explicit PakReader(ConstructionKey key);
		~PakReader();

		PakReader(const PakReader&) = delete;
		PakReader& operator=(const PakReader&) = delete;

		// Opens the pak file at `path` (native, UTF-8): reads the header and the TOC and verifies the TOC hash (§14.1). Entry
		// bytes are read on demand from the file, which stays open read-only while the reader lives (a mounted pak never
		// changes). Errors: NotFound or Io from the file system; Parse, Validation or UnsupportedVersion from ReadPakHeader
		// and ParsePakToc; Validation "TOC hash mismatch: the pak is corrupted" (RuntimeApp maps every error to
		// ExitCode::InitFailed, §14.1).
		[[nodiscard]] static Result<Ref<const PakReader>> Open(const std::filesystem::path& path);

		// The same over a pak held in memory (tests, and verifying a pak the exporter just wrote). `name` names it in errors.
		[[nodiscard]] static Result<Ref<const PakReader>> OpenMemory(Buffer bytes, std::string name);

		// The file path or the memory name.
		[[nodiscard]] const std::string& GetName() const;
		[[nodiscard]] const PakHeader& GetHeader() const;
		// The TOC's entries, sorted by (Handle, Path).
		[[nodiscard]] std::span<const PakEntry> GetEntries() const;
		// The TOC's "Metadata" object.
		[[nodiscard]] const VariantValue& GetMetadata() const;

		// The cooked-asset entry of `handle` (binary search), or nullptr; a null handle finds nothing.
		[[nodiscard]] const PakEntry* FindByHandle(AssetHandle handle) const;
		// The entry whose Path is exactly `path`, or nullptr.
		[[nodiscard]] const PakEntry* FindByPath(std::string_view path) const;

		// The bytes of `entry` (one of GetEntries()). Its XXH64 is verified on the first read of the entry in every
		// configuration (§14.1, so script bytecode is always verified before luau_load), and the reader remembers entries
		// that passed. Errors: Io; Validation "entry '<Path>' is corrupted (hash mismatch)", which the RuntimeAssetManager
		// reports as an asset diagnostic.
		[[nodiscard]] Result<Buffer> ReadEntry(const PakEntry& entry) const;
	private:
		// The header, the TOC, the source (an open file and its lock, or the bytes) and the verified-entry set (PakReader.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
