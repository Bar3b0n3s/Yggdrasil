#include "EditorPCH.h"
#include "EditorCore/Project/ProjectManager.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/Paths.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		static constexpr std::array<std::string_view, 9> AssetFolders = { "Scenes", "Scripts", "Prefabs", "Materials", "Models", "Textures", "Audio",
			"Fonts", "Tests" };
		static constexpr std::string_view AgentsFileName = "AGENTS.md";
		static constexpr std::string_view NamePlaceholder = "{{Name}}";
		static constexpr std::string_view PreferencesFormat = "EditorPreferences";
		static constexpr uint32_t PreferencesVersion = 1;
		static constexpr std::string_view PreferencesPath = "user://Editor.json";

		// `path` made absolute (std::filesystem::absolute, error_code form); `path` itself when that fails.
		static std::filesystem::path MakeAbsolute(const std::filesystem::path& path)
		{
			std::error_code error;
			std::filesystem::path absolute = std::filesystem::absolute(path, error);
			return error ? path : absolute;
		}

		static std::span<const std::byte> AsBytes(std::string_view text)
		{
			return std::as_bytes(std::span(text.data(), text.size()));
		}

		static void ReplaceAll(std::string& text, std::string_view from, std::string_view to)
		{
			for (size_t position = text.find(from); position != std::string::npos; position = text.find(from, position + to.size()))
				text.replace(position, from.size(), to);
		}

		// The first of `directory` and its ancestors that does not exist (what creating `directory` adds), or an empty path
		// when `directory` exists.
		static std::filesystem::path FindFirstMissing(const std::filesystem::path& directory)
		{
			std::filesystem::path missing;
			for (std::filesystem::path current = directory; !current.empty() && !FileSystem::Exists(current); current = current.parent_path())
			{
				missing = current;
				if (current == current.parent_path())
					break;
			}
			return missing;
		}

		// A file of a template: its path relative to the template directory and its bytes.
		struct TemplateFile
		{
			std::filesystem::path RelativePath{};
			std::string Contents{};
		};

		// Every file and directory of `templateDirectory`, read up front so that a bad template creates nothing. Errors:
		// NotFound for a missing directory; the read errors.
		static Status ReadTemplate(const std::filesystem::path& templateDirectory, std::vector<TemplateFile>& files,
			std::vector<std::filesystem::path>& directories)
		{
			Result<FileInfo> info = FileSystem::GetInfo(templateDirectory);
			if (!info || !info->IsDirectory)
				return MakeError(ErrorCode::NotFound, "the project template directory '{}' does not exist", FileSystem::PathToUtf8(templateDirectory));
			ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> entries, FileSystem::ListDirectory(templateDirectory, true));
			for (const std::filesystem::path& entry : entries)
			{
				ENGINE_TRY_ASSIGN(const FileInfo entryInfo, FileSystem::GetInfo(entry));
				const std::filesystem::path relative = entry.lexically_relative(templateDirectory);
				if (entryInfo.IsDirectory)
				{
					directories.push_back(relative);
					continue;
				}
				ENGINE_TRY_ASSIGN(std::string contents, FileSystem::ReadText(entry));
				files.push_back(TemplateFile{ relative, std::move(contents) });
			}
			return {};
		}

		// The creation steps of CreateProject after validation; the caller removes what they created on failure.
		static Result<CreatedProject> WriteNewProject(const ProjectCreateSpecification& specification, const TypeRegistry& registry,
			std::span<const TemplateFile> files, std::span<const std::filesystem::path> directories)
		{
			const std::filesystem::path& root = specification.Directory;
			ENGINE_TRY(FileSystem::CreateDirectories(root));
			for (const std::string_view folder : AssetFolders)
				ENGINE_TRY(FileSystem::CreateDirectories(root / "Assets" / folder));
			for (const std::filesystem::path& directory : directories)
				ENGINE_TRY(FileSystem::CreateDirectories(root / directory));
			for (const TemplateFile& file : files)
			{
				std::string contents = file.Contents;
				if (file.RelativePath == std::filesystem::path(AgentsFileName))
					ReplaceAll(contents, NamePlaceholder, specification.Name);
				const std::filesystem::path target = root / file.RelativePath;
				ENGINE_TRY(FileSystem::CreateDirectories(target.parent_path()));
				ENGINE_TRY(FileSystem::WriteFileAtomic(target, AsBytes(contents), AtomicWriteOptions{ .KeepBackup = false, .InjectFailure = AtomicWriteStep::None }));
			}

			ProjectSettings settings;
			settings.Name = specification.Name;
			ENGINE_TRY_ASSIGN(const std::string text, ProjectSerializer::SaveToString(settings, registry));
			const std::string fileName = std::format("{}{}", specification.Name, ProjectManager::ProjectFileExtension);
			const std::filesystem::path projectFile = root / FileSystem::PathFromUtf8(fileName);
			ENGINE_TRY(FileSystem::WriteFileAtomic(projectFile, AsBytes(text), AtomicWriteOptions{ .KeepBackup = false, .InjectFailure = AtomicWriteStep::None }));

			CreatedProject created;
			created.ProjectFile = projectFile;
			created.RecordedFiles.push_back(CreatedProjectFile{ .Path = fileName, .Hash = XXH64(text) });
			return created;
		}

		// The preferences document of user://Editor.json; a new one when the file does not exist. Errors: Parse; Validation
		// for a wrong header or a malformed RecentProjects; UnsupportedVersion for a newer file; the read errors.
		static Result<Json> ReadPreferences(const VirtualFileSystem& vfs, const VfsPath& path)
		{
			if (!vfs.Exists(path))
			{
				Json document = Json::object();
				document["Format"] = std::string(PreferencesFormat);
				document["Version"] = PreferencesVersion;
				document["RecentProjects"] = Json::array();
				return document;
			}
			ENGINE_TRY_ASSIGN(const std::string text, vfs.ReadText(path));
			Result<Json> document = JsonReader::Parse(text);
			if (!document)
				return std::unexpected(std::move(document).error().WithContext(std::format("reading '{}'", path.ToString())));
			const JsonReader reader(*document);
			if (Result<uint32_t> version = reader.ReadFormatHeader(PreferencesFormat, PreferencesVersion, PreferencesVersion); !version)
				return std::unexpected(std::move(version).error().WithContext(std::format("reading '{}'", path.ToString())));
			if (const std::optional<JsonReader> recent = reader.FindMember("RecentProjects"))
			{
				ENGINE_TRY_ASSIGN(const size_t count, recent->GetArraySize());
				for (size_t index = 0; index < count; ++index)
				{
					ENGINE_TRY_ASSIGN(const JsonReader element, recent->GetElement(index));
					if (Result<std::string> entry = element.ReadString(); !entry)
						return std::unexpected(std::move(entry).error().WithContext(std::format("reading '{}'", path.ToString())));
				}
			}
			return document;
		}

	}

	std::string_view ProjectTemplateToString(ProjectTemplate projectTemplate)
	{
		switch (projectTemplate)
		{
			case ProjectTemplate::Empty:
				return "Empty";
		}
		return "Empty";
	}

	std::optional<ProjectTemplate> ProjectTemplateFromString(std::string_view text)
	{
		const auto equalsIgnoringCase = [](std::string_view left, std::string_view right)
		{
			return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](char first, char second)
			{
				const auto lower = [](char value)
				{
					return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
				};
				return lower(first) == lower(second);
			});
		};
		if (equalsIgnoringCase(text, ProjectTemplateToString(ProjectTemplate::Empty)))
			return ProjectTemplate::Empty;
		return std::nullopt;
	}

	LoadedProject::LoadedProject(ConstructionKey /*key*/, std::filesystem::path projectFile, ProjectSettings settings, ProjectLoadReport report,
		std::optional<ProjectLock> lock, std::filesystem::path cacheDirectory)
		: m_ProjectFile(std::move(projectFile)), m_Settings(std::move(settings)), m_LoadReport(std::move(report)), m_Lock(std::move(lock)), m_CacheDirectory(std::move(cacheDirectory))
	{
	}

	LoadedProject::~LoadedProject()
	{
		// A read-only editor's cache is private and temporary (§4.13): it goes with the project.
		if (!IsReadOnly() || m_CacheDirectory.empty())
			return;
		const Status removed = FileSystem::Remove(m_CacheDirectory);
		if (!removed && removed.error().GetCode() != ErrorCode::NotFound)
			ENGINE_WARN("Could not remove the private cache '{}' of the read-only project: {}", FileSystem::PathToUtf8(m_CacheDirectory), removed.error().ToString());
	}

	Result<CreatedProject> ProjectManager::CreateProject(const ProjectCreateSpecification& specification, const TypeRegistry& registry)
	{
		ENGINE_TRY(Paths::ValidateAppName(specification.Name));
		const std::filesystem::path& root = specification.Directory;
		if (root.empty())
			return MakeError(ErrorCode::InvalidArgument, "a new project needs a directory");
		if (Result<FileInfo> existing = FileSystem::GetInfo(root); existing)
		{
			if (!existing->IsDirectory)
				return MakeError(ErrorCode::AlreadyExists, "cannot create a project in '{}': it is a file", FileSystem::PathToUtf8(root));
			ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> entries, FileSystem::ListDirectory(root));
			if (!entries.empty())
			{
				return std::unexpected(Error(ErrorCode::AlreadyExists, std::format("cannot create a project in '{}': the directory is not empty", FileSystem::PathToUtf8(root)))
						.WithHint("choose a new or empty directory"));
			}
		}
		else if (existing.error().GetCode() != ErrorCode::NotFound)
		{
			return std::unexpected(std::move(existing).error());
		}

		std::vector<Utils::TemplateFile> files;
		std::vector<std::filesystem::path> directories;
		ENGINE_TRY(Utils::ReadTemplate(specification.TemplatesDirectory / ProjectTemplateToString(specification.Template), files, directories));

		// Nothing is left behind on failure: the first missing directory on the way to the root goes again, or, when the root
		// existed (empty), everything created in it.
		const std::filesystem::path firstMissing = Utils::FindFirstMissing(root);
		Result<CreatedProject> created = Utils::WriteNewProject(specification, registry, files, directories);
		if (created)
			return created;
		if (!firstMissing.empty())
		{
			if (Status removed = FileSystem::Remove(firstMissing); !removed)
				ENGINE_WARN("Could not remove '{}' after a failed project creation: {}", FileSystem::PathToUtf8(firstMissing), removed.error().ToString());
		}
		else if (Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(root); entries)
		{
			for (const std::filesystem::path& entry : *entries)
			{
				if (Status removed = FileSystem::Remove(entry); !removed)
					ENGINE_WARN("Could not remove '{}' after a failed project creation: {}", FileSystem::PathToUtf8(entry), removed.error().ToString());
			}
		}
		return std::unexpected(std::move(created).error().WithContext(std::format("creating the project '{}'", specification.Name)));
	}

	Result<std::filesystem::path> ProjectManager::FindProjectFile(const std::filesystem::path& path)
	{
		const std::filesystem::path absolute = Utils::MakeAbsolute(path);
		Result<FileInfo> info = FileSystem::GetInfo(absolute);
		if (!info)
		{
			if (info.error().GetCode() == ErrorCode::NotFound)
				return MakeError(ErrorCode::NotFound, "no project at '{}': no such file or directory", FileSystem::PathToUtf8(absolute));
			return std::unexpected(std::move(info).error());
		}
		if (!info->IsDirectory)
		{
			if (absolute.extension() != ProjectFileExtension)
			{
				return MakeError(ErrorCode::InvalidArgument, "'{}' is not a project file: project files end in '{}'", FileSystem::PathToUtf8(absolute),
					ProjectFileExtension);
			}
			return absolute;
		}

		ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> entries, FileSystem::ListDirectory(absolute));
		std::vector<std::filesystem::path> candidates;
		for (const std::filesystem::path& entry : entries)
		{
			if (entry.extension() != ProjectFileExtension)
				continue;
			if (const Result<FileInfo> entryInfo = FileSystem::GetInfo(entry); entryInfo && !entryInfo->IsDirectory)
				candidates.push_back(entry);
		}
		if (candidates.empty())
			return MakeError(ErrorCode::NotFound, "no '{}' file in '{}'", ProjectFileExtension, FileSystem::PathToUtf8(absolute));
		if (candidates.size() > 1)
		{
			std::string names;
			for (const std::filesystem::path& candidate : candidates)
				names += std::format("{}'{}'", names.empty() ? "" : ", ", FileSystem::PathToUtf8(candidate.filename()));
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("'{}' holds several project files: {}", FileSystem::PathToUtf8(absolute), names))
					.WithHint("name the project file itself"));
		}
		return candidates.front();
	}

	Result<Scope<LoadedProject>> ProjectManager::OpenProject(const std::filesystem::path& path, const ProjectOpenOptions& options, const TypeRegistry& registry)
	{
		ENGINE_TRY_ASSIGN(std::filesystem::path projectFile, FindProjectFile(path));
		const std::filesystem::path root = projectFile.parent_path();

		std::optional<ProjectLock> lock;
		if (!options.ReadOnly)
		{
			const std::filesystem::path library = root / "Library";
			ENGINE_TRY(FileSystem::CreateDirectories(library));
			ENGINE_TRY_ASSIGN(ProjectLock acquired, ProjectLock::Acquire(root / FileSystem::PathFromUtf8(LockFilePath)));
			lock = std::move(acquired);
		}
		else
		{
			ENGINE_ASSERT(!options.ReadOnlyCacheDirectory.empty(), "a read-only project needs ProjectOpenOptions::ReadOnlyCacheDirectory");
			if (options.ReadOnlyCacheDirectory.empty())
				return MakeError(ErrorCode::InvalidArgument, "a read-only project needs a private cache directory");
		}

		// Read through a native mount of the root, so the .eproj's spelling follows the case policy (§4.10).
		VirtualFileSystem vfs;
		ENGINE_TRY_ASSIGN(Scope<NativeDirectoryMount> mount, NativeDirectoryMount::Create(root, MountAccess::ReadOnly));
		ENGINE_TRY(vfs.Mount("project", std::move(mount)));
		const std::string fileName = FileSystem::PathToUtf8(projectFile.filename());
		ENGINE_TRY_ASSIGN(const VfsPath vfsPath, VfsPath::Create("project", fileName));
		ProjectLoadReport report;
		ENGINE_TRY_ASSIGN(ProjectSettings settings,
			ProjectSerializer::LoadFromFile(vfs, vfsPath, registry, ProjectLoadOptions{ .StrictUnknowns = options.StrictUnknowns, .SourcePath = fileName }, report));

		const std::filesystem::path cacheDirectory = options.ReadOnly ? options.ReadOnlyCacheDirectory : root / "Library" / "Cache";
		if (options.ReadOnly && FileSystem::Exists(cacheDirectory))
		{
			// The leftover of a read-only editor that did not exit cleanly (its process id may be this one's now): a private
			// cache starts empty, so nothing of another project or session is read from it.
			const Status cleared = FileSystem::Remove(cacheDirectory);
			if (!cleared)
				return std::unexpected(Error(cleared.error()).WithContext(std::format("clearing the private cache '{}' of a previous read-only editor", FileSystem::PathToUtf8(cacheDirectory))));
		}
		ENGINE_TRY(FileSystem::CreateDirectories(cacheDirectory));
		return CreateScope<LoadedProject>(LoadedProject::ConstructionKey(), std::move(projectFile), std::move(settings), std::move(report), std::move(lock),
			cacheDirectory);
	}

	Result<std::vector<std::filesystem::path>> ProjectManager::ReadRecentProjects(const VirtualFileSystem& vfs)
	{
		std::vector<std::filesystem::path> recent;
		if (!vfs.IsMounted("user"))
			return recent;
		ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Parse(Utils::PreferencesPath));
		ENGINE_TRY_ASSIGN(const Json document, Utils::ReadPreferences(vfs, path));
		const JsonReader reader(document);
		if (const std::optional<JsonReader> list = reader.FindMember("RecentProjects"))
		{
			ENGINE_TRY_ASSIGN(const size_t count, list->GetArraySize());
			for (size_t index = 0; index < count; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader element, list->GetElement(index));
				ENGINE_TRY_ASSIGN(const std::string entry, element.ReadString());
				recent.push_back(FileSystem::PathFromUtf8(entry));
			}
		}
		return recent;
	}

	Status ProjectManager::AddRecentProject(VirtualFileSystem& vfs, const std::filesystem::path& projectFile)
	{
		if (!vfs.IsMounted("user"))
			return MakeError(ErrorCode::InvalidState, "the recent projects cannot be saved: user:// is not mounted");
		ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Parse(Utils::PreferencesPath));
		ENGINE_TRY_ASSIGN(Json document, Utils::ReadPreferences(vfs, path));

		const std::string added = FileSystem::PathToUtf8(Utils::MakeAbsolute(projectFile).lexically_normal());
		Json recent = Json::array();
		recent.push_back(added);
		if (const std::optional<JsonReader> list = JsonReader(document).FindMember("RecentProjects"))
		{
			ENGINE_TRY_ASSIGN(const size_t count, list->GetArraySize());
			for (size_t index = 0; index < count && recent.size() < MaxRecentProjects; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader element, list->GetElement(index));
				ENGINE_TRY_ASSIGN(const std::string entry, element.ReadString());
				if (entry != added)
					recent.push_back(entry);
			}
		}
		document["RecentProjects"] = std::move(recent);
		ENGINE_TRY_ASSIGN(const std::string text, JsonWriter::Write(document));
		return vfs.WriteFileAtomic(path, Utils::AsBytes(text));
	}

}
