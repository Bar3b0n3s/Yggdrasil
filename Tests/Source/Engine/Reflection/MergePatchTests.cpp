#include "TestsPCH.h"

#include "Engine/Reflection/MergePatch.h"

#include "Engine/Core/Json/JsonReader.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static Json ParsePatchJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("MergePatch: matches the RFC 7386 appendix A test cases" * doctest::skip(true))
		{
			struct Case
			{
				std::string_view Target;
				std::string_view Patch;
				std::string_view Expected;
			};
			const Case cases[] = {
				{ R"({"a":"b"})", R"({"a":"c"})", R"({"a":"c"})" },
				{ R"({"a":"b"})", R"({"b":"c"})", R"({"a":"b","b":"c"})" },
				{ R"({"a":"b"})", R"({"a":null})", R"({})" },
				{ R"({"a":"b","b":"c"})", R"({"a":null})", R"({"b":"c"})" },
				{ R"({"a":["b"]})", R"({"a":"c"})", R"({"a":"c"})" },
				{ R"({"a":"c"})", R"({"a":["b"]})", R"({"a":["b"]})" },
				{ R"({"a":{"b":"c"}})", R"({"a":{"b":"d","c":null}})", R"({"a":{"b":"d"}})" },
				{ R"({"a":[{"b":"c"}]})", R"({"a":[1]})", R"({"a":[1]})" },
				{ R"(["a","b"])", R"(["c","d"])", R"(["c","d"])" },
				{ R"({"a":"b"})", R"(["c"])", R"(["c"])" },
				{ R"({"a":"foo"})", R"(null)", R"(null)" },
				{ R"({"a":"foo"})", R"("bar")", R"("bar")" },
				{ R"({"e":null})", R"({"a":1})", R"({"e":null,"a":1})" },
				{ R"([1,2])", R"({"a":"b","c":null})", R"({"a":"b"})" },
				{ R"({})", R"({"a":{"bb":{"ccc":null}}})", R"({"a":{"bb":{}}})" },
			};
			for (const Case& testCase : cases)
			{
				INFO(std::string(testCase.Target), " + ", std::string(testCase.Patch));
				CHECK(ApplyMergePatch(ParsePatchJson(testCase.Target), ParsePatchJson(testCase.Patch)) == ParsePatchJson(testCase.Expected));
			}
		}

		TEST_CASE("MergePatch: new members are appended in patch order and survivors keep their order" * doctest::skip(true))
		{
			const Json result = ApplyMergePatch(ParsePatchJson(R"({"z":1,"a":2})"), ParsePatchJson(R"({"m":3,"z":4})"));
			std::vector<std::string> keys;
			for (auto it = result.begin(); it != result.end(); ++it)
				keys.push_back(it.key());
			CHECK(keys == std::vector<std::string>{ "z", "a", "m" });
		}

		TEST_CASE("MergePatch: CreateMergePatch produces a patch that reproduces the target" * doctest::skip(true))
		{
			const Json source = ParsePatchJson(R"({"Mass":1,"Layer":"Default","Scores":{"a":1,"b":2},"Tags":["x"]})");
			const Json target = ParsePatchJson(R"({"Mass":2,"Layer":"Default","Scores":{"b":3,"c":4},"Tags":["x","y"]})");
			const Json patch = CreateMergePatch(source, target);
			CHECK(patch == ParsePatchJson(R"({"Mass":2,"Scores":{"b":3,"c":4,"a":null},"Tags":["x","y"]})"));
			CHECK(ApplyMergePatch(source, patch) == target);
			CHECK(CreateMergePatch(target, target) == Json::object());
		}
	}

}
