#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <string>
#include <string_view>

// The VFS bridge of the audio module (Architecture §10.1: "a custom ma_vfs backed by the engine VFS, so loose files in the
// editor and Game.pak entries play through identical code"). The AudioEngine's resource manager opens every file through
// one AudioVfs, whose miniaudio callbacks (onOpen, onRead, onSeek, onTell, onInfo, onClose) live in Audio/Private and
// forward to Open below; this header is the first-party side, so the name rule is tested without miniaudio.
//
// Names. miniaudio's resource manager finds registered data by name (ma_resource_manager_register_decoded_data, used for
// the clips the AudioEngine decodes at registration and for synthesized PCM), but streams (MA_SOUND_FLAG_STREAM) always
// read through the VFS. A streamed clip's bytes are therefore added here as a memory file under its resource name
// (MakeAudioResourceName in Audio/AudioEngine.h: the clip's AssetHandle as 16 lowercase hex digits, '@', its version), so
// streamed and decoded clips use the same bytes, from a loose file's cooked cache entry in the editor or from Game.pak in
// an exported game, and a hot-reloaded version is a file of its own while streams of the old one keep reading theirs.
// Every other name is an engine VFS path ("project://Assets/Music.ogg", "engine://..."), opened through
// VirtualFileSystem::Open, whose streams read a snapshot (IFileStream). Frozen by the M12 contract
// (Docs/Decisions/0015-m12-decisions.md).

namespace Engine {

	// Thread safety: AddMemoryFile and RemoveMemoryFile on the main thread; Open, HasMemoryFile and GetMemoryFileCount from
	// any thread (miniaudio's resource-manager job thread opens streams in windowed runs), guarded by an internal mutex. A
	// stream returned by Open is used by one thread at a time (IFileStream) and stays valid after its memory file was removed
	// (it shares the bytes). Not copyable or movable.
	class AudioVfs
	{
	public:
		// `vfs` is a documented back-reference that outlives this object (the EngineContext's VFS, §4.1).
		explicit AudioVfs(const VirtualFileSystem& vfs);
		~AudioVfs();

		AudioVfs(const AudioVfs&) = delete;
		AudioVfs& operator=(const AudioVfs&) = delete;

		// Serves `bytes` (non-null, asserted; shared, never copied) under `name` until RemoveMemoryFile. Errors:
		// InvalidArgument for an empty name or one containing "://" (that form is reserved for VFS paths); AlreadyExists for a
		// name that is already a memory file.
		[[nodiscard]] Status AddMemoryFile(std::string name, Ref<const Buffer> bytes);

		// Stops serving `name`; open streams keep reading their bytes. Errors: NotFound.
		[[nodiscard]] Status RemoveMemoryFile(std::string_view name);

		[[nodiscard]] bool HasMemoryFile(std::string_view name) const;
		[[nodiscard]] size_t GetMemoryFileCount() const;

		// A stream over `name`: the memory file of that name, else the VFS path it spells (VfsPath::Parse). Errors: NotFound for
		// a name that is neither a memory file nor an existing VFS file; InvalidArgument for a name that is not a memory file
		// and not a valid VFS path; the VFS's errors (Validation for a case mismatch, Io).
		[[nodiscard]] Result<Scope<IFileStream>> Open(std::string_view name) const;
	private:
		// The VFS back-reference, the memory files by name and their mutex (AudioVfs.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
