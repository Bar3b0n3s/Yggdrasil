#include "TestsPCH.h"

#include "Engine/Scene/SceneSerializer.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/ChangeTracker.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/GlmApprox.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	// Bytes that keep a mutated document close to JSON, so mutations reach the loader's later stages.
	static constexpr std::string_view MutationAlphabet = "{}[],:\"0e-.tfn";

	static std::string ReadFixture(std::string_view relative)
	{
		const Result<std::string> text = Test::ReadTestDataText(relative);
		INFO(std::string(relative));
		REQUIRE(text.has_value());
		return *text;
	}

	static Json ParseDocument(std::string_view text)
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

	static bool HasDiagnostic(const LoadReport& report, std::string_view code, std::string_view pointer, DiagnosticSeverity severity)
	{
		return std::any_of(report.Diagnostics.begin(), report.Diagnostics.end(), [&](const LoadDiagnostic& diagnostic)
		{
			return diagnostic.Code == code && diagnostic.JsonPointer == pointer && diagnostic.Severity == severity;
		});
	}

	static std::string PointerOf(const Status& status)
	{
		return status ? std::string("(success)") : status.error().GetLocation().JsonPointer.value_or("(none)");
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("SceneSerializer: every fixture under Tests/Data/Scenes re-saves byte-identically")
		{
			const Result<std::vector<std::string>> fixtures = Test::ListTestDataFiles("Scenes", { ".scene" });
			REQUIRE(fixtures.has_value());
			size_t checked = 0;
			for (const std::string& fixture : *fixtures)
			{
				// Invalid/ holds the structural-defect fixtures, which strict loading rejects by design.
				if (fixture.starts_with("Scenes/Invalid/"))
					continue;
				INFO(fixture);
				const std::string text = ReadFixture(fixture);
				Test::SceneTestFixture setup;
				LoadReport report;
				const Status loaded = SceneSerializer::LoadFromString(setup.GetScene(), text, LoadOptions{}, report);
				REQUIRE_MESSAGE(loaded.has_value(), (loaded ? std::string() : loaded.error().ToString()));
				const Result<std::string> saved = SceneSerializer::SaveToString(setup.GetScene());
				REQUIRE(saved.has_value());
				CHECK(*saved == text);
				++checked;
			}
			CHECK(checked >= 4);
		}

		TEST_CASE("SceneSerializer: unknown components are preserved")
		{
			const std::string text = ReadFixture("Scenes/UnknownComponent.scene");
			Test::SceneTestFixture setup;
			LoadReport report;
			REQUIRE(SceneSerializer::LoadFromString(setup.GetScene(), text, LoadOptions{}, report).has_value());

			const Entity gadget = setup.GetScene().FindEntityByID(UUID(0x3c00000000000001));
			REQUIRE(gadget.IsValid());
			REQUIRE(gadget.HasComponent<UnknownComponentsComponent>());
			const UnknownComponentsComponent& unknown = gadget.GetComponent<UnknownComponentsComponent>();
			REQUIRE(unknown.Components.size() == 1);
			CHECK(unknown.Components[0].Name == "FutureThing");
			CHECK(unknown.Components[0].Version == 3);

			const bool warned = std::any_of(report.Diagnostics.begin(), report.Diagnostics.end(), [](const LoadDiagnostic& diagnostic)
			{
				return diagnostic.Code == SceneUnknownComponentCode && diagnostic.Severity == DiagnosticSeverity::Warning;
			});
			CHECK(warned);

			const Result<std::string> saved = SceneSerializer::SaveToString(setup.GetScene());
			REQUIRE(saved.has_value());
			CHECK(*saved == text);

			// --strict rejects the file.
			Test::SceneTestFixture strictSetup;
			LoadOptions strict;
			strict.StrictUnknowns = true;
			LoadReport strictReport;
			CHECK_FALSE(SceneSerializer::LoadFromString(strictSetup.GetScene(), text, strict, strictReport).has_value());
			CHECK(strictSetup.GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("SceneSerializer: newer Version fails with UnsupportedVersion")
		{
			const std::string text = ReadFixture("Formats/Scene/NewerVersion.scene");
			Test::SceneTestFixture setup;
			LoadReport report;
			const Status loaded = SceneSerializer::LoadFromString(setup.GetScene(), text, LoadOptions{}, report);
			REQUIRE_FALSE(loaded.has_value());
			CHECK(loaded.error().GetCode() == ErrorCode::UnsupportedVersion);
			CHECK(loaded.error().GetMessageText().find('2') != std::string::npos);
			CHECK(loaded.error().GetMessageText().find('1') != std::string::npos);
			CHECK(setup.GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("SceneSerializer: serializer copy has the same state hash")
		{
			Test::SceneTestFixture setup;
			LoadReport report;
			REQUIRE(SceneSerializer::LoadFromString(setup.GetScene(), ReadFixture("Scenes/AllComponents.scene"), LoadOptions{}, report).has_value());

			const Result<Json> document = SceneSerializer::ToJson(setup.GetScene());
			REQUIRE(document.has_value());
			const Scope<Scene> copy = setup.CreateEmptyScene(true);
			LoadReport copyReport;
			REQUIRE(SceneSerializer::FromJson(*copy, *document, LoadOptions{}, copyReport).has_value());

			CHECK(copy->GetEntityCount() == setup.GetScene().GetEntityCount());
			CHECK(copy->ComputeStateHash() == setup.GetScene().ComputeStateHash());
			CHECK(setup.GetScene().ComputeStateHash() != 0);
			CHECK(*SceneSerializer::SaveToString(*copy) == *SceneSerializer::SaveToString(setup.GetScene()));

			// Any change changes the hash.
			const Entity ball = copy->FindEntityByID(UUID(0x2b00000000000005));
			REQUIRE(ball.IsValid());
			ball.Patch<RigidBodyComponent>([](RigidBodyComponent& body)
			{
				body.Mass = 3.0f;
			});
			CHECK(copy->ComputeStateHash() != setup.GetScene().ComputeStateHash());
		}

		TEST_CASE("SceneSerializer: the state hash is XXH64 of the minified canonical document")
		{
			Test::SceneTestFixture setup;
			LoadReport report;
			REQUIRE(SceneSerializer::LoadFromString(setup.GetScene(), ReadFixture("Scenes/Hierarchy.scene"), LoadOptions{}, report).has_value());
			const Result<std::string> minified = SceneSerializer::SaveToString(setup.GetScene(), JsonStyle::Minified);
			REQUIRE(minified.has_value());
			CHECK(minified->find('\n') == std::string::npos);
			CHECK(setup.GetScene().ComputeStateHash() == XXH64(*minified));
		}

		TEST_CASE("SceneSerializer: 10,000 seeded mutations of a scene file never crash")
		{
			const std::string original = ReadFixture("Scenes/AllComponents.scene");
			const int64_t lastSymbol = static_cast<int64_t>(MutationAlphabet.size()) - 1;
			Random random(0xf022);
			// One registry for the whole run; every iteration loads into a fresh empty scene with its own repair generator.
			Test::SceneTestFixture setup;
			for (int iteration = 0; iteration < 10000; ++iteration)
			{
				std::string mutated = original;
				const int64_t edits = random.RangeInt(1, 8);
				for (int64_t edit = 0; edit < edits; ++edit)
				{
					const size_t position = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()) - 1));
					const int64_t kind = random.RangeInt(0, 3);
					if (kind == 0)
						mutated[position] = static_cast<char>(random.RangeInt(0, 255));
					else if (kind == 1)
						mutated.erase(position, 1);
					else if (kind == 2)
						mutated.insert(position, 1, MutationAlphabet[static_cast<size_t>(random.RangeInt(0, lastSymbol))]);
					else
						mutated.resize(position);
					if (mutated.empty())
						mutated = "{";
				}

				const Scope<Scene> scene = setup.CreateEmptyScene();
				UUIDGenerator repairIds = UUIDGenerator::CreateDeterministic(static_cast<uint64_t>(iteration) + 1);
				LoadOptions options;
				options.Mode = (iteration % 2 == 0) ? LoadMode::Strict : LoadMode::Repair;
				options.RepairIdGenerator = &repairIds;
				LoadReport report;
				const Status loaded = SceneSerializer::LoadFromString(*scene, mutated, options, report);
				if (!loaded)
					CHECK(scene->GetEntityCount() == 0);
			}
		}

		TEST_CASE("SceneSerializer: entity snapshots round-trip and reapply")
		{
			Test::SceneTestFixture setup;
			LoadReport report;
			REQUIRE(SceneSerializer::LoadFromString(setup.GetScene(), ReadFixture("Scenes/AllComponents.scene"), LoadOptions{}, report).has_value());
			const Entity ball = setup.GetScene().FindEntityByID(UUID(0x2b00000000000005));
			REQUIRE(ball.IsValid());

			const Result<Json> snapshot = SceneSerializer::EntityToJson(ball);
			REQUIRE(snapshot.has_value());
			CHECK((*snapshot)["ID"] == "2b00000000000005");
			CHECK((*snapshot)["Components"].contains("Script"));

			ball.Patch<ScriptComponent>([](ScriptComponent& script)
			{
				script.ExecutionOrder = 99;
			});
			ball.RemoveComponent<RigidBodyComponent>();
			REQUIRE(SceneSerializer::ApplyEntityJson(ball, JsonReader(*snapshot), LoadOptions{}, report).has_value());
			CHECK(ball.GetComponent<ScriptComponent>().ExecutionOrder == -5);
			CHECK(ball.HasComponent<RigidBodyComponent>());

			const Scope<Scene> other = setup.CreateEmptyScene();
			const Result<Entity> created = SceneSerializer::EntityFromJson(*other, JsonReader(*snapshot), std::nullopt, LoadOptions{}, report);
			REQUIRE(created.has_value());
			CHECK(created->GetUUID() == UUID(0x2b00000000000005));
			CHECK(*SceneSerializer::EntityToJson(*created) == *snapshot);
		}

		TEST_CASE("SceneSerializer: ApplyEntityJson restores the name, active state, tags and removed components")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Entity entity = scene.CreateEntity("Original");
			entity.AddTag("A");
			entity.AddTag("B");
			entity.AddComponent<SphereColliderComponent>();
			const Result<Json> snapshot = SceneSerializer::EntityToJson(entity);
			REQUIRE(snapshot.has_value());

			entity.SetName("Renamed");
			entity.SetActive(false);
			entity.RemoveTag("A");
			entity.AddTag("C");
			entity.RemoveComponent<SphereColliderComponent>();
			entity.AddComponent<RigidBodyComponent>();

			LoadReport report;
			REQUIRE(SceneSerializer::ApplyEntityJson(entity, JsonReader(*snapshot), LoadOptions{}, report).has_value());
			CHECK(entity.GetName() == "Original");
			CHECK(entity.IsActiveSelf());
			const std::span<const std::string> tagSpan = entity.GetTags();
			const std::vector<std::string> tags(tagSpan.begin(), tagSpan.end());
			CHECK(tags == std::vector<std::string>{ "A", "B" });
			CHECK(entity.HasComponent<SphereColliderComponent>());
			CHECK_FALSE(entity.HasComponent<RigidBodyComponent>());
			CHECK(*SceneSerializer::EntityToJson(entity) == *snapshot);

			// An unchanged entity is not touched: no revision increment.
			const uint64_t revision = scene.GetRevision();
			REQUIRE(SceneSerializer::ApplyEntityJson(entity, JsonReader(*snapshot), LoadOptions{}, report).has_value());
			CHECK(scene.GetRevision() == revision);
		}

		TEST_CASE("SceneSerializer: change-tracker snapshots are the canonical entity JSON from before the edit")
		{
			// Scene::CaptureEntitySnapshot (Scene/EntitySnapshot.cpp) is the serializer's part of change tracking.
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Entity moved = scene.CreateEntity("Moved");
			const Entity doomed = scene.CreateEntity("Doomed");
			const UUID doomedID = doomed.GetUUID();
			const Result<Json> movedBefore = SceneSerializer::EntityToJson(moved);
			const Result<Json> doomedBefore = SceneSerializer::EntityToJson(doomed);
			REQUIRE(movedBefore.has_value());
			REQUIRE(doomedBefore.has_value());

			scene.GetChangeTracker().Begin();
			moved.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			});
			moved.AddComponent<RigidBodyComponent>();
			scene.DestroyEntity(doomed);
			const std::vector<EntityChange> changes = scene.GetChangeTracker().End();

			REQUIRE(changes.size() == 2);
			for (const EntityChange& change : changes)
			{
				REQUIRE(change.Before != nullptr);
				if (change.EntityID == doomedID)
				{
					CHECK(*change.Before == *doomedBefore);
					continue;
				}
				CHECK(*change.Before == *movedBefore);
				CHECK((*change.Before)["Components"]["Transform"]["Translation"] == Json::array({ 0, 0, 0 }));
				CHECK_FALSE((*change.Before)["Components"].contains("RigidBody"));
			}
		}

		TEST_CASE("SceneSerializer: an invalid component value fails strict loading and repair drops or resets the component")
		{
			const Result<Json> document = JsonReader::Parse(R"({
				"Format": "Scene", "Version": 1, "Name": "BadValues", "Seed": 0,
				"ComponentVersions": { "Transform": 1, "RigidBody": 1 },
				"Entities": [
					{ "ID": "4d00000000000001", "Name": "Ball", "Parent": null, "Active": true, "Tags": [],
					  "Components": {
						"Transform": { "Translation": [0, 0, 0], "Rotation": [0, 0, 0, 1], "Scale": [1, 0, 1] },
						"RigidBody": { "Mass": "heavy" } } }
				]
			})");
			REQUIRE(document.has_value());

			Test::SceneTestFixture setup;
			LoadReport strictReport;
			const Status strict = SceneSerializer::FromJson(setup.GetScene(), *document, LoadOptions{}, strictReport);
			REQUIRE_FALSE(strict.has_value());
			CHECK(strict.error().GetCode() == ErrorCode::Validation);
			CHECK(setup.GetScene().GetEntityCount() == 0);

			LoadOptions repair;
			repair.Mode = LoadMode::Repair;
			repair.RepairIdGenerator = &setup.GetGenerator();
			LoadReport repairReport;
			const Scope<Scene> repaired = setup.CreateEmptyScene();
			REQUIRE(SceneSerializer::FromJson(*repaired, *document, repair, repairReport).has_value());
			const Entity ball = repaired->FindEntityByID(UUID(0x4d00000000000001));
			REQUIRE(ball.IsValid());
			CHECK(ball.GetComponent<TransformComponent>().Scale == glm::vec3(1.0f)); // Required: reset to its defaults
			CHECK_FALSE(ball.HasComponent<RigidBodyComponent>());                    // dropped
			REQUIRE(repairReport.Repairs.size() == 2);
			for (const LoadRepair& fix : repairReport.Repairs)
			{
				CHECK(fix.Code == SceneInvalidComponentCode);
				CHECK(fix.Entity == UUID(0x4d00000000000001));
				CHECK_FALSE(fix.Removed.Get().is_null()); // the rejected JSON
			}
		}

		TEST_CASE("SceneSerializer: files are saved and loaded through the VFS")
		{
			Test::SceneTestFixture setup;
			LoadReport report;
			const std::string text = ReadFixture("Scenes/Hierarchy.scene");
			REQUIRE(SceneSerializer::LoadFromString(setup.GetScene(), text, LoadOptions{}, report).has_value());

			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("project", CreateScope<MemoryMount>()).has_value());
			const Result<VfsPath> directory = VfsPath::Parse("project://Assets");
			const Result<VfsPath> path = VfsPath::Parse("project://Assets/Level.scene");
			REQUIRE(directory.has_value());
			REQUIRE(path.has_value());
			REQUIRE(vfs.CreateDirectories(*directory).has_value());
			REQUIRE(SceneSerializer::SaveToFile(setup.GetScene(), vfs, *path).has_value());
			const Result<std::string> written = vfs.ReadText(*path);
			REQUIRE(written.has_value());
			CHECK(*written == text);

			const Scope<Scene> loaded = setup.CreateEmptyScene();
			REQUIRE(SceneSerializer::LoadFromFile(*loaded, vfs, *path, LoadOptions{}, report).has_value());
			CHECK(loaded->GetEntityCount() == setup.GetScene().GetEntityCount());

			const Result<VfsPath> missing = VfsPath::Parse("project://Assets/Missing.scene");
			REQUIRE(missing.has_value());
			const Scope<Scene> empty = setup.CreateEmptyScene();
			const Status notFound = SceneSerializer::LoadFromFile(*empty, vfs, *missing, LoadOptions{}, report);
			REQUIRE_FALSE(notFound.has_value());
			CHECK(notFound.error().GetCode() == ErrorCode::NotFound);

			const Result<VfsPath> wrongCase = VfsPath::Parse("project://Assets/level.scene");
			REQUIRE(wrongCase.has_value());
			const Status mismatch = SceneSerializer::LoadFromFile(*empty, vfs, *wrongCase, LoadOptions{}, report);
			REQUIRE_FALSE(mismatch.has_value());
			CHECK(mismatch.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("SceneSerializer: documents start with the header and write entity keys, versions and every field in canonical order")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			scene.SetName("Written");
			scene.SetSeed(9);
			const Entity root = scene.CreateEntity("Root");
			const Entity child = scene.CreateEntity("Child", root);
			child.AddTag("Enemy");
			child.SetActive(false);
			child.AddComponent<SphereColliderComponent>();

			const Result<Json> document = SceneSerializer::ToJson(scene);
			REQUIRE(document.has_value());
			CHECK(KeysOf(*document) == std::vector<std::string>{ "Format", "Version", "Name", "Seed", "ComponentVersions", "Entities" });
			CHECK((*document)["Format"] == "Scene");
			CHECK((*document)["Version"] == 1);
			CHECK((*document)["Name"] == "Written");
			CHECK((*document)["Seed"] == 9);
			CHECK(KeysOf((*document)["ComponentVersions"]) == std::vector<std::string>{ "Transform", "SphereCollider" });

			const Json& entities = (*document)["Entities"];
			REQUIRE(entities.size() == 2);
			CHECK(KeysOf(entities[0]) == std::vector<std::string>{ "ID", "Name", "Parent", "Active", "Tags", "Components" });
			CHECK(entities[0]["ID"] == root.GetUUID().ToString());
			CHECK(entities[0]["Parent"].is_null());
			CHECK(entities[0]["Active"] == true);
			CHECK(entities[0]["Tags"] == Json::array());
			CHECK(KeysOf(entities[0]["Components"]) == std::vector<std::string>{ "Transform" });

			CHECK(entities[1]["Parent"] == root.GetUUID().ToString());
			CHECK(entities[1]["Active"] == false);
			CHECK(entities[1]["Tags"] == Json::array({ "Enemy" }));
			CHECK(KeysOf(entities[1]["Components"]) == std::vector<std::string>{ "Transform", "SphereCollider" });
			CHECK(KeysOf(entities[1]["Components"]["SphereCollider"]) == std::vector<std::string>{ "Radius", "Offset", "IsTrigger" });
			CHECK(entities[1]["Components"]["SphereCollider"]["Radius"] == 0.5);
		}

		TEST_CASE("SceneSerializer: a document without optional keys loads with their defaults")
		{
			Test::SceneTestFixture setup;
			LoadReport report;
			const std::string_view text = R"({ "Format": "Scene", "Version": 1, "Entities": [ { "ID": "4e00000000000001" } ] })";
			REQUIRE(SceneSerializer::LoadFromString(setup.GetScene(), text, LoadOptions{}, report).has_value());
			CHECK(setup.GetScene().GetName() == SceneSpecification().Name);
			CHECK(setup.GetScene().GetSeed() == 0);
			const Entity entity = setup.GetScene().FindEntityByID(UUID(0x4e00000000000001));
			REQUIRE(entity.IsValid());
			CHECK(entity.GetName() == "Entity");
			CHECK_FALSE(entity.GetParent().IsValid());
			CHECK(entity.IsActiveSelf());
			CHECK(entity.GetTags().empty());
			CHECK(entity.GetComponent<TransformComponent>().Scale == glm::vec3(1.0f));
		}

		TEST_CASE("SceneSerializer: unknown document and entity keys warn and are dropped, and --strict rejects them")
		{
			const std::string_view text = R"({ "Format": "Scene", "Version": 1, "Name": "Keys", "Seed": 0, "Extra": 1, "ComponentVersions": {},
				"Entities": [ { "ID": "4e00000000000001", "Name": "A", "Parent": null, "Active": true, "Tags": [], "Colour": "red",
					"Components": {} } ] })";
			Test::SceneTestFixture setup;
			LoadReport report;
			REQUIRE(SceneSerializer::LoadFromString(setup.GetScene(), text, LoadOptions{}, report).has_value());
			CHECK(HasDiagnostic(report, SceneUnknownKeyCode, "/Extra", DiagnosticSeverity::Warning));
			CHECK(HasDiagnostic(report, SceneUnknownKeyCode, "/Entities/0/Colour", DiagnosticSeverity::Warning));
			const Result<Json> saved = SceneSerializer::ToJson(setup.GetScene());
			REQUIRE(saved.has_value());
			CHECK_FALSE(saved->contains("Extra"));
			CHECK_FALSE((*saved)["Entities"][0].contains("Colour"));

			Test::SceneTestFixture strictSetup;
			LoadOptions strict;
			strict.StrictUnknowns = true;
			LoadReport strictReport;
			const Status rejected = SceneSerializer::LoadFromString(strictSetup.GetScene(), text, strict, strictReport);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
			CHECK(PointerOf(rejected) == "/Extra");
			CHECK(HasDiagnostic(strictReport, SceneUnknownKeyCode, "/Extra", DiagnosticSeverity::Error));
			CHECK(strictSetup.GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("SceneSerializer: malformed documents and entity keys are located Validation errors in both modes")
		{
			struct MalformedCase
			{
				std::string_view Entities;
				std::string_view Pointer;
			};
			const std::array<MalformedCase, 8> cases = { {
				{ R"([5])", "/Entities/0" },
				{ R"([{ "ID": "4e00000000000001", "Name": 5 }])", "/Entities/0/Name" },
				{ R"([{ "ID": "4e00000000000001", "Parent": 7 }])", "/Entities/0/Parent" },
				{ R"([{ "ID": "4e00000000000001", "Active": "yes" }])", "/Entities/0/Active" },
				{ R"([{ "ID": "4e00000000000001", "Tags": ["A", "A"] }])", "/Entities/0/Tags/1" },
				{ R"([{ "ID": "4e00000000000001", "Tags": [""] }])", "/Entities/0/Tags/0" },
				{ R"([{ "ID": "4e00000000000001", "Components": [] }])", "/Entities/0/Components" },
				{ R"({})", "/Entities" },
			} };

			for (const MalformedCase& malformed : cases)
			{
				const std::string text = std::format(R"({{ "Format": "Scene", "Version": 1, "Entities": {} }})", malformed.Entities);
				INFO(text);
				for (const LoadMode mode : { LoadMode::Strict, LoadMode::Repair })
				{
					Test::SceneTestFixture setup;
					LoadOptions options;
					options.Mode = mode;
					options.RepairIdGenerator = &setup.GetGenerator();
					LoadReport report;
					const Status loaded = SceneSerializer::LoadFromString(setup.GetScene(), text, options, report);
					REQUIRE_FALSE(loaded.has_value());
					CHECK(loaded.error().GetCode() == ErrorCode::Validation);
					CHECK(PointerOf(loaded) == std::string(malformed.Pointer));
					CHECK(setup.GetScene().GetEntityCount() == 0);
				}
			}
		}

		TEST_CASE("SceneSerializer: invalid JSON is a Parse error located by line and column in the source file")
		{
			Test::SceneTestFixture setup;
			LoadOptions options;
			options.SourcePath = "Assets/Scenes/Broken.scene";
			LoadReport report;
			const Status loaded = SceneSerializer::LoadFromString(setup.GetScene(), "{\n\t\"Format\": \"Scene\",\n\t\"Version\": }", options, report);
			REQUIRE_FALSE(loaded.has_value());
			CHECK(loaded.error().GetCode() == ErrorCode::Parse);
			CHECK(loaded.error().GetLocation().File == "Assets/Scenes/Broken.scene");
			CHECK(loaded.error().GetLocation().Line == 3);
			CHECK(setup.GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("SceneSerializer: components that exclude each other fail strict loading and repair keeps the first in registry order")
		{
			const Json document = ParseDocument(R"({ "Format": "Scene", "Version": 1, "Name": "Excludes", "Seed": 0,
				"ComponentVersions": { "Transform": 1, "RigidBody": 1, "CharacterController": 1 },
				"Entities": [ { "ID": "4e00000000000001", "Name": "Both", "Parent": null, "Active": true, "Tags": [],
					"Components": { "CharacterController": {}, "RigidBody": {} } } ] })");

			Test::SceneTestFixture setup;
			LoadReport strictReport;
			const Status strict = SceneSerializer::FromJson(setup.GetScene(), document, LoadOptions{}, strictReport);
			REQUIRE_FALSE(strict.has_value());
			CHECK(strict.error().GetCode() == ErrorCode::Validation);
			CHECK(PointerOf(strict) == "/Entities/0/Components/CharacterController");
			CHECK(setup.GetScene().GetEntityCount() == 0);

			LoadOptions repair;
			repair.Mode = LoadMode::Repair;
			repair.RepairIdGenerator = &setup.GetGenerator();
			LoadReport repairReport;
			const Scope<Scene> repaired = setup.CreateEmptyScene();
			REQUIRE(SceneSerializer::FromJson(*repaired, document, repair, repairReport).has_value());
			const Entity both = repaired->FindEntityByID(UUID(0x4e00000000000001));
			REQUIRE(both.IsValid());
			CHECK(both.HasComponent<RigidBodyComponent>());
			CHECK_FALSE(both.HasComponent<CharacterControllerComponent>());
			REQUIRE(repairReport.Repairs.size() == 1);
			CHECK(repairReport.Repairs[0].Code == SceneInvalidComponentCode);
			CHECK(repairReport.Repairs[0].JsonPointer == "/Entities/0/Components/CharacterController");
			CHECK(repairReport.Repairs[0].Removed.Get().is_object());
		}

		TEST_CASE("SceneSerializer: diagnostics locate entities by their position in the file, not in canonical order")
		{
			// The child comes first in the file and the duplicate-ID entity last; both carry an invalid component.
			const Json document = ParseDocument(R"({ "Format": "Scene", "Version": 1, "Name": "Positions", "Seed": 0,
				"ComponentVersions": { "Transform": 1, "SphereCollider": 1 },
				"Entities": [
					{ "ID": "4e00000000000002", "Name": "Child", "Parent": "4e00000000000001", "Components": { "SphereCollider": { "Radius": "big" } } },
					{ "ID": "4e00000000000001", "Name": "Parent", "Parent": null, "Components": {} },
					{ "ID": "4e00000000000001", "Name": "Copy", "Parent": null, "Components": { "SphereCollider": { "Radius": -1 } } } ] })");

			Test::SceneTestFixture setup;
			LoadOptions repair;
			repair.Mode = LoadMode::Repair;
			repair.RepairIdGenerator = &setup.GetGenerator();
			LoadReport report;
			REQUIRE(SceneSerializer::FromJson(setup.GetScene(), document, repair, report).has_value());

			std::vector<std::string> pointers;
			for (const LoadRepair& fix : report.Repairs)
			{
				if (fix.Code == SceneInvalidComponentCode)
					pointers.push_back(fix.JsonPointer);
			}
			CHECK(pointers == std::vector<std::string>{ "/Entities/0/Components/SphereCollider", "/Entities/2/Components/SphereCollider" });
			CHECK(HasDiagnostic(report, SceneNonCanonicalOrderCode, "/Entities/0/Parent", DiagnosticSeverity::Warning));
			CHECK(setup.GetScene().GetEntityCount() == 3);
		}

		TEST_CASE("SceneSerializer: EntityFromJson rejects a used ID or a missing parent and creates nothing")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Entity existing = scene.CreateEntity("Existing");
			const Result<Json> snapshot = SceneSerializer::EntityToJson(existing);
			REQUIRE(snapshot.has_value());
			LoadReport report;

			const Result<Entity> duplicate = SceneSerializer::EntityFromJson(scene, JsonReader(*snapshot), std::nullopt, LoadOptions{}, report);
			REQUIRE_FALSE(duplicate.has_value());
			CHECK(duplicate.error().GetCode() == ErrorCode::Validation);
			CHECK(duplicate.error().GetLocation().JsonPointer == "/ID");
			CHECK(scene.GetEntityCount() == 1);

			Json orphan = *snapshot;
			orphan["ID"] = "4e000000000000aa";
			orphan["Parent"] = "4e000000000000ff";
			const Result<Entity> missingParent = SceneSerializer::EntityFromJson(scene, JsonReader(orphan), std::nullopt, LoadOptions{}, report);
			REQUIRE_FALSE(missingParent.has_value());
			CHECK(missingParent.error().GetLocation().JsonPointer == "/Parent");
			CHECK(scene.GetEntityCount() == 1);

			Json zero = *snapshot;
			zero["ID"] = "0000000000000000";
			CHECK_FALSE(SceneSerializer::EntityFromJson(scene, JsonReader(zero), std::nullopt, LoadOptions{}, report).has_value());
			CHECK(scene.GetEntityCount() == 1);
		}

		TEST_CASE("SceneSerializer: EntityFromJson rejects a second unique-per-scene component and repair drops it")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Entity sky = scene.CreateEntity("Sky");
			sky.AddComponent<EnvironmentComponent>();
			Result<Json> json = SceneSerializer::EntityToJson(sky);
			REQUIRE(json.has_value());
			(*json)["ID"] = "4e000000000000cc";
			(*json)["Name"] = "SecondSky";

			LoadReport strictReport;
			const Result<Entity> strict = SceneSerializer::EntityFromJson(scene, JsonReader(*json), std::nullopt, LoadOptions{}, strictReport);
			REQUIRE_FALSE(strict.has_value());
			CHECK(strict.error().GetLocation().JsonPointer == "/Components/Environment");
			CHECK(strict.error().GetMessageText().find(SceneDuplicateUniqueComponentCode) != std::string::npos);
			CHECK(scene.GetEntityCount() == 1);

			LoadOptions repair;
			repair.Mode = LoadMode::Repair;
			repair.RepairIdGenerator = &setup.GetGenerator();
			LoadReport repairReport;
			const Result<Entity> repaired = SceneSerializer::EntityFromJson(scene, JsonReader(*json), std::nullopt, repair, repairReport);
			REQUIRE(repaired.has_value());
			CHECK_FALSE(repaired->HasComponent<EnvironmentComponent>());
			REQUIRE(repairReport.Repairs.size() == 1);
			CHECK(repairReport.Repairs[0].Code == SceneDuplicateUniqueComponentCode);
			CHECK(repairReport.Repairs[0].Entity == UUID(0x4e000000000000cc));
		}

		TEST_CASE("SceneSerializer: preserved components keep their versions when an entity snapshot is reapplied")
		{
			const std::string text = ReadFixture("Scenes/UnknownComponent.scene");
			Test::SceneTestFixture setup;
			LoadReport report;
			REQUIRE(SceneSerializer::LoadFromString(setup.GetScene(), text, LoadOptions{}, report).has_value());
			const Entity gadget = setup.GetScene().FindEntityByID(UUID(0x3c00000000000001));
			REQUIRE(gadget.IsValid());
			const Result<Json> snapshot = SceneSerializer::EntityToJson(gadget);
			REQUIRE(snapshot.has_value());
			CHECK((*snapshot)["Components"]["FutureThing"]["Mode"] == "Fast");

			Json edited = *snapshot;
			edited["Components"]["FutureThing"]["Speed"] = 4;
			REQUIRE(SceneSerializer::ApplyEntityJson(gadget, JsonReader(edited), LoadOptions{}, report).has_value());
			REQUIRE(gadget.GetComponent<UnknownComponentsComponent>().Components.size() == 1);
			CHECK(gadget.GetComponent<UnknownComponentsComponent>().Components[0].Version == 3);
			CHECK(gadget.GetComponent<UnknownComponentsComponent>().Components[0].Data.Get()["Speed"] == 4);

			REQUIRE(SceneSerializer::ApplyEntityJson(gadget, JsonReader(*snapshot), LoadOptions{}, report).has_value());
			CHECK(*SceneSerializer::SaveToString(setup.GetScene()) == text);

			Json withoutUnknown = *snapshot;
			withoutUnknown["Components"].erase("FutureThing");
			REQUIRE(SceneSerializer::ApplyEntityJson(gadget, JsonReader(withoutUnknown), LoadOptions{}, report).has_value());
			CHECK_FALSE(gadget.HasComponent<UnknownComponentsComponent>());
			const Result<Json> saved = SceneSerializer::ToJson(setup.GetScene());
			REQUIRE(saved.has_value());
			CHECK_FALSE((*saved)["ComponentVersions"].contains("FutureThing"));
		}

		TEST_CASE("SceneSerializer: EntityFromJson places the entity at the given sibling index")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity first = scene.CreateEntity("First", parent);
			static_cast<void>(scene.CreateEntity("Second", parent));

			Result<Json> json = SceneSerializer::EntityToJson(first);
			REQUIRE(json.has_value());
			(*json)["ID"] = "4e000000000000bb";
			(*json)["Name"] = "Inserted";
			LoadReport report;
			const Result<Entity> inserted = SceneSerializer::EntityFromJson(scene, JsonReader(*json), 0u, LoadOptions{}, report);
			REQUIRE(inserted.has_value());
			CHECK(inserted->GetParent() == parent);
			REQUIRE(parent.GetChildren().size() == 3);
			CHECK(parent.GetChildren()[0] == UUID(0x4e000000000000bb));
			CHECK(inserted->GetSiblingIndex() == 0);
		}

		TEST_CASE("SceneSerializer: repair unpacks the members of an instance root whose Prefab component it drops")
		{
			// The root has the "Prefab" key structural validation looks for, but its value is invalid (a merge left an override
			// of an unknown kind), so repair drops it: the member and the root's own link are unpacked with it.
			const Result<Json> document = JsonReader::Parse(R"({
				"Format": "Scene", "Version": 1, "Name": "BrokenInstance", "Seed": 0,
				"ComponentVersions": { "Transform": 1, "Prefab": 1, "PrefabLink": 1 },
				"Entities": [
					{ "ID": "4f00000000000001", "Name": "Root", "Parent": null, "Active": true, "Tags": [],
					  "Components": {
						"Transform": {},
						"Prefab": { "Prefab": null, "Overrides": [ { "Kind": "Rename" } ] },
						"PrefabLink": { "PrefabEntityID": "00000000000b0001", "InstanceRoot": "4f00000000000001" } } },
					{ "ID": "4f00000000000002", "Name": "Member", "Parent": "4f00000000000001", "Active": true, "Tags": [],
					  "Components": {
						"Transform": {},
						"PrefabLink": { "PrefabEntityID": "00000000000b0002", "InstanceRoot": "4f00000000000001" } } }
				]
			})");
			REQUIRE(document.has_value());

			Test::SceneTestFixture setup;
			LoadReport strictReport;
			CHECK_FALSE(SceneSerializer::FromJson(setup.GetScene(), *document, LoadOptions{}, strictReport).has_value());

			LoadOptions repair;
			repair.Mode = LoadMode::Repair;
			repair.RepairIdGenerator = &setup.GetGenerator();
			LoadReport report;
			REQUIRE(SceneSerializer::FromJson(setup.GetScene(), *document, repair, report).has_value());
			const Entity root = setup.GetScene().FindEntityByID(UUID(0x4f00000000000001));
			const Entity member = setup.GetScene().FindEntityByID(UUID(0x4f00000000000002));
			REQUIRE(root.IsValid());
			REQUIRE(member.IsValid());
			CHECK_FALSE(root.HasComponent<PrefabInstanceComponent>());
			CHECK_FALSE(root.HasComponent<PrefabLinkComponent>());
			CHECK_FALSE(member.HasComponent<PrefabLinkComponent>());

			// Each fix is reported, the dropped Prefab first, then the links in document order.
			REQUIRE(report.Repairs.size() == 3);
			CHECK(report.Repairs[0].Code == SceneInvalidComponentCode);
			CHECK(report.Repairs[0].JsonPointer == "/Entities/0/Components/Prefab");
			for (size_t index = 1; index < report.Repairs.size(); ++index)
			{
				const LoadRepair& unpacked = report.Repairs[index];
				CHECK(unpacked.Code == SceneInconsistentPrefabLinkCode);
				CHECK(unpacked.Removed.Get()["InstanceRoot"] == "4f00000000000001");
			}
			CHECK(report.Repairs[1].Entity == UUID(0x4f00000000000001));
			CHECK(report.Repairs[1].JsonPointer == "/Entities/0/Components/PrefabLink");
			CHECK(report.Repairs[2].Entity == UUID(0x4f00000000000002));
			CHECK(report.Repairs[2].JsonPointer == "/Entities/1/Components/PrefabLink");
			CHECK(HasDiagnostic(report, SceneInconsistentPrefabLinkCode, "/Entities/1/Components/PrefabLink", DiagnosticSeverity::Warning));

			// The repaired scene saves, and loads strictly again (as the play copy does).
			const Result<std::string> saved = SceneSerializer::SaveToString(setup.GetScene());
			REQUIRE(saved.has_value());
			const Scope<Scene> reloaded = setup.CreateEmptyScene();
			LoadReport reloadReport;
			const Status strict = SceneSerializer::LoadFromString(*reloaded, *saved, LoadOptions{}, reloadReport);
			INFO((strict.has_value() ? std::string() : strict.error().ToString()));
			CHECK(strict.has_value());
			CHECK(reloadReport.Repairs.empty());
		}

		TEST_CASE("SceneSerializer: deeply nested prefab overrides in a file are kept with a diagnostic instead of exhausting the stack")
		{
			// Each level is an AddComponent override of "Prefab" whose Value is another prefab instance: three JSON levels per
			// nesting, so 150 levels stay under the reader's depth limit.
			constexpr int Levels = 150;
			Json nested = Json::object();
			nested["Prefab"] = nullptr;
			nested["Overrides"] = Json::array();
			for (int level = 0; level < Levels; ++level)
			{
				Json prefabOverride = Json::object();
				prefabOverride["PrefabEntityID"] = "00000000000b0001";
				prefabOverride["Kind"] = "AddComponent";
				prefabOverride["Component"] = "Prefab";
				prefabOverride["Field"] = "";
				prefabOverride["Value"] = std::move(nested);
				nested = Json::object();
				nested["Prefab"] = nullptr;
				nested["Overrides"] = Json::array({ std::move(prefabOverride) });
			}
			Json document = ParseDocument(R"({ "Format": "Scene", "Version": 1, "Name": "Hostile", "Seed": 0, "ComponentVersions": {},
				"Entities": [ { "ID": "5000000000000001", "Name": "Root", "Parent": null, "Active": true, "Tags": [],
				                "Components": { "Transform": {} } } ] })");
			document["Entities"][0]["Components"]["Prefab"] = std::move(nested);
			const Result<std::string> text = JsonWriter::Write(document, JsonStyle::Minified);
			REQUIRE(text.has_value());

			Test::SceneTestFixture setup;
			LoadReport report;
			REQUIRE(SceneSerializer::LoadFromString(setup.GetScene(), *text, LoadOptions{}, report).has_value());
			CHECK(HasDiagnostic(report, VariantUnresolvedCode, "/Entities/0/Components/Prefab/Overrides/0/Value", DiagnosticSeverity::Warning));
			const Entity root = setup.GetScene().FindEntityByID(UUID(0x5000000000000001));
			REQUIRE(root.IsValid());
			REQUIRE(root.GetComponent<PrefabInstanceComponent>().Overrides.size() == 1);
			CHECK(root.GetComponent<PrefabInstanceComponent>().Overrides[0].Value.Get()["Overrides"].size() == 1);

			// --strict reports the unresolvable value as an error instead.
			const Scope<Scene> strictScene = setup.CreateEmptyScene();
			LoadOptions strict;
			strict.StrictUnknowns = true;
			LoadReport strictReport;
			const Status rejected = SceneSerializer::LoadFromString(*strictScene, *text, strict, strictReport);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
		}
	}

}
