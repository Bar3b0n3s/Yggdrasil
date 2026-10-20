#include "TestsPCH.h"

#include "Engine/Scene/Migrations.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		// A component at version 2 whose version 1 called its only field "Old".
		struct VersionedTestComponent
		{
			int32_t New = 0;
		};

	}

	// The migration of VersionedTestComponent from version 1 to 2: renames "Old" to "New".
	static Status RenameOldToNew(Json& component)
	{
		const auto old = component.find("Old");
		if (!component.is_object() || old == component.end())
		{
			ErrorLocation location;
			location.JsonPointer = std::string();
			return std::unexpected(
				Error(ErrorCode::Validation, "a version 1 'Versioned' component has the field 'Old'").WithLocation(std::move(location)));
		}
		Json value = *old;
		component.erase("Old");
		component["New"] = std::move(value);
		return {};
	}

	static Scope<TypeRegistry> CreateVersionedRegistry()
	{
		Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
		registry->Component<VersionedTestComponent>("Versioned", "A test component at version 2.")
			.Category("Core")
			.Version(2)
			.Migration(1, &RenameOldToNew)
			.Field("New", &VersionedTestComponent::New, "The field version 1 called 'Old'.");
		registry->Freeze();
		return registry;
	}

	static Json ParseJson(std::string_view text)
	{
		Result<Json> document = JsonReader::Parse(text);
		REQUIRE_MESSAGE(document.has_value(), (document ? std::string() : document.error().ToString()));
		return std::move(*document);
	}

	static std::vector<std::string> KeysOf(const Json& object)
	{
		std::vector<std::string> keys;
		for (auto member = object.begin(); member != object.end(); ++member)
			keys.push_back(member.key());
		return keys;
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Migrations: v0 fixture upgrades to v1")
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

		// Docs/Decisions/0031-m20-contract.md decision 10: the golden fixture of the Prefab component's migration to version 2.
		// The file loads, migrated, and saves as the upgraded fixture: each override's PrefabEntityID a one-element Path, and
		// every PrefabLink with its additive OuterPrefabEntityID written as null.
		TEST_CASE("Migrations: PrefabInstanceV1 fixture upgrades to version 2")
		{
			const Result<std::string> legacy = Test::ReadTestDataText("Formats/Scene/PrefabInstanceV1.scene");
			const Result<std::string> golden = Test::ReadTestDataText("Formats/Scene/PrefabInstanceV1.upgraded.scene");
			REQUIRE(legacy.has_value());
			REQUIRE(golden.has_value());

			Test::SceneTestFixture setup;
			LoadReport report;
			const Status loaded = SceneSerializer::LoadFromString(setup.GetScene(), *legacy, LoadOptions{}, report);
			REQUIRE_MESSAGE(loaded.has_value(), (loaded ? std::string() : loaded.error().ToString()));
			CHECK(report.FileVersion == 1);
			CHECK(report.Migrated);

			const Result<std::string> saved = SceneSerializer::SaveToString(setup.GetScene());
			REQUIRE(saved.has_value());
			CHECK(*saved == *golden);
		}

		TEST_CASE("Migrations: the version 0 step renames Enabled and keys components by type")
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

		TEST_CASE("Migrations: a version 0 entity listing a component type twice is a located error")
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

		TEST_CASE("Migrations: newer documents and newer component versions are UnsupportedVersion")
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

		TEST_CASE("Migrations: a current document is left unchanged")
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

		TEST_CASE("Migrations: component migrations upgrade the component on every entity and record its new version")
		{
			const Scope<TypeRegistry> registry = CreateVersionedRegistry();
			Json document = ParseJson(R"({ "Format": "Scene", "Version": 1, "Name": "V", "Seed": 0,
				"ComponentVersions": { "Versioned": 1, "Mystery": 4 },
				"Entities": [ { "ID": "5e00000000000001", "Components": { "Versioned": { "Old": 5 }, "Mystery": { "Old": 1 } } },
				              { "ID": "5e00000000000002", "Components": {} },
				              { "ID": "5e00000000000003", "Components": { "Versioned": { "Old": 7 } } } ] })");
			LoadReport report;
			const Status upgraded = Migrations::UpgradeDocument(document, DocumentKind::Scene, *registry, report);
			REQUIRE_MESSAGE(upgraded.has_value(), (upgraded ? std::string() : upgraded.error().ToString()));
			CHECK(report.FileVersion == 1);
			CHECK(report.Migrated);
			CHECK(document["ComponentVersions"]["Versioned"] == 2);
			CHECK(document["ComponentVersions"]["Mystery"] == 4); // unknown components are left to the serializer
			CHECK(document["Entities"][0]["Components"]["Versioned"] == ParseJson(R"({ "New": 5 })"));
			CHECK(document["Entities"][0]["Components"]["Mystery"] == ParseJson(R"({ "Old": 1 })"));
			CHECK(document["Entities"][2]["Components"]["Versioned"] == ParseJson(R"({ "New": 7 })"));
		}

		// Docs/Decisions/0031-m20-contract.md decision 10 (PrefabInstance version 2): an override names its target by a path,
		// so version 1's single PrefabEntityID becomes a one-element Path at the same place, everything else unchanged.
		TEST_CASE("Migrations: a Prefab component at version 1 migrates each override's PrefabEntityID to a one-element Path")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			Json document = ParseJson(R"({ "Format": "Scene", "Version": 1, "Name": "Bricks", "Seed": 0,
				"ComponentVersions": { "Prefab": 1, "PrefabLink": 1 },
				"Entities": [ { "ID": "5e00000000000001", "Name": "Brick", "Parent": null, "Active": true, "Tags": [],
				                "Components": {
				                  "Prefab": { "Prefab": "11d04163305d2e9f", "Overrides": [
				                    { "PrefabEntityID": "55425a047af78945", "Kind": "Field", "Component": "Script", "Field": "Fields", "Value": { "Points": 3 } },
				                    { "PrefabEntityID": "55425a047af78945", "Kind": "EntityKey", "Component": "", "Field": "Name", "Value": "Brick" } ] },
				                  "PrefabLink": { "PrefabEntityID": "55425a047af78945", "InstanceRoot": "5e00000000000001" } } } ] })");
			LoadReport report;
			const Status upgraded = Migrations::UpgradeDocument(document, DocumentKind::Scene, *registry, report);
			REQUIRE_MESSAGE(upgraded.has_value(), (upgraded ? std::string() : upgraded.error().ToString()));
			CHECK(report.Migrated);
			CHECK(document["ComponentVersions"]["Prefab"] == 2);
			const Json& overrides = document["Entities"][0]["Components"]["Prefab"]["Overrides"];
			REQUIRE(overrides.size() == 2);
			CHECK(overrides[0] == ParseJson(R"({ "Path": ["55425a047af78945"], "Kind": "Field", "Component": "Script", "Field": "Fields", "Value": { "Points": 3 } })"));
			CHECK(KeysOf(overrides[1]) == std::vector<std::string>{ "Path", "Kind", "Component", "Field", "Value" });
			CHECK(overrides[1]["Path"] == ParseJson(R"(["55425a047af78945"])"));
			// PrefabLink stays at version 1; its additive key needs no migration (a missing key reads as its default).
			CHECK(document["ComponentVersions"]["PrefabLink"] == 1);
		}

		// Docs/Decisions/0031-m20-contract.md decisions 17 and 19 (Script version 2, S-MULTI): the Script component becomes
		// an ordered list of slots, so version 1's one script is the list's only slot, unchanged inside it; and the prefab
		// overrides that target the Script component follow it in the same upgrade, whatever the Prefab component's version:
		// a Field override names slot 0's member, and an AddComponent override's value becomes a one-slot list.
		TEST_CASE("Migrations: a Script component at version 1 becomes a one-slot list, its prefab overrides with it" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			Json document = ParseJson(R"({ "Format": "Scene", "Version": 1, "Name": "Slots", "Seed": 0,
				"ComponentVersions": { "Prefab": 1, "PrefabLink": 1, "Script": 1 },
				"Entities": [ { "ID": "5e00000000000001", "Name": "Brick", "Parent": null, "Active": true, "Tags": [],
				                "Components": {
				                  "Prefab": { "Prefab": "11d04163305d2e9f", "Overrides": [
				                    { "PrefabEntityID": "55425a047af78945", "Kind": "Field", "Component": "Script", "Field": "Fields", "Value": { "Points": 3 } },
				                    { "PrefabEntityID": "55425a047af78945", "Kind": "Field", "Component": "Script", "Field": "ExecutionOrder", "Value": 2 },
				                    { "PrefabEntityID": "66425a047af78946", "Kind": "AddComponent", "Component": "Script", "Field": "",
				                      "Value": { "Script": "22d04163305d2e9f", "Fields": {}, "ExecutionOrder": 0 } },
				                    { "PrefabEntityID": "55425a047af78945", "Kind": "EntityKey", "Component": "", "Field": "Name", "Value": "Brick" } ] },
				                  "PrefabLink": { "PrefabEntityID": "55425a047af78945", "InstanceRoot": "5e00000000000001" },
				                  "Script": { "Script": "33d04163305d2e9f", "Fields": { "Points": 3 }, "ExecutionOrder": 2 } } },
				              { "ID": "5e00000000000002", "Name": "Plain", "Parent": null, "Active": true, "Tags": [],
				                "Components": { "Script": { "Script": "44d04163305d2e9f" } } } ] })");
			LoadReport report;
			const Status upgraded = Migrations::UpgradeDocument(document, DocumentKind::Scene, *registry, report);
			REQUIRE_MESSAGE(upgraded.has_value(), (upgraded ? std::string() : upgraded.error().ToString()));
			CHECK(report.Migrated);
			CHECK(document["ComponentVersions"]["Script"] == 2);
			const Json& components = document["Entities"][0]["Components"];
			CHECK(components["Script"] == ParseJson(R"({ "Scripts": [ { "Script": "33d04163305d2e9f", "Fields": { "Points": 3 }, "ExecutionOrder": 2 } ] })"));
			// A version 1 component without its optional members keeps them absent inside its slot.
			CHECK(document["Entities"][1]["Components"]["Script"] == ParseJson(R"({ "Scripts": [ { "Script": "44d04163305d2e9f" } ] })"));
			const Json& overrides = components["Prefab"]["Overrides"];
			REQUIRE(overrides.size() == 4);
			CHECK(overrides[0]["Field"] == Json("Scripts/0/Fields"));
			CHECK(overrides[0]["Value"] == ParseJson(R"({ "Points": 3 })"));
			CHECK(overrides[1]["Field"] == Json("Scripts/0/ExecutionOrder"));
			CHECK(overrides[2]["Value"] == ParseJson(R"({ "Scripts": [ { "Script": "22d04163305d2e9f", "Fields": {}, "ExecutionOrder": 0 } ] })"));
			CHECK(overrides[3]["Field"] == Json("Name"));
		}

		TEST_CASE("Migrations: a failing component migration is located in the document and changes nothing")
		{
			const Scope<TypeRegistry> registry = CreateVersionedRegistry();
			Json document = ParseJson(R"({ "Format": "Scene", "Version": 1, "Name": "V", "Seed": 0, "ComponentVersions": { "Versioned": 1 },
				"Entities": [ { "ID": "5e00000000000001", "Components": { "Versioned": { "Old": 5 } } },
				              { "ID": "5e00000000000002", "Components": { "Versioned": { "Wrong": 1 } } } ] })");
			const Json original = document;
			LoadReport report;
			const Status upgraded = Migrations::UpgradeDocument(document, DocumentKind::Scene, *registry, report);
			REQUIRE_FALSE(upgraded.has_value());
			CHECK(upgraded.error().GetCode() == ErrorCode::Validation);
			CHECK(upgraded.error().GetLocation().JsonPointer == std::string("/Entities/1/Components/Versioned"));
			CHECK(document == original);
			CHECK_FALSE(report.Migrated);
		}

		TEST_CASE("Migrations: component version entries are positive integers")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			for (const std::string_view entry : { "0", "-1", "1.5", "\"1\"" })
			{
				INFO(std::string(entry));
				Json document =
					ParseJson(std::format(R"({{ "Format": "Scene", "Version": 1, "ComponentVersions": {{ "Transform": {} }}, "Entities": [] }})", entry));
				const Json original = document;
				LoadReport report;
				const Status upgraded = Migrations::UpgradeDocument(document, DocumentKind::Scene, *registry, report);
				REQUIRE_FALSE(upgraded.has_value());
				CHECK(upgraded.error().GetCode() == ErrorCode::Validation);
				CHECK(upgraded.error().GetLocation().JsonPointer == std::string("/ComponentVersions/Transform"));
				CHECK(document == original);
			}
		}

		TEST_CASE("Migrations: a missing or non-integer Version is a located Validation error")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::array<std::pair<std::string_view, std::string_view>, 3> cases = { {
				{ R"({ "Format": "Scene", "Entities": [] })", "" },
				{ R"({ "Format": "Scene", "Version": "1", "Entities": [] })", "/Version" },
				{ R"({ "Format": "Scene", "Version": -1, "Entities": [] })", "/Version" },
			} };
			for (const auto& [text, pointer] : cases)
			{
				INFO(std::string(text));
				Json document = ParseJson(text);
				LoadReport report;
				const Status upgraded = Migrations::UpgradeDocument(document, DocumentKind::Scene, *registry, report);
				REQUIRE_FALSE(upgraded.has_value());
				CHECK(upgraded.error().GetCode() == ErrorCode::Validation);
				CHECK(upgraded.error().GetLocation().JsonPointer == std::string(pointer));
			}
		}

		TEST_CASE("Migrations: version 0 documents that do not have the version 0 shape are located errors")
		{
			const std::array<std::pair<std::string_view, std::string_view>, 6> cases = { {
				{ R"({ "Version": 0, "ComponentVersions": {}, "Entities": [] })", "/ComponentVersions" },
				{ R"({ "Version": 0, "Entities": {} })", "/Entities" },
				{ R"({ "Version": 0, "Entities": [ { "ID": "5e00000000000001", "Active": true } ] })", "/Entities/0/Active" },
				{ R"({ "Version": 0, "Entities": [ { "ID": "5e00000000000001", "Components": {} } ] })", "/Entities/0/Components" },
				{ R"({ "Version": 0, "Entities": [ { "ID": "5e00000000000001", "Components": [ { "X": 1 } ] } ] })", "/Entities/0/Components/0" },
				{ R"({ "Version": 0, "Entities": [ { "ID": "5e00000000000001", "Components": [ { "Type": 3 } ] } ] })", "/Entities/0/Components/0/Type" },
			} };
			for (const auto& [text, pointer] : cases)
			{
				INFO(std::string(text));
				Json document = ParseJson(text);
				const Json original = document;
				const Status upgraded = Migrations::UpgradeVersion0To1(document);
				REQUIRE_FALSE(upgraded.has_value());
				CHECK(upgraded.error().GetCode() == ErrorCode::Validation);
				CHECK(upgraded.error().GetLocation().JsonPointer == std::string(pointer));
				CHECK(document == original);
			}
		}

		TEST_CASE("Migrations: the version 0 step keeps the member order and puts ComponentVersions before Entities")
		{
			Json document = ParseJson(R"({ "Format": "Scene", "Version": 0, "Name": "Legacy", "Seed": 3,
				"Entities": [ { "ID": "5e00000000000001", "Name": "A", "Parent": null, "Enabled": true, "Tags": [],
				                "Components": [ { "Type": "SphereCollider", "Radius": 1 }, { "Type": "Transform" } ] } ] })");
			REQUIRE(Migrations::UpgradeVersion0To1(document).has_value());
			CHECK(KeysOf(document) == std::vector<std::string>{ "Format", "Version", "Name", "Seed", "ComponentVersions", "Entities" });
			CHECK(KeysOf(document["ComponentVersions"]) == std::vector<std::string>{ "SphereCollider", "Transform" });
			const Json& entity = document["Entities"][0];
			CHECK(KeysOf(entity) == std::vector<std::string>{ "ID", "Name", "Parent", "Active", "Tags", "Components" });
			CHECK(KeysOf(entity["Components"]) == std::vector<std::string>{ "SphereCollider", "Transform" });
			CHECK(entity["Components"]["Transform"] == Json::object());

			Json empty = ParseJson(R"({ "Format": "Prefab", "Version": 0, "Name": "Empty", "Root": "5e00000000000001" })");
			REQUIRE(Migrations::UpgradeVersion0To1(empty).has_value());
			CHECK(KeysOf(empty) == std::vector<std::string>{ "Format", "Version", "Name", "Root", "ComponentVersions" });
			CHECK(empty["Version"] == 1);
		}
	}

}
