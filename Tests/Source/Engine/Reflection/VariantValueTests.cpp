#include "TestsPCH.h"

#include "Engine/Reflection/VariantValue.h"

#include "Engine/Core/Json/JsonReader.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Reflection")
	{
		TEST_CASE("VariantValue: holds one JSON value verbatim and defaults to null" * doctest::skip(true))
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

		TEST_CASE("VariantValue: copies share the payload and Set replaces only one instance" * doctest::skip(true))
		{
			VariantValue first(Json(42));
			const VariantValue second = first;
			CHECK(first == second);
			CHECK(&first.Get() == &second.Get());

			first.Set(Json("changed"));
			CHECK(second.Get() == Json(42));
			CHECK_FALSE(first == second);
		}
	}

}
