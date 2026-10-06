#pragma once

#include "Engine/Core/Base.h"

#include <cstddef>
#include <type_traits>

namespace Engine {

	// A compile-time list of types. `using BuiltinComponents = TypeList<...>` is the one list of component types
	// (Architecture §5.4, Scene/Components/BuiltinComponents.h).
	template<typename... Types>
	struct TypeList
	{
		static constexpr size_t Size = sizeof...(Types);
	};

	// Calls `function.template operator()<T>()` once for each T of the list, in list order:
	//     ForEachType(BuiltinComponents{}, [&]<typename T>() { CHECK(registry.FindComponent<T>() != nullptr); });
	template<typename... Types, typename Function>
	constexpr void ForEachType(TypeList<Types...> /*list*/, Function&& function)
	{
		(function.template operator()<Types>(), ...);
	}

	// True when T is one of the types of List.
	template<typename T, typename List>
	inline constexpr bool TypeListContains = false;

	template<typename T, typename... Types>
	inline constexpr bool TypeListContains<T, TypeList<Types...>> = (std::is_same_v<T, Types> || ...);

}
