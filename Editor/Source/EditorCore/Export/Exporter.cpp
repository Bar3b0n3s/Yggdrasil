#include "EditorPCH.h"
#include "EditorCore/Export/Exporter.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/EngineAssetGenerators.h"
#include "EditorCore/Export/Private/ExportPaths.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetReference.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/PakFormat.h"
#include "Engine/Asset/PakReader.h"
#include "Engine/AssetPipeline/AssetCache.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/EngineAssetBaker.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/AssetPipeline/PakWriter.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/Paths.h"
#include "Engine/Platform/Process.h"
#include "Engine/Project/GameManifest.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <map>
#include <set>
#include <system_error>
#include <utility>

namespace Engine {

	namespace {

		constexpr std::string_view DataDirectoryName = "Data";
		constexpr std::string_view EnginePakFileName = "Engine.pak";
		constexpr std::string_view GamePakFileName = "Game.pak";
		constexpr std::string_view ShadersDirectoryName = "Shaders";
		constexpr std::string_view RuntimeProjectName = "Runtime";
		constexpr std::string_view CacheScheme = "cache";
		constexpr std::string_view ProjectScheme = "project";
		// The staging, earlier-export and smoke-test directories beside the output directory:
		// ".export-<kind>-<pid>-<tag>", where <tag> is the 8 hex digits of the low 32 bits of the XXH64 of the output
		// directory's name (SiblingTag). The pid tells a crashed editor's leftovers from a running one's (Process::IsRunning),
		// the tag the leftovers of this output from another's, and the length does not grow with the game's name: the staged
		// executable's temporary file then stays as short as the exported executable's path, inside Windows' MAX_PATH where
		// long paths are off.
		constexpr std::string_view SiblingPrefix = ".export-";
		constexpr std::string_view StagingKind = "staging";
		constexpr std::string_view PreviousKind = "previous";
		constexpr std::string_view SmokeTestKind = "smoke";
		// The bound of the smoke test's run, a failure bound only: a game that runs SmokeTestFrames headless frames at its
		// FixedHz takes seconds, and one that hangs is killed after it.
		constexpr std::chrono::seconds SmokeTestTimeout{ 300 };
		// The bound of the wait for a killed smoke test's output (its readers reach the end of the pipes once the process
		// is gone), a failure bound only.
		constexpr std::chrono::seconds SmokeTestDrainTimeout{ 10 };
		// How much of the smoke test's output a failure quotes.
		constexpr size_t SmokeTestOutputLines = 20;

		// One pak as written: the XXH64 of the whole file and its entry count.
		struct WrittenPak
		{
			uint64_t Hash = 0;
			uint32_t EntryCount = 0;
		};

		// What the WritePaks job wrote.
		struct WrittenPaks
		{
			uint64_t EngineHash = 0;
			uint64_t GameHash = 0;
			uint32_t EngineEntryCount = 0;
			uint32_t GameEntryCount = 0;
		};

		// Everything the WritePaks job needs, moved into it (jobs take values, §4.11).
		struct PakJobInput
		{
			std::vector<PakWriterEntry> EngineEntries{};
			std::vector<PakWriterEntry> GameEntries{};
			Json EngineMetadata{};
			Json GameMetadata{};
			std::filesystem::path ShaderDirectory{};
			std::filesystem::path DataDirectory{};
		};

		[[nodiscard]] std::string ExecutableFileName(std::string_view name)
		{
#if defined(ENGINE_PLATFORM_WINDOWS)
			return std::format("{}.exe", name);
#else
			return std::string(name);
#endif
		}

		// One problem of a failed validation, located at its project setting's pointer ("" when it has none).
		[[nodiscard]] ErrorIssue MakeIssue(std::string pointer, std::string message, std::string hint = {})
		{
			return ErrorIssue{ .JsonPointer = std::move(pointer), .Message = std::move(message), .Hint = std::move(hint), .Suggestions = {} };
		}

		[[nodiscard]] std::string BuildCommand(ExportConfiguration configuration)
		{
			return std::format("python Scripts/Build.py --config {} --project {}", ExportConfigurationToString(configuration), RuntimeProjectName);
		}

		// The project setting's name is the executable's and the user-data folder's name (§14.1): the rule the Runtime reads
		// Game.json's Name with (Paths::ValidateAppName: one folder name valid on every host, at most 64 bytes), and one path
		// segment by VfsPath's rules, so validation refuses what the manifest could not carry.
		[[nodiscard]] Status CheckGameName(std::string_view name)
		{
			if (name.empty())
				return MakeError(ErrorCode::Validation, "the project's Name is empty, and it names the exported executable and its user-data folder");
			if (Status valid = Paths::ValidateAppName(name); !valid.has_value())
			{
				return MakeError(ErrorCode::Validation, "the project's Name '{}' cannot name the exported game and its user-data folder: {}", name,
					valid.error().GetMessageText());
			}
			if (Status valid = VfsPath::ValidateRelativePath(name); !valid.has_value())
				return MakeError(ErrorCode::Validation, "the project's Name '{}' is not a valid file name: {}", name, valid.error().GetMessageText());
			return {};
		}

		[[nodiscard]] Status CopyFile(const std::filesystem::path& source, const std::filesystem::path& target)
		{
			ENGINE_TRY_ASSIGN(const Buffer bytes, FileSystem::ReadFile(source));
			ENGINE_TRY(FileSystem::WriteFileAtomic(target, bytes, { .KeepBackup = false }));
			// The executable bit (POSIX) and the read-only attribute (Windows) travel with the copy.
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::status(source, error);
			if (!error)
				std::filesystem::permissions(target, status.permissions(), std::filesystem::perm_options::replace, error);
			if (error)
			{
				return MakeError(ErrorCode::Io, "cannot give '{}' the permissions of '{}': {}", FileSystem::PathToUtf8(target),
					FileSystem::PathToUtf8(source), error.message());
			}
			return {};
		}

		// Writes one pak atomically, reopens it and reads every entry back (PakReader verifies the TOC hash at open and each
		// entry's hash on its read), and returns the XXH64 of the whole file as read back, which Game.json records.
		[[nodiscard]] Result<WrittenPak> WritePak(std::vector<PakWriterEntry> entries, Json metadata, const std::filesystem::path& path)
		{
			PakWriter writer;
			for (PakWriterEntry& entry : entries)
			{
				const std::string entryPath = entry.Path;
				ENGINE_TRY(WithContext(writer.Add(std::move(entry)), std::format("while adding '{}'", entryPath)));
			}
			writer.SetMetadata(VariantValue(std::move(metadata)));
			const Buffer bytes = writer.Build();
			ENGINE_TRY(FileSystem::WriteFileAtomic(path, bytes, { .KeepBackup = false }));

			{
				ENGINE_TRY_ASSIGN(const Ref<const PakReader> reader, PakReader::Open(path));
				for (const PakEntry& entry : reader->GetEntries())
					ENGINE_TRY(WithContext(reader->ReadEntry(entry), std::format("while verifying '{}'", FileSystem::PathToUtf8(path))));
			}
			ENGINE_TRY_ASSIGN(const Buffer written, FileSystem::ReadFile(path));
			const uint64_t hash = XXH64(written);
			if (hash != XXH64(bytes))
				return MakeError(ErrorCode::Io, "'{}' does not read back as it was written", FileSystem::PathToUtf8(path));
			return WrittenPak{ .Hash = hash, .EntryCount = static_cast<uint32_t>(writer.GetEntryCount()) };
		}

		// The WritePaks job: the target configuration's shader files join Engine.pak as plain files under Shaders/, then both
		// paks are written and verified.
		[[nodiscard]] Result<WrittenPaks> WritePaks(PakJobInput input)
		{
			ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> files, FileSystem::ListDirectory(input.ShaderDirectory, true));
			for (const std::filesystem::path& file : files)
			{
				const std::string relative = FileSystem::PathToUtf8(file.lexically_relative(input.ShaderDirectory));
				if (!relative.ends_with(".spv") && !relative.ends_with(".refl.json"))
					continue;
				std::error_code error;
				if (!std::filesystem::is_regular_file(file, error) || error)
					continue;
				ENGINE_TRY_ASSIGN(Buffer bytes, FileSystem::ReadFile(file));
				input.EngineEntries.push_back({
					.Handle = AssetHandle(),
					.Type = std::string(PakFileEntryType),
					.Path = std::format("{}/{}", ShadersDirectoryName, relative),
					.Data = std::move(bytes),
				});
			}

			ENGINE_TRY(FileSystem::CreateDirectories(input.DataDirectory));
			ENGINE_TRY_ASSIGN(const WrittenPak engine,
				WithContext(WritePak(std::move(input.EngineEntries), std::move(input.EngineMetadata), input.DataDirectory / EnginePakFileName),
					std::format("while writing {}", EnginePakFileName)));
			ENGINE_TRY_ASSIGN(const WrittenPak game,
				WithContext(WritePak(std::move(input.GameEntries), std::move(input.GameMetadata), input.DataDirectory / GamePakFileName),
					std::format("while writing {}", GamePakFileName)));
			return WrittenPaks{
				.EngineHash = engine.Hash,
				.GameHash = game.Hash,
				.EngineEntryCount = engine.EntryCount,
				.GameEntryCount = game.EntryCount,
			};
		}

		// The end of a process's output for an error message: its last SmokeTestOutputLines non-empty lines.
		[[nodiscard]] std::string TailOfOutput(const ProcessResult& result)
		{
			std::vector<std::string_view> lines;
			for (const std::string* stream : { &result.StandardOutput, &result.StandardError })
			{
				std::string_view text = *stream;
				while (!text.empty())
				{
					const size_t end = text.find('\n');
					std::string_view line = text.substr(0, end);
					while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
						line.remove_suffix(1);
					if (!line.empty())
						lines.push_back(line);
					if (end == std::string_view::npos)
						break;
					text.remove_prefix(end + 1);
				}
			}
			const size_t first = lines.size() > SmokeTestOutputLines ? lines.size() - SmokeTestOutputLines : 0;
			std::string tail;
			for (size_t index = first; index < lines.size(); ++index)
			{
				tail += tail.empty() ? "" : "\n";
				tail += lines[index];
			}
			return tail.empty() ? std::string("(no output)") : tail;
		}

		// The <tag> of the sibling directories of the output directory named `outputName` (see SiblingPrefix).
		[[nodiscard]] std::string SiblingTag(std::string_view outputName)
		{
			return std::format("{:08x}", static_cast<uint32_t>(XXH64(outputName)));
		}

		// The sibling directory of `kind` that process `processId` uses for the output directory named `outputName`.
		[[nodiscard]] std::string SiblingName(std::string_view kind, uint32_t processId, std::string_view outputName)
		{
			return std::format("{}{}-{}-{}", SiblingPrefix, kind, processId, SiblingTag(outputName));
		}

		// The process ID in a sibling directory name ".export-<kind>-<pid>-<tag>" of the output directory named `outputName`,
		// or nullopt for any other name.
		[[nodiscard]] std::optional<uint32_t> ParseSiblingProcessId(std::string_view name, std::string_view outputName)
		{
			if (!name.starts_with(SiblingPrefix))
				return std::nullopt;
			name.remove_prefix(SiblingPrefix.size());
			const std::string suffix = "-" + SiblingTag(outputName);
			if (!name.ends_with(suffix))
				return std::nullopt;
			name.remove_suffix(suffix.size());
			for (const std::string_view kind : { StagingKind, PreviousKind, SmokeTestKind })
			{
				if (!name.starts_with(kind) || name.size() <= kind.size() + 1 || name[kind.size()] != '-')
					continue;
				const std::string_view digits = name.substr(kind.size() + 1);
				uint32_t processId = 0;
				const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), processId);
				if (error == std::errc() && end == digits.data() + digits.size())
					return processId;
			}
			return std::nullopt;
		}

	}

	struct Exporter::State
	{
		EditorContext* Editor = nullptr; // documented back-reference: the editor outlives the exporter
		ExportSpecification Specification{};
		ExportPhase Phase = ExportPhase::Validate;
		bool Resolved = false; // Poll returned a value or Cancel ran
		std::chrono::steady_clock::time_point Started{};
		std::filesystem::path ProjectRoot{}; // the project being exported, so a project change mid-export is detected

		// Validate's results.
		ProjectSettings Settings{};
		AssetHandle StartScene{};
		std::vector<AssetHandle> Sources{}; // the included main assets, sorted
		std::filesystem::path BinaryDirectory{};
		std::filesystem::path RuntimeExecutable{};
		std::vector<std::filesystem::path> Redist{}; // the app-local CRT (Windows)
		std::filesystem::path OutputDirectory{};
		std::filesystem::path StagingDirectory{};
		std::filesystem::path PreviousDirectory{};
		std::filesystem::path SmokeTestUserData{};
		// Directories this export created on the way to the output directory, outermost first: removed again when they are
		// empty after a failure.
		std::vector<std::filesystem::path> CreatedDirectories{};
		bool StagingCreated = false;

		// Cook.
		std::vector<JobHandle<AssetRef<Asset>>> Loads{};
		bool LoadsStarted = false;
		std::vector<PakWriterEntry> EngineEntries{};
		std::vector<PakWriterEntry> GameEntries{};

		// WritePaks.
		std::optional<JobHandle<WrittenPaks>> PakJob{};
		WrittenPaks Paks{};

		// SmokeTest.
		std::optional<Process> SmokeTest{};
		std::chrono::steady_clock::time_point SmokeTestDeadline{};

		ExportReport Report{};

		// --- Steps -------------------------------------------------------------------------------------------------------

		// Runs the current phase's work for this frame: true once the phase is complete.
		[[nodiscard]] Result<bool> Step()
		{
			ENGINE_TRY(CheckProject());
			switch (Phase)
			{
				case ExportPhase::Validate:      return Validate();
				case ExportPhase::Cook:          return Cook();
				case ExportPhase::WritePaks:     return WritePakFiles();
				case ExportPhase::CopyRuntime:   return CopyRuntime();
				case ExportPhase::WriteManifest: return WriteManifest();
				case ExportPhase::SmokeTest:     return RunSmokeTest();
				case ExportPhase::MoveToOutput:  return MoveToOutput();
				case ExportPhase::Done:          return true;
			}
			ENGINE_ASSERT(false, "Unknown ExportPhase {}", std::to_underlying(Phase));
			return MakeError(ErrorCode::InvalidState, "unknown export phase");
		}

		[[nodiscard]] Status CheckProject() const
		{
			if (!Editor->HasProject() || Editor->GetProject().GetRoot() != ProjectRoot)
				return MakeError(ErrorCode::InvalidState, "the project '{}' was closed during the export", FileSystem::PathToUtf8(ProjectRoot));
			return {};
		}

		// --- 1. Validate -------------------------------------------------------------------------------------------------

		[[nodiscard]] Result<bool> Validate()
		{
			EditorAssetManager& assets = Editor->GetAssets();
			// The registry and the asset diagnostics reflect the files on disk, which is what the export packages.
			ENGINE_TRY(WithContext(assets.Refresh(), "while refreshing the assets"));
			Settings = Editor->GetProject().GetSettings();

			std::vector<ErrorIssue> issues;
			if (Status name = CheckGameName(Settings.Name); !name.has_value())
				issues.push_back(MakeIssue("/Name", name.error().GetMessageText(), "set a name such as \"Tetris\" (project.setSettings)"));

			std::vector<std::pair<AssetHandle, std::string>> scenes; // (handle, project-relative path) of every build scene
			if (Settings.StartScene.empty())
			{
				issues.push_back(MakeIssue("/StartScene", "StartScene is not set, so the game has no scene to start with",
					"set it with project.setSettings {\"patch\": {\"StartScene\": \"Assets/Scenes/Main.scene\"}}"));
			}
			else if (std::optional<AssetHandle> handle = ResolveBuildScene(Settings.StartScene, "/StartScene", "StartScene", issues))
			{
				StartScene = *handle;
				scenes.emplace_back(*handle, Settings.StartScene);
			}
			for (size_t index = 0; index < Settings.Export.BuildScenes.size(); ++index)
			{
				const std::string& path = Settings.Export.BuildScenes[index];
				if (std::optional<AssetHandle> handle = ResolveBuildScene(path, std::format("/Export/BuildScenes/{}", index), "the build scene", issues))
					scenes.emplace_back(*handle, path);
			}

			for (const AssetDiagnostic& diagnostic : assets.GetDiagnostics())
			{
				// The rule of AssetManager::HasErrorDiagnostics: reference diagnostics and GPU upload failures describe a use of
				// an asset in this process, not the asset.
				if (diagnostic.Severity != DiagnosticSeverity::Error || diagnostic.Code == AssetMissingCode || diagnostic.Code == AssetTypeMismatchCode
					|| diagnostic.Code == AssetUploadFailedCode)
				{
					continue;
				}
				issues.push_back(MakeIssue("", AssetDiagnosticToString(diagnostic), diagnostic.Hint));
			}
			if (!issues.empty())
				return std::unexpected(ValidationFailure(std::move(issues)));

			// Every build scene loads strictly (§14.2 step 1), as the Runtime loads it.
			std::ranges::sort(scenes);
			const auto duplicates = std::ranges::unique(scenes);
			scenes.erase(duplicates.begin(), duplicates.end());
			for (const auto& [handle, path] : scenes)
			{
				LoadReport report;
				if (Status loaded = LoadSceneStrictly(path, report); !loaded.has_value())
				{
					issues.push_back(MakeIssue("", std::format("the build scene '{}' does not load strictly: {}", path, loaded.error().ToString()),
						"open it with scene.open {\"repair\": true} and save it, or fix what project.validate reports"));
					continue;
				}
				for (const LoadDiagnostic& diagnostic : report.Diagnostics)
				{
					const std::string pointer = diagnostic.JsonPointer.empty() ? std::string("(root)") : diagnostic.JsonPointer;
					Report.Warnings.push_back(std::format("'{}' {}: {} ({})", path, pointer, diagnostic.Message, diagnostic.Code));
				}
			}
			if (!issues.empty())
				return std::unexpected(ValidationFailure(std::move(issues)));

			if (Editor->HasScene() && Editor->IsSceneDirty() && Editor->GetScenePath().has_value())
			{
				Report.Warnings.push_back(std::format("the open scene '{}' has unsaved changes, which the export leaves out: it packages the saved file "
													  "(scene.save first)",
					Editor->GetScenePath()->GetPath()));
			}

			CollectSources();
			ENGINE_TRY(LocateBinaries());
			ENGINE_TRY(LocateOutput());
			return true;
		}

		// The registered Scene asset `path` names (a project setting at `pointer`), or nullopt after adding the issue.
		[[nodiscard]] std::optional<AssetHandle> ResolveBuildScene(const std::string& path, const std::string& pointer, std::string_view what,
			std::vector<ErrorIssue>& issues) const
		{
			const Result<VfsPath> sourcePath = VfsPath::Create(ProjectScheme, path);
			if (!sourcePath.has_value())
			{
				issues.push_back(MakeIssue(pointer, std::format("{} '{}' is not a valid project path: {}", what, path, sourcePath.error().GetMessageText())));
				return std::nullopt;
			}
			const AssetRecord* record = Editor->GetAssets().GetRegistry().FindBySourcePath(*sourcePath);
			if (record == nullptr)
			{
				issues.push_back(MakeIssue(pointer, std::format("{} '{}' names no registered scene", what, path),
					"create the scene (scene.new) or correct the setting (project.setSettings)"));
				return std::nullopt;
			}
			if (record->Metadata.Kind != AssetMetaKind::Asset || record->Metadata.Type != AssetType::Scene)
			{
				const std::string_view type = record->Metadata.Kind == AssetMetaKind::Asset ? AssetTypeToString(record->Metadata.Type)
																							: AssetMetadata::DependencyTypeName;
				issues.push_back(MakeIssue(pointer, std::format("{} '{}' is a {}, not a scene", what, path, type)));
				return std::nullopt;
			}
			if (const std::optional<std::string> glob = FindExcludingGlob(path))
			{
				issues.push_back(MakeIssue(pointer, std::format("{} '{}' is left out of the export by the Export.Exclude glob '{}'", what, path, *glob),
					"remove the glob from Export.Exclude or move the scene"));
				return std::nullopt;
			}
			return record->Metadata.Handle;
		}

		// The Export.Exclude glob that leaves `path` (project-relative) out, or nullopt.
		[[nodiscard]] std::optional<std::string> FindExcludingGlob(std::string_view path) const
		{
			for (const std::string& glob : Settings.Export.Exclude)
			{
				if (Utils::MatchesExportGlob(glob, path))
					return glob;
			}
			return std::nullopt;
		}

		[[nodiscard]] Status LoadSceneStrictly(const std::string& path, LoadReport& report) const
		{
			ENGINE_TRY_ASSIGN(const VfsPath sourcePath, VfsPath::Create(ProjectScheme, path));
			UUIDGenerator ids = UUIDGenerator::CreateDeterministic(0);
			SceneSpecification specification;
			specification.Name = std::string(sourcePath.GetStem());
			specification.Registry = &Editor->GetTypeRegistry();
			specification.IdGenerator = &ids;
			const Scope<Scene> scene = Scene::Create(specification);
			LoadOptions options;
			options.Mode = LoadMode::Strict;
			options.SourcePath = path;
			return SceneSerializer::LoadFromFile(*scene, Editor->GetVfs(), sourcePath, options, report);
		}

		[[nodiscard]] static Error ValidationFailure(std::vector<ErrorIssue> issues)
		{
			const size_t count = issues.size();
			std::string message = count == 1 ? issues.front().Message : std::format("the project cannot be exported: {} problems", count);
			return Error(ErrorCode::Validation, std::move(message))
				.WithHint("project.validate reports what to fix in the scenes and assets")
				.WithIssues(std::move(issues));
		}

		// Every registered main asset under Assets/ that no Export.Exclude glob leaves out (§7.6).
		void CollectSources()
		{
			Sources.clear();
			for (const AssetRecord* record : Editor->GetAssets().GetRegistry().GetRecords())
			{
				if (record->Metadata.Kind != AssetMetaKind::Asset)
					continue;
				if (FindExcludingGlob(record->SourcePath.GetPath()).has_value())
					continue;
				Sources.push_back(record->Metadata.Handle);
			}
			std::ranges::sort(Sources);
		}

		// The target configuration's build outputs (§14.2 step 4). The editor never builds: a missing binary is NotFound, and its
		// message names the build command.
		[[nodiscard]] Status LocateBinaries()
		{
			const ExportConfiguration configuration = Specification.Configuration;
			const std::string_view name = ExportConfigurationToString(configuration);
			BinaryDirectory = Specification.BinaryRoot / GetBuildOutputDirectoryName(configuration);
			const std::filesystem::path runtimeDirectory = BinaryDirectory / RuntimeProjectName;
			RuntimeExecutable = runtimeDirectory / ExecutableFileName(RuntimeProjectName);
			std::error_code error;
			if (!std::filesystem::is_regular_file(RuntimeExecutable, error))
				return MissingBinary(std::format("the {} Runtime '{}' is not built", name, FileSystem::PathToUtf8(RuntimeExecutable)));
			const std::filesystem::path shaders = BinaryDirectory / ShadersDirectoryName;
			if (!std::filesystem::is_directory(shaders, error))
				return MissingBinary(std::format("the {} shaders '{}' are not built", name, FileSystem::PathToUtf8(shaders)));
			Redist.clear();
#if defined(ENGINE_PLATFORM_WINDOWS)
			// The app-local CRT that Scripts/Build.py copies next to every Runtime it builds (§2.3).
			const std::filesystem::path redistDirectory = runtimeDirectory / "Redist";
			if (std::filesystem::is_directory(redistDirectory, error))
			{
				ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> files, FileSystem::ListDirectory(redistDirectory, false));
				for (const std::filesystem::path& file : files)
				{
					if (file.extension() == ".dll" && std::filesystem::is_regular_file(file, error))
						Redist.push_back(file);
				}
			}
			if (Redist.empty())
				return MissingBinary(std::format("the app-local CRT of the {} Runtime is missing ('{}' holds no DLL)", name, FileSystem::PathToUtf8(redistDirectory)));
#endif
			return {};
		}

		[[nodiscard]] std::unexpected<Error> MissingBinary(std::string what) const
		{
			return std::unexpected(Error(ErrorCode::NotFound, std::format("{}: build it with {}", what, BuildCommand(Specification.Configuration)))
					.WithHint("exports package what Scripts/Build.py builds; the editor never builds"));
		}

		// The output directory (§14.1) and its siblings: the staging directory, the earlier export's place during the move
		// and the smoke test's user-data directory.
		[[nodiscard]] Status LocateOutput()
		{
			const std::string defaultDirectory = GetDefaultOutputDirectory(Settings.Name, Specification.Configuration);
			if (!Specification.ExportRoot.empty())
			{
				// --export-root (M15) replaces the project's Build/ directory.
				const std::string_view relative = std::string_view(defaultDirectory).substr(Utils::ExportBuildDirectory.size() + 1);
				OutputDirectory = Specification.ExportRoot / FileSystem::PathFromUtf8(relative);
			}
			else
			{
				const std::string_view relative = Specification.OutputDirectory.empty() ? std::string_view(defaultDirectory)
																						: std::string_view(Specification.OutputDirectory);
				OutputDirectory = ProjectRoot / FileSystem::PathFromUtf8(relative);
			}
			OutputDirectory = OutputDirectory.lexically_normal();
			const std::filesystem::path parent = OutputDirectory.parent_path();
			const std::string name = FileSystem::PathToUtf8(OutputDirectory.filename());
			const uint32_t processId = Process::GetCurrentId();
			StagingDirectory = parent / FileSystem::PathFromUtf8(SiblingName(StagingKind, processId, name));
			PreviousDirectory = parent / FileSystem::PathFromUtf8(SiblingName(PreviousKind, processId, name));
			SmokeTestUserData = parent / FileSystem::PathFromUtf8(SiblingName(SmokeTestKind, processId, name));
			return {};
		}

		// --- 2. Cook -----------------------------------------------------------------------------------------------------

		[[nodiscard]] Result<bool> Cook()
		{
			EditorAssetManager& assets = Editor->GetAssets();
			if (!LoadsStarted)
			{
				// The imports run on jobs, from Library/Cache when it holds a valid entry (§14.2 step 2).
				LoadsStarted = true;
				Loads.reserve(Sources.size());
				for (const AssetHandle source : Sources)
					Loads.push_back(assets.LoadAsync(source));
				return false;
			}
			for (const JobHandle<AssetRef<Asset>>& load : Loads)
			{
				if (!load.IsReady())
					return false;
			}
			// Publishes the imports, which stores them in the cache; no import job of the manager runs until this frame ends,
			// so the cache reads below never race a store.
			assets.WaitIdle();
			std::vector<Result<AssetRef<Asset>>> loaded;
			loaded.reserve(Loads.size());
			for (JobHandle<AssetRef<Asset>>& load : Loads)
				loaded.push_back(load.Take());
			Loads.clear();
			for (size_t index = 0; index < loaded.size(); ++index)
			{
				if (!loaded[index].has_value())
					return std::unexpected(std::move(loaded[index]).error().WithContext(std::format("while cooking '{}'", assets.GetReferencePath(Sources[index]))));
			}

			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			const std::vector<ImportAssetLookupEntry> lookups = MakeLookupSnapshot();
			std::map<AssetHandle, std::vector<AssetHandle>> references; // what each included artifact references
			std::set<AssetHandle> packaged;
			for (const AssetHandle source : Sources)
			{
				ENGINE_TRY_ASSIGN(CachedImport cooked, WithContext(ReadCooked(source, importers, lookups), std::format("while cooking '{}'", assets.GetReferencePath(source))));
				const AssetRecord* record = assets.GetRegistry().Find(source);
				ENGINE_ASSERT(record != nullptr, "A cooked source is registered");
				references[source] = std::move(cooked.Import.Dependencies);
				for (ImportedArtifact& artifact : cooked.Import.Artifacts)
				{
					AssetReference reference;
					reference.Kind = AssetReferenceKind::ProjectPath;
					reference.Path = record->SourcePath;
					reference.SubAssetKey = artifact.SubAssetKey;
					packaged.insert(artifact.Handle);
					GameEntries.push_back({
						.Handle = artifact.Handle,
						.Type = std::string(AssetTypeToString(artifact.Type)),
						.Path = FormatAssetReference(reference),
						.Data = std::move(artifact.Cooked),
					});
				}
			}

			ENGINE_TRY(CookEngineAssets(importers, packaged));
			ENGINE_TRY(CheckReferences(references, packaged));
			return true;
		}

		// The lookup snapshot of the registry, as the asset manager takes it for an import (every record, sorted by source
		// path), against which a cached import's lookups must still hold.
		[[nodiscard]] std::vector<ImportAssetLookupEntry> MakeLookupSnapshot() const
		{
			std::vector<ImportAssetLookupEntry> snapshot;
			for (const AssetRecord* record : Editor->GetAssets().GetRegistry().GetRecords())
			{
				snapshot.push_back({
					.SourcePath = record->SourcePath,
					.Handle = record->Metadata.Handle,
					.Kind = record->Metadata.Kind,
					.Type = record->Metadata.Type,
					.Owner = record->Metadata.Owner,
				});
			}
			return snapshot;
		}

		// The cooked artifacts of `source` from Library/Cache (§7.5) under its current key. A missing or stale entry (a source
		// changed since it loaded, a cache that could not be written) is imported again, which stores it.
		[[nodiscard]] Result<CachedImport> ReadCooked(AssetHandle source, const ImporterRegistry& importers, std::span<const ImportAssetLookupEntry> lookups)
		{
			ENGINE_TRY_ASSIGN(std::optional<CachedImport> cached, FindCached(source, importers, lookups));
			if (!cached.has_value())
			{
				ENGINE_TRY(Editor->GetAssets().Reimport(source));
				ENGINE_TRY_ASSIGN(cached, FindCached(source, importers, MakeLookupSnapshot()));
			}
			if (!cached.has_value())
			{
				return MakeError(ErrorCode::Io, "the asset cache holds no current entry for {} after importing it (see the cache warnings in the log)",
					source.ToString());
			}
			if (cached->Import.Artifacts.empty() || cached->Import.Artifacts.front().Handle != source)
				return MakeError(ErrorCode::ImportFailed, "the cached import of {} does not start with its main artifact", source.ToString());
			return std::move(*cached);
		}

		[[nodiscard]] Result<std::optional<CachedImport>> FindCached(AssetHandle source, const ImporterRegistry& importers,
			std::span<const ImportAssetLookupEntry> lookups) const
		{
			EditorAssetManager& assets = Editor->GetAssets();
			VirtualFileSystem& vfs = Editor->GetVfs();
			const AssetRecord* record = assets.GetRegistry().Find(source);
			if (record == nullptr || record->Metadata.Kind != AssetMetaKind::Asset)
				return MakeError(ErrorCode::NotFound, "{} is no longer registered", source.ToString());
			const IAssetImporter* importer = importers.FindById(record->Metadata.Importer);
			if (importer == nullptr)
				return MakeError(ErrorCode::NotFound, "no importer has the id '{}'", record->Metadata.Importer);
			// The cache key of §7.5 (AssetCache::ComputeKey) over the complete settings, as the asset manager computes it.
			VariantValue settings;
			if (!importer->GetSettingsTypeName().empty())
			{
				ENGINE_TRY_ASSIGN(settings, assets.MergeImportSettings(importer->GetId(), record->Metadata.Settings, Json()));
			}
			ENGINE_TRY_ASSIGN(const Buffer sourceBytes, vfs.ReadFile(record->SourcePath));
			const uint64_t key = AssetCache::ComputeKey(sourceBytes, importer->GetId(), importer->GetVersion(), settings.Get(), EngineCookVersion);
			ENGINE_TRY_ASSIGN(const VfsPath cacheRoot, VfsPath::Create(CacheScheme, ""));
			const AssetCache cache(vfs, cacheRoot);
			ENGINE_TRY_ASSIGN(std::optional<CachedImport> cached, cache.Find(source, key));
			if (!cached.has_value() || !IsManifestCurrent(vfs, cached->Reads, cached->Lookups, lookups))
				return std::optional<CachedImport>();
			return cached;
		}

		// The File and Generated built-ins of the catalogue from the engine cooked cache (§7.6), baked on first use. The
		// built-in environments need M8's baker and are left out with a warning; procedural built-ins are generated by the
		// Runtime's asset manager.
		[[nodiscard]] Status CookEngineAssets(const ImporterRegistry& importers, std::set<AssetHandle>& packaged)
		{
			EditorAssetManager& assets = Editor->GetAssets();
			const EngineBakeSpecification specification{
				.Vfs = &Editor->GetVfs(),
				.Importers = &importers,
				.Registry = &Editor->GetTypeRegistry(),
				.Jobs = &Editor->GetEngine().GetJobSystem(),
				.EnvironmentBaker = Editor->GetSpecification().EnvironmentBaker,
				.Generators = GetEngineAssetGenerators(),
			};
			std::vector<std::string> environments;
			for (const BuiltinAssetEntry& entry : assets.GetBuiltins().GetEntries())
			{
				if (entry.Source == BuiltinAssetSource::Procedural)
					continue;
				if (entry.Type == AssetType::Environment)
				{
					environments.push_back(entry.Path);
					continue;
				}
				ENGINE_TRY_ASSIGN(std::vector<Buffer> artifacts, WithContext(GetOrBakeEngineAsset(specification, entry), std::format("while cooking '{}'", entry.Path)));
				if (artifacts.size() != 1)
				{
					return MakeError(ErrorCode::Unsupported, "the built-in '{}' cooks to {} artifacts, and Engine.pak holds one per built-in", entry.Path,
						artifacts.size());
				}
				packaged.insert(entry.Handle);
				EngineEntries.push_back({
					.Handle = entry.Handle,
					.Type = std::string(AssetTypeToString(entry.Type)),
					.Path = entry.Path,
					.Data = std::move(artifacts.front()),
				});
			}
			if (!environments.empty())
			{
				std::string list;
				for (const std::string& path : environments)
					list += std::format("{}'{}'", list.empty() ? "" : ", ", path);
				Report.Warnings.push_back(std::format("Engine.pak leaves out the built-in environments {} until M8 bakes them", list));
			}
			return {};
		}

		// Every handle an included asset references resolves in the package (§14.2 step 1): a Game.pak or Engine.pak entry,
		// or a procedural built-in. A built-in environment (left out until M8) is a warning: the game renders with the
		// ambient fallback.
		[[nodiscard]] Status CheckReferences(const std::map<AssetHandle, std::vector<AssetHandle>>& references, const std::set<AssetHandle>& packaged)
		{
			const EditorAssetManager& assets = Editor->GetAssets();
			const std::span<const BuiltinAssetEntry> procedural = GetProceduralBuiltinEntries();
			std::vector<ErrorIssue> issues;
			std::set<AssetHandle> warned;
			for (const auto& [source, handles] : references)
			{
				const std::string sourcePath = assets.GetReferencePath(source);
				for (const AssetHandle handle : handles)
				{
					if (packaged.contains(handle) || std::ranges::find(procedural, handle, &BuiltinAssetEntry::Handle) != procedural.end())
						continue;
					const std::string referencePath = assets.GetReferencePath(handle);
					const std::string named = referencePath.empty() ? handle.ToString() : std::format("'{}' ({})", referencePath, handle.ToString());
					if (const BuiltinAssetEntry* builtin = assets.GetBuiltins().Find(handle); builtin != nullptr && builtin->Type == AssetType::Environment)
					{
						if (warned.insert(handle).second)
							Report.Warnings.push_back(std::format("'{}' references {}, which Engine.pak leaves out until M8", sourcePath, named));
						continue;
					}
					std::string hint = "restore the asset or remove the reference (project.validate reports ASSET_MISSING)";
					if (const std::optional<AssetRegistry::Location> location = assets.GetRegistry().Locate(handle))
					{
						if (const std::optional<std::string> glob = FindExcludingGlob(location->Record->SourcePath.GetPath()))
							hint = std::format("the Export.Exclude glob '{}' leaves it out: remove the glob or the reference", *glob);
					}
					issues.push_back(MakeIssue("", std::format("'{}' references {}, which the export does not include", sourcePath, named), std::move(hint)));
				}
			}
			if (!issues.empty())
				return std::unexpected(ValidationFailure(std::move(issues)));
			return {};
		}

		// --- 3. Write the paks -------------------------------------------------------------------------------------------

		[[nodiscard]] Result<bool> WritePakFiles()
		{
			if (!PakJob.has_value())
			{
				ENGINE_TRY(CreateStaging());
				ENGINE_TRY_ASSIGN(Json project, ProjectSerializer::ToJson(Settings, Editor->GetTypeRegistry()));
				Json gameMetadata = Json::object();
				gameMetadata["Project"] = std::move(project);
				Json engineMetadata = Json::object();
				engineMetadata["Configuration"] = std::string(ExportConfigurationToString(Specification.Configuration));
				engineMetadata["EngineVersion"] = std::string(EngineVersionString);
				PakJobInput input{
					.EngineEntries = std::move(EngineEntries),
					.GameEntries = std::move(GameEntries),
					.EngineMetadata = std::move(engineMetadata),
					.GameMetadata = std::move(gameMetadata),
					.ShaderDirectory = BinaryDirectory / ShadersDirectoryName,
					.DataDirectory = StagingDirectory / DataDirectoryName,
				};
				EngineEntries.clear();
				GameEntries.clear();
				PakJob = Editor->GetEngine().GetJobSystem().Submit([job = std::move(input)]() mutable -> Result<WrittenPaks>
				{
					return WritePaks(std::move(job));
				});
				return false;
			}
			if (!PakJob->IsReady())
				return false;
			Result<WrittenPaks> written = PakJob->Take();
			PakJob.reset();
			if (!written.has_value())
				return std::unexpected(std::move(written).error());
			Paks = *written;
			Report.GameAssetCount = Paks.GameEntryCount;
			Report.EngineEntryCount = Paks.EngineEntryCount;
			return true;
		}

		// Creates the staging directory beside the output directory (and the directories above it that are missing), after
		// removing what crashed exports into the same directory left there.
		[[nodiscard]] Status CreateStaging()
		{
			const std::filesystem::path parent = StagingDirectory.parent_path();
			std::vector<std::filesystem::path> missing;
			for (std::filesystem::path directory = parent; !directory.empty() && !FileSystem::Exists(directory); directory = directory.parent_path())
			{
				missing.push_back(directory);
				if (directory == directory.parent_path())
					break;
			}
			ENGINE_TRY(FileSystem::CreateDirectories(parent));
			CreatedDirectories.assign(missing.rbegin(), missing.rend());

			ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> siblings, FileSystem::ListDirectory(parent, false));
			const std::string outputName = FileSystem::PathToUtf8(OutputDirectory.filename());
			for (const std::filesystem::path& sibling : siblings)
			{
				const std::optional<uint32_t> owner = ParseSiblingProcessId(FileSystem::PathToUtf8(sibling.filename()), outputName);
				if (!owner.has_value() || *owner == Process::GetCurrentId() || Process::IsRunning(*owner))
					continue;
				if (Status removed = FileSystem::Remove(sibling); !removed.has_value())
					ENGINE_WARN("Cannot remove '{}', left by an earlier export: {}", FileSystem::PathToUtf8(sibling), removed.error());
			}
			if (FileSystem::Exists(StagingDirectory))
			{
				return MakeError(ErrorCode::InvalidState, "another export into '{}' is running in this editor ('{}' exists)",
					FileSystem::PathToUtf8(OutputDirectory), FileSystem::PathToUtf8(StagingDirectory));
			}
			ENGINE_TRY(FileSystem::CreateDirectories(StagingDirectory));
			StagingCreated = true;
			return {};
		}

		// --- 4. Copy the Runtime -----------------------------------------------------------------------------------------

		[[nodiscard]] Result<bool> CopyRuntime()
		{
			const std::filesystem::path executable = StagingDirectory / FileSystem::PathFromUtf8(ExecutableFileName(Settings.Name));
			ENGINE_TRY(WithContext(CopyFile(RuntimeExecutable, executable), "while copying the Runtime"));
			for (const std::filesystem::path& library : Redist)
				ENGINE_TRY(WithContext(CopyFile(library, StagingDirectory / library.filename()), "while copying the app-local CRT"));
			return true;
		}

		// --- 5. Write Game.json ------------------------------------------------------------------------------------------

		[[nodiscard]] Result<bool> WriteManifest()
		{
			GameManifest manifest;
			manifest.Name = Settings.Name;
			manifest.EngineVersion = std::string(EngineVersionString);
			manifest.StartScene = StartScene;
			manifest.Paks = {
				GameManifestPak{ .Path = std::format("{}/{}", DataDirectoryName, EnginePakFileName), .Hash = Paks.EngineHash },
				GameManifestPak{ .Path = std::format("{}/{}", DataDirectoryName, GamePakFileName), .Hash = Paks.GameHash },
			};
			manifest.Window = Settings.Window;
			manifest.Simulation = Settings.Simulation;
			manifest.Testing = false;
			ENGINE_TRY_ASSIGN(const std::string text, GameManifestSerializer::SaveToString(manifest));
			ENGINE_TRY(FileSystem::WriteFileAtomic(StagingDirectory / GameManifest::FileName, AsBytes(text), { .KeepBackup = false }));
			return true;
		}

		// --- 6. Smoke test -----------------------------------------------------------------------------------------------

		[[nodiscard]] Result<bool> RunSmokeTest()
		{
			if (!Specification.SmokeTest)
				return true;
			if (!SmokeTest.has_value())
			{
				std::vector<std::string> arguments = { "--headless", "--frames", std::to_string(Specification.SmokeTestFrames), "--expect-no-errors" };
				const bool hasDevice = Editor->GetEngine().GetGraphicsDevice() != nullptr;
				if (Specification.Configuration == ExportConfiguration::Dist)
				{
					// Dist honours only §13.9's options: no --user-data-dir, no --renderer none.
					if (!hasDevice)
					{
						Report.Warnings.push_back("the smoke test did not run: a Dist Runtime has no --renderer none and needs a graphics device, which "
												  "this editor lacks (--renderer none, or no Vulkan device); export Release to smoke-test without one");
						return true;
					}
				}
				else
				{
					ENGINE_TRY(FileSystem::CreateDirectories(SmokeTestUserData));
					// Absolute, as --user-data-dir requires: the output directory is below the absolute project root or export root.
					arguments.push_back("--user-data-dir");
					arguments.push_back(FileSystem::PathToUtf8(SmokeTestUserData));
					if (!hasDevice)
					{
						arguments.push_back("--renderer");
						arguments.push_back("none");
					}
				}
				const std::filesystem::path executable = StagingDirectory / FileSystem::PathFromUtf8(ExecutableFileName(Settings.Name));
				ENGINE_TRY_ASSIGN(Process process, Process::Spawn({
													   .Executable = executable,
													   .Arguments = std::move(arguments),
													   .WorkingDirectory = StagingDirectory,
													   .Environment = {},
												   }));
				SmokeTest = std::move(process);
				SmokeTestDeadline = std::chrono::steady_clock::now() + SmokeTestTimeout;
				return false;
			}

			if (!SmokeTest->HasExited())
			{
				if (std::chrono::steady_clock::now() < SmokeTestDeadline)
					return false;
				ENGINE_TRY(SmokeTest->Kill());
				// The process is gone; the wait bounds only its output readers' drain, so the error quotes what the game printed.
				Result<ProcessResult> killed = SmokeTest->Wait(SmokeTestDrainTimeout);
				SmokeTest.reset();
				return MakeError(ErrorCode::Timeout, "the smoke test ran longer than {} s and was killed: {}", SmokeTestTimeout.count(),
					killed.has_value() ? TailOfOutput(*killed) : killed.error().ToString());
			}
			ENGINE_TRY_ASSIGN(const ProcessResult result, SmokeTest->Wait(SmokeTestTimeout));
			SmokeTest.reset();
			RemoveSmokeTestUserData();
			Report.SmokeTestRan = true;
			Report.SmokeTestExitCode = result.ExitCode;
			if (result.ExitCode != 0)
			{
				return std::unexpected(Error(ErrorCode::Validation, std::format("the exported game's smoke test exited with code {}: {}", result.ExitCode, TailOfOutput(result)))
						.WithHint("run the exported executable with --headless --frames 300 --expect-no-errors to see why"));
			}
			return true;
		}

		void RemoveSmokeTestUserData() const
		{
			if (!FileSystem::Exists(SmokeTestUserData))
				return;
			if (Status removed = FileSystem::Remove(SmokeTestUserData); !removed.has_value())
				ENGINE_WARN("Cannot remove the smoke test's user-data directory '{}': {}", FileSystem::PathToUtf8(SmokeTestUserData), removed.error());
		}

		// --- 7. Move into the output directory ---------------------------------------------------------------------------

		[[nodiscard]] Result<bool> MoveToOutput()
		{
			ENGINE_TRY(ListPackage());
			if (FileSystem::Exists(OutputDirectory))
			{
				if (FileSystem::Exists(PreviousDirectory))
					ENGINE_TRY(FileSystem::Remove(PreviousDirectory));
				ENGINE_TRY(WithContext(FileSystem::Move(OutputDirectory, PreviousDirectory), "while moving the earlier export aside"));
				if (Status moved = FileSystem::Move(StagingDirectory, OutputDirectory); !moved.has_value())
				{
					if (Status restored = FileSystem::Move(PreviousDirectory, OutputDirectory); !restored.has_value())
					{
						ENGINE_ERROR("Cannot move the earlier export '{}' back to '{}': {}", FileSystem::PathToUtf8(PreviousDirectory),
							FileSystem::PathToUtf8(OutputDirectory), restored.error());
					}
					return std::unexpected(std::move(moved).error());
				}
				if (Status removed = FileSystem::Remove(PreviousDirectory); !removed.has_value())
				{
					Report.Warnings.push_back(
						std::format("the earlier export, moved to '{}', could not be removed: {}", FileSystem::PathToUtf8(PreviousDirectory), removed.error().ToString()));
				}
			}
			else
			{
				ENGINE_TRY(FileSystem::Move(StagingDirectory, OutputDirectory));
			}
			StagingCreated = false;
			CreatedDirectories.clear();
			Report.OutputDirectory = OutputDirectory;
			Report.Executable = OutputDirectory / FileSystem::PathFromUtf8(ExecutableFileName(Settings.Name));
			return true;
		}

		// The report's file list: every file of the staged package with its size and XXH64.
		[[nodiscard]] Status ListPackage()
		{
			ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> entries, FileSystem::ListDirectory(StagingDirectory, true));
			Report.Files.clear();
			for (const std::filesystem::path& entry : entries)
			{
				std::error_code error;
				if (!std::filesystem::is_regular_file(entry, error))
					continue;
				ENGINE_TRY_ASSIGN(const Buffer bytes, FileSystem::ReadFile(entry));
				Report.Files.push_back({
					.Path = FileSystem::PathToUtf8(entry.lexically_relative(StagingDirectory)),
					.Size = bytes.size(),
					.Hash = XXH64(bytes),
				});
			}
			std::ranges::sort(Report.Files, std::less<>(), &ExportedFile::Path);
			return {};
		}

		// --- Failure and cancellation ------------------------------------------------------------------------------------

		// Releases everything the export holds and removes what it wrote: the smoke test's process, the jobs, the staging
		// and smoke-test directories, and the directories it created that are empty again. An earlier export in the output
		// directory is never touched (the move is the last step).
		void Release()
		{
			SmokeTest.reset();
			for (const JobHandle<AssetRef<Asset>>& load : Loads)
			{
				if (load.IsValid())
					static_cast<void>(load.Wait());
			}
			Loads.clear();
			if (PakJob.has_value())
			{
				static_cast<void>(PakJob->Wait());
				PakJob.reset();
			}
			if (StagingCreated && FileSystem::Exists(StagingDirectory))
			{
				if (Status removed = FileSystem::Remove(StagingDirectory); !removed.has_value())
					ENGINE_WARN("Cannot remove the export's staging directory '{}': {}", FileSystem::PathToUtf8(StagingDirectory), removed.error());
			}
			StagingCreated = false;
			if (!SmokeTestUserData.empty())
				RemoveSmokeTestUserData();
			for (auto directory = CreatedDirectories.rbegin(); directory != CreatedDirectories.rend(); ++directory)
			{
				std::error_code error;
				if (std::filesystem::is_empty(*directory, error) && !error)
					static_cast<void>(FileSystem::Remove(*directory));
			}
			CreatedDirectories.clear();
		}
	};

	Exporter::Exporter(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	Exporter::~Exporter()
	{
		Cancel();
	}

	Result<Scope<Exporter>> Exporter::Start(EditorContext& editor, const ExportSpecification& specification)
	{
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "no project is open: open one to export it");
		if (editor.IsReadOnly())
			return MakeError(ErrorCode::InvalidState, "the project is open read-only, and an export writes below its Build/ directory");
		if (specification.ExportRoot.empty())
			ENGINE_TRY(Utils::CheckExportOutputDirectory(specification.OutputDirectory));
		else if (!specification.ExportRoot.is_absolute())
			return MakeError(ErrorCode::InvalidArgument, "the export root '{}' is not an absolute path", FileSystem::PathToUtf8(specification.ExportRoot));
		if (specification.BinaryRoot.empty())
			return MakeError(ErrorCode::InvalidArgument, "the export needs the directory of the build outputs (BinaryRoot, <repository>/bin)");
		if (specification.SmokeTestFrames == 0)
			return MakeError(ErrorCode::InvalidArgument, "the smoke test needs at least one frame");

		Scope<Exporter> exporter = CreateScope<Exporter>(ConstructionKey());
		State& state = *exporter->m_State;
		state.Editor = &editor;
		state.Specification = specification;
		state.ProjectRoot = editor.GetProject().GetRoot();
		state.Started = std::chrono::steady_clock::now();
		state.Report.Configuration = specification.Configuration;
		ENGINE_INFO("Exporting '{}' ({})", editor.GetProject().GetSettings().Name, ExportConfigurationToString(specification.Configuration));
		return exporter;
	}

	std::optional<Result<ExportReport>> Exporter::Poll()
	{
		State& state = *m_State;
		ENGINE_ASSERT(!state.Resolved, "Exporter::Poll is called after the export resolved");
		const ExportPhase phase = state.Phase;
		Result<bool> stepped = state.Step();
		if (!stepped.has_value())
		{
			state.Release();
			state.Resolved = true;
			ENGINE_WARN("The export failed while exporting {}: {}", ExportPhaseToString(phase), stepped.error());
			return Result<ExportReport>(std::unexpected(std::move(stepped).error().WithContext(std::format("while exporting {}", ExportPhaseToString(phase)))));
		}
		if (!*stepped)
			return std::nullopt;
		state.Phase = static_cast<ExportPhase>(std::to_underlying(phase) + 1);
		if (state.Phase != ExportPhase::Done)
			return std::nullopt;

		state.Resolved = true;
		state.Report.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.Started).count();
		ENGINE_INFO("Exported '{}' to '{}' in {:.1f} s ({} files, {} warnings)", state.Settings.Name, FileSystem::PathToUtf8(state.Report.OutputDirectory),
			state.Report.Seconds, state.Report.Files.size(), state.Report.Warnings.size());
		return Result<ExportReport>(std::move(state.Report));
	}

	void Exporter::Cancel()
	{
		State& state = *m_State;
		if (state.Resolved)
			return;
		state.Resolved = true;
		state.Release();
		if (state.Editor != nullptr)
			ENGINE_INFO("Cancelled the export of '{}' while exporting {}", FileSystem::PathToUtf8(state.ProjectRoot), ExportPhaseToString(state.Phase));
	}

	ExportPhase Exporter::GetPhase() const
	{
		return m_State->Phase;
	}

	std::string Exporter::GetDefaultOutputDirectory(std::string_view projectName, ExportConfiguration configuration)
	{
		return std::format("{}/{}-{}/{}", Utils::ExportBuildDirectory, GetPlatformName(), ExportConfigurationToString(configuration), projectName);
	}

	std::string_view Exporter::GetPlatformName()
	{
#if defined(ENGINE_PLATFORM_WINDOWS)
		return "Windows";
#elif defined(ENGINE_PLATFORM_LINUX)
		return "Linux";
#elif defined(ENGINE_PLATFORM_MACOS)
		return "macOS";
#endif
	}

	std::string Exporter::GetBuildOutputDirectoryName(ExportConfiguration configuration)
	{
		// premake5.lua's OutputDir, "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}" (Scripts/Lib/paths.py).
#if defined(ENGINE_PLATFORM_WINDOWS)
		constexpr std::string_view SystemAndArchitecture = "windows-x86_64";
#elif defined(ENGINE_PLATFORM_LINUX)
		constexpr std::string_view SystemAndArchitecture = "linux-x86_64";
#elif defined(ENGINE_PLATFORM_MACOS)
		constexpr std::string_view SystemAndArchitecture = "macosx-AARCH64";
#endif
		return std::format("{}-{}", ExportConfigurationToString(configuration), SystemAndArchitecture);
	}

	std::string_view ExportConfigurationToString(ExportConfiguration configuration)
	{
		switch (configuration)
		{
			case ExportConfiguration::Debug:   return "Debug";
			case ExportConfiguration::Release: return "Release";
			case ExportConfiguration::Dist:    return "Dist";
		}

		ENGINE_ASSERT(false, "Unknown ExportConfiguration {}", std::to_underlying(configuration));
		return "Unknown";
	}

	std::string_view ExportPhaseToString(ExportPhase phase)
	{
		switch (phase)
		{
			case ExportPhase::Validate:      return "Validate";
			case ExportPhase::Cook:          return "Cook";
			case ExportPhase::WritePaks:     return "WritePaks";
			case ExportPhase::CopyRuntime:   return "CopyRuntime";
			case ExportPhase::WriteManifest: return "WriteManifest";
			case ExportPhase::SmokeTest:     return "SmokeTest";
			case ExportPhase::MoveToOutput:  return "MoveToOutput";
			case ExportPhase::Done:          return "Done";
		}

		ENGINE_ASSERT(false, "Unknown ExportPhase {}", std::to_underlying(phase));
		return "Unknown";
	}

}
