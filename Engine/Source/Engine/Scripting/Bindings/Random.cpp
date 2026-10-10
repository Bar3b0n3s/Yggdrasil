#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Core/DetMath.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

#include <lua.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Engine {

	namespace Utils {

		static int64_t RandomIntegerArgument(ScriptCall& call, int index, bool seed = false)
		{
			const double value = Lua::Check<double>(call, index);
			constexpr double MaxExactInteger = 9007199254740991.0;
			if (std::floor(value) != value || value < (seed ? 0.0 : -MaxExactInteger) || value > MaxExactInteger)
				Lua::RaiseError(call, seed ? "seed must be an integer in [0, 9007199254740991]" : "bounds must be integers in [-9007199254740991, 9007199254740991]");
			return static_cast<int64_t>(value);
		}

		template<bool Local>
		static Random& BindingRandom(ScriptCall& call)
		{
			if constexpr (Local)
				return Lua::CheckRandomStorage(call, 1);
			else
			{
				const auto permission = call.PrepareHostMutation();
				if (!permission)
					Lua::RaiseError(call, permission.error());
				return call.Engine->GetHost().GetRandom();
			}
		}

		static int RandomNew(ScriptCall& call)
		{
			const auto seed = static_cast<uint64_t>(RandomIntegerArgument(call, 1, true));
			Lua::PushRandomGenerator(call, Random(seed));
			return 1;
		}

		template<bool Local>
		static int RandomSeed(ScriptCall& call)
		{
			const auto seed = static_cast<uint64_t>(RandomIntegerArgument(call, Local ? 2 : 1, true));
			BindingRandom<Local>(call).Seed(seed);
			return 0;
		}

		template<bool Local>
		static int RandomInteger(ScriptCall& call)
		{
			constexpr int First = Local ? 2 : 1;
			const auto min = RandomIntegerArgument(call, First);
			const auto max = RandomIntegerArgument(call, First + 1);
			if (min > max)
				return Lua::RaiseError(call, "minimum must not exceed maximum");
			Lua::Push(call, static_cast<double>(BindingRandom<Local>(call).RangeInt(min, max)));
			return 1;
		}

		template<bool Local>
		static int RandomNumber(ScriptCall& call)
		{
			constexpr int First = Local ? 2 : 1;
			double min = 0.0;
			double max = 1.0;
			if (!Lua::IsNoneOrNil(call, First))
			{
				if (Lua::IsNoneOrNil(call, First + 1))
					max = Lua::Check<double>(call, First);
				else
				{
					min = Lua::Check<double>(call, First);
					max = Lua::Check<double>(call, First + 1);
				}
			}
			else if (!Lua::IsNoneOrNil(call, First + 1))
				max = Lua::Check<double>(call, First + 1);
			if (min > max)
				return Lua::RaiseError(call, "minimum must not exceed maximum");
			Lua::Push(call, BindingRandom<Local>(call).RangeDouble(min, max));
			return 1;
		}

		template<bool Local>
		static int RandomBool(ScriptCall& call)
		{
			constexpr int First = Local ? 2 : 1;
			const double probability = Lua::IsNoneOrNil(call, First) ? 0.5 : Lua::Check<double>(call, First);
			if (probability < 0.0 || probability > 1.0)
				return Lua::RaiseError(call, "probability must be between zero and one");
			Lua::Push(call, BindingRandom<Local>(call).NextBool(probability));
			return 1;
		}

		static int RandomArraySize(ScriptCall& call, int index, bool writable)
		{
			if (lua_type(call.State, index) != LUA_TTABLE)
				Lua::RaiseError(call, "expected a dense array table");
			if (writable && lua_getreadonly(call.State, index) != 0)
				Lua::RaiseError(call, "cannot shuffle a frozen array");
			const int length = lua_objlen(call.State, index);
			int count = 0;
			lua_pushnil(call.State);
			while (lua_next(call.State, index) != 0)
			{
				if (lua_type(call.State, -2) != LUA_TNUMBER)
					Lua::RaiseError(call, "array must contain only consecutive integer keys starting at one");
				const double key = lua_tonumber(call.State, -2);
				if (key < 1.0 || key > static_cast<double>(length) || std::floor(key) != key)
					Lua::RaiseError(call, "array must contain only consecutive integer keys starting at one");
				++count;
				lua_pop(call.State, 1);
			}
			if (count != length)
				Lua::RaiseError(call, "array must not contain holes");
			return length;
		}

		template<bool Local>
		static int RandomChoice(ScriptCall& call)
		{
			constexpr int First = Local ? 2 : 1;
			const int length = RandomArraySize(call, First, false);
			if (length == 0)
				return Lua::RaiseError(call, "cannot choose from an empty array");
			const int index = static_cast<int>(BindingRandom<Local>(call).RangeInt(1, length));
			lua_rawgeti(call.State, First, index);
			return 1;
		}

		template<bool Local>
		static int RandomShuffle(ScriptCall& call)
		{
			constexpr int First = Local ? 2 : 1;
			const int length = RandomArraySize(call, First, true);
			if constexpr (Local)
				static_cast<void>(Lua::CheckRandomStorage(call, 1));
			if (length > 1)
			{
				auto& random = BindingRandom<Local>(call);
				for (int i = length; i > 1; --i)
				{
					const int other = static_cast<int>(random.RangeInt(1, i));
					lua_rawgeti(call.State, First, i);
					lua_rawgeti(call.State, First, other);
					lua_rawseti(call.State, First, i);
					lua_rawseti(call.State, First, other);
				}
			}
			lua_pushvalue(call.State, First);
			return 1;
		}

		template<bool Local>
		static int RandomUnitVector(ScriptCall& call)
		{
			auto& random = BindingRandom<Local>(call);
			const double z = random.RangeDouble(-1.0, 1.0);
			const auto angle = DetMath::SinCos(random.RangeDouble(0.0, 2.0 * std::numbers::pi));
			const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
			Lua::Push(call, glm::vec3(static_cast<float>(radius * angle.Cos), static_cast<float>(radius * angle.Sin), static_cast<float>(z)));
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterRandom(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions local{ .Mutates = false };
			api.Module("Random", "Deterministic shared simulation randomness. Invalid arguments never advance the stream.")
				.Function("Seed", &Utils::RandomSeed<false>, "(seed: number) -> ()", "Reseeds with a nonnegative exact integer no greater than 2^53-1.")
				.Function("Integer", &Utils::RandomInteger<false>, "(min: number, max: number) -> number", "Uniform integer in inclusive bounds, each within the exact-number integer range.")
				.Function("Number", &Utils::RandomNumber<false>, "(min: number?, max: number?) -> number", "Uniform in [min,max); defaults to [0,1), one argument is the maximum. Equal bounds still consume one draw.")
				.Function("Bool", &Utils::RandomBool<false>, "(probability: number?) -> boolean", "True with probability in [0,1], default one half.")
				.Function("Choice", &Utils::RandomChoice<false>, "<T>(array: {T}) -> T", "Chooses from a nonempty dense one-based array; frozen arrays may be read.")
				.Function("Shuffle", &Utils::RandomShuffle<false>, "<T>(array: {T}) -> {T}", "Fisher-Yates shuffle in place, returning the same array. Rejects holes, dictionary keys and frozen tables.")
				.Function("UnitVector", &Utils::RandomUnitVector<false>, "() -> vector", "Uniform unit direction using two draws and deterministic trigonometry.")
				.Function("New", &Utils::RandomNew, "(seed: number) -> RandomGenerator", "Creates an independent local stream; allowed in read-only evaluation.", local);
			api.Type("RandomGenerator", "An independent value-owned seeded stream; its methods never change session randomness.")
				.Method("Seed", &Utils::RandomSeed<true>, "(self: RandomGenerator, seed: number) -> ()", "Reseeds this local generator with a nonnegative exact integer.", local)
				.Method("Integer", &Utils::RandomInteger<true>, "(self: RandomGenerator, min: number, max: number) -> number", "Draws an inclusive bounded integer from this stream.", local)
				.Method("Number", &Utils::RandomNumber<true>, "(self: RandomGenerator, min: number?, max: number?) -> number", "Draws in [min,max); no arguments means [0,1), one means [0,max).", local)
				.Method("Bool", &Utils::RandomBool<true>, "(self: RandomGenerator, probability: number?) -> boolean", "Draws a boolean with the given probability, default one half.", local)
				.Method("Choice", &Utils::RandomChoice<true>, "<T>(self: RandomGenerator, array: {T}) -> T", "Chooses from a nonempty dense array using only this stream.", local)
				.Method("Shuffle", &Utils::RandomShuffle<true>, "<T>(self: RandomGenerator, array: {T}) -> {T}", "Shuffles a mutable dense array in place using only this stream.", local)
				.Method("UnitVector", &Utils::RandomUnitVector<true>, "(self: RandomGenerator) -> vector", "Draws a uniform direction from this stream with deterministic trigonometry.", local);
			return {};
		}

	}

}
