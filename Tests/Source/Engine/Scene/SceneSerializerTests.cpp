#include "TestsPCH.h"

#include "Engine/Scene/SceneSerializer.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/ChangeTracker.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
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

	TEST_SUITE("Scene")
	{
		TEST_CASE("SceneSerializer: every fixture under Tests/Data/Scenes re-saves byte-identically" * doctest::skip(true))
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

		TEST_CASE("SceneSerializer: unknown components are preserved" * doctest::skip(true))
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

		TEST_CASE("SceneSerializer: newer Version fails with UnsupportedVersion" * doctest::skip(true))
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

		TEST_CASE("SceneSerializer: serializer copy has the same state hash" * doctest::skip(true))
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

		TEST_CASE("SceneSerializer: 10,000 seeded mutations of a scene file never crash" * doctest::skip(true))
		{
			const std::string original = ReadFixture("Scenes/AllComponents.scene");
			const int64_t lastSymbol = static_cast<int64_t>(MutationAlphabet.size()) - 1;
			Random random(0xf022);
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

				Test::SceneTestFixture setup(static_cast<uint64_t>(iteration) + 1);
				LoadOptions options;
				options.Mode = (iteration % 2 == 0) ? LoadMode::Strict : LoadMode::Repair;
				options.RepairIdGenerator = &setup.GetGenerator();
				LoadReport report;
				const Status loaded = SceneSerializer::LoadFromString(setup.GetScene(), mutated, options, report);
				if (!loaded)
					CHECK(setup.GetScene().GetEntityCount() == 0);
			}
		}

		TEST_CASE("SceneSerializer: entity snapshots round-trip and reapply" * doctest::skip(true))
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

		TEST_CASE("SceneSerializer: change-tracker snapshots are the canonical entity JSON from before the edit" * doctest::skip(true))
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

		TEST_CASE("SceneSerializer: an invalid component value fails strict loading and repair drops or resets the component" * doctest::skip(true))
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

		TEST_CASE("SceneSerializer: files are saved and loaded through the VFS" * doctest::skip(true))
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
	}

}
