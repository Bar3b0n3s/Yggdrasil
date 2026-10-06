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

		TEST_CASE("UniqueFunction: moving transfers the callable and empties the source")
		{
			Ref<int> witness = CreateRef<int>(3);
			UniqueFunction<int()> source = [witness]()
			{
				return *witness;
			};
			CHECK(witness.use_count() == 2);

			UniqueFunction<int()> target;
			target = std::move(source);
			CHECK_FALSE(static_cast<bool>(source)); // a moved-from UniqueFunction is empty
			CHECK(static_cast<bool>(target));
			CHECK(target() == 3);
			CHECK(witness.use_count() == 2); // moved, not copied
		}

		TEST_CASE("UniqueFunction: destruction and reassignment release the callable")
		{
			Ref<int> witness = CreateRef<int>(0);
			{
				const UniqueFunction<void()> holder = [witness]()
				{
					static_cast<void>(*witness);
				};
				CHECK(witness.use_count() == 2);
			}
			CHECK(witness.use_count() == 1);

			UniqueFunction<void()> reassigned = [witness]()
			{
				static_cast<void>(*witness);
			};
			CHECK(witness.use_count() == 2);
			reassigned = []() {};
			CHECK(witness.use_count() == 1);
		}

		TEST_CASE("UniqueFunction: a mutable callable keeps its state between calls")
		{
			UniqueFunction<int()> counter = [count = 0]() mutable
			{
				return ++count;
			};
			CHECK(counter() == 1);
			CHECK(counter() == 2);
			CHECK(counter() == 3);
		}

		TEST_CASE("UniqueFunction: calling an empty function is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/EmptyUniqueFunctionCall", "Calling an empty UniqueFunction");
		}
	}

}
