#include "TestsPCH.h"

#include "Engine/Project/ProjectSettings.h"

#include "Engine/Core/Json/JsonReader.h"
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

	TEST_SUITE("Project")
	{
		TEST_CASE("ProjectSettings: AllSettings.eproj sets every field to a non-default value" * doctest::skip(true))
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

		TEST_CASE("ProjectSettings: every settings struct and enum is registered" * doctest::skip(true))
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

		TEST_CASE("ProjectSettings: validators reject out-of-range settings" * doctest::skip(true))
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
	}

}
