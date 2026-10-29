#include "TestsPCH.h"

#include "Engine/Scene/Prefab.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/Fuzz.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		// A prefab that loaded saves again (the fuzz case's oracle), naming the error when it does not.
		void CheckPrefabSaves(const Prefab& prefab)
		{
			const Result<std::string> saved = prefab.SaveToString();
			CHECK_MESSAGE(saved.has_value(), saved.error().ToString());
		}

	}

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

		// Docs/Decisions/0031-m20-contract.md decision 8: an instance below the new prefab's root stays live in it; the root's
		// own instance is unpacked in the copy (the prefab's root is never an instance root itself).
		TEST_CASE("Prefab: creating a prefab keeps a nested instance live, unpacks the root's own and nulls the root's parent")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Entity holder = scene.CreateEntity("Holder");
			const Entity root = scene.CreateEntity("Root", holder);
			const Entity nested = scene.CreateEntity("Nested", root);
			root.AddComponent<PrefabInstanceComponent>();
			root.AddComponent<PrefabLinkComponent>(PrefabLinkComponent{ UUID(0x76), root.GetUUID(), UUID() });
			nested.AddComponent<PrefabInstanceComponent>();
			nested.AddComponent<PrefabLinkComponent>(PrefabLinkComponent{ UUID(0x77), nested.GetUUID(), UUID() });
			root.AddComponent<MeshRendererComponent>();

			const Result<Prefab> prefab = Prefab::CreateFromEntity(root, "Thing");
			REQUIRE(prefab.has_value());
			CHECK(prefab->GetRootID() == root.GetUUID());
			CHECK(prefab->GetEntityIDs().size() == 2);
			const Json* rootJson = prefab->FindEntity(root.GetUUID());
			REQUIRE(rootJson != nullptr);
			CHECK((*rootJson)["Parent"].is_null());
			CHECK_FALSE((*rootJson)["Components"].contains("Prefab"));
			CHECK_FALSE((*rootJson)["Components"].contains("PrefabLink"));
			const Json* nestedJson = prefab->FindEntity(nested.GetUUID());
			REQUIRE(nestedJson != nullptr);
			CHECK((*nestedJson)["Components"].contains("Prefab"));
			CHECK((*nestedJson)["Components"]["PrefabLink"]["InstanceRoot"] == Json(nested.GetUUID().ToString()));
		}

		TEST_CASE("Prefab: several roots or an instance on the root entity fail a strict load; repair drops the root's instance")
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

			// The prefab's root entity is never an instance root or member itself (PREFAB_NESTED_INSTANCE,
			// Docs/Decisions/0031-m20-contract.md decision 8): a strict load fails, a repair load drops its link and reports
			// the fix.
			const std::string_view rooted = R"({
				"Format": "Prefab", "Version": 1, "Name": "Rooted", "Root": "00000000000b0001", "ComponentVersions": {},
				"Entities": [ { "ID": "00000000000b0001", "Name": "A", "Parent": null, "Active": true, "Tags": [],
				                "Components": { "PrefabLink": { "PrefabEntityID": "0000000000000077", "InstanceRoot": "00000000000b0001" } } },
				              { "ID": "00000000000b0002", "Name": "B", "Parent": "00000000000b0001", "Active": true, "Tags": [], "Components": {} } ]
			})";
			const Result<Prefab> strict = Prefab::LoadFromString(rooted, *registry, LoadOptions{}, report);
			REQUIRE_FALSE(strict.has_value());
			CHECK(strict.error().GetCode() == ErrorCode::Validation);
			CHECK(strict.error().GetMessageText().starts_with(PrefabNestedInstanceCode));
			CHECK(strict.error().GetLocation().JsonPointer == "/Entities/0/Components/PrefabLink");

			UUIDGenerator repairIds = UUIDGenerator::CreateDeterministic(3);
			LoadOptions repair;
			repair.Mode = LoadMode::Repair;
			repair.RepairIdGenerator = &repairIds;
			const Result<Prefab> repaired = Prefab::LoadFromString(rooted, *registry, repair, report);
			REQUIRE(repaired.has_value());
			REQUIRE(report.Repairs.size() == 1);
			CHECK(report.Repairs[0].Code == PrefabNestedInstanceCode);
			CHECK(report.Repairs[0].Removed.Get()["InstanceRoot"] == "00000000000b0001");
			const Json* root = repaired->FindEntity(UUID(0x00000000000b0001));
			REQUIRE(root != nullptr);
			CHECK_FALSE((*root)["Components"].contains("PrefabLink"));

			// An instance below the root is live: it loads strictly with its components.
			const std::string_view nested = R"({
				"Format": "Prefab", "Version": 1, "Name": "Nested", "Root": "00000000000b0001", "ComponentVersions": {},
				"Entities": [ { "ID": "00000000000b0001", "Name": "A", "Parent": null, "Active": true, "Tags": [], "Components": {} },
				              { "ID": "00000000000b0002", "Name": "B", "Parent": "00000000000b0001", "Active": true, "Tags": [],
				                "Components": { "Prefab": { "Prefab": null, "Overrides": [] },
				                                "PrefabLink": { "PrefabEntityID": "0000000000000077", "InstanceRoot": "00000000000b0002",
				                                                "OuterPrefabEntityID": null } } } ]
			})";
			const Result<Prefab> live = Prefab::LoadFromString(nested, *registry, LoadOptions{}, report);
			REQUIRE_MESSAGE(live.has_value(), (live ? std::string() : live.error().ToString()));
			const Json* nestedRoot = live->FindEntity(UUID(0x00000000000b0002));
			REQUIRE(nestedRoot != nullptr);
			CHECK((*nestedRoot)["Components"].contains("Prefab"));
			CHECK((*nestedRoot)["Components"]["PrefabLink"]["InstanceRoot"] == "00000000000b0002");
		}

		TEST_CASE("Prefab: 1,000 seeded mutations of a prefab file and of its cooked form never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The seed corpus: the prefab fixtures of Tests/Data, Room.prefab holding a live nested Lamp instance with an
			// override (Docs/Decisions/0031-m20-contract.md decision 8). The text goes through a strict or a repair load; the
			// cooked form (the payload hash sealed again) through LoadCookedPrefab and then Prefab::FromJson, as a Runtime loads
			// it. Every seed loads strictly, so mutations start from documents the reader accepts.
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			std::vector<std::string> seeds;
			for (const std::string_view fixture : { "Prefabs/Block.prefab", "Assets/Prefabs/Lamp.prefab", "Assets/Prefabs/Room.prefab" })
			{
				CAPTURE(fixture);
				const Result<std::string> text = Test::ReadTestDataText(fixture);
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				LoadReport report;
				const Result<Prefab> seed = Prefab::LoadFromString(*text, *registry, LoadOptions{}, report);
				REQUIRE_MESSAGE(seed.has_value(), (seed ? std::string() : seed.error().ToString()));
				seeds.push_back(*text);
			}
			// Room's lamp stays a live instance through the strict load.
			LoadReport roomReport;
			const Result<Prefab> room = Prefab::LoadFromString(seeds.back(), *registry, LoadOptions{}, roomReport);
			REQUIRE(room.has_value());
			const Json* lamp = room->FindEntity(UUID(0x00000000000d0002));
			REQUIRE(lamp != nullptr);
			CHECK((*lamp)["Components"].contains("PrefabLink"));
			CHECK((*lamp)["Components"]["Prefab"]["Overrides"].size() == 1);
			const Test::FuzzRun run = Test::RunFuzzCase(0x9EFAB, 1000, [&registry, &seeds](Random& random, uint64_t index)
			{
				std::string mutated = seeds[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(seeds.size()) - 1))];
				Test::MutateText(mutated, random);
				UUIDGenerator repairIds = UUIDGenerator::CreateDeterministic(index + 1);
				LoadOptions options;
				options.Mode = random.NextBool() ? LoadMode::Strict : LoadMode::Repair;
				options.RepairIdGenerator = &repairIds;
				LoadReport report;
				if (random.NextBool())
				{
					const Result<Prefab> loaded = Prefab::LoadFromString(mutated, *registry, options, report);
					if (loaded.has_value())
						CheckPrefabSaves(*loaded);
					return loaded.has_value();
				}
				const Result<AssetRef<PrefabData>> cooked = LoadCookedPrefab(WriteCookedArtifact(AssetType::Prefab, PrefabData::FormatVersion, 1, AsBytes(mutated)));
				if (!cooked.has_value())
					return false;
				const Result<Prefab> loaded = Prefab::FromJson(*(*cooked)->Document, *registry, options, report);
				if (loaded.has_value())
					CheckPrefabSaves(*loaded);
				return loaded.has_value();
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
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
