#include "EditorPCH.h"
#include "EditorCore/Export/TestingExport.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectValidator.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/PathGlob.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Project/ProjectSettings.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace Engine {

	namespace {

		constexpr std::string_view ProjectScheme = "project";
		constexpr std::string_view ProjectSchemePrefix = "project://";
		constexpr std::string_view AssetsDirectory = "Assets/";
		constexpr std::string_view SuitesField = "Testing.Suites[";
		constexpr std::string_view ReplayExtension = ".replay";

		// A Testing.Suites or Testing.Replays path without the "project://" a setting may spell (PathGlob.h: patterns and
		// paths carry no scheme).
		[[nodiscard]] std::string_view StripProjectScheme(std::string_view path)
		{
			if (path.starts_with(ProjectSchemePrefix))
				path.remove_prefix(ProjectSchemePrefix.size());
			return path;
		}

		// The registered main asset of the project path `path` ("Assets/Scenes/Main.scene" or "project://..."), or null.
		[[nodiscard]] const AssetRecord* FindMainAsset(const EditorContext& editor, std::string_view path)
		{
			const Result<VfsPath> source = VfsPath::Create(ProjectScheme, StripProjectScheme(path));
			const AssetRecord* record = source.has_value() ? editor.GetAssets().GetRegistry().FindBySourcePath(*source) : nullptr;
			return record != nullptr && record->Metadata.Kind == AssetMetaKind::Asset ? record : nullptr;
		}

		// The index of a TEST_SUITE_INVALID diagnostic's entry, from its field "Testing.Suites[<i>]".
		[[nodiscard]] std::optional<size_t> ParseSuiteIndex(std::string_view field)
		{
			if (!field.starts_with(SuitesField) || !field.ends_with(']'))
				return std::nullopt;
			const std::string_view digits = field.substr(SuitesField.size(), field.size() - SuitesField.size() - 1);
			size_t index = 0;
			const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
			if (digits.empty() || error != std::errc() || end != digits.data() + digits.size())
				return std::nullopt;
			return index;
		}

		// The setting a TEST_SUITE_INVALID diagnostic is about: "/Testing/Suites/<i>/Scene" for the scene's (its id was made
		// from the subject "<i>/Scene", ProjectValidator::CheckTestSuites), "/Testing/Suites/<i>/Script" for the script's.
		[[nodiscard]] std::string SuiteIssuePointer(const ProjectDiagnostic& diagnostic)
		{
			const std::optional<size_t> index = ParseSuiteIndex(diagnostic.Field);
			if (!index.has_value())
				return "/Testing/Suites";
			const std::string sceneId = ProjectValidator::MakeDiagnosticId(diagnostic.Code, diagnostic.File, diagnostic.Entity, diagnostic.Component,
				diagnostic.Field, std::format("{}/Scene", *index));
			return std::format("/Testing/Suites/{}/{}", *index, diagnostic.Id == sceneId ? "Scene" : "Script");
		}

		// Whether a Testing.Replays entry, its wildcards aside, is a project path below Assets/, the only entries the test
		// runners accept (the editor's and the Runtime's replay expansion).
		[[nodiscard]] bool IsConfinedReplayGlob(std::string_view pattern)
		{
			std::string checked(pattern);
			std::ranges::replace(checked, '*', 'x');
			std::ranges::replace(checked, '?', 'x');
			const Result<VfsPath> path = VfsPath::Create(ProjectScheme, checked);
			return path.has_value() && path->GetPath().starts_with(AssetsDirectory);
		}

		[[nodiscard]] ErrorIssue MakeIssue(std::string pointer, std::string message, std::string hint)
		{
			return ErrorIssue{ .JsonPointer = std::move(pointer), .Message = std::move(message), .Hint = std::move(hint), .Suggestions = {} };
		}

		[[nodiscard]] std::vector<std::string> SortedUnique(std::set<std::string> paths)
		{
			return { paths.begin(), paths.end() };
		}

	}

	Result<TestingExportContent> CollectTestingExportContent(EditorContext& editor, const ProjectSettings& settings, std::vector<ErrorIssue>& issues)
	{
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "no project is open, so there is nothing to export for testing");
		if (editor.GetScriptService() == nullptr)
		{
			return MakeError(ErrorCode::InvalidState, "the test suites of project '{}' cannot be checked without the script service; repair the script "
													  "checker configuration before exporting",
				settings.Name);
		}

		TestingExportContent content;
		content.SuiteCount = static_cast<uint32_t>(settings.Testing.Suites.size());

		// The suites: the one rule of project.validate (TEST_SUITE_INVALID), located at each entry's setting.
		ENGINE_TRY_ASSIGN(const std::vector<ProjectDiagnostic> diagnostics, ProjectValidator::CheckTestSuites(editor));
		for (const ProjectDiagnostic& diagnostic : diagnostics)
			issues.push_back(MakeIssue(SuiteIssuePointer(diagnostic), std::format("{}: {}", diagnostic.Code, diagnostic.Message), diagnostic.Hint));
		std::set<std::string> scripts;
		std::set<std::string> scenes;
		for (const TestSuiteSettings& suite : settings.Testing.Suites)
		{
			if (const AssetRecord* script = FindMainAsset(editor, suite.Script); script != nullptr && script->Metadata.Type == AssetType::Script)
				scripts.insert(std::string(script->SourcePath.GetPath()));
			if (suite.Scene.empty())
				continue;
			if (const AssetRecord* scene = FindMainAsset(editor, suite.Scene); scene != nullptr && scene->Metadata.Type == AssetType::Scene)
				scenes.insert(std::string(scene->SourcePath.GetPath()));
		}
		content.SuiteScripts = SortedUnique(std::move(scripts));
		content.SuiteScenes = SortedUnique(std::move(scenes));

		// The replays: every registered .replay below Assets/ a glob matches, as the editor's and the Runtime's expansions
		// match them (MatchesPathGlob), each attributed to the first entry that matched it.
		std::vector<const AssetRecord*> registered;
		for (const AssetRecord* record : editor.GetAssets().GetRegistry().GetRecords())
		{
			const std::string_view path = record->SourcePath.GetPath();
			if (record->Metadata.Kind == AssetMetaKind::Asset && record->Metadata.Type == AssetType::Replay && path.starts_with(AssetsDirectory)
				&& path.ends_with(ReplayExtension))
			{
				registered.push_back(record);
			}
		}
		std::map<std::string, std::pair<AssetHandle, size_t>> replays; // path -> (handle, the first Testing.Replays entry)
		for (size_t index = 0; index < settings.Testing.Replays.size(); ++index)
		{
			const std::string& glob = settings.Testing.Replays[index];
			const std::string pointer = std::format("/Testing/Replays/{}", index);
			const std::string_view pattern = StripProjectScheme(glob);
			if (!IsConfinedReplayGlob(pattern))
			{
				issues.push_back(MakeIssue(pointer, std::format("the Testing.Replays entry '{}' is not a project path below Assets/, which the test runners refuse", glob),
					"spell replays as project-relative globs such as \"Assets/Tests/Replays/*.replay\""));
				continue;
			}
			bool matched = false;
			for (const AssetRecord* record : registered)
			{
				const std::string path(record->SourcePath.GetPath());
				if (!MatchesPathGlob(pattern, path))
					continue;
				matched = true;
				replays.try_emplace(path, record->Metadata.Handle, index);
			}
			// A glob that matches nothing is not an issue (as at run time); a plain path must name a replay.
			if (!matched && glob.find_first_of("*?") == std::string::npos)
			{
				issues.push_back(MakeIssue(pointer, std::format("the Testing.Replays entry '{}' names no registered replay", glob),
					"record the replay (input.record) or correct the entry (project.setSettings)"));
			}
		}

		// Every packaged replay imports, which compiles each expectation with ReplayImporter (§14.2 step 1 "every replay
		// compiles"), so the Runtime only ever runs precompiled expectations.
		EditorAssetManager& assets = editor.GetAssets();
		for (const auto& [path, replay] : replays)
		{
			const auto& [handle, index] = replay;
			const Result<AssetRef<Asset>> loaded = assets.Load(handle);
			if (!loaded.has_value())
			{
				if (loaded.error().GetCode() == ErrorCode::InvalidState)
					return std::unexpected(Error(loaded.error()).WithContext(std::format("while importing the replay '{}'", path)));
				issues.push_back(MakeIssue(std::format("/Testing/Replays/{}", index),
					std::format("the replay '{}' does not import: {}", path, loaded.error().ToString()),
					"correct the replay's Expect entries (each is a Luau expression or chunk), or record it again"));
				continue;
			}
			content.Replays.push_back(path);
		}
		return content;
	}

	bool IsForcedByTestingExport(std::string_view projectPath, const TestingExportContent& content)
	{
		if (projectPath.starts_with(TestingExportForcedDirectory))
			return true;
		for (const std::vector<std::string>* paths : { &content.SuiteScripts, &content.SuiteScenes, &content.Replays })
		{
			if (std::ranges::find(*paths, projectPath) != paths->end())
				return true;
		}
		return false;
	}

}
