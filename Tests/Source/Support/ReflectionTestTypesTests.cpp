#include "TestsPCH.h"

#include "Support/ReflectionTestTypes.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("ReflectionTestTypes: TestAllFields covers every FieldType and TestComponent can be randomized" * doctest::skip(true))
		{
			TypeRegistry registry;
			Test::RegisterReflectionTestTypes(registry);
			registry.Freeze();

			const StructInfo* allFields = registry.FindStruct<Test::TestAllFields>();
			REQUIRE(allFields != nullptr);
			std::vector<FieldType> kinds;
			for (const Scope<FieldInfo>& field : allFields->GetFields())
			{
				if (std::find(kinds.begin(), kinds.end(), field->GetKind()) == kinds.end())
					kinds.push_back(field->GetKind());
			}
			CHECK(kinds.size() == 19); // every FieldType (FieldTypeTests: "every kind has a distinct name")

			// TestComponent has a validator, so Freeze requires its Generate hook (the registry suite's random values).
			const ComponentInfo* component = registry.FindComponent<Test::TestComponent>();
			REQUIRE(component != nullptr);
			CHECK(component->HasValidators());
			CHECK(component->HasGenerators());
			CHECK(component->GetHostOps() == nullptr); // Reflection-only: registered without a scene host
		}
	}

}
