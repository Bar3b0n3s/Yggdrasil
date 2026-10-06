#include "TestsPCH.h"

#include "Engine/Reflection/FieldType.h"

#include <set>

namespace Engine {

	TEST_SUITE("Reflection")
	{
		TEST_CASE("FieldType: every kind has a distinct name")
		{
			std::set<std::string_view> names;
			for (uint8_t value = 0; value <= std::to_underlying(FieldType::Variant); ++value)
			{
				const std::string_view name = FieldTypeToString(static_cast<FieldType>(value));
				CHECK(name != "Unknown");
				CHECK(names.insert(name).second);
			}
			CHECK(names.size() == 19);
			CHECK(FieldTypeToString(FieldType::Color3) == "Color3");
			CHECK(FieldTypeToString(static_cast<FieldType>(200)) == "Unknown");
		}

		TEST_CASE("FieldType: helpers classify container, scalar and numeric kinds")
		{
			CHECK(IsContainerFieldType(FieldType::Array));
			CHECK(IsContainerFieldType(FieldType::Map));
			CHECK_FALSE(IsContainerFieldType(FieldType::Struct));
			CHECK_FALSE(IsContainerFieldType(FieldType::Variant));

			CHECK(IsScalarFieldType(FieldType::Variant));
			CHECK(IsScalarFieldType(FieldType::Enum));
			CHECK(IsScalarFieldType(FieldType::AssetRef));
			CHECK_FALSE(IsScalarFieldType(FieldType::Array));
			CHECK_FALSE(IsScalarFieldType(FieldType::Map));
			CHECK_FALSE(IsScalarFieldType(FieldType::Struct));

			CHECK(IsNumericFieldType(FieldType::Float));
			CHECK(IsNumericFieldType(FieldType::Color4));
			CHECK(IsNumericFieldType(FieldType::UInt32));
			CHECK_FALSE(IsNumericFieldType(FieldType::Quat));
			CHECK_FALSE(IsNumericFieldType(FieldType::Bool3));
			CHECK_FALSE(IsNumericFieldType(FieldType::Enum));
		}

		TEST_CASE("RunModes: EditorOnly is the editor bit and All covers every mode")
		{
			static_assert(RunModes::EditorOnly == RunModes::Editor);
			CHECK(HasFlag(RunModes::All, RunModes::Editor));
			CHECK(HasFlag(RunModes::All, RunModes::Release));
			CHECK(HasFlag(RunModes::All, RunModes::Dist));
			CHECK_FALSE(HasFlag(RunModes::EditorOnly, RunModes::Dist));
			CHECK((RunModes::Editor | RunModes::Release | RunModes::Dist) == RunModes::All);
		}
	}

}
