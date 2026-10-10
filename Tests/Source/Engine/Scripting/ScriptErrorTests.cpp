#include "TestsPCH.h"
#include "Engine/Scripting/ScriptError.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptError: a repeated diagnostic advances the cursor and preserves its count" * doctest::skip())
		{
			ScriptErrorStream errors;
			ScriptError first{ .Script = "Assets/Scripts/Ball.luau", .Line = 12, .Message = "bad field" };
			const uint64_t firstId = errors.Add(first).ID;
			first.Tick = 18;
			const ScriptError repeated = errors.Add(first);
			CHECK(repeated.ID > firstId);
			CHECK(repeated.Count == 2);
			CHECK(repeated.Tick == 18);
			const auto updates = errors.Read(firstId);
			REQUIRE(updates.size() == 1);
			CHECK(updates.front().Count == 2);
			CHECK(errors.GetErrors().size() == 1);
		}

		TEST_CASE("ScriptError: JSON retains structured source frames and entity identity" * doctest::skip())
		{
			FAIL("M13 contract: exact §11.7 schema with owned strings and canonical identities");
		}

		TEST_CASE("ScriptError: separate embedded replay expectations retain distinct cursors and authored locations" * doctest::skip())
		{
			FAIL("M13 contract: identical text and Luau line in different JSON pointers remain separate diagnostics");
		}
	}

}
