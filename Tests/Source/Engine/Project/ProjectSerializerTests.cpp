#include "TestsPCH.h"

#include "Engine/Project/ProjectSerializer.h"

#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Project")
	{
		TEST_CASE("ProjectSerializer: AllSettings.eproj re-saves byte-identically")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const Result<std::string> text = Test::ReadTestDataText("Project/AllSettings.eproj");
			REQUIRE(text.has_value());
			ProjectLoadReport report;
			const Result<ProjectSettings> settings = ProjectSerializer::LoadFromString(*text, *registry, ProjectLoadOptions{}, report);
			REQUIRE(settings.has_value());
			CHECK(report.FileVersion == 1);
			const Result<std::string> saved = ProjectSerializer::SaveToString(*settings, *registry);
			REQUIRE(saved.has_value());
			CHECK(*saved == *text);
		}

		TEST_CASE("ProjectSerializer: defaults are written explicitly with action keys sorted")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			ProjectSettings settings;
			settings.Input.Actions["Zoom"] = InputActionSettings{};
			settings.Input.Actions["Accelerate"] = InputActionSettings{};
			const Result<Json> document = ProjectSerializer::ToJson(settings, *registry);
			REQUIRE(document.has_value());
			auto it = document->begin();
			CHECK(it.key() == "Format");
			++it;
			CHECK(it.key() == "Version");
			++it;
			CHECK(it.key() == "Name");
			CHECK((*document)["Simulation"]["MaxEntities"] == 65536);
			CHECK((*document)["Input"]["Actions"].begin().key() == "Accelerate");
		}

		TEST_CASE("ProjectSerializer: unknown keys warn, wrong types and newer versions fail")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();

			ProjectLoadReport report;
			const std::string_view unknownKey = R"({ "Format": "Project", "Version": 1, "Name": "X", "Colour": 1 })";
			const Result<ProjectSettings> unknown = ProjectSerializer::LoadFromString(unknownKey, *registry, ProjectLoadOptions{}, report);
			REQUIRE(unknown.has_value());
			CHECK(unknown->Name == "X");
			REQUIRE(report.Diagnostics.size() == 1);
			CHECK(report.Diagnostics[0].JsonPointer == "/Colour");

			ProjectLoadOptions strict;
			strict.StrictUnknowns = true;
			const std::string_view strictUnknown = R"({ "Format": "Project", "Version": 1, "Colour": 1 })";
			CHECK_FALSE(ProjectSerializer::LoadFromString(strictUnknown, *registry, strict, report).has_value());

			const std::string_view wrongWidth = R"({ "Format": "Project", "Version": 1, "Window": { "Width": "wide" } })";
			const Result<ProjectSettings> wrongType = ProjectSerializer::LoadFromString(wrongWidth, *registry, ProjectLoadOptions{}, report);
			REQUIRE_FALSE(wrongType.has_value());
			CHECK(wrongType.error().GetCode() == ErrorCode::Validation);
			CHECK(wrongType.error().GetLocation().JsonPointer == "/Window/Width");

			const Result<ProjectSettings> newer = ProjectSerializer::LoadFromString(R"({ "Format": "Project", "Version": 2 })", *registry,
				ProjectLoadOptions{}, report);
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);

			const Result<ProjectSettings> wrongFormat = ProjectSerializer::LoadFromString(R"({ "Format": "Scene", "Version": 1 })", *registry,
				ProjectLoadOptions{}, report);
			REQUIRE_FALSE(wrongFormat.has_value());
			CHECK(wrongFormat.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("ProjectSerializer: files are saved and loaded through the VFS")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("project", CreateScope<MemoryMount>()).has_value());
			const Result<VfsPath> path = VfsPath::Parse("project://Game.eproj");
			REQUIRE(path.has_value());

			ProjectSettings settings;
			settings.Name = "Game";
			settings.Simulation.Seed = 99;
			REQUIRE(ProjectSerializer::SaveToFile(settings, *registry, vfs, *path).has_value());

			ProjectLoadReport report;
			const Result<ProjectSettings> loaded = ProjectSerializer::LoadFromFile(vfs, *path, *registry, ProjectLoadOptions{}, report);
			REQUIRE(loaded.has_value());
			CHECK(loaded->Name == "Game");
			CHECK(loaded->Simulation.Seed == 99);

			const Result<VfsPath> missing = VfsPath::Parse("project://Missing.eproj");
			REQUIRE(missing.has_value());
			const Result<ProjectSettings> notFound = ProjectSerializer::LoadFromFile(vfs, *missing, *registry, ProjectLoadOptions{}, report);
			REQUIRE_FALSE(notFound.has_value());
			CHECK(notFound.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("ProjectSerializer: a missing key keeps its default and a document with only the header is a default project")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			ProjectLoadReport report;
			const std::string_view partial = R"({ "Format": "Project", "Version": 1, "Window": { "Width": 640 } })";
			const Result<ProjectSettings> settings = ProjectSerializer::LoadFromString(partial, *registry, ProjectLoadOptions{}, report);
			REQUIRE(settings.has_value());
			CHECK(report.Diagnostics.empty());
			CHECK(settings->Window.Width == 640);
			CHECK(settings->Window.Height == WindowSettings{}.Height);
			CHECK(settings->Name == ProjectSettings{}.Name);

			const Result<ProjectSettings> header = ProjectSerializer::LoadFromString(R"({ "Format": "Project", "Version": 1 })", *registry,
				ProjectLoadOptions{}, report);
			REQUIRE(header.has_value());
			CHECK(*ProjectSerializer::SaveToString(*header, *registry) == *ProjectSerializer::SaveToString(ProjectSettings{}, *registry));
		}

		TEST_CASE("ProjectSerializer: a malformed header or an invalid setting is a located Validation error naming the file")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			ProjectLoadOptions options;
			options.SourcePath = "Projects/Game/Game.eproj";
			const std::array<std::pair<std::string_view, std::string_view>, 6> cases = { {
				{ R"({ "Version": 1 })", "" },
				{ R"({ "Format": "Project", "Version": 0 })", "/Version" },
				{ R"({ "Format": "Project", "Version": "1" })", "/Version" },
				{ R"({ "Format": "Project", "Version": 1, "Simulation": { "FixedHz": 0 } })", "/Simulation/FixedHz" },
				{ R"({ "Format": "Project", "Version": 1, "Scripting": { "CallbackBudgetMs": 9 } })", "/Scripting/CallbackBudgetMs" },
				{ R"({ "Format": "Project", "Version": 1, "Testing": { "Suites": [ { "Isolation": "case" } ] } })", "/Testing/Suites/0/Isolation" },
			} };
			for (const auto& [text, pointer] : cases)
			{
				INFO(std::string(text));
				ProjectLoadReport report;
				const Result<ProjectSettings> settings = ProjectSerializer::LoadFromString(text, *registry, options, report);
				REQUIRE_FALSE(settings.has_value());
				CHECK(settings.error().GetCode() == ErrorCode::Validation);
				CHECK(settings.error().GetLocation().File == options.SourcePath);
				CHECK(settings.error().GetLocation().JsonPointer == std::string(pointer));
			}

			ProjectLoadReport report;
			const Result<ProjectSettings> parse = ProjectSerializer::LoadFromString("{ \"Format\": ", *registry, options, report);
			REQUIRE_FALSE(parse.has_value());
			CHECK(parse.error().GetCode() == ErrorCode::Parse);
			CHECK(parse.error().GetLocation().File == options.SourcePath);
		}
	}

}
