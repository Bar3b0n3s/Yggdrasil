#include "TestsPCH.h"

#include "Engine/Core/UniqueFunction.h"

#include "Support/DeathTest.h"

namespace Engine {

	ENGINE_DEATH_TEST("Core/EmptyUniqueFunctionCall")
	{
		UniqueFunction<void()> empty;
		empty();
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("UniqueFunction: holds move-only callables")
		{
			Scope<int> input = CreateScope<int>(41);
			UniqueFunction<int()> function = [owned = std::move(input)]()
			{
				return *owned + 1;
			};
			CHECK(static_cast<bool>(function));

			UniqueFunction<int()> moved = std::move(function);
			CHECK(moved() == 42);
		}

		TEST_CASE("UniqueFunction: forwards arguments and returns values")
		{
			UniqueFunction<std::string(std::string, int)> repeat = [](std::string text, int count)
			{
				std::string result;
				for (int index = 0; index < count; ++index)
					result += text;
				return result;
			};
			CHECK(repeat("ab", 3) == "ababab");

			UniqueFunction<void(int&)> increment = [](int& target)
			{
				++target;
			};
			int counter = 1;
			increment(counter);
			CHECK(counter == 2);
		}

		TEST_CASE("UniqueFunction: a void signature discards the callable's result")
		{
			int calls = 0;
			UniqueFunction<void()> discard = [&calls]()
			{
				return ++calls;
			};
			discard();
			CHECK(calls == 1);
		}

		TEST_CASE("UniqueFunction: an empty function converts to false")
		{
			const UniqueFunction<void()> empty;
			CHECK_FALSE(static_cast<bool>(empty));
		}

		TEST_CASE("UniqueFunction: calling an empty function is a programmer error" * doctest::skip(true))
		{
			Test::CheckDeath("Core/EmptyUniqueFunctionCall", "Calling an empty UniqueFunction");
		}
	}

}
