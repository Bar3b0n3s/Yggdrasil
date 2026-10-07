#include "EditorPCH.h"
#include "EditorCore/Automation/ProjectMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/Automation/SceneMethods.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		constexpr std::string_view SettingsLabel = "Set Project Settings";
		constexpr std::string_view LibraryPrefix = "Library/";

		// The host path of a project param (native, absolute or relative to the working directory: the one exception to
		// project:// confinement, §13.2). Errors: InvalidArgument at `pointer` for an empty path.
		static Result<std::filesystem::path> ResolveNativeProjectPath(std::string_view path, std::string_view pointer)
		{
			if (path.empty())
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, "the project path must not be empty",
					"give the project's directory or .eproj file, such as \"Projects/Tetris\""));
			}
			std::error_code error;
			const std::filesystem::path absolute = std::filesystem::absolute(FileSystem::PathFromUtf8(path), error);
			if (error)
				return MakeError(ErrorCode::Io, "cannot resolve '{}' against the working directory: {}", path, error.message());
			return absolute.lexically_normal();
		}

		static Status RequireNoProject(const EditorContext& editor)
		{
			if (!editor.HasProject())
				return {};
			return std::unexpected(Error(ErrorCode::InvalidState, std::format("a project is already open ('{}'): one project per editor", FileSystem::PathToUtf8(editor.GetProject().GetProjectFile())))
					.WithHint("start another editor for another project"));
		}

		// Records the files CreateProject wrote (§13.4: the .eproj and Assets/) with the current request's attribution, by
		// writing Automation/Provenance.json before the project opens (EditorContext::OpenProject then loads it): the files
		// were written before project:// existed, so they never passed EditorContext::WriteProjectFile.
		static Status RecordCreatedFiles(const CreatedProject& created, const WriteAttribution& attribution)
		{
			ProvenanceRecorder recorder;
			for (const CreatedProjectFile& file : created.RecordedFiles)
			{
				if (ProvenanceRecorder::IsRecordedPath(file.Path))
					recorder.Record(file.Path, file.Hash, attribution);
			}
			const std::filesystem::path root = created.ProjectFile.parent_path();
			const std::filesystem::path provenance = root / FileSystem::PathFromUtf8(ProvenanceRecorder::FilePath);
			ENGINE_TRY(FileSystem::CreateDirectories(provenance.parent_path()));
			const std::string text = ProvenanceRecorder::ToText(recorder.GetEntries());
			return FileSystem::WriteFileAtomic(provenance, std::as_bytes(std::span(text.data(), text.size())), AtomicWriteOptions{ .KeepBackup = false });
		}

		// The files under `root` (project-relative, sorted), except Library/.
		static Result<std::vector<std::string>> ListProjectFiles(const std::filesystem::path& root)
		{
			ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> entries, FileSystem::ListDirectory(root, true));
			std::vector<std::string> files;
			for (const std::filesystem::path& entry : entries)
			{
				const Result<FileInfo> info = FileSystem::GetInfo(entry);
				if (!info || info->IsDirectory)
					continue;
				std::string relative = FileSystem::PathToUtf8(entry.lexically_relative(root));
				if (!relative.starts_with(LibraryPrefix))
					files.push_back(std::move(relative));
			}
			std::sort(files.begin(), files.end());
			return files;
		}

		// The canonical bytes of one project file after migration (§6), or nullopt when it has no canonical form here (other
		// file types). Errors: the file's load errors (Parse, Validation, UnsupportedVersion).
		static Result<std::optional<std::string>> MakeCanonicalFile(const EditorContext& editor, const VfsPath& path, std::string_view text)
		{
			const TypeRegistry& registry = editor.GetTypeRegistry();
			const std::string source(path.GetPath());
			const std::string_view extension = path.GetExtension();
			if (extension == ProjectManager::ProjectFileExtension)
			{
				ProjectLoadOptions options;
				options.SourcePath = source;
				ProjectLoadReport report;
				ENGINE_TRY_ASSIGN(const ProjectSettings settings, ProjectSerializer::LoadFromString(text, registry, options, report));
				ENGINE_TRY_ASSIGN(std::string canonical, ProjectSerializer::SaveToString(settings, registry));
				return std::optional<std::string>(std::move(canonical));
			}
			if (extension == ".scene")
			{
				UUIDGenerator scratchIds = UUIDGenerator::CreateDeterministic(0);
				SceneSpecification specification;
				specification.Name = std::string(path.GetStem());
				specification.Registry = &registry;
				specification.IdGenerator = &scratchIds;
				const Scope<Scene> scene = Scene::Create(specification);
				LoadOptions options;
				options.SourcePath = source;
				LoadReport report;
				ENGINE_TRY(SceneSerializer::LoadFromString(*scene, text, options, report));
				ENGINE_TRY_ASSIGN(std::string canonical, SceneSerializer::SaveToString(*scene));
				return std::optional<std::string>(std::move(canonical));
			}
			if (extension == ".prefab")
			{
				LoadOptions options;
				options.SourcePath = source;
				LoadReport report;
				ENGINE_TRY_ASSIGN(const Prefab prefab, Prefab::LoadFromString(text, registry, options, report));
				ENGINE_TRY_ASSIGN(std::string canonical, prefab.SaveToString());
				return std::optional<std::string>(std::move(canonical));
			}
			return std::optional<std::string>();
		}

		// The project files project.upgrade rewrites: the .eproj, then every .scene and .prefab under Assets/, sorted.
		static Result<std::vector<VfsPath>> ListUpgradeFiles(const EditorContext& editor)
		{
			std::vector<VfsPath> files;
			ENGINE_TRY_ASSIGN(VfsPath projectFile, VfsPath::Create("project", FileSystem::PathToUtf8(editor.GetProject().GetProjectFile().filename())));
			files.push_back(std::move(projectFile));

			ENGINE_TRY_ASSIGN(const VfsPath assets, VfsPath::Create("project", "Assets"));
			const Result<std::vector<VfsEntry>> entries = editor.GetVfs().List(assets, true);
			if (!entries)
			{
				if (entries.error().GetCode() == ErrorCode::NotFound)
					return files;
				return std::unexpected(ToEditorFileError(entries.error()));
			}
			for (const VfsEntry& entry : *entries)
			{
				const std::string_view extension = entry.Path.GetExtension();
				if (!entry.Info.IsDirectory && (extension == ".scene" || extension == ".prefab"))
					files.push_back(entry.Path);
			}
			return files;
		}

	}

	namespace Automation {

		Result<ProjectCreateResult> ProjectCreate(EditorMethodContext& context, const ProjectCreateParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY(Utils::RequireNoProject(editor));
			ENGINE_TRY_ASSIGN(const std::filesystem::path directory, Utils::ResolveNativeProjectPath(params.Path, "/path"));

			ProjectCreateSpecification specification;
			specification.Directory = directory;
			specification.Name = params.Name;
			specification.Template = params.Template;
			specification.TemplatesDirectory = editor.GetSpecification().TemplatesDirectory;
			ENGINE_TRY_ASSIGN(const CreatedProject created, ProjectManager::CreateProject(specification, editor.GetTypeRegistry()));
			ENGINE_TRY(Utils::RecordCreatedFiles(created, editor.GetWriteAttribution()));
			ENGINE_TRY_ASSIGN(std::vector<std::string> files, Utils::ListProjectFiles(created.ProjectFile.parent_path()));

			ENGINE_TRY_ASSIGN(Scope<LoadedProject> project, ProjectManager::OpenProject(created.ProjectFile, {}, editor.GetTypeRegistry()));
			ENGINE_TRY(editor.OpenProject(std::move(project)));
			ENGINE_INFO("Created and opened the project '{}' at '{}'", params.Name, FileSystem::PathToUtf8(created.ProjectFile));

			ProjectCreateResult result;
			result.Project = Utils::MakeProjectSummary(editor);
			result.CreatedFiles = std::move(files);
			return result;
		}

		Result<ProjectOpenResult> ProjectOpen(EditorMethodContext& context, const ProjectOpenParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY(Utils::RequireNoProject(editor));
			ENGINE_TRY_ASSIGN(const std::filesystem::path path, Utils::ResolveNativeProjectPath(params.Path, "/path"));
			ENGINE_TRY_ASSIGN(Scope<LoadedProject> project, ProjectManager::OpenProject(path, {}, editor.GetTypeRegistry()));

			ProjectOpenResult result;
			const std::string projectFile = FileSystem::PathToUtf8(project->GetProjectFile());
			for (const ValidationIssue& issue : project->GetLoadReport().Diagnostics)
			{
				Utils::LogLoadDiagnostic(projectFile, issue.Severity, issue.JsonPointer, issue.Message, issue.Code);
				result.Warnings.push_back(issue.JsonPointer.empty() ? issue.Message : std::format("{}: {}", issue.JsonPointer, issue.Message));
			}
			ENGINE_TRY(editor.OpenProject(std::move(project)));
			result.Project = Utils::MakeProjectSummary(editor);
			return result;
		}

		Result<ProjectSaveResult> ProjectSave(EditorMethodContext& context, const NoParams& /*params*/)
		{
			EditorContext& editor = context.GetEditor();
			ProjectSaveResult result;
			if (editor.HasScene() && editor.IsSceneDirty())
			{
				ENGINE_TRY_ASSIGN(const VfsPath path, Utils::GetOwnScenePath(editor));
				ENGINE_TRY(Utils::SaveOpenScene(editor, path));
				result.SavedFiles.push_back(Utils::ToProjectRelative(path));
			}
			return result;
		}

		Result<ProjectInfoResult> ProjectInfo(EditorMethodContext& context, const NoParams& /*params*/)
		{
			const EditorContext& editor = context.GetEditor();
			ProjectInfoResult result;
			result.Project = Utils::MakeProjectSummary(editor);
			if (editor.HasScene())
			{
				const SceneSummary scene = Utils::MakeSceneSummary(editor);
				result.Scene.Open = true;
				result.Scene.Path = scene.Path;
				result.Scene.Name = scene.Name;
				result.Scene.Revision = scene.Revision;
				result.Scene.Dirty = scene.Dirty;
				result.Scene.EntityCount = scene.EntityCount;
			}
			const ProvenanceRecorder* provenance = editor.GetProvenance();
			result.ProvenanceEntries = provenance != nullptr ? ToAutomationCounter(provenance->GetEntries().size()) : 0;
			return result;
		}

		Result<ProjectGetSettingsResult> ProjectGetSettings(EditorMethodContext& context, const NoParams& /*params*/)
		{
			ProjectGetSettingsResult result;
			result.Settings = context.GetEditor().GetProject().GetSettings();
			return result;
		}

		Result<ProjectSetSettingsResult> ProjectSetSettings(EditorMethodContext& context, const ProjectSetSettingsParams& params)
		{
			EditorContext& editor = context.GetEditor();
			if (!params.Patch.Get().is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/patch", "the patch must be an object (an RFC 7386 merge patch)",
					"for example {\"Window\": {\"Title\": \"Tetris\"}}"));
			}

			// Enum spellings are case-insensitive in automation (§13.4); the settings reader is case-sensitive like every reader
			// of authored files (§6), so the patch is spelled canonically first.
			Json patch = params.Patch.Get();
			const StructInfo* settingsType = editor.GetTypeRegistry().FindStruct<ProjectSettings>();
			ENGINE_ASSERT(settingsType != nullptr, "The project settings types are registered by the engine context");
			Utils::CanonicalizeEnumSpellings(patch, settingsType->GetType());

			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, patch, std::string(Utils::SettingsLabel));
			if (!command)
				return std::unexpected(Utils::PrefixPointers(command.error(), "/patch"));
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(std::move(*command)));

			ProjectSetSettingsResult result;
			result.Settings = editor.GetProject().GetSettings();
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<ProjectValidateResult> ProjectValidate(EditorMethodContext& context, const ProjectValidateParams& params)
		{
			EditorContext& editor = context.GetEditor();
			FixSelection selection;
			bool fixes = false;
			const Json& fix = params.Fix.Get();
			if (fix.is_boolean())
			{
				selection.All = JsonReader(fix).ReadBool().value_or(false);
				fixes = selection.All;
			}
			else if (fix.is_array())
			{
				for (size_t index = 0; index < fix.size(); ++index)
				{
					const Result<std::string> item = JsonReader(fix[index]).ReadString();
					if (!item || item->empty())
					{
						return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/fix/{}", index),
							"each entry of fix must be a diagnostic id or a code"));
					}
					selection.IdsOrCodes.push_back(*item);
				}
				fixes = !selection.IdsOrCodes.empty();
			}
			else if (!fix.is_null())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/fix",
					"fix must be true, false or an array of diagnostic ids and codes", "for example {\"fix\": [\"BUILD_SCENE_MISSING\"]}"));
			}

			ProjectValidateResult result;
			if (!fixes)
			{
				ENGINE_TRY_ASSIGN(ValidationReport report, ProjectValidator::Validate(editor, params.Scope));
				result.Diagnostics = std::move(report.Diagnostics);
				result.ErrorCount = report.ErrorCount;
				result.WarningCount = report.WarningCount;
				return result;
			}

			ENGINE_TRY_ASSIGN(FixReport fixed, ProjectValidator::Fix(editor, params.Scope, selection));
			result.Diagnostics = std::move(fixed.After.Diagnostics);
			result.Fixed = std::move(fixed.Fixed);
			result.ErrorCount = fixed.After.ErrorCount;
			result.WarningCount = fixed.After.WarningCount;
			result.UndoIndex = ToAutomationCounter(fixed.UndoIndex);
			return result;
		}

		Result<ProjectUpgradeResult> ProjectUpgrade(EditorMethodContext& context, const NoParams& /*params*/)
		{
			EditorContext& editor = context.GetEditor();
			if (editor.HasScene() && editor.IsSceneDirty())
			{
				return std::unexpected(Error(ErrorCode::InvalidState, "the open scene has unsaved changes")
						.WithHint("save them with scene.save (or drop them with scene.open {discardChanges: true}) before project.upgrade"));
			}

			ENGINE_TRY_ASSIGN(const std::vector<VfsPath> files, Utils::ListUpgradeFiles(editor));
			ProjectUpgradeResult result;
			bool openSceneChanged = false;
			for (const VfsPath& file : files)
			{
				const std::string relative = Utils::ToProjectRelative(file);
				context.SetPhase(std::format("Automation:project.upgrade {}", relative));
				Result<std::string> text = editor.GetVfs().ReadText(file);
				if (!text)
					return std::unexpected(Utils::ToEditorFileError(std::move(text).error()).WithContext(std::format("while upgrading '{}'", relative)));
				ENGINE_TRY_ASSIGN(const std::optional<std::string> canonical,
					WithContext(Utils::MakeCanonicalFile(editor, file, *text), std::format("while upgrading '{}'", relative)));
				if (!canonical.has_value() || *canonical == *text)
				{
					++result.UnchangedCount;
					continue;
				}
				ENGINE_TRY(WithContext(editor.WriteProjectFile(file, std::as_bytes(std::span(canonical->data(), canonical->size()))),
					std::format("while upgrading '{}'", relative)));
				result.ChangedFiles.push_back(relative);
				openSceneChanged = openSceneChanged || (editor.GetScenePath().has_value() && *editor.GetScenePath() == file);
			}
			std::sort(result.ChangedFiles.begin(), result.ChangedFiles.end());

			// The open scene's file changed: reload it (a dry run changed nothing on disk, and its sandbox holds the scene).
			if (openSceneChanged && !context.IsDryRun())
			{
				const VfsPath path = *editor.GetScenePath();
				Scope<Scene> scene = editor.CreateScene(std::string(path.GetStem()));
				LoadReport report;
				ENGINE_TRY(Utils::LoadSceneFile(editor, *scene, path, LoadMode::Strict, nullptr, report));
				Utils::LogLoadDiagnostics(Utils::ToProjectRelative(path), report);
				editor.SetScene(std::move(scene), path, false);
			}
			if (!result.ChangedFiles.empty() && !context.IsDryRun())
				ENGINE_INFO("project.upgrade rewrote {} file(s)", result.ChangedFiles.size());
			return result;
		}

		Result<ProjectRefreshAssetsResult> ProjectRefreshAssets(EditorMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::ProjectRefreshAssets is an M6 contract stub");
		}

	}

	void RegisterProjectMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<ProjectTemplate>("ProjectTemplate", "What a new project starts with.")
			.Entry(ProjectTemplate::Empty, "Empty", "The folder skeleton, .luaurc, .gitignore and an AGENTS.md stub; no scene.");

		registry.Struct<ProjectSummary>("ProjectSummary", "The open project.")
			.Field("name", &ProjectSummary::Name, "The project's name (ProjectSettings.Name).")
			.Field("root", &ProjectSummary::Root, "The absolute project directory, with '/' separators.")
			.Field("projectFile", &ProjectSummary::ProjectFile, "The absolute .eproj path, with '/' separators.")
			.Field("readOnly", &ProjectSummary::ReadOnly, "Whether the project was opened read-only (--read-only).");

		registry.Struct<OpenSceneSummary>("OpenSceneSummary", "The open scene, or open: false.")
			.Field("open", &OpenSceneSummary::Open, "Whether a scene is open.")
			.Field("path", &OpenSceneSummary::Path, "The project-relative path of its file; empty for a scene never saved.")
			.Field("name", &OpenSceneSummary::Name, "The scene's name.")
			.Field("revision", &OpenSceneSummary::Revision, "The editor's revision.")
			.Field("dirty", &OpenSceneSummary::Dirty, "Whether the scene has unsaved changes.")
			.Field("entityCount", &OpenSceneSummary::EntityCount, "The number of entities.");

		registry.Struct<ProjectCreateParams>("ProjectCreateParams", "The params of project.create.")
			.Field("path", &ProjectCreateParams::Path, "The new project's directory: native, absolute or relative to the editor's working directory.")
			.Field("name", &ProjectCreateParams::Name, "The game's name: the .eproj file name and ProjectSettings.Name.")
			.Field("template", &ProjectCreateParams::Template, "What the project starts with.");

		registry.Struct<ProjectCreateResult>("ProjectCreateResult", "The created and opened project.")
			.Field("project", &ProjectCreateResult::Project, "The open project.")
			.Field("createdFiles", &ProjectCreateResult::CreatedFiles, "The files created, project-relative and sorted.");

		registry.Struct<ProjectOpenParams>("ProjectOpenParams", "The params of project.open.")
			.Field("path", &ProjectOpenParams::Path, "The .eproj, or the directory holding exactly one: native, absolute or relative.");

		registry.Struct<ProjectOpenResult>("ProjectOpenResult", "The opened project.")
			.Field("project", &ProjectOpenResult::Project, "The open project.")
			.Field("warnings", &ProjectOpenResult::Warnings, "The .eproj's load warnings, also logged.");

		registry.Struct<ProjectSaveResult>("ProjectSaveResult", "What project.save wrote.")
			.Field("savedFiles", &ProjectSaveResult::SavedFiles, "The project-relative files written.");

		registry.Struct<ProjectInfoResult>("ProjectInfoResult", "The open project, its open scene and its provenance.")
			.Field("project", &ProjectInfoResult::Project, "The open project.")
			.Field("scene", &ProjectInfoResult::Scene, "The open scene.")
			.Field("provenanceEntries", &ProjectInfoResult::ProvenanceEntries, "The number of files provenance records (0 when read-only).");

		registry.Struct<ProjectGetSettingsResult>("ProjectGetSettingsResult", "The project settings.")
			.Field("settings", &ProjectGetSettingsResult::Settings, "The settings, as the .eproj holds them.");

		registry.Struct<ProjectSetSettingsParams>("ProjectSetSettingsParams", "The params of project.setSettings.")
			.Field("patch", &ProjectSetSettingsParams::Patch,
				"An RFC 7386 merge patch of the settings: members replace, null resets a field (or deletes an input action), arrays replace.");

		registry.Struct<ProjectSetSettingsResult>("ProjectSetSettingsResult", "The settings after the patch.")
			.Field("settings", &ProjectSetSettingsResult::Settings, "The settings after the patch.")
			.Field("undoIndex", &ProjectSetSettingsResult::UndoIndex, "The command's undo index; 0 in a dry run or a batch.");

		registry.Struct<ProjectValidateParams>("ProjectValidateParams", "The params of project.validate.")
			.Field("scope", &ProjectValidateParams::Scope, "What to check: the whole project or the open scene.")
			.Field("fix", &ProjectValidateParams::Fix,
				"true fixes every auto-fixable diagnostic; an array fixes only the listed diagnostic ids and codes; false or absent fixes nothing.");

		registry.Struct<ProjectValidateResult>("ProjectValidateResult", "The diagnostics, after any fixes.")
			.Field("diagnostics", &ProjectValidateResult::Diagnostics, "The diagnostics, sorted by file, entity and code.")
			.Field("fixed", &ProjectValidateResult::Fixed, "The ids of the diagnostics fixed.")
			.Field("errorCount", &ProjectValidateResult::ErrorCount, "The number of Error diagnostics.")
			.Field("warningCount", &ProjectValidateResult::WarningCount, "The number of Warning diagnostics.")
			.Field("undoIndex", &ProjectValidateResult::UndoIndex, "The undo index of the fixes; 0 when nothing was fixed, in a dry run or a batch.");

		registry.Struct<ProjectUpgradeResult>("ProjectUpgradeResult", "What project.upgrade rewrote.")
			.Field("changedFiles", &ProjectUpgradeResult::ChangedFiles, "The files rewritten (or that a dry run would rewrite), project-relative and sorted.")
			.Field("unchangedCount", &ProjectUpgradeResult::UnchangedCount, "The project files already canonical.");
	}

	void RegisterProjectMethods(MethodRegistry& methods)
	{
		Json createExample = Json::object();
		createExample["path"] = "Projects/Tetris";
		createExample["name"] = "Tetris";
		createExample["template"] = "Empty";
		methods.Add(
			{
				.Name = "project.create",
				.Description = "Creates a project from a template at a native path and opens it, taking its lock. Provenance records the .eproj.",
				.RequiredParams = { "path", "name" },
				.ExposeAsTool = true,
				.Mutates = true,
				.AvailableInLauncher = true,
				.Examples = { { .Description = "Create the Tetris project.", .Params = createExample } },
			},
			&Automation::ProjectCreate);

		Json openExample = Json::object();
		openExample["path"] = "Projects/Tetris";
		methods.Add(
			{
				.Name = "project.open",
				.Description = "Opens the project a native path names (its .eproj, or its directory), taking its lock, and reports the .eproj's "
							   "load warnings.",
				.RequiredParams = { "path" },
				.ExposeAsTool = true,
				.AvailableInLauncher = true,
				.Examples = { { .Description = "Open the Tetris project.", .Params = openExample } },
			},
			&Automation::ProjectOpen);

		methods.Add(
			{
				.Name = "project.save",
				.Description = "Saves the open scene when it has unsaved changes. Settings are saved as they change.",
				.ExposeAsTool = true,
				.Mutates = true,
				.Examples = { { .Description = "Save everything unsaved.", .Params = Json::object() } },
			},
			&Automation::ProjectSave);

		methods.Add(
			{
				.Name = "project.info",
				.Description = "Reports the open project, its open scene (path, revision, unsaved changes, entity count) and how many files "
							   "provenance records.",
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the project's state.", .Params = Json::object() } },
			},
			&Automation::ProjectInfo);

		methods.Add(
			{
				.Name = "project.getSettings",
				.Description = "Returns the project settings (window, simulation, physics, input actions, rendering, scripting, export, testing).",
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the settings.", .Params = Json::object() } },
			},
			&Automation::ProjectGetSettings);

		Json setExample = Json::object();
		setExample["patch"] = Json::object();
		setExample["patch"]["Window"] = Json::object();
		setExample["patch"]["Window"]["Title"] = "Tetris";
		setExample["patch"]["StartScene"] = "Assets/Scenes/Main.scene";
		methods.Add(
			{
				.Name = "project.setSettings",
				.Description = "Applies an RFC 7386 merge patch to the project settings as one undoable command, which writes the .eproj.",
				.RequiredParams = { "patch" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Name the window and set the start scene.", .Params = setExample } },
			},
			&Automation::ProjectSetSettings);

		Json validateExample = Json::object();
		validateExample["fix"] = Json::array({ "BUILD_SCENE_MISSING" });
		methods.Add(
			{
				.Name = "project.validate",
				.Description = "Validates the project (or the open scene): diagnostics with stable ids, codes, locations and hints. fix: true "
							   "fixes every auto-fixable one, fix: [ids or codes] only those, as one undoable command.",
				.ExposeAsTool = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Remove the build scenes that do not exist.", .Params = validateExample } },
			},
			&Automation::ProjectValidate);

		methods.Add(
			{
				.Name = "project.upgrade",
				.Description = "Applies migrations and canonical re-saves to every project file (the .eproj, scenes and prefabs), records each "
							   "rewrite in provenance and reports the changed files. Not undoable; needs a saved open scene.",
				.Mutates = true,
				.SupportsDryRun = true,
				.TimeoutSeconds = 600,
				.Examples = { { .Description = "Rewrite every project file in the current format.", .Params = Json::object() } },
			},
			&Automation::ProjectUpgrade);
	}

}
