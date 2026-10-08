#include "EnginePCH.h"
#include "Engine/Audio/Private/MiniaudioVfs.h"

#include "Engine/Audio/AudioVfs.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"

#include <span>
#include <type_traits>
#include <utility>

namespace Engine {

	static_assert(std::is_standard_layout_v<MiniaudioVfsHandle>, "miniaudio reads the handle as its leading ma_vfs_callbacks");

	namespace Utils {

		// The miniaudio result closest to an AudioVfs error, so a failed open reads sensibly in miniaudio's own messages; every
		// other code (Io and the rest) is an I/O error. An if-chain: a switch over the enum class would have to list every
		// ErrorCode, since CodeStyle §3.8 forbids default:.
		static ma_result ToMiniaudioResult(ErrorCode code)
		{
			if (code == ErrorCode::NotFound || code == ErrorCode::Validation)
				return MA_DOES_NOT_EXIST;
			if (code == ErrorCode::InvalidArgument)
				return MA_INVALID_ARGS;
			if (code == ErrorCode::PermissionDenied)
				return MA_ACCESS_DENIED;
			return MA_IO_ERROR;
		}

	}

	MiniaudioVfs::MiniaudioVfs(const AudioVfs& vfs)
		: m_Vfs(&vfs)
	{
		m_Handle.Callbacks.onOpen = &MiniaudioVfs::OnOpen;
		m_Handle.Callbacks.onOpenW = nullptr; // the resource manager is given UTF-8 names only (miniaudio: MA_NOT_IMPLEMENTED)
		m_Handle.Callbacks.onClose = &MiniaudioVfs::OnClose;
		m_Handle.Callbacks.onRead = &MiniaudioVfs::OnRead;
		m_Handle.Callbacks.onWrite = &MiniaudioVfs::OnWrite;
		m_Handle.Callbacks.onSeek = &MiniaudioVfs::OnSeek;
		m_Handle.Callbacks.onTell = &MiniaudioVfs::OnTell;
		m_Handle.Callbacks.onInfo = &MiniaudioVfs::OnInfo;
		m_Handle.Bridge = this;
	}

	MiniaudioVfs::~MiniaudioVfs()
	{
		// The resource manager closes every file before it goes (the engine uninitializes it first).
		ENGINE_CORE_ASSERT(GetOpenFileCount() == 0, "{} audio files are still open in the miniaudio VFS", GetOpenFileCount());
	}

	size_t MiniaudioVfs::GetOpenFileCount() const
	{
		const std::lock_guard lock(m_FilesMutex);
		return m_Files.size();
	}

	MiniaudioVfs& MiniaudioVfs::FromVfs(ma_vfs* vfs)
	{
		// ma_vfs is void: miniaudio passes back the handle Get() returned.
		const auto* handle = static_cast<MiniaudioVfsHandle*>(vfs);
		ENGINE_CORE_VERIFY(handle != nullptr && handle->Bridge != nullptr, "miniaudio called the audio VFS without its handle");
		return *handle->Bridge;
	}

	IFileStream& MiniaudioVfs::FromFile(ma_vfs_file file)
	{
		// ma_vfs_file is a void*: the stream OnOpen handed out.
		ENGINE_CORE_VERIFY(file != nullptr, "miniaudio used a null audio VFS file");
		return *static_cast<IFileStream*>(file);
	}

	ma_result MiniaudioVfs::OnOpen(ma_vfs* vfs, const char* path, ma_uint32 openMode, ma_vfs_file* file)
	{
		if (file == nullptr || path == nullptr)
			return MA_INVALID_ARGS;
		*file = nullptr;
		if ((openMode & MA_OPEN_MODE_WRITE) != 0)
			return MA_ACCESS_DENIED;

		MiniaudioVfs& bridge = FromVfs(vfs);
		Result<Scope<IFileStream>> stream = bridge.m_Vfs->Open(path);
		if (!stream.has_value())
		{
			ENGINE_CORE_TRACE("The audio VFS cannot open '{}': {}", path, stream.error().ToString());
			return Utils::ToMiniaudioResult(stream.error().GetCode());
		}

		IFileStream* opened = stream->get();
		const std::lock_guard lock(bridge.m_FilesMutex);
		bridge.m_Files.emplace(opened, std::move(*stream));
		*file = opened;
		return MA_SUCCESS;
	}

	ma_result MiniaudioVfs::OnClose(ma_vfs* vfs, ma_vfs_file file)
	{
		MiniaudioVfs& bridge = FromVfs(vfs);
		Scope<IFileStream> closed;
		{
			const std::lock_guard lock(bridge.m_FilesMutex);
			const auto found = bridge.m_Files.find(static_cast<const IFileStream*>(file));
			if (found == bridge.m_Files.end())
				return MA_INVALID_ARGS;
			closed = std::move(found->second);
			bridge.m_Files.erase(found);
		}
		return MA_SUCCESS;
	}

	ma_result MiniaudioVfs::OnRead(ma_vfs* /*vfs*/, ma_vfs_file file, void* destination, size_t size, size_t* bytesRead)
	{
		if (bytesRead != nullptr)
			*bytesRead = 0;
		if (destination == nullptr)
			return MA_INVALID_ARGS;

		const Result<size_t> read = FromFile(file).Read(std::span<std::byte>(static_cast<std::byte*>(destination), size));
		if (!read.has_value())
		{
			ENGINE_CORE_TRACE("Reading an audio VFS file failed: {}", read.error().ToString());
			return MA_IO_ERROR;
		}
		if (bytesRead != nullptr)
			*bytesRead = *read;
		// ma_vfs_read turns a read of no bytes into MA_AT_END.
		return MA_SUCCESS;
	}

	ma_result MiniaudioVfs::OnWrite(ma_vfs* /*vfs*/, ma_vfs_file /*file*/, const void* /*source*/, size_t /*size*/, size_t* bytesWritten)
	{
		if (bytesWritten != nullptr)
			*bytesWritten = 0;
		return MA_ACCESS_DENIED;
	}

	ma_result MiniaudioVfs::OnSeek(ma_vfs* /*vfs*/, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin)
	{
		IFileStream& stream = FromFile(file);
		int64_t base = 0;
		switch (origin)
		{
			case ma_seek_origin_start:   base = 0; break;
			case ma_seek_origin_current: base = static_cast<int64_t>(stream.GetPosition()); break;
			case ma_seek_origin_end:     base = static_cast<int64_t>(stream.GetSize()); break;
			default:                     return MA_INVALID_ARGS;
		}

		const int64_t size = static_cast<int64_t>(stream.GetSize());
		if ((offset < 0 && base < -offset) || (offset > 0 && offset > size - base))
			return MA_BAD_SEEK;
		if (!stream.Seek(static_cast<uint64_t>(base + offset)).has_value())
			return MA_BAD_SEEK;
		return MA_SUCCESS;
	}

	ma_result MiniaudioVfs::OnTell(ma_vfs* /*vfs*/, ma_vfs_file file, ma_int64* cursor)
	{
		if (cursor == nullptr)
			return MA_INVALID_ARGS;
		*cursor = static_cast<ma_int64>(FromFile(file).GetPosition());
		return MA_SUCCESS;
	}

	ma_result MiniaudioVfs::OnInfo(ma_vfs* /*vfs*/, ma_vfs_file file, ma_file_info* info)
	{
		if (info == nullptr)
			return MA_INVALID_ARGS;
		info->sizeInBytes = FromFile(file).GetSize();
		return MA_SUCCESS;
	}

}
