#include "TestsPCH.h"

#include "Engine/Scene/Migrations.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("Migrations: v0 fixture upgrades to v1" * doctest::skip(true))
		{
			const Result<std::string> legacy = Test::ReadTestDataText("Formats/Scene/v0.scene");
			const Result<std::string> golden = Test::ReadTestDataText("Formats/Scene/v0.upgraded.scene");
			REQUIRE(legacy.has_value());
			REQUIRE(golden.has_value());

			Test::SceneTestFixture setup;
			LoadReport report;
			REQUIRE(SceneSerializer::LoadFromString(setup.GetScene(), *legacy, LoadOptions{}, report).has_value());
			CHECK(report.FileVersion == 0);
			CHECK(report.Migrated);

			const Result<std::string> saved = SceneSerializer::SaveToString(setup.GetScene());
			REQUIRE(saved.has_value());
			CHECK(*saved == *golden);
		}

		TEST_CASE("Migrations: the version 0 step renames Enabled and keys components by type" * doctest::skip(true))
		{
			Result<Json> document = JsonReader::Parse(R"({
				"Format": "Scene", "Version": 0, "Name": "Legacy", "Seed": 3,
				"Entities": [ { "ID": "5e00000000000001", "Name": "A", "Parent": null, "Enabled": false, "Tags": [],
				                "Components": [ { "Type": "Transform", "Translation": [1, 2, 3] }, { "Type": "Mystery", "X": 1 } ] } ]
			})");
			REQUIRE(document.has_value());
			REQUIRE(Migrations::UpgradeVersion0To1(*document).has_value());

			const Json& entity = (*document)["Entities"][0];
			CHECK((*document)["Version"] == 1);
			CHECK(entity["Active"] == false);
			CHECK_FALSE(entity.contains("Enabled"));
			CHECK(entity["Components"]["Transform"]["Translation"] == Json::array({ 1, 2, 3 }));
			CHECK_FALSE(entity["Components"]["Transform"].contains("Type"));
			CHECK(entity["Components"].contains("Mystery"));
			CHECK((*document)["ComponentVersions"]["Transform"] == 1);
			CHECK((*document)["ComponentVersions"]["Mystery"] == 1);
		}

		TEST_CASE("Migrations: a version 0 entity listing a component type twice is a located error" * doctest::skip(true))
		{
			Result<Json> document = JsonReader::Parse(R"({
				"Format": "Scene", "Version": 0, "Name": "Legacy", "Seed": 0,
				"Entities": [ { "ID": "5e00000000000001", "Name": "A", "Parent": null, "Enabled": true, "Tags": [],
				                "Components": [ { "Type": "Transform" }, { "Type": "Transform" } ] } ]
			})");
			REQUIRE(document.has_value());
			const Json original = *document;
			const Status status = Migrations::UpgradeVersion0To1(*document);
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::Validation);
			CHECK(status.error().GetLocation().JsonPointer == "/Entities/0/Components/1/Type");
			CHECK(*document == original);
		}

		TEST_CASE("Migrations: newer documents and newer component versions are UnsupportedVersion" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			Result<Json> newerFile =
				JsonReader::Parse(R"({ "Format": "Scene", "Version": 2, "Name": "F", "Seed": 0, "ComponentVersions": {}, "Entities": [] })");
			REQUIRE(newerFile.has_value());
			LoadReport report;
			const Status file = Migrations::UpgradeDocument(*newerFile, DocumentKind::Scene, *registry, report);
			REQUIRE_FALSE(file.has_value());
			CHECK(file.error().GetCode() == ErrorCode::UnsupportedVersion);

			Result<Json> newerComponent = JsonReader::Parse(R"({ "Format": "Scene", "Version": 1, "Name": "F", "Seed": 0,
				"ComponentVersions": { "Transform": 9 }, "Entities": [] })");
			REQUIRE(newerComponent.has_value());
			const Status component = Migrations::UpgradeDocument(*newerComponent, DocumentKind::Scene, *registry, report);
			REQUIRE_FALSE(component.has_value());
			CHECK(component.error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("Migrations: a current document is left unchanged" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const Result<std::string> text = Test::ReadTestDataText("Scenes/Hierarchy.scene");
			REQUIRE(text.has_value());
			Result<Json> document = JsonReader::Parse(*text);
			REQUIRE(document.has_value());
			const Json original = *document;
			LoadReport report;
			REQUIRE(Migrations::UpgradeDocument(*document, DocumentKind::Scene, *registry, report).has_value());
			CHECK(*document == original);
			CHECK(report.FileVersion == 1);
			CHECK_FALSE(report.Migrated);
		}
	}

}
