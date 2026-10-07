#include "TestsPCH.h"

#include "EditorCore/Automation/ComponentMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Support/AutomationTestClient.h"

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ComponentMethods: component.list lists the serializable components in registry order")
		{
			Test::AutomationFixture setup("ComponentList", false);
			Result<Json> list = setup.Call("component.list", Json::object());
			REQUIRE(list.has_value());
			std::vector<std::string> names;
			for (Json& component : (*list)["components"])
			{
				names.push_back(JsonReader(component["name"]).ReadString().value_or(std::string()));
				if (component["name"] == Json("RigidBody"))
				{
					CHECK(component["category"] == Json("Physics"));
					CHECK(component["requires"] == Json::array({ "Transform" }));
					CHECK(component["excludes"] == Json::array({ "CharacterController" }));
				}
			}
			CHECK(std::find(names.begin(), names.end(), "Transform") != names.end());
			CHECK(std::find(names.begin(), names.end(), "Script") != names.end());
		}

		TEST_CASE("ComponentMethods: component.schema describes fields, ranges, defaults and enum values")
		{
			Test::AutomationFixture setup("ComponentSchema", false);
			Result<Json> schema = setup.Call("component.schema", Json{ { "name", "RigidBody" } });
			REQUIRE(schema.has_value());
			CHECK((*schema)["component"]["name"] == Json("RigidBody"));
			bool sawMass = false;
			bool sawType = false;
			for (Json& field : (*schema)["fields"])
			{
				if (field["name"] == Json("Mass"))
				{
					sawMass = true;
					CHECK(field["unit"] == Json("kg"));
					// A float field's bound is the float value, as its validator compares it (JsonSchema rounds bounds the same
					// way); over the wire the canonical writer prints it as 0.001.
					CHECK(field["min"] == Json(static_cast<double>(0.001f)));
					CHECK(field["default"] == Json(1));
				}
				if (field["name"] == Json("Type"))
				{
					sawType = true;
					CHECK(field["enumValues"] == Json::array({ "Static", "Kinematic", "Dynamic" }));
				}
			}
			CHECK(sawMass);
			CHECK(sawType);
			CHECK(JsonSchema::Validate((*schema)["schema"], Json{ { "Mass", 2 } }).has_value());

			const Result<Json> unknown = setup.Call("component.schema", Json{ { "name", "RigidBdy" } });
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
		}
	}

}
