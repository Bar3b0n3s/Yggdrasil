#include "TestsPCH.h"

#include "Engine/Core/Result.h"

namespace Engine {

	static Status LoadLeaf(bool fail)
	{
		if (!fail)
			return {};

		ErrorLocation location;
		location.File = "Leaf.json";
		location.Line = 3;
		return std::unexpected(Error(ErrorCode::NotFound, "no file 'Leaf.json'").WithLocation(std::move(location)).WithHint("create it"));
	}

	static Status LoadMiddle(bool fail)
	{
		ENGINE_TRY(WithContext(LoadLeaf(fail), "while loading 'Middle.json'"));
		return {};
	}

	static Status LoadRoot(bool fail)
	{
		ENGINE_TRY(WithContext(LoadMiddle(fail), "while opening the project"));
		return {};
	}

	static Result<int> ParsePositive(int value)
	{
		if (value <= 0)
			return MakeError(ErrorCode::InvalidArgument, "{} is not positive", value);
		return value;
	}

	static Result<int> Doubled(int value)
	{
		ENGINE_TRY_ASSIGN(const int parsed, ParsePositive(value));
		return parsed * 2;
	}

	static Result<std::string> Describe(int value)
	{
		ENGINE_TRY(ParsePositive(value));
		return std::format("{} is fine", value);
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("Result: ENGINE_TRY propagates the error with context")
		{
			CHECK(LoadRoot(false).has_value());

			const Status status = LoadRoot(true);
			REQUIRE_FALSE(status.has_value());
			const Error& error = status.error();
			CHECK(error.GetCode() == ErrorCode::NotFound);
			CHECK(error.GetMessageText() == "no file 'Leaf.json'");
			REQUIRE(error.GetContexts().size() == 2);
			CHECK(error.GetContexts()[0] == "while loading 'Middle.json'");
			CHECK(error.GetContexts()[1] == "while opening the project");
			CHECK(error.GetLocation().File == "Leaf.json");
			CHECK(error.GetLocation().Line == 3);
			CHECK(error.GetHint() == "create it");
		}

		TEST_CASE("Result: ENGINE_TRY propagates into a Result of another value type")
		{
			const Result<std::string> described = Describe(-1);
			REQUIRE_FALSE(described.has_value());
			CHECK(described.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(described.error().GetMessageText() == "-1 is not positive");

			const Result<std::string> fine = Describe(4);
			REQUIRE(fine.has_value());
			CHECK(*fine == "4 is fine");
		}

		TEST_CASE("Result: ENGINE_TRY_ASSIGN declares the value or returns the error")
		{
			const Result<int> doubled = Doubled(21);
			REQUIRE(doubled.has_value());
			CHECK(*doubled == 42);

			const Result<int> failed = Doubled(0);
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(failed.error().GetMessageText() == "0 is not positive");
		}

		TEST_CASE("Result: ENGINE_TRY evaluates its expression exactly once")
		{
			int calls = 0;
			auto countedCall = [&calls]() -> Status
			{
				++calls;
				return {};
			};
			auto propagate = [&countedCall]() -> Status
			{
				ENGINE_TRY(countedCall());
				return {};
			};

			CHECK(propagate().has_value());
			CHECK(calls == 1);
		}

		TEST_CASE("Result: MakeError formats its message with the given code")
		{
			const Status status = MakeError(ErrorCode::Timeout, "waited {} ms for '{}'", 250, "Editor.lock");
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::Timeout);
			CHECK(status.error().GetMessageText() == "waited 250 ms for 'Editor.lock'");
			CHECK(status.error().GetContexts().empty());
			CHECK_FALSE(status.error().GetLocation().IsSet());
		}

		TEST_CASE("Result: WithContext leaves a success untouched")
		{
			const Result<int> value = WithContext(Result<int>(7), "while counting");
			REQUIRE(value.has_value());
			CHECK(*value == 7);

			const Status ok = WithContext(Status(), "while doing nothing");
			CHECK(ok.has_value());
		}
	}

}
