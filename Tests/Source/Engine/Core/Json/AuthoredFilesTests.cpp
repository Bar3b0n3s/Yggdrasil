#include "TestsPCH.h"

#include "Engine/Core/Error.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

// Architecture §6 "Load -> save is byte-identical ... for every authored file under Projects/ and Tests/Data" at the JSON
// level (ADR 0003 decision 27): every authored file is canonical JsonWriter output. Line-oriented logs (.jsonl) and
// project Automation/ folders are exempt, and so are the files a validation fixture lists as deliberately malformed in its
// Expected.json "NonCanonical" (Docs/Decisions/0022-m14-contract.md decision 11). Projects/ joined the walk in M14 (ADR 0006
// decision 13).

namespace Engine {

	namespace {

		// Authored-format extensions (§6), as in Scripts/ModuleRules.json "Json".
		const std::vector<std::string> AuthoredExtensions = { ".eproj", ".scene", ".prefab", ".material", ".sfx", ".meta", ".replay" };

		// Whether `text` is its own canonical JsonWriter form.
		void CheckCanonical(const std::string& label, const std::string& text)
		{
			INFO(label);
			const Result<Json> parsed = JsonReader::Parse(text);
			REQUIRE_MESSAGE(parsed.has_value(), parsed.error().ToString());
			const Result<std::string> written = JsonWriter::Write(*parsed);
			REQUIRE(written.has_value());
			CHECK(*written == text);
		}

		// Tests/Data-relative paths of every file the validation fixtures list as NonCanonical: Validation/<Name>/Project/<path>.
		std::set<std::string> ListNonCanonicalFiles()
		{
			std::set<std::string> files;
			const auto expected = Test::ListTestDataFiles("Validation", { ".json" });
			REQUIRE_MESSAGE(expected.has_value(), expected.error().ToString());
			for (const std::string& file : *expected)
			{
				if (!file.ends_with("/Expected.json") || std::ranges::count(file, '/') != 2)
					continue;
				const auto text = Test::ReadTestDataText(file);
				REQUIRE(text.has_value());
				const auto document = JsonReader::Parse(*text);
				REQUIRE(document.has_value());
				const auto list = JsonReader(*document).GetMember("NonCanonical");
				REQUIRE(list.has_value());
				const auto size = list->GetArraySize();
				REQUIRE(size.has_value());
				const std::string directory = file.substr(0, file.size() - std::string_view("Expected.json").size());
				for (size_t index = 0; index < *size; ++index)
				{
					const auto entry = list->GetElement(index);
					REQUIRE(entry.has_value());
					const auto path = entry->ReadString();
					REQUIRE(path.has_value());
					files.insert(directory + "Project/" + *path);
				}
			}
			return files;
		}

		// The repository directories whose projects are authored input (Scripts/Lib/games.py GAME_PROJECT_ROOTS): Projects/
		// (FeatureTest and the demo games) and Tests/Projects/ (the M16 dry run, Docs/Decisions/0026-m16-contract.md
		// decision 8).
		const std::vector<std::string> ProjectRoots = { "Projects", "Tests/Projects" };

		// Every file below the repository directory `root`, as paths that start with it; a root that does not exist holds
		// none (Tests/Projects/ has no project before the dry run builds one).
		std::vector<std::filesystem::path> ListProjectTreeFiles(const std::string& root)
		{
			const std::filesystem::path directory = Test::GetRepositoryRoot() / FileSystem::PathFromUtf8(root);
			const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(directory, true);
			if (!entries.has_value() && entries.error().GetCode() == ErrorCode::NotFound)
				return {};
			REQUIRE_MESSAGE(entries.has_value(), entries.error().ToString());
			std::vector<std::filesystem::path> files;
			for (const std::filesystem::path& path : *entries)
			{
				std::error_code error;
				if (!std::filesystem::is_directory(path, error))
					files.push_back(path);
			}
			return files;
		}

	}

	TEST_SUITE("Core")
	{
		TEST_CASE("AuthoredFiles: every authored file under Tests/Data is canonical JSON")
		{
			const Result<std::vector<std::string>> files = Test::ListTestDataFiles("", AuthoredExtensions);
			REQUIRE(files.has_value());
			const std::set<std::string> nonCanonical = ListNonCanonicalFiles();
			std::set<std::string> exempted;
			size_t checked = 0;
			for (const std::string& file : *files)
			{
				// Lint fixtures are seeded defects of their own kind, never engine input; the provenance-audit fixtures are byte-hash
				// inputs of Scripts/AuditProvenance.py, some edited by hand on purpose (Docs/Decisions/0026-m16-contract.md decision 6).
				if (file.starts_with("Lint/") || file.starts_with("Provenance/") || file.find("/Automation/") != std::string::npos)
					continue;
				if (nonCanonical.contains(file))
				{
					exempted.insert(file);
					continue;
				}
				const Result<std::string> text = Test::ReadTestDataText(file);
				REQUIRE(text.has_value());
				CheckCanonical(file, *text);
				++checked;
			}
			CHECK(checked >= 15);
			// Every exemption names a real, deliberately malformed file.
			CHECK(exempted == nonCanonical);
			for (const std::string& file : nonCanonical)
			{
				CAPTURE(file);
				const Result<std::string> text = Test::ReadTestDataText(file);
				REQUIRE(text.has_value());
				const Result<Json> parsed = JsonReader::Parse(*text);
				const bool canonical = parsed.has_value() && JsonWriter::Write(*parsed).value_or(std::string()) == *text;
				CHECK_FALSE(canonical);
			}
		}

		TEST_CASE("AuthoredFiles: every authored file under Projects is canonical JSON")
		{
			// Projects/ and, from M16, Tests/Projects/ (ADR 0026 decision 8), with the same skips.
			const std::filesystem::path repository = Test::GetRepositoryRoot();
			std::vector<std::string> files;
			size_t underProjects = 0;
			for (const std::string& root : ProjectRoots)
			{
				const std::filesystem::path base = repository / FileSystem::PathFromUtf8(root);
				for (const std::filesystem::path& path : ListProjectTreeFiles(root))
				{
					std::string relative = FileSystem::PathToUtf8(path.lexically_relative(base));
					std::ranges::replace(relative, '\\', '/');
					// Library/ is the editor's cache and Automation/ holds logs; batch scaffolds (.jsonl) are line-oriented.
					const size_t project = relative.find('/');
					const std::string_view inside = project == std::string::npos ? std::string_view() : std::string_view(relative).substr(project + 1);
					if (inside.starts_with("Library/") || inside.starts_with("Build/") || inside.starts_with("Automation/"))
						continue;
					const std::string extension = FileSystem::PathToUtf8(path.extension());
					if (std::ranges::find(AuthoredExtensions, extension) == AuthoredExtensions.end())
						continue;
					files.push_back(root + "/" + relative);
					if (root == "Projects")
						++underProjects;
				}
			}
			std::ranges::sort(files);
			for (const std::string& file : files)
			{
				const Result<std::string> text = FileSystem::ReadText(repository / FileSystem::PathFromUtf8(file));
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				CheckCanonical(file, *text);
			}
			// The floor proves the walk finds FeatureTest; it counts Projects/ only, since Tests/Projects/ may be empty.
			CHECK(underProjects >= 100);
		}

		// §6.7 (ADR 0022 "Integration" fix 4; review findings M22 and M26): a replay header's Config is the run mode of the
		// recording process, never the build configuration, so no committed replay, fixture or project, shows "Debug" or
		// another value the format no longer produces.
		TEST_CASE("AuthoredFiles: every committed replay records a run mode as its Config")
		{
			std::vector<std::filesystem::path> replays;
			const Result<std::vector<std::string>> data = Test::ListTestDataFiles("", { ".replay" });
			REQUIRE(data.has_value());
			for (const std::string& file : *data)
			{
				if (!file.starts_with("Lint/"))
					replays.push_back(Test::GetTestDataPath(file));
			}
			// Projects/ and, from M16, Tests/Projects/ (ADR 0026 decision 8).
			for (const std::string& root : ProjectRoots)
			{
				const std::filesystem::path base = Test::GetRepositoryRoot() / FileSystem::PathFromUtf8(root);
				for (const std::filesystem::path& path : ListProjectTreeFiles(root))
				{
					// A project's Library/ (the editor's cache) and Build/ (exports) are not committed.
					const std::filesystem::path relative = path.lexically_relative(base);
					const bool committed = std::ranges::none_of(relative, [](const std::filesystem::path& part)
					{
						return part == "Library" || part == "Build";
					});
					if (path.extension() == ".replay" && committed)
						replays.push_back(path);
				}
			}
			std::ranges::sort(replays);
			for (const std::filesystem::path& replay : replays)
			{
				CAPTURE(FileSystem::PathToUtf8(replay));
				const Result<std::string> text = FileSystem::ReadText(replay);
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				const Result<Json> document = JsonReader::Parse(*text);
				REQUIRE_MESSAGE(document.has_value(), document.error().ToString());
				const Result<std::string> config = JsonReader(*document).ReadMember<std::string>("Config");
				REQUIRE_MESSAGE(config.has_value(), config.error().ToString());
				CHECK((*config == "Editor" || *config == "Release" || *config == "Dist"));
			}
			CHECK(replays.size() >= 3);
		}
	}

}
