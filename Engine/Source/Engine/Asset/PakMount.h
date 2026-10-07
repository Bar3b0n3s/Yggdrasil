#pragma once

#include "Engine/Asset/PakReader.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <span>
#include <vector>

namespace Engine {

	// A read-only VFS mount over a pak's plain-file entries (Architecture §4.10: "PakMount (read-only)"): exported games
	// mount Engine.pak as engine:// (its SPIR-V under Shaders/, M7) and Game.pak as project://. Each "File" entry is a file
	// at its Path; directories are implied by the paths. Cooked assets are not files of the mount: RuntimeAssetManager reads
	// them by handle. Follows every IMount rule: the case policy (a path matching an entry only ignoring ASCII case is
	// Validation "case mismatch"), sorted listings, and PermissionDenied for every mutating call (GetAccess is ReadOnly).
	// Streams read from the reader, which never changes while mounted (IFileStream comment). Thread-safe.
	class PakMount final : public IMount
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class PakMount;
		};

		// Use Create.
		PakMount(ConstructionKey key, Ref<const PakReader> pak);
		~PakMount() override;

		// A mount over `pak` (non-null, asserted). Errors: Validation when two plain-file paths differ only in ASCII case, or a
		// file path is also a directory of another path.
		[[nodiscard]] static Result<Scope<PakMount>> Create(Ref<const PakReader> pak);

		[[nodiscard]] Result<Buffer> ReadFile(const VfsPath& path) const override;
		[[nodiscard]] Result<Scope<IFileStream>> Open(const VfsPath& path) const override;
		[[nodiscard]] Status WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data) override;
		[[nodiscard]] Result<FileInfo> GetInfo(const VfsPath& path) const override;
		[[nodiscard]] Result<std::vector<VfsEntry>> List(const VfsPath& directory, bool recursive) const override;
		[[nodiscard]] Status CreateDirectories(const VfsPath& directory) override;
		[[nodiscard]] Status Remove(const VfsPath& path) override;
		[[nodiscard]] Status Move(const VfsPath& from, const VfsPath& to) override;
		[[nodiscard]] MountAccess GetAccess() const override { return MountAccess::ReadOnly; }
	private:
		// The reader and the path index of its plain files and implied directories (PakMount.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
