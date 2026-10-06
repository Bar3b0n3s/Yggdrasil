#include "TestsPCH.h"

#include "Engine/Scene/StructuralValidator.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <tuple>

// One test per structural-defect fixture in Tests/Data/Scenes/Invalid/ (Architecture §6, Roadmap M3): strict loading
// returns a located Result and never asserts; repair loading applies the documented fix and reports it.

namespace Engine {

	namespace {

		struct InvalidLoadOutcome
		{
			Status Strict;
			Status Repair;
			LoadReport StrictReport;
			LoadReport RepairReport;
		};

	}

	static bool HasCode(const std::vector<LoadDiagnostic>& diagnostics, std::string_view code)
	{
		return std::any_of(diagnostics.begin(), diagnostics.end(), [code](const LoadDiagnostic& diagnostic)
		{
			return diagnostic.Code == code;
		});
	}

	static bool HasRepair(const std::vector<LoadRepair>& repairs, std::string_view code)
	{
		return std::any_of(repairs.begin(), repairs.end(), [code](const LoadRepair& repair)
		{
			return repair.Code == code;
		});
	}

	// Loads `fixture` strictly into `strictScene` and with repairs into `repairScene`.
	static InvalidLoadOutcome LoadInvalidFixture(Test::SceneTestFixture& setup, Scene& strictScene, Scene& repairScene, std::string_view fixture)
	{
		const Result<std::string> text = Test::ReadTestDataText(fixture);
		REQUIRE(text.has_value());

		InvalidLoadOutcome outcome{ Status{}, Status{}, {}, {} };
		LoadOptions strict;
		strict.SourcePath = std::string(fixture);
		outcome.Strict = SceneSerializer::LoadFromString(strictScene, *text, strict, outcome.StrictReport);

		LoadOptions repair = strict;
		repair.Mode = LoadMode::Repair;
		repair.RepairIdGenerator = &setup.GetGenerator();
		outcome.Repair = SceneSerializer::LoadFromString(repairScene, *text, repair, outcome.RepairReport);
		return outcome;
	}

	// The strict load failed with a Validation error located in the fixture at `pointer`, and created nothing.
	static void CheckStrictFailure(const InvalidLoadOutcome& outcome, const Scene& strictScene, std::string_view fixture, std::string_view code,
		std::string_view pointer)
	{
		REQUIRE_FALSE(outcome.Strict.has_value());
		CHECK(outcome.Strict.error().GetCode() == ErrorCode::Validation);
		CHECK(outcome.Strict.error().GetLocation().File == fixture);
		CHECK(outcome.Strict.error().GetLocation().JsonPointer == std::string(pointer));
		CHECK(outcome.Strict.error().GetMessageText().find(code) != std::string::npos);
		CHECK(HasCode(outcome.StrictReport.Diagnostics, code));
		CHECK(strictScene.GetEntityCount() == 0);
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("StructuralValidator: duplicate IDs fail strict loading and repair gives the later duplicate a fresh ID" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Scope<Scene> repaired = setup.CreateEmptyScene();
			const InvalidLoadOutcome outcome = LoadInvalidFixture(setup, setup.GetScene(), *repaired, "Scenes/Invalid/DuplicateIds.scene");
			CheckStrictFailure(outcome, setup.GetScene(), "Scenes/Invalid/DuplicateIds.scene", SceneDuplicateIdCode, "/Entities/2/ID");

			REQUIRE(outcome.Repair.has_value());
			CHECK(HasRepair(outcome.RepairReport.Repairs, SceneDuplicateIdCode));
			CHECK(repaired->GetEntityCount() == 3);
			const Entity first = repaired->FindEntityByID(UUID(0x4d00000000000001));
			REQUIRE(first.IsValid());
			CHECK(first.GetName() == "First");
			CHECK(repaired->FindEntityByPath("/First/Child").IsValid()); // children stay with the first
			CHECK(repaired->FindEntityByPath("/Duplicate").IsValid());
			CHECK(repaired->FindEntityByPath("/Duplicate").GetUUID() != UUID(0x4d00000000000001));
		}

		TEST_CASE("StructuralValidator: a zero ID fails strict loading and repair assigns a fresh ID" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Scope<Scene> repaired = setup.CreateEmptyScene();
			const InvalidLoadOutcome outcome = LoadInvalidFixture(setup, setup.GetScene(), *repaired, "Scenes/Invalid/ZeroId.scene");
			CheckStrictFailure(outcome, setup.GetScene(), "Scenes/Invalid/ZeroId.scene", SceneInvalidIdCode, "/Entities/1/ID");

			REQUIRE(outcome.Repair.has_value());
			CHECK(HasRepair(outcome.RepairReport.Repairs, SceneInvalidIdCode));
			const Entity zero = repaired->FindEntityByPath("/Zero");
			REQUIRE(zero.IsValid());
			CHECK(zero.GetUUID().IsValid());
		}

		TEST_CASE("StructuralValidator: a dangling parent fails strict loading and repair reparents the orphan to the root" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Scope<Scene> repaired = setup.CreateEmptyScene();
			const InvalidLoadOutcome outcome = LoadInvalidFixture(setup, setup.GetScene(), *repaired, "Scenes/Invalid/DanglingParent.scene");
			CheckStrictFailure(outcome, setup.GetScene(), "Scenes/Invalid/DanglingParent.scene", SceneDanglingParentCode, "/Entities/1/Parent");

			REQUIRE(outcome.Repair.has_value());
			CHECK(HasRepair(outcome.RepairReport.Repairs, SceneDanglingParentCode));
			const Entity orphan = repaired->FindEntityByID(UUID(0x4d00000000000002));
			REQUIRE(orphan.IsValid());
			CHECK_FALSE(orphan.GetParent().IsValid());
			CHECK(repaired->GetRootEntities().size() == 2);
		}

		TEST_CASE("StructuralValidator: a parent cycle fails strict loading and repair cuts it at its first back-edge" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Scope<Scene> repaired = setup.CreateEmptyScene();
			const InvalidLoadOutcome outcome = LoadInvalidFixture(setup, setup.GetScene(), *repaired, "Scenes/Invalid/ParentCycle.scene");
			CheckStrictFailure(outcome, setup.GetScene(), "Scenes/Invalid/ParentCycle.scene", SceneParentCycleCode, "/Entities/1/Parent");

			REQUIRE(outcome.Repair.has_value());
			CHECK(HasRepair(outcome.RepairReport.Repairs, SceneParentCycleCode));
			// A (first in file order) loses its parent; B stays A's child.
			const Entity a = repaired->FindEntityByID(UUID(0x4d00000000000002));
			const Entity b = repaired->FindEntityByID(UUID(0x4d00000000000003));
			REQUIRE(a.IsValid());
			REQUIRE(b.IsValid());
			CHECK_FALSE(a.GetParent().IsValid());
			CHECK(b.GetParent() == a);
		}

		TEST_CASE("StructuralValidator: a child before its parent loads in both modes with SCENE_NONCANONICAL_ORDER" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Scope<Scene> repaired = setup.CreateEmptyScene();
			const InvalidLoadOutcome outcome = LoadInvalidFixture(setup, setup.GetScene(), *repaired, "Scenes/Invalid/ChildBeforeParent.scene");

			const std::array<std::tuple<const Status*, const LoadReport*, Scene*>, 2> loads = {
				std::tuple(&outcome.Strict, &outcome.StrictReport, &setup.GetScene()),
				std::tuple(&outcome.Repair, &outcome.RepairReport, repaired.get()),
			};
			for (const auto& [status, report, scene] : loads)
			{
				REQUIRE(status->has_value());
				CHECK(HasCode(report->Diagnostics, SceneNonCanonicalOrderCode));
				CHECK(report->Repairs.empty());
				const Entity child = scene->FindEntityByID(UUID(0x4d00000000000002));
				REQUIRE(child.IsValid());
				CHECK(child.GetParent().GetUUID() == UUID(0x4d00000000000001));
				REQUIRE(scene->GetCanonicalOrder().size() == 2);
				CHECK(scene->GetCanonicalOrder()[0] == UUID(0x4d00000000000001));
			}
		}

		TEST_CASE("StructuralValidator: a PrefabLink to a missing root fails strict loading and repair unpacks the member" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Scope<Scene> repaired = setup.CreateEmptyScene();
			const InvalidLoadOutcome outcome = LoadInvalidFixture(setup, setup.GetScene(), *repaired, "Scenes/Invalid/PrefabLinkMissingRoot.scene");
			CheckStrictFailure(outcome, setup.GetScene(), "Scenes/Invalid/PrefabLinkMissingRoot.scene", SceneInconsistentPrefabLinkCode,
				"/Entities/0/Components/PrefabLink/InstanceRoot");

			REQUIRE(outcome.Repair.has_value());
			CHECK(HasRepair(outcome.RepairReport.Repairs, SceneInconsistentPrefabLinkCode));
			const Entity member = repaired->FindEntityByID(UUID(0x4d00000000000001));
			REQUIRE(member.IsValid());
			CHECK_FALSE(member.HasComponent<PrefabLinkComponent>());
		}

		TEST_CASE("StructuralValidator: a PrefabLink to a non-instance root fails strict loading and repair unpacks the member" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Scope<Scene> repaired = setup.CreateEmptyScene();
			const std::string_view fixture = "Scenes/Invalid/PrefabLinkRootNotInstance.scene";
			const InvalidLoadOutcome outcome = LoadInvalidFixture(setup, setup.GetScene(), *repaired, fixture);
			CheckStrictFailure(outcome, setup.GetScene(), fixture, SceneInconsistentPrefabLinkCode,
				"/Entities/1/Components/PrefabLink/InstanceRoot");

			REQUIRE(outcome.Repair.has_value());
			const Entity member = repaired->FindEntityByID(UUID(0x4d00000000000002));
			REQUIRE(member.IsValid());
			CHECK_FALSE(member.HasComponent<PrefabLinkComponent>());
		}

		TEST_CASE("StructuralValidator: a duplicated unique component fails strict loading and repair drops the extra" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Scope<Scene> repaired = setup.CreateEmptyScene();
			const std::string_view fixture = "Scenes/Invalid/DuplicateUniqueComponent.scene";
			const InvalidLoadOutcome outcome = LoadInvalidFixture(setup, setup.GetScene(), *repaired, fixture);
			CheckStrictFailure(outcome, setup.GetScene(), fixture, SceneDuplicateUniqueComponentCode,
				"/Entities/1/Components/Environment");

			REQUIRE(outcome.Repair.has_value());
			REQUIRE(outcome.RepairReport.Repairs.size() == 1);
			const LoadRepair& repair = outcome.RepairReport.Repairs[0];
			CHECK(repair.Code == SceneDuplicateUniqueComponentCode);
			CHECK(repair.Entity == UUID(0x4d00000000000002));
			CHECK(repair.Removed.Get()["Intensity"] == 1); // the dropped JSON is in the report
			CHECK(repaired->FindEntityByID(UUID(0x4d00000000000001)).HasComponent<EnvironmentComponent>());
			CHECK_FALSE(repaired->FindEntityByID(UUID(0x4d00000000000002)).HasComponent<EnvironmentComponent>());
		}

		TEST_CASE("StructuralValidator: a misplaced entity-level component fails strict loading and repair drops it" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Scope<Scene> repaired = setup.CreateEmptyScene();
			const InvalidLoadOutcome outcome = LoadInvalidFixture(setup, setup.GetScene(), *repaired, "Scenes/Invalid/MisplacedComponent.scene");
			CheckStrictFailure(outcome, setup.GetScene(), "Scenes/Invalid/MisplacedComponent.scene", SceneMisplacedComponentCode,
				"/Entities/1/Components/Name");

			REQUIRE(outcome.Repair.has_value());
			REQUIRE(outcome.RepairReport.Repairs.size() == 1);
			const LoadRepair& repair = outcome.RepairReport.Repairs[0];
			CHECK(repair.Code == SceneMisplacedComponentCode);
			CHECK(repair.Entity == UUID(0x4d00000000000002));
			CHECK(repair.Removed.Get()["Name"] == "Shadow");
			const Entity misplaced = repaired->FindEntityByID(UUID(0x4d00000000000002));
			REQUIRE(misplaced.IsValid());
			CHECK(misplaced.GetName() == "Misplaced"); // the entity key wins; the misplaced copy is never applied
		}

		TEST_CASE("StructuralValidator: prefab documents need exactly one root named by Root" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const Result<Json> document = JsonReader::Parse(R"({
				"Format": "Prefab", "Version": 1, "Name": "Broken", "Root": "00000000000b0009",
				"ComponentVersions": {},
				"Entities": [ { "ID": "00000000000b0001", "Name": "A", "Parent": null, "Active": true, "Tags": [], "Components": {} },
				              { "ID": "00000000000b0002", "Name": "B", "Parent": null, "Active": true, "Tags": [], "Components": {} } ]
			})");
			REQUIRE(document.has_value());
			LoadReport report;
			LoadOptions options;
			options.Mode = LoadMode::Repair;
			Test::SceneTestFixture setup;
			options.RepairIdGenerator = &setup.GetGenerator();
			const Result<Json> result = StructuralValidator::Validate(*document, DocumentKind::Prefab, *registry, options, report);
			REQUIRE_FALSE(result.has_value()); // not repairable
			CHECK(result.error().GetMessageText().find(PrefabInvalidRootCode) != std::string::npos);
		}
	}

}
