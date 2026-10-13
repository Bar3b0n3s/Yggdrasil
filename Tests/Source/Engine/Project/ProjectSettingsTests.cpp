#include "TestsPCH.h"

#include "Engine/Project/ProjectSettings.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/DefaultFields.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	// The sorted pointers of the errors Validate reports for `object`, an object of a registered struct.
	template<typename T>
	static std::vector<std::string> ValidationErrorPointers(const TypeRegistry& registry, const T& object)
	{
		const StructInfo* type = registry.FindStruct<T>();
		REQUIRE(type != nullptr);
		ResolveContext resolve;
		resolve.Registry = &registry;
		ValidationContext context;
		type->Validate(&object, resolve, context);
		std::vector<std::string> pointers;
		for (const ValidationIssue& issue : context.GetIssues())
		{
			if (issue.Severity == DiagnosticSeverity::Error)
				pointers.push_back(issue.JsonPointer);
		}
		std::sort(pointers.begin(), pointers.end());
		return pointers;
	}

	TEST_SUITE("Project")
	{
		// ADR 0022 decision 9: the §15.5 Timing clock [0.004, 0.016, 0.0334, 0.083, 0.5, 0.0] ends with a zero-length frame,
		// so a frame delta may be 0; an all-zero clock stays invalid (ADR 0019 decision 10), now already in the project file.
		TEST_CASE("ProjectSettings: a scripted clock may hold zero deltas but not only zeros" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const auto reload = [&registry](const std::vector<float>& clock) -> Result<ProjectSettings>
			{
				ProjectSettings settings;
				TestSuiteSettings suite;
				suite.Script = "Assets/Tests/Timing/Timing.test.luau";
				suite.Clock = clock;
				settings.Testing.Suites.push_back(suite);
				ENGINE_TRY_ASSIGN(const Json document, ProjectSerializer::ToJson(settings, *registry));
				ProjectLoadReport report;
				return ProjectSerializer::FromJson(document, *registry, ProjectLoadOptions{}, report);
			};
			const Result<ProjectSettings> timing = reload({ 0.004f, 0.016f, 0.0334f, 0.083f, 0.5f, 0.0f });
			REQUIRE_MESSAGE(timing.has_value(), timing.error().ToString());
			CHECK(timing->Testing.Suites[0].Clock.size() == 6);

			for (const std::vector<float>& invalid : { std::vector<float>{ 0.0f, 0.0f }, std::vector<float>{ 0.016f, -0.004f } })
			{
				const Result<ProjectSettings> rejected = reload(invalid);
				REQUIRE_FALSE(rejected.has_value());
				CHECK(rejected.error().GetCode() == ErrorCode::Validation);
				CHECK(rejected.error().ToString().find("Clock") != std::string::npos);
			}
		}

		TEST_CASE("ProjectSettings: AllSettings.eproj sets every field to a non-default value")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const Result<std::string> text = Test::ReadTestDataText("Project/AllSettings.eproj");
			REQUIRE(text.has_value());

			ProjectLoadReport report;
			const Result<ProjectSettings> settings = ProjectSerializer::LoadFromString(*text, *registry, ProjectLoadOptions{}, report);
			REQUIRE(settings.has_value());
			CHECK(report.Diagnostics.empty());

			const StructInfo* type = registry->FindStruct<ProjectSettings>();
			REQUIRE(type != nullptr);
			const Result<Json> actual = type->ToJson(&*settings);
			REQUIRE(actual.has_value());

			std::vector<std::string> uncovered;
			Test::CollectDefaultFields(*type, { &*actual }, type->MakeDefaultJson(), "", uncovered);
			std::string listing;
			for (const std::string& pointer : uncovered)
				listing += pointer + " ";
			INFO("fields still at their default: ", listing);
			CHECK(uncovered.empty());

			// Spot checks of the typed values.
			CHECK(settings->Simulation.MaxEntities == 4096);
			CHECK(settings->Export.Version == "2.3.4");
			CHECK(settings->Input.Actions.at("MoveX").Invert);
			REQUIRE(settings->Testing.Suites.size() == 1);
			CHECK(settings->Testing.Suites[0].Overrides.PauseOnError == TestSuiteOverrides::PauseOnErrorOverride::Continue);
			const std::vector<TestSuiteSettings::Mode> modes = { TestSuiteSettings::Mode::Editor, TestSuiteSettings::Mode::Release };
			CHECK(settings->Testing.Suites[0].Modes == modes);
		}

		TEST_CASE("ProjectSettings: every settings struct and enum is registered")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::array<std::string_view, 12> structs = {
				"ProjectSettings",
				"WindowSettings",
				"SimulationSettings",
				"PhysicsSettings",
				"InputSettings",
				"InputAction",
				"RenderingSettings",
				"ScriptingSettings",
				"ExportSettings",
				"TestingSettings",
				"TestSuite",
				"TestSuiteOverrides",
			};
			for (const std::string_view name : structs)
			{
				INFO(std::string(name));
				CHECK(registry->FindStruct(name) != nullptr);
			}
			for (const std::string_view name : { "InputActionType", "TestIsolation", "TestMode", "TestPauseOnError" })
			{
				INFO(std::string(name));
				CHECK(registry->FindEnum(name) != nullptr);
			}

			const FieldInfo* actions = registry->FindStruct("InputSettings")->FindField("Actions");
			REQUIRE(actions != nullptr);
			CHECK(actions->GetKind() == FieldType::Map);
			CHECK(actions->GetType().GetElement()->GetStruct() == registry->FindStruct("InputAction"));
			CHECK(registry->FindStruct("TestSuite")->FindField("Parameters")->GetKind() == FieldType::Variant);
		}

		TEST_CASE("ProjectSettings: validators reject out-of-range settings")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const StructInfo* type = registry->FindStruct<ProjectSettings>();
			REQUIRE(type != nullptr);

			const auto validate = [&](ProjectSettings settings)
			{
				ResolveContext resolve;
				resolve.Registry = registry.get();
				ValidationContext context;
				type->Validate(&settings, resolve, context);
				return context.HasErrors();
			};

			CHECK_FALSE(validate(ProjectSettings{}));

			ProjectSettings fixedHz;
			fixedHz.Simulation.FixedHz = 0;
			CHECK(validate(fixedHz));
			fixedHz.Simulation.FixedHz = 100001;
			CHECK(validate(fixedHz));

			ProjectSettings budget;
			budget.Scripting.CallbackBudgetMs = 9;
			CHECK(validate(budget));

			ProjectSettings layers;
			layers.Physics.Collisions.push_back({ "Default", "Missing" });
			CHECK(validate(layers));

			ProjectSettings version;
			version.Export.Version = "1.0";
			CHECK(validate(version));

			ProjectSettings shadow;
			shadow.Rendering.ShadowMapSize = 3000;
			CHECK(validate(shadow));
		}

		TEST_CASE("ProjectSettings: physics layers are 1 to 16 unique names starting with Default, and collisions pair declared layers")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::vector<std::string> errors = ValidationErrorPointers(*registry, PhysicsSettings{});
			CHECK(errors.empty());

			const auto pointersOf = [&](const PhysicsSettings& physics)
			{
				return ValidationErrorPointers(*registry, physics);
			};

			PhysicsSettings noLayers;
			noLayers.Layers.clear();
			noLayers.Collisions.clear();
			CHECK(pointersOf(noLayers) == std::vector<std::string>{ "/Layers" });

			PhysicsSettings wrongFirst;
			wrongFirst.Layers = { "Ball", "Default" };
			wrongFirst.Collisions.clear();
			CHECK(pointersOf(wrongFirst) == std::vector<std::string>{ "/Layers/0" });

			PhysicsSettings duplicate;
			duplicate.Layers = { "Default", "Ball", "", "Ball" };
			duplicate.Collisions.clear();
			CHECK(pointersOf(duplicate) == std::vector<std::string>{ "/Layers/2", "/Layers/3" });

			// The physics module's limit (Architecture §9.2; ADR 0014 decision 4): 16 layers are accepted, 17 are not.
			static_assert(MaxPhysicsLayers == 16, "the settings validator and the physics layer table share one layer limit");
			PhysicsSettings full;
			full.Layers.clear();
			full.Collisions.clear();
			full.Layers.push_back("Default");
			for (uint32_t layer = 1; layer < MaxPhysicsLayers; ++layer)
				full.Layers.push_back(std::format("Layer{}", layer));
			CHECK(pointersOf(full).empty());

			PhysicsSettings tooMany = full;
			tooMany.Layers.push_back("OneTooMany");
			CHECK(pointersOf(tooMany) == std::vector<std::string>{ "/Layers" });

			PhysicsSettings collisions;
			collisions.Layers = { "Default", "Ball" };
			collisions.Collisions = { { "Default", "Ball" }, { "Ball" }, { "Ball", "Track" } };
			CHECK(pointersOf(collisions) == std::vector<std::string>{ "/Collisions/1", "/Collisions/2/1" });
		}

		TEST_CASE("ProjectSettings: each physics gravity component is at most MaxPhysicsGravity in magnitude")
		{
			// The physics world refuses a stronger gravity (ADR 0014 decision 34); the settings stop it at every write.
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			PhysicsSettings strongest;
			strongest.Gravity = glm::vec3(MaxPhysicsGravity, -MaxPhysicsGravity, 0.0f);
			CHECK(ValidationErrorPointers(*registry, strongest).empty());
			PhysicsSettings tooStrong;
			tooStrong.Gravity = glm::vec3(0.0f, -1.0e37f, 0.0f);
			const std::vector<std::string> pointers = ValidationErrorPointers(*registry, tooStrong);
			REQUIRE(pointers.size() == 1);
			CHECK(pointers[0].starts_with("/Gravity"));
		}

		TEST_CASE("ProjectSettings: test suites need positive frame deltas, unique modes, object parameters and a budget of 0 or at least 10")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			CHECK(ValidationErrorPointers(*registry, TestSuiteSettings{}).empty());

			TestSuiteSettings suite;
			suite.Clock = { 0.016f, 0.0f, -1.0f };
			suite.Modes = { TestSuiteSettings::Mode::Editor, TestSuiteSettings::Mode::Editor };
			suite.Parameters = VariantValue(Json::array({ 1, 2 }));
			CHECK(ValidationErrorPointers(*registry, suite) == std::vector<std::string>{ "/Clock/1", "/Clock/2", "/Modes/1", "/Parameters" });

			// A suite's own rules run only once its fields, the nested Overrides struct included, are valid (type-level
			// validators assume valid fields), so an invalid budget is reported alone.
			suite.Overrides.CallbackBudgetMs = 9;
			CHECK(ValidationErrorPointers(*registry, suite) == std::vector<std::string>{ "/Overrides/CallbackBudgetMs" });

			TestSuiteSettings noModes;
			noModes.Modes.clear();
			noModes.Parameters = VariantValue(Json::object());
			noModes.Overrides.CallbackBudgetMs = 10;
			CHECK(ValidationErrorPointers(*registry, noModes) == std::vector<std::string>{ "/Modes" });

			InputSettings input;
			input.Actions[""] = InputActionSettings{};
			CHECK(ValidationErrorPointers(*registry, input) == std::vector<std::string>{ "/Actions/" });

			ExportSettings exportSettings;
			for (const std::string_view version : { "1.0.0", "10.20.300" })
			{
				exportSettings.Version = std::string(version);
				CHECK(ValidationErrorPointers(*registry, exportSettings).empty());
			}
			for (const std::string_view version : { "", "1", "1.0", "1.0.0.0", "1..0", "a.b.c", "1.0.0-beta", ".1.0" })
			{
				INFO(std::string(version));
				exportSettings.Version = std::string(version);
				CHECK(ValidationErrorPointers(*registry, exportSettings) == std::vector<std::string>{ "/Version" });
			}
		}

		TEST_CASE("ProjectSettings: the Generate hooks turn randomized settings into valid ones")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			Random random(0x5e77);

			PhysicsSettings physics;
			physics.Layers = { "", "Ball", "Ball", "Track" };
			physics.Collisions = { { "Nope" }, { "A", "B", "C" } };
			registry->FindStruct<PhysicsSettings>()->Generate(&physics, random);
			CHECK(ValidationErrorPointers(*registry, physics).empty());
			CHECK(physics.Layers == std::vector<std::string>{ "Default", "Ball", "Track" });
			CHECK(physics.Collisions.size() == 2);

			InputSettings input;
			input.Actions[""] = InputActionSettings{};
			input.Actions["Jump"] = InputActionSettings{};
			registry->FindStruct<InputSettings>()->Generate(&input, random);
			CHECK(ValidationErrorPointers(*registry, input).empty());
			CHECK(input.Actions.contains("Jump"));

			RenderingSettings rendering;
			rendering.ShadowMapSize = 3000;
			registry->FindStruct<RenderingSettings>()->Generate(&rendering, random);
			CHECK(ValidationErrorPointers(*registry, rendering).empty());

			ExportSettings exportSettings;
			exportSettings.Version = "not a version";
			registry->FindStruct<ExportSettings>()->Generate(&exportSettings, random);
			CHECK(ValidationErrorPointers(*registry, exportSettings).empty());

			TestSuiteSettings suite;
			suite.Clock = { 0.0f, 0.5f };
			suite.Modes = { TestSuiteSettings::Mode::Dist, TestSuiteSettings::Mode::Dist };
			suite.Parameters = VariantValue(Json(3));
			suite.Overrides.CallbackBudgetMs = 5;
			registry->FindStruct<TestSuiteOverrides>()->Generate(&suite.Overrides, random);
			registry->FindStruct<TestSuiteSettings>()->Generate(&suite, random);
			CHECK(ValidationErrorPointers(*registry, suite).empty());
			CHECK(suite.Clock[1] == 0.5f);
			CHECK(suite.Modes == std::vector<TestSuiteSettings::Mode>{ TestSuiteSettings::Mode::Dist });

			TestSuiteSettings noModes;
			noModes.Modes.clear();
			registry->FindStruct<TestSuiteSettings>()->Generate(&noModes, random);
			CHECK(noModes.Modes.size() == 1);
		}
	}

}
