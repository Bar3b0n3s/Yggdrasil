#include "EnginePCH.h"
#include "Engine/Core/FileSystem.h"

// M1 contract stub (Roadmap rule 3): stream C implements host file access with std::error_code overloads and the
// atomic write protocol.

namespace Engine {

	Result<Buffer> FileSystem::ReadFile(const std::filesystem::path& /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "FileSystem::ReadFile is an M1 contract stub");
	}

	Result<std::string> FileSystem::ReadText(const std::filesystem::path& /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "FileSystem::ReadText is an M1 contract stub");
	}

	Status FileSystem::WriteFileAtomic(const std::filesystem::path& /*path*/, std::span<const std::byte> /*data*/,
		const AtomicWriteOptions& /*options*/)
	{
		return MakeError(ErrorCode::Unsupported, "FileSystem::WriteFileAtomic is an M1 contract stub");
	}

	bool FileSystem::Exists(const std::filesystem::path& /*path*/)
	{
		return false;
	}

	Result<FileInfo> FileSystem::GetInfo(const std::filesystem::path& /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "FileSystem::GetInfo is an M1 contract stub");
	}

	Status FileSystem::CreateDirectories(const std::filesystem::path& /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "FileSystem::CreateDirectories is an M1 contract stub");
	}

	Status FileSystem::Remove(const std::filesystem::path& /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "FileSystem::Remove is an M1 contract stub");
	}

	Status FileSystem::Move(const std::filesystem::path& /*from*/, const std::filesystem::path& /*to*/)
	{
		return MakeError(ErrorCode::Unsupported, "FileSystem::Move is an M1 contract stub");
	}

	Result<std::vector<std::filesystem::path>> FileSystem::ListDirectory(const std::filesystem::path& /*directory*/,
		bool /*recursive*/)
	{
		return MakeError(ErrorCode::Unsupported, "FileSystem::ListDirectory is an M1 contract stub");
	}

	Status FileSystem::VerifyCase(const std::filesystem::path& /*root*/, std::string_view /*relativePath*/)
	{
		return MakeError(ErrorCode::Unsupported, "FileSystem::VerifyCase is an M1 contract stub");
	}

}
