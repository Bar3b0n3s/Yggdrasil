#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <miniaudio.h>

#include <map>
#include <mutex>

// The miniaudio side of the AudioVfs (Architecture §10.1 "a custom ma_vfs backed by the engine VFS";
// Docs/Decisions/0015-m12-decisions.md decision 6): the ma_vfs callbacks the AudioEngine's resource manager opens every
// file through, forwarding to AudioVfs::Open.

namespace Engine {

	class AudioVfs;
	class MiniaudioVfs;

	// The object miniaudio receives as its ma_vfs*: miniaudio reads it as an ma_vfs_callbacks, so the callbacks are the first
	// member of this standard-layout struct, followed by the bridge they belong to.
	struct MiniaudioVfsHandle
	{
		ma_vfs_callbacks Callbacks{};
		MiniaudioVfs* Bridge = nullptr;
	};

	// Read-only files only: an open for writing is refused (MA_ACCESS_DENIED). Each open file is an IFileStream from
	// AudioVfs::Open, owned here until miniaudio closes it, and handed to miniaudio as its ma_vfs_file. Thread safety: the
	// callbacks may run on any thread (the resource manager's job thread in windowed runs); the table of open files is
	// guarded by a mutex, and each file is used by one thread at a time (IFileStream). `vfs` is a documented
	// back-reference that outlives this object. Not copyable or movable: miniaudio keeps the handle's address.
	class MiniaudioVfs
	{
	public:
		explicit MiniaudioVfs(const AudioVfs& vfs);
		~MiniaudioVfs();

		MiniaudioVfs(const MiniaudioVfs&) = delete;
		MiniaudioVfs& operator=(const MiniaudioVfs&) = delete;

		// The pointer for ma_resource_manager_config::pVFS.
		[[nodiscard]] ma_vfs* Get() { return &m_Handle; }

		// Files miniaudio has open (streams and loading files).
		[[nodiscard]] size_t GetOpenFileCount() const;
	private:
		[[nodiscard]] static MiniaudioVfs& FromVfs(ma_vfs* vfs);
		[[nodiscard]] static IFileStream& FromFile(ma_vfs_file file);

		static ma_result OnOpen(ma_vfs* vfs, const char* path, ma_uint32 openMode, ma_vfs_file* file);
		static ma_result OnClose(ma_vfs* vfs, ma_vfs_file file);
		static ma_result OnRead(ma_vfs* vfs, ma_vfs_file file, void* destination, size_t size, size_t* bytesRead);
		static ma_result OnWrite(ma_vfs* vfs, ma_vfs_file file, const void* source, size_t size, size_t* bytesWritten);
		static ma_result OnSeek(ma_vfs* vfs, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin);
		static ma_result OnTell(ma_vfs* vfs, ma_vfs_file file, ma_int64* cursor);
		static ma_result OnInfo(ma_vfs* vfs, ma_vfs_file file, ma_file_info* info);
	private:
		MiniaudioVfsHandle m_Handle{};
		const AudioVfs* m_Vfs = nullptr; // documented back-reference
		mutable std::mutex m_FilesMutex; // guards m_Files
		std::map<const IFileStream*, Scope<IFileStream>> m_Files;
	};

}
