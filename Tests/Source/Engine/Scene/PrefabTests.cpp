#include "TestsPCH.h"

#include "Engine/Scene/Prefab.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("Prefab: the Block fixture loads and re-saves byte-identically")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const Result<std::string> text = Test::ReadTestDataText("Prefabs/Block.prefab");
			REQUIRE(text.has_value());

			LoadReport report;
			const Result<Prefab> prefab = Prefab::LoadFromString(*text, *registry, LoadOptions{}, report);
			REQUIRE(prefab.has_value());
			CHECK(prefab->GetName() == "Block");
			CHECK(prefab->GetRootID() == UUID(0x00000000000b0001));
			CHECK(prefab->GetEntityIDs() == std::vector<UUID>{ UUID(0x00000000000b0001), UUID(0x00000000000b0002) });
			REQUIRE(prefab->FindEntity(UUID(0x00000000000b0002)) != nullptr);
			CHECK((*prefab->FindEntity(UUID(0x00000000000b0002)))["Name"] == "Visual");

			const Result<std::string> saved = prefab->SaveToString();
			REQUIRE(saved.has_value());
			CHECK(*saved == *text);
		}

		TEST_CASE("Prefab: creating a prefab flattens nested instances and nulls the root's parent")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Entity holder = scene.CreateEntity("Holder");
			const Entity root = scene.CreateEntity("Root", holder);
			const Entity nested = scene.CreateEntity("Nested", root);
			nested.AddComponent<PrefabInstanceComponent>();
			nested.AddComponent<PrefabLinkComponent>(PrefabLinkComponent{ UUID(0x77), nested.GetUUID() });
			root.AddComponent<MeshRendererComponent>();

			const Result<Prefab> prefab = Prefab::CreateFromEntity(root, "Thing");
			REQUIRE(prefab.has_value());
			CHECK(prefab->GetRootID() == root.GetUUID());
			CHECK(prefab->GetEntityIDs().size() == 2);
			const Json* rootJson = prefab->FindEntity(root.GetUUID());
			REQUIRE(rootJson != nullptr);
			CHECK((*rootJson)["Parent"].is_null());
			const Json* nestedJson = prefab->FindEntity(nested.GetUUID());
			REQUIRE(nestedJson != nullptr);
			CHECK_FALSE((*nestedJson)["Components"].contains("Prefab"));
			CHECK_FALSE((*nestedJson)["Components"].contains("PrefabLink"));
		}

		TEST_CASE("Prefab: nested instances and several roots fail a strict load; repair flattens nested instances")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			LoadReport report;
			const Result<Prefab> twoRoots = Prefab::LoadFromString(R"({
				"Format": "Prefab", "Version": 1, "Name": "Bad", "Root": "00000000000b0001", "ComponentVersions": {},
				"Entities": [ { "ID": "00000000000b0001", "Name": "A", "Parent": null, "Active": true, "Tags": [], "Components": {} },
				              { "ID": "00000000000b0002", "Name": "B", "Parent": null, "Active": true, "Tags": [], "Components": {} } ]
			})",
				*registry, LoadOptions{}, report);
			REQUIRE_FALSE(twoRoots.has_value());
			CHECK(twoRoots.error().GetCode() == ErrorCode::Validation);

			const Result<Prefab> sceneFormat = Prefab::LoadFromString(R"({ "Format": "Scene", "Version": 1 })", *registry, LoadOptions{}, report);
			CHECK_FALSE(sceneFormat.has_value());

			// A nested instance (PREFAB_NESTED_INSTANCE) fails a strict load; a repair load flattens it and reports the fix.
			const std::string_view nested = R"({
				"Format": "Prefab", "Version": 1, "Name": "Nested", "Root": "00000000000b0001", "ComponentVersions": {},
				"Entities": [ { "ID": "00000000000b0001", "Name": "A", "Parent": null, "Active": true, "Tags": [], "Components": {} },
				              { "ID": "00000000000b0002", "Name": "B", "Parent": "00000000000b0001", "Active": true, "Tags": [],
				                "Components": { "PrefabLink": { "PrefabEntityID": "0000000000000077", "InstanceRoot": "00000000000b0002" } } } ]
			})";
			const Result<Prefab> strict = Prefab::LoadFromString(nested, *registry, LoadOptions{}, report);
			REQUIRE_FALSE(strict.has_value());
			CHECK(strict.error().GetCode() == ErrorCode::Validation);
			CHECK(strict.error().GetMessageText().starts_with(PrefabNestedInstanceCode));
			CHECK(strict.error().GetLocation().JsonPointer == "/Entities/1/Components/PrefabLink");

			UUIDGenerator repairIds = UUIDGenerator::CreateDeterministic(3);
			LoadOptions repair;
			repair.Mode = LoadMode::Repair;
			repair.RepairIdGenerator = &repairIds;
			const Result<Prefab> repaired = Prefab::LoadFromString(nested, *registry, repair, report);
			REQUIRE(repaired.has_value());
			REQUIRE(report.Repairs.size() == 1);
			CHECK(report.Repairs[0].Code == PrefabNestedInstanceCode);
			CHECK(report.Repairs[0].Removed.Get()["InstanceRoot"] == "00000000000b0002");
			const Json* flattened = repaired->FindEntity(UUID(0x00000000000b0002));
			REQUIRE(flattened != nullptr);
			CHECK_FALSE((*flattened)["Components"].contains("PrefabLink"));
		}

		TEST_CASE("Prefab: an empty prefab has no document and no entities")
		{
			const Prefab empty;
			CHECK(empty.IsEmpty());
			CHECK_FALSE(empty.GetRootID().IsValid());
			CHECK(empty.GetEntities().is_array());
			CHECK(empty.GetEntities().empty());
			CHECK(empty.GetEntityIDs().empty());
			CHECK(empty.FindEntity(UUID(0x1)) == nullptr);
			const Result<Json> document = empty.ToJson();
			REQUIRE_FALSE(document.has_value());
			CHECK(document.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("Prefab: files are saved and loaded through the VFS")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const Result<std::string> text = Test::ReadTestDataText("Prefabs/Block.prefab");
			REQUIRE(text.has_value());
			LoadReport report;
			const Result<Prefab> prefab = Prefab::LoadFromString(*text, *registry, LoadOptions{}, report);
			REQUIRE(prefab.has_value());

			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("project", CreateScope<MemoryMount>()).has_value());
			const Result<VfsPath> directory = VfsPath::Parse("project://Assets/Prefabs");
			const Result<VfsPath> path = VfsPath::Parse("project://Assets/Prefabs/Block.prefab");
			REQUIRE(directory.has_value());
			REQUIRE(path.has_value());
			REQUIRE(vfs.CreateDirectories(*directory).has_value());
			REQUIRE(prefab->SaveToFile(vfs, *path).has_value());
			const Result<std::string> written = vfs.ReadText(*path);
			REQUIRE(written.has_value());
			CHECK(*written == *text);

			const Result<Prefab> loaded = Prefab::LoadFromFile(vfs, *path, *registry, LoadOptions{}, report);
			REQUIRE(loaded.has_value());
			CHECK(loaded->GetEntityIDs() == prefab->GetEntityIDs());

			// Errors name the file.
			const Result<VfsPath> missing = VfsPath::Parse("project://Assets/Prefabs/Missing.prefab");
			REQUIRE(missing.has_value());
			const Result<Prefab> notFound = Prefab::LoadFromFile(vfs, *missing, *registry, LoadOptions{}, report);
			REQUIRE_FALSE(notFound.has_value());
			CHECK(notFound.error().GetCode() == ErrorCode::NotFound);
			LoadOptions named;
			named.SourcePath = "Bad.prefab";
			const Result<Prefab> malformed = Prefab::LoadFromString("{", *registry, named, report);
			REQUIRE_FALSE(malformed.has_value());
			CHECK(malformed.error().GetCode() == ErrorCode::Parse);
			CHECK(malformed.error().GetLocation().File == "Bad.prefab");
		}

		TEST_CASE("Prefab: unknown components keep their data and version")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::string text = R"({
	"Format": "Prefab",
	"Version": 1,
	"Name": "Future",
	"Root": "00000000000e0001",
	"ComponentVersions": {
		"Transform": 1,
		"Hologram": 3
	},
	"Entities": [
		{
			"ID": "00000000000e0001",
			"Name": "Future",
			"Parent": null,
			"Active": true,
			"Tags": [],
			"Components": {
				"Transform": {
					"Translation": [0, 0, 0],
					"Rotation": [0, 0, 0, 1],
					"Scale": [1, 1, 1]
				},
				"Hologram": {
					"Flicker": 0.5
				}
			}
		}
	]
}
)";
			LoadReport report;
			const Result<Prefab> prefab = Prefab::LoadFromString(text, *registry, LoadOptions{}, report);
			REQUIRE(prefab.has_value());
			REQUIRE(report.Diagnostics.size() == 1);
			CHECK(report.Diagnostics[0].Code == SceneUnknownComponentCode);
			const Result<std::string> saved = prefab->SaveToString();
			REQUIRE(saved.has_value());
			CHECK(*saved == text);
		}
	}

}
