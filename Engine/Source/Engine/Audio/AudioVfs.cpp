#include "EnginePCH.h"
#include "Engine/Audio/AudioVfs.h"

namespace Engine {

	struct AudioVfs::State
	{
		const VirtualFileSystem* Vfs = nullptr; // documented back-reference
	};

	AudioVfs::AudioVfs(const VirtualFileSystem& vfs)
		: m_State(CreateScope<State>())
	{
		m_State->Vfs = &vfs;
	}

	AudioVfs::~AudioVfs() = default;

	Status AudioVfs::AddMemoryFile(std::string /*name*/, Ref<const Buffer> /*bytes*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the audio VFS is not implemented yet (M12 stream A)");
	}

	Status AudioVfs::RemoveMemoryFile(std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the audio VFS is not implemented yet (M12 stream A)");
	}

	bool AudioVfs::HasMemoryFile(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	size_t AudioVfs::GetMemoryFileCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Result<Scope<IFileStream>> AudioVfs::Open(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the audio VFS is not implemented yet (M12 stream A)");
	}

}
