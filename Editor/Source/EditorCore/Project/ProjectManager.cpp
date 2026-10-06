#include "EditorPCH.h"
#include "EditorCore/Project/ProjectManager.h"

#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"

// M4 contract stub (Roadmap rule 3): stream A (commands, editor state and projects) implements project creation, opening
// and the recent list. LoadedProject's constructor and accessors are real.

namespace Engine {

	std::string_view ProjectTemplateToString(ProjectTemplate /*projectTemplate*/)
	{
		ENGINE_CONTRACT_STUB();
		return "Empty";
	}

	std::optional<ProjectTemplate> ProjectTemplateFromString(std::string_view /*text*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	LoadedProject::LoadedProject(ConstructionKey /*key*/, std::filesystem::path projectFile, ProjectSettings settings, ProjectLoadReport report,
		std::optional<ProjectLock> lock, std::filesystem::path cacheDirectory)
		: m_ProjectFile(std::move(projectFile)), m_Settings(std::move(settings)), m_LoadReport(std::move(report)), m_Lock(std::move(lock)), m_CacheDirectory(std::move(cacheDirectory))
	{
	}

	LoadedProject::~LoadedProject() = default;

	Result<CreatedProject> ProjectManager::CreateProject(const ProjectCreateSpecification& /*specification*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectManager::CreateProject is an M4 contract stub");
	}

	Result<std::filesystem::path> ProjectManager::FindProjectFile(const std::filesystem::path& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectManager::FindProjectFile is an M4 contract stub");
	}

	Result<Scope<LoadedProject>> ProjectManager::OpenProject(const std::filesystem::path& /*path*/, const ProjectOpenOptions& /*options*/,
		const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectManager::OpenProject is an M4 contract stub");
	}

	Result<std::vector<std::filesystem::path>> ProjectManager::ReadRecentProjects(const VirtualFileSystem& /*vfs*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectManager::ReadRecentProjects is an M4 contract stub");
	}

	Status ProjectManager::AddRecentProject(VirtualFileSystem& /*vfs*/, const std::filesystem::path& /*projectFile*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectManager::AddRecentProject is an M4 contract stub");
	}

}
