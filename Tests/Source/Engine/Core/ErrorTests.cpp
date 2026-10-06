#include "TestsPCH.h"

#include "Engine/Core/Error.h"

namespace Engine {

	static ErrorLocation MakeLocation(std::string file, uint32_t line, uint32_t column)
	{
		ErrorLocation location;
		location.File = std::move(file);
		location.Line = line;
		location.Column = column;
		return location;
	}

	static ErrorIssue MakeIssue(std::string pointer, std::string message, std::string hint = {},
		std::vector<std::string> suggestions = {})
	{
		ErrorIssue issue;
		issue.JsonPointer = std::move(pointer);
		issue.Message = std::move(message);
		issue.Hint = std::move(hint);
		issue.Suggestions = std::move(suggestions);
		return issue;
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("Error: hint and location survive WithContext")
		{
			ErrorLocation location = MakeLocation("Assets/Scenes/Level1.scene", 12, 7);
			location.JsonPointer = "/Entities/3/Name";
			location.Entity = UUID(0x5d1c9a7e33b04f12);

			const Error error = Error(ErrorCode::Validation, "expected string, got number")
									.WithLocation(location)
									.WithHint("names are strings")
									.WithContext("while loading scene 'Level1'")
									.WithContext("while opening the project");

			CHECK(error.GetCode() == ErrorCode::Validation);
			CHECK(error.GetMessageText() == "expected string, got number");
			CHECK(error.GetHint() == "names are strings");
			CHECK(error.GetLocation().File == "Assets/Scenes/Level1.scene");
			CHECK(error.GetLocation().Line == 12);
			CHECK(error.GetLocation().Column == 7);
			CHECK(error.GetLocation().JsonPointer == "/Entities/3/Name");
			CHECK(error.GetLocation().Entity == UUID(0x5d1c9a7e33b04f12));
			REQUIRE(error.GetContexts().size() == 2);
			CHECK(error.GetContexts()[0] == "while loading scene 'Level1'");
			CHECK(error.GetContexts()[1] == "while opening the project");
		}

		TEST_CASE("Error: WithLocation merges only the fields that are set")
		{
			ErrorLocation pointerOnly;
			pointerOnly.JsonPointer = "/Simulation/FixedHz";

			const Error error = Error(ErrorCode::Validation, "must be > 0")
									.WithLocation(pointerOnly)
									.WithLocation(MakeLocation("Game.eproj", 0, 0));

			CHECK(error.GetLocation().JsonPointer == "/Simulation/FixedHz");
			CHECK(error.GetLocation().File == "Game.eproj");
			CHECK(error.GetLocation().Line == 0);
			CHECK_FALSE(error.GetLocation().Entity.IsValid());
		}

		TEST_CASE("Error: the root pointer is a set location that WithLocation keeps and merges")
		{
			ErrorLocation root;
			root.JsonPointer = "";
			CHECK(root.IsSet());

			const Error error = Error(ErrorCode::Validation, "missing required field 'Format'")
									.WithLocation(root)
									.WithLocation(MakeLocation("Game.eproj", 0, 0));
			REQUIRE(error.GetLocation().JsonPointer.has_value());
			CHECK(error.GetLocation().JsonPointer->empty());
			CHECK(error.GetLocation().File == "Game.eproj");

			const Error unset = Error(ErrorCode::Validation, "no pointer").WithLocation(MakeLocation("Game.eproj", 3, 0));
			CHECK_FALSE(unset.GetLocation().JsonPointer.has_value());
		}

		TEST_CASE("Error: issues keep their order and survive WithContext")
		{
			std::vector<ErrorIssue> issues;
			issues.push_back(MakeIssue("/components/RigidBody/Mas", "unknown field 'Mas' on 'RigidBody'", "did you mean 'Mass'?",
				{ "Mass" }));
			issues.push_back(MakeIssue("/components/RigidBody/Friction", "must be >= 0 (got -0.2)"));

			const Error error = Error(ErrorCode::Validation, "2 invalid fields")
									.WithIssues(std::move(issues))
									.WithIssue(MakeIssue("", "missing required field 'entity'"))
									.WithContext("while running 'entity.addComponent'");

			REQUIRE(error.GetIssues().size() == 3);
			CHECK(error.GetIssues()[0].JsonPointer == "/components/RigidBody/Mas");
			CHECK(error.GetIssues()[0].Hint == "did you mean 'Mass'?");
			CHECK(error.GetIssues()[0].Suggestions == std::vector<std::string>{ "Mass" });
			CHECK(error.GetIssues()[1].Message == "must be >= 0 (got -0.2)");
			CHECK(error.GetIssues()[1].Hint.empty());
			CHECK(error.GetIssues()[1].Suggestions.empty());
			CHECK(error.GetIssues()[2].JsonPointer.empty());
			CHECK(error.GetMessageText() == "2 invalid fields");
			CHECK(error.GetContexts().size() == 1);
		}

		TEST_CASE("Error: WithHint replaces the previous hint")
		{
			const Error error = Error(ErrorCode::NotFound, "no component 'Mas'").WithHint("first").WithHint("did you mean 'Mass'?");
			CHECK(error.GetHint() == "did you mean 'Mass'?");
		}

		TEST_CASE("Error: ToString renders code, location, message, contexts and hint" * doctest::skip(true))
		{
			SUBCASE("message only")
			{
				CHECK(Error(ErrorCode::NotFound, "no such file").ToString() == "NotFound: no such file");
			}

			SUBCASE("file and pointer, context and hint")
			{
				ErrorLocation location;
				location.File = "Assets/Scenes/Level1.scene";
				location.JsonPointer = "/Entities/12/Components/RigidBody/Mass";
				const Error error = Error(ErrorCode::Validation, "expected number, got string")
										.WithLocation(location)
										.WithContext("while loading scene 'Level1'")
										.WithHint("write a number such as 1.5");
				CHECK(error.ToString()
					== "Validation: Assets/Scenes/Level1.scene /Entities/12/Components/RigidBody/Mass: expected number, got string; "
					   "while loading scene 'Level1' (hint: write a number such as 1.5)");
			}

			SUBCASE("line, column and entity")
			{
				ErrorLocation location = MakeLocation("Game.luau", 4, 9);
				location.Entity = UUID(0xff);
				const Error error = Error(ErrorCode::Script, "attempt to index nil").WithLocation(location);
				CHECK(error.ToString() == "Script: Game.luau:4:9 entity 00000000000000ff: attempt to index nil");
			}

			SUBCASE("several contexts in propagation order")
			{
				const Error error = Error(ErrorCode::Io, "disk full").WithContext("while writing 'a'").WithContext("while saving");
				CHECK(error.ToString() == "Io: disk full; while writing 'a'; while saving");
			}

			SUBCASE("the root pointer")
			{
				ErrorLocation root;
				root.JsonPointer = "";
				const Error error = Error(ErrorCode::Validation, "missing required field 'Format'").WithLocation(root);
				CHECK(error.ToString() == "Validation: (root): missing required field 'Format'");
			}

			SUBCASE("issues with hints and suggestions")
			{
				const Error error = Error(ErrorCode::Validation, "2 invalid fields")
										.WithIssue(MakeIssue("/components/RigidBody/Mas", "unknown field 'Mas' on 'RigidBody'",
											"did you mean 'Mass'?", { "Mass", "MassScale" }))
										.WithIssue(MakeIssue("", "missing required field 'entity'"))
										.WithContext("while running 'entity.addComponent'");
				CHECK(error.ToString()
					== "Validation: 2 invalid fields; while running 'entity.addComponent'"
					   " | /components/RigidBody/Mas: unknown field 'Mas' on 'RigidBody' (hint: did you mean 'Mass'?)"
					   " (suggestions: Mass, MassScale) | (root): missing required field 'entity'");
			}
		}

		TEST_CASE("Error: ErrorCodeToString names every code" * doctest::skip(true))
		{
			CHECK(ErrorCodeToString(ErrorCode::Unknown) == "Unknown");
			CHECK(ErrorCodeToString(ErrorCode::InvalidArgument) == "InvalidArgument");
			CHECK(ErrorCodeToString(ErrorCode::NotFound) == "NotFound");
			CHECK(ErrorCodeToString(ErrorCode::AlreadyExists) == "AlreadyExists");
			CHECK(ErrorCodeToString(ErrorCode::InvalidState) == "InvalidState");
			CHECK(ErrorCodeToString(ErrorCode::Io) == "Io");
			CHECK(ErrorCodeToString(ErrorCode::Parse) == "Parse");
			CHECK(ErrorCodeToString(ErrorCode::Validation) == "Validation");
			CHECK(ErrorCodeToString(ErrorCode::UnsupportedVersion) == "UnsupportedVersion");
			CHECK(ErrorCodeToString(ErrorCode::ImportFailed) == "ImportFailed");
			CHECK(ErrorCodeToString(ErrorCode::CompileFailed) == "CompileFailed");
			CHECK(ErrorCodeToString(ErrorCode::Script) == "Script");
			CHECK(ErrorCodeToString(ErrorCode::Gpu) == "Gpu");
			CHECK(ErrorCodeToString(ErrorCode::Timeout) == "Timeout");
			CHECK(ErrorCodeToString(ErrorCode::PermissionDenied) == "PermissionDenied");
			CHECK(ErrorCodeToString(ErrorCode::Unsupported) == "Unsupported");
			CHECK(ErrorCodeToString(ErrorCode::Conflict) == "Conflict");
			CHECK(ErrorCodeToString(ErrorCode::Cancelled) == "Cancelled");
			CHECK(ErrorCodeToString(static_cast<ErrorCode>(0xffff)) == "Unknown");
		}

		TEST_CASE("Error: std::format writes ToString and code names" * doctest::skip(true))
		{
			const Error error(ErrorCode::Conflict, "revision 7 is not 9");
			CHECK(std::format("{}", error) == error.ToString());
			CHECK(std::format("{}", ErrorCode::Conflict) == "Conflict");
		}

		TEST_CASE("ErrorLocation: IsSet is true when any field is set")
		{
			ErrorLocation location;
			CHECK_FALSE(location.IsSet());
			location.Column = 1;
			CHECK(location.IsSet());

			ErrorLocation entityOnly;
			entityOnly.Entity = UUID(1);
			CHECK(entityOnly.IsSet());

			ErrorLocation rootPointer;
			rootPointer.JsonPointer = "";
			CHECK(rootPointer.IsSet());
		}
	}

}
