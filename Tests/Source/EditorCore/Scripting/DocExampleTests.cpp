#include "TestsPCH.h"

#include "EditorCore/Scripting/ScriptTypeChecker.h"

#include "Support/TestData.h"

#include <filesystem>
#include <system_error>

namespace Engine {

	namespace {

		// ADR 0022 decision 14: at least this many blocks must be checked, so a broken extractor cannot pass vacuously. Stream
		// D sets it to the count at completion; it is raised as documents grow and lowered only with a reason in the review.
		constexpr size_t MinimumCheckedBlocks = 1;

	}

	TEST_SUITE("EditorCore")
	{
		// Roadmap M14 acceptance; Architecture §11.9 (ADR 0022 decision 14). Every fenced block whose info string starts with
		// lua or luau in Docs/*.md, Docs/Reference/*.md, AGENTS.md, Projects/*/AGENTS.md, Resources/Templates/**/*.md and
		// .claude/skills/*/SKILL.md (Docs/Decisions/ excluded) is checked with the engine's strict checker at its path=
		// (default Assets/Scripts/DocExample.luau), wrapped by its prelude= stub, unless it carries skip="<reason>". Stub
		// modules and preludes live in Tests/Data/TypeChecker/DocExamples/. Zero Error findings pass; at least
		// MinimumCheckedBlocks blocks must be checked.
		TEST_CASE("DocExamples: every Luau example in the docs type-checks" * doctest::skip(true))
		{
			std::error_code error;
			REQUIRE(std::filesystem::is_directory(Test::GetTestDataPath("TypeChecker/DocExamples/Assets"), error));
			const size_t checked = 0;
			CHECK(checked >= MinimumCheckedBlocks);
			FAIL("contract skeleton: extract every Luau block of the documents above and check it with ScriptTypeChecker::CheckScript");
		}

		// Tests/Data/TypeChecker/DocExamples/Seeded/Seeded.md holds one example with a type error; the finding is reported at
		// the document's line, through the fence and prelude offsets.
		TEST_CASE("DocExamples: a seeded mistake is reported at its document line" * doctest::skip(true))
		{
			const auto seeded = Test::ReadTestDataText("TypeChecker/DocExamples/Seeded/Seeded.md");
			REQUIRE_MESSAGE(seeded.has_value(), seeded.error().ToString());
			FAIL("contract skeleton: check the seeded document and compare the finding's line with its Expected.json");
		}
	}

}
