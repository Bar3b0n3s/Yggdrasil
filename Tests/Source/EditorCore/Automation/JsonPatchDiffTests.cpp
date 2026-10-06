#include "TestsPCH.h"

#include "EditorCore/Automation/JsonPatchDiff.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static Json ParseDiffJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	// The value `pointer` names inside `document` (created when `create`), for the test's own patch applier.
	static Json* ResolveDiffPointer(Json& document, std::string_view pointer, bool create)
	{
		Json* current = &document;
		size_t position = 1;
		while (position <= pointer.size() && !pointer.empty())
		{
			const size_t end = std::min(pointer.find('/', position), pointer.size());
			std::string token(pointer.substr(position, end - position));
			for (size_t found = token.find("~1"); found != std::string::npos; found = token.find("~1", found))
				token.replace(found, 2, "/");
			for (size_t found = token.find("~0"); found != std::string::npos; found = token.find("~0", found))
				token.replace(found, 2, "~");
			if (current->is_array())
				current = &(*current)[std::stoul(token)];
			else if (create || current->contains(token))
				current = &(*current)[token];
			else
				return nullptr;
			position = end + 1;
		}
		return current;
	}

	// Applies add, remove and replace operations (what DiffJson emits) to a copy of `document`.
	static Json ApplyDiffPatch(Json document, const Json& patch)
	{
		for (const Json& operation : patch)
		{
			const JsonReader reader(operation);
			const std::string op = reader.ReadMember<std::string>("op").value_or("");
			const std::string path = reader.ReadMember<std::string>("path").value_or("");
			if (op == "remove")
			{
				const size_t slash = path.rfind('/');
				Json* parent = ResolveDiffPointer(document, path.substr(0, slash), false);
				REQUIRE(parent != nullptr);
				std::string key = path.substr(slash + 1);
				if (parent->is_array())
					parent->erase(std::stoul(key));
				else
					parent->erase(key);
				continue;
			}
			Json* target = path.empty() ? &document : ResolveDiffPointer(document, path, true);
			REQUIRE(target != nullptr);
			*target = Json(operation)["value"];
		}
		return document;
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("JsonPatchDiff: equal documents give an empty patch" * doctest::skip(true))
		{
			const Json document = ParseDiffJson(R"({"a":[1,2],"b":{"c":null}})");
			CHECK(DiffJson(document, document) == Json::array());
		}

		TEST_CASE("JsonPatchDiff: members are added, removed and replaced with escaped pointers" * doctest::skip(true))
		{
			const Json from = ParseDiffJson(R"({"Name":"A","Gone":1,"Components":{"Transform":{"Translation":[0,0,0]}},"a/b":1})");
			const Json to = ParseDiffJson(R"({"Name":"B","Components":{"Transform":{"Translation":[0,2,0]},"Camera":{}},"a/b":2})");
			const Json patch = DiffJson(from, to);
			CHECK(patch == ParseDiffJson(R"([
				{"op":"remove","path":"/Gone"},
				{"op":"replace","path":"/Name","value":"B"},
				{"op":"replace","path":"/Components/Transform/Translation/1","value":2},
				{"op":"add","path":"/Components/Camera","value":{}},
				{"op":"replace","path":"/a~1b","value":2}])"));
		}

		TEST_CASE("JsonPatchDiff: arrays of equal length diff by index and others are replaced whole" * doctest::skip(true))
		{
			CHECK(DiffJson(ParseDiffJson("[1,2,3]"), ParseDiffJson("[1,5,3]")) == ParseDiffJson(R"([{"op":"replace","path":"/1","value":5}])"));
			CHECK(DiffJson(ParseDiffJson("[1,2]"), ParseDiffJson("[1,2,3]")) == ParseDiffJson(R"([{"op":"replace","path":"","value":[1,2,3]}])"));
			CHECK(DiffJson(ParseDiffJson(R"({"a":1})"), ParseDiffJson("[1]")) == ParseDiffJson(R"([{"op":"replace","path":"","value":[1]}])"));
		}

		TEST_CASE("JsonPatchDiff: applying the patch to the source yields the target for random documents" * doctest::skip(true))
		{
			Random random(6902);
			const auto makeValue = [&random](int depth, const auto& self) -> Json
			{
				switch (random.RangeInt(0, depth > 2 ? 2 : 4))
				{
					case 0: return Json(random.RangeInt(-3, 3));
					case 1: return Json(std::format("s{}", random.RangeInt(0, 2)));
					case 2: return Json();
					case 3:
					{
						Json array = Json::array();
						for (int64_t index = random.RangeInt(0, 3); index > 0; --index)
							array.push_back(self(depth + 1, self));
						return array;
					}
					default:
					{
						Json object = Json::object();
						for (int64_t index = random.RangeInt(0, 3); index > 0; --index)
							object[std::format("k{}", random.RangeInt(0, 4))] = self(depth + 1, self);
						return object;
					}
				}
			};
			for (int iteration = 0; iteration < 500; ++iteration)
			{
				const Json from = makeValue(0, makeValue);
				const Json to = makeValue(0, makeValue);
				INFO(from.dump() << " -> " << to.dump());
				CHECK(ApplyDiffPatch(from, DiffJson(from, to)) == to);
			}
		}
	}

}
