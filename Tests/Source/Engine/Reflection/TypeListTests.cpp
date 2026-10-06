#include "TestsPCH.h"

#include "Engine/Reflection/TypeList.h"

namespace Engine {

	namespace {

		struct First
		{
		};

		struct Second
		{
		};

		struct Third
		{
		};

	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("TypeList: ForEachType visits every type once in list order")
		{
			using List = TypeList<First, Second, Third>;
			static_assert(List::Size == 3);

			std::vector<int> visited;
			ForEachType(List{}, [&]<typename T>()
			{
				if constexpr (std::is_same_v<T, First>)
					visited.push_back(1);
				else if constexpr (std::is_same_v<T, Second>)
					visited.push_back(2);
				else
					visited.push_back(3);
			});
			CHECK(visited == std::vector<int>{ 1, 2, 3 });
		}

		TEST_CASE("TypeList: TypeListContains finds exactly the listed types")
		{
			using List = TypeList<First, Second>;
			static_assert(TypeListContains<First, List>);
			static_assert(TypeListContains<Second, List>);
			static_assert(!TypeListContains<Third, List>);
			static_assert(!TypeListContains<First, TypeList<>>);
			static_assert(TypeList<>::Size == 0);
			CHECK(TypeListContains<Second, List>);
		}
	}

}
