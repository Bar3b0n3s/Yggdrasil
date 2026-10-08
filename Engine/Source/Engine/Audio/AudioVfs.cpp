#include "EnginePCH.h"
#include "Engine/Audio/AudioVfs.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/VfsPath.h"

#include <cstring>
#include <map>
#include <mutex>
#include <utility>

namespace Engine {

	struct AudioVfs::State
	{
		const VirtualFileSystem* Vfs = nullptr; // documented back-reference
		mutable std::mutex Mutex;               // guards MemoryFiles
		std::map<std::string, Ref<const Buffer>, std::less<>> MemoryFiles;
	};

	namespace {

		// A stream over a memory file's shared bytes: it keeps them alive, so it stays valid after the file was removed.
		class SharedBufferStream final : public IFileStream
		{
		public:
			explicit SharedBufferStream(Ref<const Buffer> bytes)
				: m_Bytes(std::move(bytes))
			{
			}

			[[nodiscard]] Result<size_t> Read(std::span<std::byte> destination) override
			{
				const auto position = static_cast<size_t>(m_Position);
				const size_t count = std::min(destination.size(), m_Bytes->size() - position);
				if (count != 0)
					std::memcpy(destination.data(), m_Bytes->data() + position, count);
				m_Position += count;
				return count;
			}

			[[nodiscard]] Status Seek(uint64_t position) override
			{
				if (position > m_Bytes->size())
					return MakeError(ErrorCode::InvalidArgument, "cannot seek to offset {}: the file has {} bytes", position, m_Bytes->size());
				m_Position = position;
				return {};
			}

			[[nodiscard]] uint64_t GetPosition() const override { return m_Position; }
			[[nodiscard]] uint64_t GetSize() const override { return m_Bytes->size(); }
		private:
			Ref<const Buffer> m_Bytes;
			uint64_t m_Position = 0;
		};

	}

	AudioVfs::AudioVfs(const VirtualFileSystem& vfs)
		: m_State(CreateScope<State>())
	{
		m_State->Vfs = &vfs;
	}

	AudioVfs::~AudioVfs() = default;

	Status AudioVfs::AddMemoryFile(std::string name, Ref<const Buffer> bytes)
	{
		ENGINE_CORE_ASSERT(bytes != nullptr, "AudioVfs::AddMemoryFile needs bytes");
		if (name.empty())
			return MakeError(ErrorCode::InvalidArgument, "an audio memory file needs a name");
		if (name.contains("://"))
			return MakeError(ErrorCode::InvalidArgument, "'{}' cannot name an audio memory file: names with \"://\" are VFS paths", name);
		if (bytes == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the audio memory file '{}' has no bytes", name);

		const std::lock_guard lock(m_State->Mutex);
		if (m_State->MemoryFiles.contains(name))
			return MakeError(ErrorCode::AlreadyExists, "the audio memory file '{}' already exists", name);
		m_State->MemoryFiles.emplace(std::move(name), std::move(bytes));
		return {};
	}

	Status AudioVfs::RemoveMemoryFile(std::string_view name)
	{
		Ref<const Buffer> removed;
		{
			const std::lock_guard lock(m_State->Mutex);
			const auto found = m_State->MemoryFiles.find(name);
			if (found == m_State->MemoryFiles.end())
				return MakeError(ErrorCode::NotFound, "there is no audio memory file '{}'", name);
			removed = std::move(found->second);
			m_State->MemoryFiles.erase(found);
		}
		return {};
	}

	bool AudioVfs::HasMemoryFile(std::string_view name) const
	{
		const std::lock_guard lock(m_State->Mutex);
		return m_State->MemoryFiles.contains(name);
	}

	size_t AudioVfs::GetMemoryFileCount() const
	{
		const std::lock_guard lock(m_State->Mutex);
		return m_State->MemoryFiles.size();
	}

	Result<Scope<IFileStream>> AudioVfs::Open(std::string_view name) const
	{
		{
			const std::lock_guard lock(m_State->Mutex);
			if (const auto found = m_State->MemoryFiles.find(name); found != m_State->MemoryFiles.end())
				return Scope<IFileStream>(CreateScope<SharedBufferStream>(found->second));
		}

		if (!name.contains("://"))
			return MakeError(ErrorCode::InvalidArgument, "'{}' is neither an audio memory file nor a VFS path", name);
		Result<VfsPath> path = VfsPath::Parse(name);
		if (!path.has_value())
		{
			return MakeError(ErrorCode::InvalidArgument, "'{}' is neither an audio memory file nor a valid VFS path: {}", name,
				path.error().ToString());
		}
		// The VFS's own errors (NotFound, Validation for a case mismatch, Io) pass through unchanged.
		return m_State->Vfs->Open(*path);
	}

}
