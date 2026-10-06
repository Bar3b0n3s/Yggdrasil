#include "TestsPCH.h"

#include "Engine/Reflection/VariantValue.h"

#include "Engine/Core/Json/JsonReader.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Reflection")
	{
		TEST_CASE("VariantValue: holds one JSON value verbatim and defaults to null")
		{
			const VariantValue empty;
			CHECK(empty.IsNull());
			CHECK(empty.Get().is_null());

			const Result<Json> json = JsonReader::Parse(R"({ "b": [1, 2], "a": "x" })");
			REQUIRE(json.has_value());
			const VariantValue value(*json);
			CHECK_FALSE(value.IsNull());
			CHECK(value.Get() == *json);
			CHECK(value.Get().begin().key() == "b"); // member order is kept
		}

		TEST_CASE("VariantValue: copies share the payload and Set replaces only one instance")
		{
			VariantValue first(Json(42));
			const VariantValue second = first;
			CHECK(first == second);
			CHECK(&first.Get() == &second.Get());

			first.Set(Json("changed"));
			CHECK(second.Get() == Json(42));
			CHECK_FALSE(first == second);
		}

		TEST_CASE("VariantValue: setting JSON null empties it and equality is deep")
		{
			VariantValue value(Json::array({ 1, 2 }));
			CHECK_FALSE(value.IsNull());
			value.Set(Json());
			CHECK(value.IsNull());
			CHECK(value == VariantValue());

			// Distinct payloads with equal JSON compare equal; member order is significant.
			const Result<Json> ab = JsonReader::Parse(R"({ "a": 1, "b": 2 })");
			const Result<Json> ba = JsonReader::Parse(R"({ "b": 2, "a": 1 })");
			REQUIRE(ab.has_value());
			REQUIRE(ba.has_value());
			CHECK(VariantValue(*ab) == VariantValue(*ab));
			CHECK_FALSE(VariantValue(*ab) == VariantValue(*ba));
		}
	}

}
