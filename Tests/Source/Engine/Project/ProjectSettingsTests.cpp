#include "TestsPCH.h"

#include "Engine/Project/ProjectSettings.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	// Collects the JSON pointer of every field below `type` that does not hold a non-default value anywhere in `actual`
	// (gate 8, §15.6). Struct fields recurse; arrays and maps of structs count a field as covered when any element has a
	// non-default value for it; other arrays and maps count as covered when they differ from the default.
	static void CollectDefaultFields(const StructInfo& type, const std::vector<const Json*>& actuals, const Json& defaults,
		const std::string& pointer, std::vector<std::string>& uncovered)
	{
		for (const Scope<FieldInfo>& field : type.GetFields())
		{
			if (!field->GetMeta().Serialized)
				continue;
			const std::string& name = field->GetName();
			const std::string fieldPointer = JsonReader::AppendPointer(pointer, name);
			const TypeInfo& fieldType = field->GetType();

			if (fieldType.GetKind() == FieldType::Struct)
			{
				std::vector<const Json*> nested;
				for (const Json* actual : actuals)
				{
					if (actual->contains(name))
						nested.push_back(&(*actual)[name]);
				}
				CollectDefaultFields(*fieldType.GetStruct(), nested, defaults[name], fieldPointer, uncovered);
				continue;
			}

			const TypeInfo* element = fieldType.GetElement();
			if (element != nullptr && element->GetKind() == FieldType::Struct)
			{
				std::vector<const Json*> elements;
				for (const Json* actual : actuals)
				{
					if (!actual->contains(name))
						continue;
					for (const Json& value : (*actual)[name])
						elements.push_back(&value);
				}
				if (elements.empty())
					uncovered.push_back(fieldPointer);
				else
					CollectDefaultFields(*element->GetStruct(), elements, element->GetStruct()->MakeDefaultJson(), fieldPointer + "/*", uncovered);
				continue;
			}

			const bool covered = std::any_of(actuals.begin(), actuals.end(), [&](const Json* actual)
			{
				return actual->contains(name) && (*actual)[name] != defaults[name];
			});
			if (!covered)
				uncovered.push_back(fieldPointer);
		}
	}

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
			CollectDefaultFields(*type, { &*actual }, type->MakeDefaultJson(), "", uncovered);
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
