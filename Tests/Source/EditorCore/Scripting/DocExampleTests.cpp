#include "TestsPCH.h"

#include "EditorCore/Scripting/ScriptTypeChecker.h"

#include "EditorCore/Automation/ObserveMethods.h"
#include "Engine/Automation/Protocol/ResultOffload.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/DocExamples.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// ADR 0022 decision 14: at least this many blocks must be checked, so a broken extractor cannot pass vacuously. Stream
		// D sets it to the count at completion; it is raised as documents grow and lowered only with a reason in the review.
		constexpr size_t MinimumCheckedBlocks = 5;

		// The size the dispatcher compares with its offload threshold (DefaultOffloadThresholdBytes, §13.10) for the docs.get
		// result of the skill `name` with the text `content`: the minified JSON of the serialized result without "_meta"
		// (Dispatcher.cpp), so every newline, tab, quote and backslash of the text counts as its escape, and the topic and
		// path members count too. §13.11 step 1 fetches the game-building skill inline, which needs it below the threshold.
		size_t MeasureDocsGetResult(std::string_view name, const std::string& content)
		{
			TypeRegistry types;
			RegisterEditorObserveMethodTypes(types);
			types.Freeze();
			const StructInfo* type = types.FindStruct<DocsGetResult>();
			REQUIRE(type != nullptr);
			DocsGetResult result;
			result.Topic = std::format("skills/{}", name);
			result.Content = content;
			result.Path = std::format(".claude/skills/{}/SKILL.md", name);
			const Result<Json> serialized = type->ToJson(&result);
			REQUIRE_MESSAGE(serialized.has_value(), serialized.error().ToString());
			return serialized->dump(-1, ' ', false, Json::error_handler_t::replace).size();
		}

		// The documents of the tree at `root` (the repository) whose Luau blocks are checked, root-relative and sorted:
		// Docs/*.md, Docs/Reference/*.md, AGENTS.md, Projects/*/AGENTS.md, Tests/Projects/*/AGENTS.md (the dry-run game's
		// notes, Docs/Decisions/0026-m16-contract.md decision 13; the directory may not exist yet),
		// Resources/Templates/**/*.md and .claude/skills/*/SKILL.md (Docs/Decisions/ is history and is not checked).
		std::vector<std::string> ListCheckedDocuments(const std::filesystem::path& root)
		{
			std::vector<std::string> documents;
			const auto relative = [&root](const std::filesystem::path& path)
			{
				std::string text = FileSystem::PathToUtf8(path.lexically_relative(root));
				std::ranges::replace(text, '\\', '/');
				return text;
			};
			const auto addMarkdown = [&](const std::filesystem::path& directory, bool recursive)
			{
				const auto files = FileSystem::ListDirectory(directory, recursive);
				REQUIRE_MESSAGE(files.has_value(), (files ? std::string() : files.error().ToString()));
				for (const std::filesystem::path& file : *files)
				{
					std::error_code error;
					if (file.extension() == std::filesystem::path(".md") && std::filesystem::is_regular_file(file, error))
						documents.push_back(relative(file));
				}
			};
			const auto addFile = [&](const std::filesystem::path& file)
			{
				std::error_code error;
				if (std::filesystem::is_regular_file(file, error))
					documents.push_back(relative(file));
			};
			addMarkdown(root / "Docs", false);
			addMarkdown(root / "Docs" / "Reference", false);
			addFile(root / "AGENTS.md");
			addMarkdown(root / "Resources" / "Templates", true);
			// Each directory, the file each of its subdirectories holds, and whether the directory may be missing.
			struct DocumentDirectory
			{
				std::string_view Directory{};
				std::string_view File{};
				bool Optional = false;
			};
			constexpr DocumentDirectory Directories[] = {
				{ .Directory = "Projects", .File = "AGENTS.md", .Optional = false },
				{ .Directory = "Tests/Projects", .File = "AGENTS.md", .Optional = true },
				{ .Directory = ".claude/skills", .File = "SKILL.md", .Optional = false },
			};
			for (const DocumentDirectory& directory : Directories)
			{
				const std::filesystem::path path = root / FileSystem::PathFromUtf8(directory.Directory);
				std::error_code error;
				if (directory.Optional && !std::filesystem::is_directory(path, error))
					continue;
				const auto entries = FileSystem::ListDirectory(path, false);
				REQUIRE_MESSAGE(entries.has_value(), (entries ? std::string() : entries.error().ToString()));
				for (const std::filesystem::path& entry : *entries)
					addFile(entry / FileSystem::PathFromUtf8(directory.File));
			}
			std::ranges::sort(documents);
			documents.erase(std::unique(documents.begin(), documents.end()), documents.end());
			return documents;
		}

		// A game's own notes (Projects/*/AGENTS.md, Tests/Projects/*/AGENTS.md): checked for type errors like every other
		// document, but not for lint warnings, since the games suite's script.check never reads them and the notes of an
		// unstaged demo are written by its builder, which has no tool that lints them (Docs/Decisions/0027-m17-contract.md
		// decision 5, C1).
		bool IsProjectNotes(std::string_view document)
		{
			return document.ends_with("/AGENTS.md") && (document.starts_with("Projects/") || document.starts_with("Tests/Projects/"));
		}

		std::string DescribeFinding(std::string_view document, const Test::DocExampleFinding& finding)
		{
			const std::string where = finding.DocumentLine != 0 ? std::format("{}:{}", document, finding.DocumentLine)
																: std::format("{} (from {})", finding.Elsewhere, document);
			return std::format("{}: {}: {}", where, finding.Diagnostic.Code, finding.Diagnostic.Message);
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Roadmap M14 acceptance; Architecture §11.9 (ADR 0022 decision 14). Every fenced block whose info string starts with
		// lua or luau in Docs/*.md, Docs/Reference/*.md, AGENTS.md, Projects/*/AGENTS.md, Tests/Projects/*/AGENTS.md,
		// Resources/Templates/**/*.md and .claude/skills/*/SKILL.md (Docs/Decisions/ excluded) is checked with the engine's
		// strict checker at its path=
		// (default Assets/Scripts/DocExample.luau), wrapped by its prelude= stub, unless it carries skip="<reason>". Stub
		// modules and preludes live in Tests/Data/TypeChecker/DocExamples/. Zero Error findings pass; at least
		// MinimumCheckedBlocks blocks must be checked.
		TEST_CASE("DocExamples: every Luau example in the docs type-checks")
		{
			std::error_code error;
			REQUIRE(std::filesystem::is_directory(Test::GetTestDataPath("TypeChecker/DocExamples/Assets"), error));
			const Test::DocExampleChecker checker(Test::GetTestDataPath("TypeChecker/DocExamples"));
			size_t checked = 0;
			std::vector<std::string> failures;
			for (const std::string& document : ListCheckedDocuments(Test::GetRepositoryRoot()))
			{
				const Result<std::string> text = FileSystem::ReadText(Test::GetRepositoryRoot() / FileSystem::PathFromUtf8(document));
				REQUIRE_MESSAGE(text.has_value(), (text ? std::string() : text.error().ToString()));
				const auto blocks = Test::ExtractDocExamples(*text);
				if (!blocks)
				{
					failures.push_back(std::format("{}:{}: {}", document, blocks.error().GetLocation().Line, blocks.error().GetMessageText()));
					continue;
				}
				for (const Test::DocExampleBlock& block : *blocks)
				{
					if (block.Skip.has_value())
						continue;
					const auto findings = checker.Check(block);
					if (!findings)
					{
						failures.push_back(std::format("{}:{}: {}", document, block.FenceLine, findings.error().ToString()));
						continue;
					}
					++checked;
					for (const Test::DocExampleFinding& finding : *findings)
					{
						// Lint warnings are the next case's (Docs/Decisions/0027-m17-contract.md decision 5, C1).
						if (finding.Diagnostic.Severity == DiagnosticSeverity::Error)
							failures.push_back(DescribeFinding(document, finding));
					}
				}
			}
			std::string report;
			for (const std::string& failure : failures)
				report += failure + "\n";
			INFO(report);
			CHECK(failures.empty());
			CAPTURE(checked);
			CHECK(checked >= MinimumCheckedBlocks);
		}

		// Docs/Decisions/0027-m17-contract.md decision 5, C1 (M17 gameplay map GAF-10): the games suite fails a demo on any
		// script.check diagnostic, lint warnings included, while the case above counts only errors, so Architecture §13.11
		// taught `then self:HardDrop() return end`, two statements on one line that Luau's SameLineStatement lint warns
		// about. DocExampleChecker::Check reports Warning findings too: a seeded block proves a lint warning comes back at
		// its document line, then no example of the checked documents may warn, except in a game's own notes
		// (IsProjectNotes), which only the case above checks.
		TEST_CASE("DocExamples: every Luau example in the docs is free of lint warnings")
		{
			const Test::DocExampleChecker checker(Test::GetTestDataPath("TypeChecker/DocExamples"));
			const std::string_view seeded = "# Seeded\n\n```luau\nlocal function clamp(value: number): number\n\tif value < 0 then value = 0 return value end\n\treturn value\nend\nreturn clamp\n```\n";
			const auto seededBlocks = Test::ExtractDocExamples(seeded);
			REQUIRE_MESSAGE(seededBlocks.has_value(), (seededBlocks ? std::string() : seededBlocks.error().ToString()));
			REQUIRE(seededBlocks->size() == 1);
			const auto seededFindings = checker.Check(seededBlocks->front());
			REQUIRE_MESSAGE(seededFindings.has_value(), (seededFindings ? std::string() : seededFindings.error().ToString()));
			const auto warning = std::ranges::find_if(*seededFindings, [](const Test::DocExampleFinding& finding)
			{
				return finding.Diagnostic.Severity == DiagnosticSeverity::Warning;
			});
			REQUIRE_MESSAGE(warning != seededFindings->end(), "the seeded block's SameLineStatement warning was not reported");
			CHECK(warning->DocumentLine == 5);
			CHECK(warning->Diagnostic.Message.find("same line") != std::string::npos);
			CHECK(std::ranges::none_of(*seededFindings, [](const Test::DocExampleFinding& finding)
			{
				return finding.Diagnostic.Severity == DiagnosticSeverity::Error;
			}));

			size_t checked = 0;
			std::vector<std::string> failures;
			for (const std::string& document : ListCheckedDocuments(Test::GetRepositoryRoot()))
			{
				if (IsProjectNotes(document))
					continue;
				const Result<std::string> text = FileSystem::ReadText(Test::GetRepositoryRoot() / FileSystem::PathFromUtf8(document));
				REQUIRE_MESSAGE(text.has_value(), (text ? std::string() : text.error().ToString()));
				const auto blocks = Test::ExtractDocExamples(*text);
				REQUIRE_MESSAGE(blocks.has_value(), std::format("{}: {}", document, blocks ? std::string() : blocks.error().ToString()));
				for (const Test::DocExampleBlock& block : *blocks)
				{
					if (block.Skip.has_value())
						continue;
					const auto findings = checker.Check(block);
					REQUIRE_MESSAGE(findings.has_value(), std::format("{}:{}: {}", document, block.FenceLine, findings ? std::string() : findings.error().ToString()));
					++checked;
					for (const Test::DocExampleFinding& finding : *findings)
					{
						if (finding.Diagnostic.Severity == DiagnosticSeverity::Warning)
							failures.push_back(DescribeFinding(document, finding));
					}
				}
			}
			std::string report;
			for (const std::string& failure : failures)
				report += failure + "\n";
			INFO(report);
			CHECK(failures.empty());
			CAPTURE(checked);
			CHECK(checked >= MinimumCheckedBlocks);
		}

		// Docs/Decisions/0026-m16-contract.md decision 13: the dry-run game lives under Tests/Projects/, and the notes its
		// builder writes there are checked like a demo's under Projects/; the directory may be missing (before the dry run).
		TEST_CASE("DocExamples: the AGENTS.md of every project under Projects/ and Tests/Projects/ is checked")
		{
			Test::TempDirectory tree("DocExampleDocuments");
			const std::string_view notes = "# Notes\n\n```luau\nlocal speed: number = 8\n```\n";
			for (const std::string_view directory : { "Docs/Reference", "Resources/Templates", ".claude/skills/a-skill", "Projects/Game" })
				REQUIRE(FileSystem::CreateDirectories(tree / directory).has_value());
			for (const std::string_view file : { "Projects/Game/AGENTS.md", ".claude/skills/a-skill/SKILL.md" })
				REQUIRE(FileSystem::WriteFileAtomic(tree / file, AsBytes(notes)).has_value());
			CHECK(ListCheckedDocuments(tree.GetPath()) == std::vector<std::string>{ ".claude/skills/a-skill/SKILL.md", "Projects/Game/AGENTS.md" });

			REQUIRE(FileSystem::CreateDirectories(tree / "Tests/Projects/Breakout").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(tree / "Tests/Projects/Breakout/AGENTS.md", AsBytes(notes)).has_value());
			CHECK(ListCheckedDocuments(tree.GetPath())
				== std::vector<std::string>{ ".claude/skills/a-skill/SKILL.md", "Projects/Game/AGENTS.md", "Tests/Projects/Breakout/AGENTS.md" });
		}

		// Tests/Data/TypeChecker/DocExamples/Seeded/Seeded.md holds three examples with a type error each: one wrapped by the
		// Behaviour prelude, reported at its document line through the fence and prelude offsets; one without a prelude,
		// through the fence offset alone; and one whose mistake surfaces in the prelude's epilogue (after its marker),
		// reported at that line of the prelude file (review finding M16: the prelude arithmetic was never exercised).
		// Expected.json lists Error findings only; lint warnings are judged by the lint case above (Docs/Decisions/
		// 0027-m17-contract.md decision 5, C1).
		TEST_CASE("DocExamples: a seeded mistake is reported at its document line")
		{
			const auto seeded = Test::ReadTestDataText("TypeChecker/DocExamples/Seeded/Seeded.md");
			REQUIRE_MESSAGE(seeded.has_value(), seeded.error().ToString());
			const auto expectedText = Test::ReadTestDataText("TypeChecker/DocExamples/Seeded/Expected.json");
			REQUIRE_MESSAGE(expectedText.has_value(), expectedText.error().ToString());
			const auto expectedDocument = JsonReader::Parse(*expectedText);
			REQUIRE(expectedDocument.has_value());
			const JsonReader expected(*expectedDocument);
			REQUIRE(expected.ReadFormatHeader("DocExampleSeed", 1, 1).has_value());

			const auto blocks = Test::ExtractDocExamples(*seeded);
			REQUIRE_MESSAGE(blocks.has_value(), blocks.error().ToString());
			const Test::DocExampleChecker checker(Test::GetTestDataPath("TypeChecker/DocExamples"));
			size_t checked = 0;
			size_t skipped = 0;
			std::vector<Test::DocExampleFinding> findings;
			for (const Test::DocExampleBlock& block : *blocks)
			{
				if (block.Skip.has_value())
				{
					++skipped;
					continue;
				}
				const auto blockFindings = checker.Check(block);
				REQUIRE_MESSAGE(blockFindings.has_value(), blockFindings.error().ToString());
				++checked;
				std::ranges::copy_if(*blockFindings, std::back_inserter(findings), [](const Test::DocExampleFinding& finding)
				{
					return finding.Diagnostic.Severity == DiagnosticSeverity::Error;
				});
			}
			CHECK(expected.ReadMember<uint32_t>("Checked") == static_cast<uint32_t>(checked));
			CHECK(expected.ReadMember<uint32_t>("Skipped") == static_cast<uint32_t>(skipped));
			const auto expectedFindings = expected.GetMember("Findings");
			REQUIRE(expectedFindings.has_value());
			std::string actual;
			for (const Test::DocExampleFinding& finding : findings)
				actual += DescribeFinding("Seeded.md", finding) + "\n";
			INFO(actual);
			REQUIRE(expectedFindings->GetArraySize() == findings.size());
			for (size_t index = 0; index < findings.size(); ++index)
			{
				const auto entry = expectedFindings->GetElement(index);
				REQUIRE(entry.has_value());
				CHECK(entry->ReadMember<uint32_t>("Line") == findings[index].DocumentLine);
				CHECK(entry->ReadMember<std::string>("Elsewhere") == findings[index].Elsewhere);
				CHECK(entry->ReadMember<std::string>("Code") == findings[index].Diagnostic.Code);
				const auto message = entry->ReadMember<std::string>("MessageContains");
				REQUIRE(message.has_value());
				CHECK(findings[index].Diagnostic.Message.find(*message) != std::string::npos);
			}
		}

		TEST_CASE("DocExamples: fence attributes are parsed and malformed ones are located")
		{
			const auto blocks = Test::ExtractDocExamples(
				"Text\n\n  ```luau path=Assets/Tests/A.test.luau prelude=Behaviour\n  local a = 1\n    local b = 2\n  ```\n"
				"````markdown\n```lua\nnot = an example\n```\n````\n~~~lua skip=\"an excerpt\"\nlocal = 1\n~~~\n```c++\nint x;\n```\n");
			REQUIRE_MESSAGE(blocks.has_value(), blocks.error().ToString());
			REQUIRE(blocks->size() == 2);
			CHECK((*blocks)[0].FenceLine == 3);
			CHECK((*blocks)[0].Path == "Assets/Tests/A.test.luau");
			CHECK((*blocks)[0].Prelude == "Behaviour");
			CHECK_FALSE((*blocks)[0].Skip.has_value());
			CHECK((*blocks)[0].Source == "local a = 1\n  local b = 2\n");
			CHECK((*blocks)[1].FenceLine == 12);
			CHECK((*blocks)[1].Path == Test::DefaultDocExamplePath);
			CHECK((*blocks)[1].Skip == std::optional<std::string>("an excerpt"));

			for (const auto& [text, line] : { std::pair{ "a\n```luau unknown=1\n```\n", 2u }, std::pair{ "```luau path\n```\n", 1u },
					 std::pair{ "```luau skip=\"\"\n```\n", 1u }, std::pair{ "```luau path=Scripts/A.luau\n```\n", 1u },
					 std::pair{ "```luau path=Assets/A.lua\n```\n", 1u }, std::pair{ "x\ny\n```luau\nlocal a = 1\n", 3u },
					 std::pair{ "```luau skip=\"unclosed\n```\n", 1u } })
			{
				CAPTURE(std::string(text));
				const auto malformed = Test::ExtractDocExamples(text);
				REQUIRE_FALSE(malformed.has_value());
				CHECK(malformed.error().GetCode() == ErrorCode::Validation);
				CHECK(malformed.error().GetLocation().Line == line);
			}
		}

		// ADR 0022 decision 17: every skill opens with the frontmatter the docs.get parser reads (name equal to its directory,
		// a single-line description) and is small enough for docs.get to return inline: its serialized result, which the
		// dispatcher measures, stays within the offload threshold (review finding M21: the raw text's size is not it).
		TEST_CASE("DocExamples: every skill document has the docs.get frontmatter and fits inline")
		{
			// A text below the threshold in raw bytes whose characters JSON escapes is still offloaded, so the raw size
			// cannot stand in for the measure.
			const std::string escaped(DefaultOffloadThresholdBytes - 1024, '\n');
			CHECK(MeasureDocsGetResult("game-building", escaped) > DefaultOffloadThresholdBytes);
			const std::filesystem::path skills = Test::GetRepositoryRoot() / ".claude" / "skills";
			const auto entries = FileSystem::ListDirectory(skills, false);
			REQUIRE_MESSAGE(entries.has_value(), (entries ? std::string() : entries.error().ToString()));
			std::vector<std::string> names;
			for (const std::filesystem::path& entry : *entries)
			{
				std::error_code error;
				if (std::filesystem::is_directory(entry, error))
					names.push_back(FileSystem::PathToUtf8(entry.filename()));
			}
			std::ranges::sort(names);
			for (const std::string_view required : { "add-automation-method", "add-component", "add-script-api", "build-and-test",
					 "commit-review", "game-building", "game-debugging", "golden-images", "luau-gameplay" })
				CHECK_MESSAGE(std::ranges::binary_search(names, required), required);
			for (const std::string& name : names)
			{
				CAPTURE(name);
				const Result<std::string> text = FileSystem::ReadText(skills / FileSystem::PathFromUtf8(name) / "SKILL.md");
				REQUIRE_MESSAGE(text.has_value(), (text ? std::string() : text.error().ToString()));
				CHECK(MeasureDocsGetResult(name, *text) <= DefaultOffloadThresholdBytes);
				const std::string header = std::format("---\nname: {}\ndescription: ", name);
				REQUIRE(text->starts_with(header));
				const size_t end = text->find('\n', header.size());
				REQUIRE(end != std::string::npos);
				CHECK(end > header.size());
				CHECK(std::string_view(*text).substr(end + 1).starts_with("---\n"));
			}
		}
	}

}
