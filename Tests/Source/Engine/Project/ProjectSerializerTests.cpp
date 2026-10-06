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
		TEST_CASE("ProjectSerializer: AllSettings.eproj re-saves byte-identically" * doctest::skip(true))
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

		TEST_CASE("ProjectSerializer: defaults are written explicitly with action keys sorted" * doctest::skip(true))
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

		TEST_CASE("ProjectSerializer: unknown keys warn, wrong types and newer versions fail" * doctest::skip(true))
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

		TEST_CASE("ProjectSerializer: files are saved and loaded through the VFS" * doctest::skip(true))
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
		}
	}

}
