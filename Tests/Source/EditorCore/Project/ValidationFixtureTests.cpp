#include "TestsPCH.h"

#include "EditorCore/Project/ProjectValidator.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Asset/AssetDiagnostic.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"
#include "Support/TestTimeout.h"
#include "Support/ValidatorTestTypes.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <list>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace Engine {

	namespace {

		// One entry of a fixture's Expand: the test repeats the entity until the scene holds Count of it.
		struct ValidationExpansion
		{
			std::string Path{};
			std::string Entity{};
			uint32_t Count = 0;
		};

		// A fixture's Expected.json (ADR 0022 decision 11).
		struct ValidationFixture
		{
			std::string Name{};
			std::vector<std::string> Codes{};
			bool TestComponents = false;
			std::vector<ValidationExpansion> Expand{};
			std::vector<std::string> NonCanonical{};
		};

		std::vector<std::string> ReadStrings(const JsonReader& reader, std::string_view member)
		{
			const auto array = reader.GetMember(member);
			REQUIRE_MESSAGE(array.has_value(), array.error().ToString());
			const auto size = array->GetArraySize();
			REQUIRE(size.has_value());
			std::vector<std::string> values;
			for (size_t index = 0; index < *size; ++index)
			{
				const auto element = array->GetElement(index);
				REQUIRE(element.has_value());
				const auto value = element->ReadString();
				REQUIRE(value.has_value());
				values.push_back(*value);
			}
			return values;
		}

		// Tests/Data/Validation/<Name>/Expected.json of every fixture (ADR 0022 decision 11), sorted.
		std::vector<std::string> ListValidationFixtures()
		{
			const auto files = Test::ListTestDataFiles("Validation", { ".json" });
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			std::vector<std::string> fixtures;
			for (const std::string& file : *files)
			{
				const std::string_view path(file);
				if (path.ends_with("/Expected.json") && std::ranges::count(path, '/') == 2)
					fixtures.push_back(file);
			}
			return fixtures;
		}

		ValidationFixture ReadValidationFixture(const std::string& file)
		{
			const auto text = Test::ReadTestDataText(file);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const auto document = JsonReader::Parse(*text);
			REQUIRE(document.has_value());
			const JsonReader reader(*document);
			REQUIRE(reader.ReadFormatHeader("ValidationFixture", 1, 1).has_value());
			CHECK(reader.ReadMember<std::string>("Scope") == std::string("Project"));
			ValidationFixture fixture;
			fixture.Name = file.substr(std::string_view("Validation/").size(),
				file.size() - std::string_view("Validation/").size() - std::string_view("/Expected.json").size());
			fixture.Codes = ReadStrings(reader, "Codes");
			CHECK(std::ranges::is_sorted(fixture.Codes));
			const auto testComponents = reader.ReadMember<bool>("TestComponents");
			REQUIRE(testComponents.has_value());
			fixture.TestComponents = *testComponents;
			fixture.NonCanonical = ReadStrings(reader, "NonCanonical");
			const auto expand = reader.GetMember("Expand");
			REQUIRE(expand.has_value());
			const auto count = expand->GetArraySize();
			REQUIRE(count.has_value());
			for (size_t index = 0; index < *count; ++index)
			{
				const auto entry = expand->GetElement(index);
				REQUIRE(entry.has_value());
				const auto path = entry->ReadMember<std::string>("Path");
				const auto entity = entry->ReadMember<std::string>("Entity");
				const auto copies = entry->ReadMember<uint32_t>("Count");
				REQUIRE((path && entity && copies));
				fixture.Expand.push_back(ValidationExpansion{ *path, *entity, *copies });
			}
			return fixture;
		}

		// Repeats the entity `expansion` names in the copied scene until it holds Count of it: each copy is a sibling with
		// the next id after 0xfe00000000000000.
		void ExpandScene(const std::filesystem::path& project, const ValidationExpansion& expansion)
		{
			const std::filesystem::path file = project / FileSystem::PathFromUtf8(expansion.Path);
			const auto text = FileSystem::ReadText(file);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			auto document = JsonReader::Parse(*text);
			REQUIRE(document.has_value());
			Json& entities = (*document)["Entities"];
			REQUIRE(entities.is_array());
			const auto original = std::ranges::find_if(entities, [&expansion](const Json& entity)
			{
				return entity["ID"] == Json(expansion.Entity);
			});
			REQUIRE_MESSAGE(original != entities.end(), "no entity ", expansion.Entity, " in ", expansion.Path);
			const Json prototype = *original;
			REQUIRE(expansion.Count >= 1);
			for (uint32_t copy = 1; copy < expansion.Count; ++copy)
			{
				Json entity = prototype;
				entity["ID"] = std::format("{:016x}", 0xfe00000000000000ull + copy);
				entities.push_back(std::move(entity));
			}
			const auto written = JsonWriter::Write(*document);
			REQUIRE(written.has_value());
			REQUIRE(FileSystem::WriteFileAtomic(file, AsBytes(*written), { .KeepBackup = false }).has_value());
		}

		// What a fixture's project reports: its codes, sorted and unique, the codes of the diagnostics it reports as
		// autoFixable, and every diagnostic for failure messages.
		struct ValidationOutcome
		{
			std::vector<std::string> Codes{};
			std::set<std::string> FixableCodes{};
			std::string Diagnostics{};
		};

		// The fixture's project copied into the editor fixture's directory, expanded, opened, its assets loaded (so the
		// importers record their diagnostics), and validated with the project scope.
		ValidationOutcome ValidateFixture(const ValidationFixture& fixture)
		{
			// The asset manager logs a failed import and every missing or mistyped reference it hands a placeholder for at Error
			// (§7.2): a fixture reporting those codes declares the entries.
			std::list<Test::ExpectLog> expectedLogs;
			for (const std::string_view code : { AssetImportFailedCode, AssetMissingCode, AssetTypeMismatchCode })
			{
				if (std::ranges::find(fixture.Codes, code) != fixture.Codes.end())
					expectedLogs.emplace_back(LogLevel::Error, code);
			}
			Test::EditorTestFixture editorFixture("ValidationFixture", {}, fixture.TestComponents ? &Test::RegisterValidatorTestTypes : nullptr);
			const std::filesystem::path project = editorFixture.GetDirectory().GetPath() / "Project";
			std::error_code error;
			std::filesystem::copy(Test::GetTestDataPath(std::format("Validation/{}/Project", fixture.Name)), project, std::filesystem::copy_options::recursive,
				error);
			REQUIRE_MESSAGE(!error, error.message());
			for (const ValidationExpansion& expansion : fixture.Expand)
				ExpandScene(project, expansion);

			EditorContext& editor = editorFixture.GetEditor();
			auto opened = ProjectManager::OpenProject(project / "Validation.eproj", {}, editor.GetTypeRegistry());
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			const Status open = editor.OpenProject(std::move(*opened));
			REQUIRE_MESSAGE(open.has_value(), open.error().ToString());
			// Every main asset but scenes and scripts is imported now, as a running editor imports what its scenes and the user
			// touch: the validator checks scene files itself, and the script service compiles and checks every script.
			for (const AssetRecord* record : editor.GetAssets().GetRegistry().GetRecords())
			{
				const AssetType type = record->Metadata.Type;
				if (record->Metadata.Kind == AssetMetaKind::Asset && type != AssetType::Scene && type != AssetType::Script)
					static_cast<void>(editor.GetAssets().Load(record->Metadata.Handle));
			}
			editor.GetAssets().WaitIdle();
			const auto report = ProjectValidator::Validate(editor, ValidationScope::Project);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			std::set<std::string> codes;
			ValidationOutcome outcome;
			for (const ProjectDiagnostic& diagnostic : report->Diagnostics)
			{
				codes.insert(diagnostic.Code);
				if (diagnostic.AutoFixable)
					outcome.FixableCodes.insert(diagnostic.Code);
				outcome.Diagnostics += std::format("{} {} {} {}.{}: {}\n", diagnostic.Code, diagnostic.File, diagnostic.Entity, diagnostic.Component,
					diagnostic.Field, diagnostic.Message);
			}
			outcome.Codes.assign(codes.begin(), codes.end());
			return outcome;
		}

		// The codes no fixture's Codes contain, the fixtures named in `excluded` left out: gate 6's Missing list, sorted.
		std::vector<std::string> FindUntriggeredCodes(const std::set<std::string>& excluded)
		{
			std::set<std::string> triggered;
			for (const std::string& file : ListValidationFixtures())
			{
				const ValidationFixture fixture = ReadValidationFixture(file);
				if (!excluded.contains(fixture.Name))
					triggered.insert(fixture.Codes.begin(), fixture.Codes.end());
			}
			std::vector<std::string> missing;
			for (const std::string_view code : ProjectValidator::GetCodes())
			{
				if (!triggered.contains(std::string(code)))
					missing.emplace_back(code);
			}
			return missing;
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Architecture §15.6 gate 6. Each directory is named after the code it exists for (or "Clean", the baseline that
		// reports nothing) and its Expected.json lists every code the fixture reports.
		TEST_CASE("Validation: every project.validate code is triggered by a fixture in Tests/Data/Validation")
		{
			const std::vector<std::string> fixtures = ListValidationFixtures();
			REQUIRE_FALSE(fixtures.empty());
			bool clean = false;
			for (const std::string& file : fixtures)
			{
				CAPTURE(file);
				const ValidationFixture fixture = ReadValidationFixture(file);
				if (fixture.Name == "Clean")
				{
					clean = true;
					CHECK(fixture.Codes.empty());
					continue;
				}
				CHECK(std::ranges::find(ProjectValidator::GetCodes(), fixture.Name) != ProjectValidator::GetCodes().end());
				CHECK(std::ranges::find(fixture.Codes, fixture.Name) != fixture.Codes.end());
				for (const std::string& code : fixture.Codes)
				{
					CAPTURE(code);
					CHECK(std::ranges::find(ProjectValidator::GetCodes(), code) != ProjectValidator::GetCodes().end());
				}
			}
			CHECK(clean);
			CHECK(FindUntriggeredCodes({}).empty());
		}

		TEST_CASE("Validation: each fixture reports exactly its expected codes")
		{
			Test::SetTestCaseTimeLimit(600.0);
			const std::vector<std::string> fixtures = ListValidationFixtures();
			REQUIRE_FALSE(fixtures.empty());
			std::set<std::string> fixable;
			for (const std::string& file : fixtures)
			{
				CAPTURE(file);
				const ValidationFixture fixture = ReadValidationFixture(file);
				const ValidationOutcome outcome = ValidateFixture(fixture);
				INFO(outcome.Diagnostics);
				CHECK(outcome.Codes == fixture.Codes);
				fixable.insert(outcome.FixableCodes.begin(), outcome.FixableCodes.end());
			}
			// GetCodeInfos documents a code AutoFixable when project.validate {fix} can fix some of its diagnostics
			// (Validation.md, the game-debugging skill), so it must agree with what the fixtures report (review finding M13:
			// ASSET_TYPE_MISMATCH's rewritable .meta was documented as not fixable).
			for (const ValidationCodeInfo& info : ProjectValidator::GetCodeInfos())
			{
				CAPTURE(info.Code);
				if (fixable.contains(std::string(info.Code)))
					CHECK(info.AutoFixable);
			}
		}

		// Tests/Data/CoverageGaps/Gate6/Expected.json: {"Format": "CoverageGap", "Version": 1, "Gate": "6", "Exclude": [fixture
		// directories], "Missing": [codes]}: without the excluded fixtures, gate 6 fails and names exactly Missing.
		TEST_CASE("Validation: the gate 6 seeded-gap fixture names exactly its untriggered code")
		{
			const auto expectedText = Test::ReadTestDataText("CoverageGaps/Gate6/Expected.json");
			REQUIRE_MESSAGE(expectedText.has_value(), expectedText.error().ToString());
			const auto document = JsonReader::Parse(*expectedText);
			REQUIRE(document.has_value());
			const JsonReader expected(*document);
			REQUIRE(expected.ReadFormatHeader("CoverageGap", 1, 1).has_value());
			CHECK(expected.ReadMember<std::string>("Gate") == std::string("6"));
			const std::vector<std::string> exclude = ReadStrings(expected, "Exclude");
			const std::vector<std::string> missing = ReadStrings(expected, "Missing");
			REQUIRE_FALSE(exclude.empty());
			REQUIRE(missing.size() == 1);
			CHECK(FindUntriggeredCodes(std::set<std::string>(exclude.begin(), exclude.end())) == missing);
		}

		// The game-debugging skill's error-code table (ADR 0022 decision 17) covers every validation code GetCodeInfos documents.
		TEST_CASE("Validation: the game-debugging skill's error table names every validation code")
		{
			const auto skill = FileSystem::ReadText(Test::GetRepositoryRoot() / ".claude" / "skills" / "game-debugging" / "SKILL.md");
			REQUIRE_MESSAGE(skill.has_value(), skill.error().ToString());
			for (const ValidationCodeInfo& info : ProjectValidator::GetCodeInfos())
			{
				CAPTURE(info.Code);
				CHECK(skill->find(std::format("| `{}` | {} |", info.Code, info.Severity)) != std::string::npos);
			}
		}
	}

}
