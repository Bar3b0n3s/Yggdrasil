#include "EditorPCH.h"
#include "EditorCore/Automation/RecoveryMethods.h"

#include "EditorCore/Autosave/Private/AutosaveRecoveryData.h"
#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/FileSystem.h"

namespace Engine {

	Result<ProjectOpenResult> OpenProjectWithRecovery(EditorMethodContext& context, const ProjectOpenParams& params)
	{
		EditorContext& editor = context.GetEditor();
		if (editor.IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "read-only projects cannot adopt recoveries");
		if (editor.HasProject() || editor.IsDryRun())
			return MakeError(ErrorCode::InvalidState, "project.open recovery requires the launcher outside a dry run");
		if (params.Path.empty())
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/path", "the project path must not be empty"));
		std::error_code error;
		const std::filesystem::path path = std::filesystem::absolute(FileSystem::PathFromUtf8(params.Path), error);
		if (error)
			return MakeError(ErrorCode::Io, "cannot resolve project path: {}", error.message());
		ENGINE_TRY_ASSIGN(Scope<LoadedProject> project, ProjectManager::OpenProject(path.lexically_normal(), {}, editor.GetTypeRegistry()));
		ENGINE_TRY_ASSIGN(const auto recovery, Utils::ReadAutosaveRecovery(*project));
		Autosave* service = context.GetServer().GetSpecification().AutosaveService;
		if (recovery && service == nullptr)
			return MakeError(ErrorCode::Unsupported, "project.open recovery needs the host's long-lived autosave service");
		ProjectOpenResult result;
		for (const ValidationIssue& issue : project->GetLoadReport().Diagnostics)
		{
			Utils::LogLoadDiagnostic(FileSystem::PathToUtf8(project->GetProjectFile()), issue.Severity, issue.JsonPointer, issue.Message, issue.Code);
			result.Warnings.push_back(issue.JsonPointer.empty() ? issue.Message : std::format("{}: {}", issue.JsonPointer, issue.Message));
		}
		if (recovery)
		{
			ENGINE_TRY(service->OpenRecoveredProject(std::move(project), recovery->Info));
		}
		else
		{
			ENGINE_TRY(editor.OpenProject(std::move(project)));
		}
		result.Project = Utils::MakeProjectSummary(editor);
		result.RecoveryAvailable = recovery.has_value();
		result.Recovered = recovery.has_value();
		return result;
	}

}
