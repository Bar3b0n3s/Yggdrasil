#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "EditorCore/Project/ProjectManager.h"
#include "EditorCore/Project/ProjectValidator.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <vector>

// project.* (Architecture §13.5), the M4 subset: create, open, save, info, getSettings, setSettings, validate, upgrade.
// project.refreshAssets arrives with M6 and project.export with M7. Conventions as in MethodRegistry.h (JSON key = member
// name with a lower-case first letter unless commented).

namespace Engine {

	class AutomationServer;
	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry struct "ProjectSummary".
	struct ProjectSummary
	{
		std::string Name{};        // ProjectSettings::Name
		std::string Root{};        // absolute project directory, '/' separators
		std::string ProjectFile{}; // absolute .eproj path, '/' separators
		bool ReadOnly = false;
	};

	// project.create {path, name, template}: creates the project (ProjectManager::CreateProject, template files from
	// Resources/Templates/Projects) and opens it (taking the lock). `path` is a native directory, absolute or relative to
	// the editor's working directory (project paths are the one exception to project:// confinement, §13.2). Available in the
	// launcher state; with a project open it is InvalidState (one project per editor). Provenance records the .eproj.
	struct ProjectCreateParams
	{
		std::string Path{};
		std::string Name{};
		ProjectTemplate Template = ProjectTemplate::Empty; // registry enum "ProjectTemplate"
	};

	struct ProjectCreateResult
	{
		ProjectSummary Project{};
		std::vector<std::string> CreatedFiles{}; // project-relative, sorted
	};

	// project.open {path}: opens the .eproj `path` names (or the directory holding exactly one), taking the lock (§4.13); a
	// read-only editor (--read-only) opens without it. Available in the launcher state; with a project open it is
	// InvalidState. project.open {recover} arrives with autosave (M10).
	struct ProjectOpenParams
	{
		std::string Path{};
	};

	struct ProjectOpenResult
	{
		ProjectSummary Project{};
		std::vector<std::string> Warnings{}; // the .eproj's load warnings ("unknown field 'Foo'"), also logged at Warn
	};

	// project.save {}: writes the open scene when it is dirty (to its path; an unsaved scene needs scene.save {path}) and,
	// from M6, dirty native assets. Settings are already on disk (ProjectSettingsCommand writes through).
	struct ProjectSaveResult
	{
		std::vector<std::string> SavedFiles{}; // project-relative
	};

	// Registry struct "OpenSceneSummary": the open scene, or open = false.
	struct OpenSceneSummary
	{
		bool Open = false;
		std::string Path{}; // project-relative; empty for a scene never saved
		std::string Name{};
		uint32_t Revision = 0;
		bool Dirty = false;
		uint32_t EntityCount = 0;
	};

	// project.info {}.
	struct ProjectInfoResult
	{
		ProjectSummary Project{};
		OpenSceneSummary Scene{};
		uint32_t ProvenanceEntries = 0; // the number of recorded files (0 for read-only editors)
	};

	// project.getSettings {} -> the project settings (§6.1), registry PascalCase inside.
	struct ProjectGetSettingsResult
	{
		ProjectSettings Settings{};
	};

	// project.setSettings {patch}: an RFC 7386 merge patch of the settings (null resets a field to its default, Input.Actions
	// keys merge and null deletes one, arrays replace), applied as one undoable ProjectSettingsCommand that writes the .eproj.
	// Supports dry runs (the .eproj then goes to the overlay).
	struct ProjectSetSettingsParams
	{
		VariantValue Patch{}; // an object (InvalidArgument otherwise)
	};

	struct ProjectSetSettingsResult
	{
		ProjectSettings Settings{}; // the settings after the patch
		uint32_t UndoIndex = 0;
	};

	// project.validate {scope?, fix?}: ProjectValidator::Validate, and with "fix" ProjectValidator::Fix: true fixes every
	// auto-fixable diagnostic, an array fixes only the listed diagnostic ids and/or codes (§13.7), false or absent fixes
	// nothing. It changes something only when fixing, so it is not flagged Mutates (read-only editors can validate); fixes go
	// through EditorContext::Execute, which refuses them in read-only editors. Supports dry runs (the would-be report after
	// fixing).
	struct ProjectValidateParams
	{
		ValidationScope Scope = ValidationScope::Project; // registry enum "ValidationScope"
		VariantValue Fix{};                               // true, false or an array of strings
	};

	struct ProjectValidateResult
	{
		std::vector<ProjectDiagnostic> Diagnostics{}; // after the fixes
		std::vector<std::string> Fixed{};             // ids fixed
		uint32_t ErrorCount = 0;
		uint32_t WarningCount = 0;
		uint32_t UndoIndex = 0; // the fixes' undo step; 0 when nothing was fixed
	};

	// project.upgrade {}: applies migrations and canonical re-saves to every project file: the .eproj and every .scene and
	// .prefab under Assets/ (§6, §13.5, §13.12). A file is written only when its canonical bytes differ, through
	// EditorContext::WriteProjectFile, so provenance records each rewrite with the method "project.upgrade". The open scene
	// must not be dirty (InvalidState); it is reloaded afterwards when its file changed. Supports dry runs: changedFiles then
	// lists what a real upgrade would rewrite, and nothing is written. Not undoable (it is a file-format operation); the
	// history is cleared when the open scene reloads.
	struct ProjectUpgradeResult
	{
		std::vector<std::string> ChangedFiles{}; // project-relative, sorted
		uint32_t UnchangedCount = 0;
	};

	namespace Automation {

		// project.create. Errors: InvalidState with a project open; Validation for an invalid name; AlreadyExists for a
		// non-empty directory; the open errors.
		[[nodiscard]] Result<ProjectCreateResult> ProjectCreate(EditorMethodContext& context, const ProjectCreateParams& params);
		// project.open. Errors: InvalidState with a project open; NotFound; AlreadyExists for a locked project ("locked by
		// process <pid>"); Parse, Validation or UnsupportedVersion for the .eproj.
		[[nodiscard]] Result<ProjectOpenResult> ProjectOpen(EditorMethodContext& context, const ProjectOpenParams& params);
		// project.save. Errors: InvalidState for a dirty scene that has no path; PermissionDenied (read-only); write errors.
		[[nodiscard]] Result<ProjectSaveResult> ProjectSave(EditorMethodContext& context, const NoParams& params);
		[[nodiscard]] Result<ProjectInfoResult> ProjectInfo(EditorMethodContext& context, const NoParams& params);
		[[nodiscard]] Result<ProjectGetSettingsResult> ProjectGetSettings(EditorMethodContext& context, const NoParams& params);
		// project.setSettings. Errors: InvalidArgument for a patch that is not an object; Validation for invalid settings.
		[[nodiscard]] Result<ProjectSetSettingsResult> ProjectSetSettings(EditorMethodContext& context, const ProjectSetSettingsParams& params);
		// project.validate. Errors: InvalidArgument for a malformed fix or an unknown id or code; InvalidState for scope
		// "scene" without an open scene; PermissionDenied for fixes in a read-only editor.
		[[nodiscard]] Result<ProjectValidateResult> ProjectValidate(EditorMethodContext& context, const ProjectValidateParams& params);
		// project.upgrade. Errors: InvalidState for a dirty open scene; the first file that fails to load or write (files
		// rewritten before it stay rewritten and recorded).
		[[nodiscard]] Result<ProjectUpgradeResult> ProjectUpgrade(EditorMethodContext& context, const NoParams& params);

	}

	// Registers ProjectSummary, OpenSceneSummary and the params and result structs above, and the enum ProjectTemplate.
	void RegisterProjectMethodTypes(TypeRegistry& registry);

	// Registers the eight methods: project.create and project.open are available in the launcher state; create, open, save,
	// getSettings, setSettings and validate are tools (§13.8; info and upgrade are reached through engine_call); create,
	// save, setSettings and upgrade mutate; setSettings, validate and upgrade support dry runs; info, getSettings,
	// setSettings and validate are AllowedInBatch (setSettings and validate's fixes go through EditorContext::Execute, and
	// a fix inside a batch joins the batch's transaction), while create, open, save and upgrade are not.
	void RegisterProjectMethods(MethodRegistry& methods);

}
