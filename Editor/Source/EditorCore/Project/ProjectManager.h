#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/ProjectLock.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Project/ProjectSettings.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Projects on disk (Architecture §2.1, §6.1, §12.1, §12.4, §4.13): create from a template, find and open with the
// single-writer lock or read-only, and the recent list.
//
// Project layout: <Root>/<Name>.eproj, Assets/{Scenes,Scripts,Prefabs,Materials,Models,Textures,Audio,Fonts,Tests}/,
// .luaurc, .gitignore ("Library/"), AGENTS.md (game notes), Automation/ (committed: Provenance.json, BuildLog.jsonl) and
// Library/ (gitignored: Cache/, Autosave/, Editor.lock, Automation/Out/).

namespace Engine {

	class TypeRegistry;
	class VirtualFileSystem;

	// New-project templates (§12.4). Basic3D's generated scene uses BuildBasic3DScene (M10).
	enum class ProjectTemplate : uint8_t
	{
		Empty,
		Basic3D
	};

	// "Empty" or "Basic3D".
	[[nodiscard]] std::string_view ProjectTemplateToString(ProjectTemplate projectTemplate);
	// ASCII case-insensitive inverse (automation, §13.4); nullopt otherwise.
	[[nodiscard]] std::optional<ProjectTemplate> ProjectTemplateFromString(std::string_view text);

	struct ProjectCreateSpecification
	{
		// The new project's root. Created with its parents; it may exist only as an empty directory.
		std::filesystem::path Directory{};
		// The game's name: the project file <Name>.eproj and ProjectSettings::Name. It must pass Paths::ValidateAppName,
		// because exported games use it as their user-data folder name (§14.1).
		std::string Name{};
		ProjectTemplate Template = ProjectTemplate::Empty;
		// Resources/Templates/Projects: holds one directory per template (Empty/, Basic3D/) whose files (.luaurc, .gitignore,
		// AGENTS.md) are copied verbatim, AGENTS.md with "{{Name}}" replaced by the name.
		std::filesystem::path TemplatesDirectory{};
	};

	// A file CreateProject wrote that provenance records (§13.4: the .eproj and files under Assets/).
	struct CreatedProjectFile
	{
		std::string Path{}; // project-relative, '/' separators
		uint64_t Hash = 0;  // XXH64 of the bytes written
	};

	struct CreatedProject
	{
		std::filesystem::path ProjectFile{}; // <Root>/<Name>.eproj
		std::vector<CreatedProjectFile> RecordedFiles{};
	};

	struct ProjectOpenOptions
	{
		// --read-only (§4.13): no lock, no writes under the project (Library/ included), autosave off, and a private
		// temporary cache (CacheDirectory below).
		bool ReadOnly = false;
		// --strict: unknown keys in the .eproj are errors (ProjectLoadOptions).
		bool StrictUnknowns = false;
		// The private cache of a read-only editor (required when ReadOnly, asserted). It starts empty (a leftover of an editor
		// that did not exit cleanly is cleared first) and is removed when the LoadedProject is destroyed.
		std::filesystem::path ReadOnlyCacheDirectory{};
	};

	// An open project: its files, settings and, unless read-only, the held lock. Created by ProjectManager::OpenProject and
	// owned by EditorContext; destroying it releases the lock, or removes the private cache of a read-only project. Main
	// thread only.
	class LoadedProject
	{
	public:
		// Restricts construction to ProjectManager; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ProjectManager;
		};

		LoadedProject(ConstructionKey key, std::filesystem::path projectFile, ProjectSettings settings, ProjectLoadReport report,
			std::optional<ProjectLock> lock, std::filesystem::path cacheDirectory);
		~LoadedProject();

		LoadedProject(const LoadedProject&) = delete;
		LoadedProject& operator=(const LoadedProject&) = delete;

		// The project root (the .eproj's directory), absolute.
		[[nodiscard]] std::filesystem::path GetRoot() const { return m_ProjectFile.parent_path(); }
		// <Root>/<Name>.eproj, absolute.
		[[nodiscard]] const std::filesystem::path& GetProjectFile() const { return m_ProjectFile; }
		// The settings the editor runs with (EditorContext::ApplyProjectSettings replaces them after writing the file).
		[[nodiscard]] const ProjectSettings& GetSettings() const { return m_Settings; }
		void SetSettings(ProjectSettings settings) { m_Settings = std::move(settings); }
		// What loading the .eproj reported (warnings for unknown keys).
		[[nodiscard]] const ProjectLoadReport& GetLoadReport() const { return m_LoadReport; }
		// Opened with --read-only (no lock held).
		[[nodiscard]] bool IsReadOnly() const { return !m_Lock.has_value(); }
		// <Root>/Library/Cache, or the private cache of a read-only editor; mounted as cache:// (§4.10).
		[[nodiscard]] const std::filesystem::path& GetCacheDirectory() const { return m_CacheDirectory; }
	private:
		std::filesystem::path m_ProjectFile;
		ProjectSettings m_Settings;
		ProjectLoadReport m_LoadReport;
		std::optional<ProjectLock> m_Lock; // held for the project's lifetime unless read-only
		std::filesystem::path m_CacheDirectory;
	};

	// Static functions only. Main thread (they touch the file system and the project lock).
	class ProjectManager
	{
	public:
		static constexpr std::string_view ProjectFileExtension = ".eproj";
		// Project-relative (§4.13).
		static constexpr std::string_view LockFilePath = "Library/Editor.lock";
		// The most recent projects kept in user://Editor.json.
		static constexpr size_t MaxRecentProjects = 10;

		ProjectManager() = delete;

		// Creates a project from `specification` (layout in the file comment): the directories, the template's files, and
		// <Name>.eproj written canonically from default ProjectSettings with Name set (ProjectSerializer). Does not open it.
		// Errors, with nothing left behind that was not there before: Validation for an invalid name; AlreadyExists when the
		// directory exists and is not empty; NotFound for a missing template directory; Io.
		[[nodiscard]] static Result<CreatedProject> CreateProject(const ProjectCreateSpecification& specification, const TypeRegistry& registry);

		// The project file `path` names: `path` itself when it is a .eproj file, or the only .eproj in the directory `path`.
		// Errors: NotFound (no such file or directory, or no .eproj in it); InvalidArgument for a directory with several .eproj
		// files (each named in the message) or a file with another extension.
		[[nodiscard]] static Result<std::filesystem::path> FindProjectFile(const std::filesystem::path& path);

		// Opens the project `path` names (FindProjectFile): unless read-only, creates Library/ and takes the lock on
		// Library/Editor.lock (§4.13), then loads the .eproj (ProjectSerializer::LoadFromFile through a temporary native mount).
		// Errors: those of FindProjectFile; AlreadyExists "'<lock>' is locked by process <pid>" when another editor holds the
		// project (the editor then exits with code 3, §4.13); Parse, Validation or UnsupportedVersion for the .eproj (the lock
		// is released again); Io.
		[[nodiscard]] static Result<Scope<LoadedProject>> OpenProject(const std::filesystem::path& path, const ProjectOpenOptions& options,
			const TypeRegistry& registry);

		// The recent projects (absolute .eproj paths, most recent first) from user://Editor.json
		// ({"Format": "EditorPreferences", "Version": 1, "RecentProjects": [...]}, other members preserved). A missing file
		// or one without user:// mounted gives an empty list. Errors: Parse or Validation for a malformed file.
		[[nodiscard]] static Result<std::vector<std::filesystem::path>> ReadRecentProjects(const VirtualFileSystem& vfs);

		// Moves `projectFile` to the front of the recent list (keeping at most MaxRecentProjects) and writes user://Editor.json.
		// Errors: InvalidState without user://; Parse or Validation for a malformed existing file; the write errors.
		[[nodiscard]] static Status AddRecentProject(VirtualFileSystem& vfs, const std::filesystem::path& projectFile);
	};

}
