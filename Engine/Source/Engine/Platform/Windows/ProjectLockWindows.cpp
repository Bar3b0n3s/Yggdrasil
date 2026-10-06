#include "EnginePCH.h"
#include "Engine/Platform/ProjectLock.h"

// M2 contract stub (Roadmap rule 3): stream B (process, crash handler, paths, project lock) implements the lock on
// Windows (LockFileEx on a byte past the pid text). Until then Acquire and IsHeld fail with Unsupported, so no
// ProjectLock exists.

#if defined(ENGINE_PLATFORM_WINDOWS)

namespace Engine {

	struct ProjectLock::Impl
	{
	};

	ProjectLock::ProjectLock(Scope<Impl> impl)
		: m_Impl(std::move(impl))
	{
	}

	ProjectLock::~ProjectLock() = default;

	ProjectLock::ProjectLock(ProjectLock&& other) noexcept = default;

	ProjectLock& ProjectLock::operator=(ProjectLock&& other) noexcept = default;

	Result<ProjectLock> ProjectLock::Acquire(const std::filesystem::path& /*lockFile*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectLock::Acquire is not implemented yet");
	}

	Result<bool> ProjectLock::IsHeld(const std::filesystem::path& /*lockFile*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectLock::IsHeld is not implemented yet");
	}

	const std::filesystem::path& ProjectLock::GetPath() const
	{
		ENGINE_CONTRACT_STUB();
		static const std::filesystem::path EmptyPath;
		return EmptyPath;
	}

}

#endif
