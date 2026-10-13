#include "TestsPCH.h"

#include "EditorCore/Scripting/ScriptTypeChecker.h"

#include "Support/TestData.h"

#include <string>
#include <vector>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		// Architecture §15.5 "Errors.scene": compile and type error kinds are covered by unit fixtures, not by the project
		// (ADR 0022 decision 15). Tests/Data/Scripts/Errors/*.luau and Expected.json are written by MakeScriptFixtures.py;
		// each entry pins {File, Kind (compile|type), Code, Line, Column, Message (a substring)}.
		TEST_CASE("ScriptErrors: each Tests/Data/Scripts/Errors fixture reports its kind, code and location" * doctest::skip(true))
		{
			const auto sources = Test::ListTestDataFiles("Scripts/Errors", { ".luau" });
			REQUIRE_MESSAGE(sources.has_value(), sources.error().ToString());
			REQUIRE_FALSE(sources->empty());
			const auto expected = Test::ReadTestDataText("Scripts/Errors/Expected.json");
			REQUIRE_MESSAGE(expected.has_value(), expected.error().ToString());
			FAIL("contract skeleton: import or check each fixture and compare kind, code, line, column and message with Expected.json");
		}
	}

}
