#include "TestsPCH.h"

#include "Engine/Core/Handle.h"

#include "Support/DeathTest.h"

namespace Engine {

	namespace {

		struct Voice
		{
			explicit Voice(std::string clip)
				: Clip(std::move(clip))
			{
			}

			std::string Clip;
		};

	}

	ENGINE_DEATH_TEST("Core/StaleHandleGet")
	{
		HandlePool<Voice> pool;
		const Result<Handle<Voice>> handle = pool.Create("Jump.sfx");
		if (handle.has_value() && pool.Destroy(*handle).has_value())
			static_cast<void>(pool.Get(*handle).Clip);
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("HandlePool: a recreated slot gets a new generation and the old handle is stale")
		{
			HandlePool<Voice> pool;
			const Result<Handle<Voice>> first = pool.Create("Jump.sfx");
			REQUIRE(first.has_value());
			CHECK(first->GetIndex() == 0);
			CHECK(first->GetGeneration() == 1);
			CHECK(pool.Get(*first).Clip == "Jump.sfx");

			REQUIRE(pool.Destroy(*first).has_value());
			CHECK_FALSE(pool.IsAlive(*first));
			CHECK(pool.TryGet(*first) == nullptr);

			const Result<Handle<Voice>> second = pool.Create("Land.sfx");
			REQUIRE(second.has_value());
			CHECK(second->GetIndex() == 0);
			CHECK(second->GetGeneration() == 2);
			CHECK(*second != *first);
			CHECK(pool.TryGet(*first) == nullptr);
			REQUIRE(pool.TryGet(*second) != nullptr);
			CHECK(pool.TryGet(*second)->Clip == "Land.sfx");
		}

		TEST_CASE("HandlePool: Destroy of a stale or null handle is NotFound")
		{
			HandlePool<Voice> pool;
			const Result<Handle<Voice>> handle = pool.Create("Jump.sfx");
			REQUIRE(handle.has_value());
			REQUIRE(pool.Destroy(*handle).has_value());

			const Status again = pool.Destroy(*handle);
			REQUIRE_FALSE(again.has_value());
			CHECK(again.error().GetCode() == ErrorCode::NotFound);
			CHECK_FALSE(pool.Destroy(Handle<Voice>()).has_value());
			CHECK_FALSE(pool.IsAlive(Handle<Voice>::FromValue(0x0000000500000009ull)));
		}

		TEST_CASE("HandlePool: capacity limits the live objects")
		{
			HandlePool<Voice> pool(2);
			const Result<Handle<Voice>> first = pool.Create("a");
			REQUIRE(pool.Create("b").has_value());
			const Result<Handle<Voice>> full = pool.Create("c");
			REQUIRE_FALSE(full.has_value());
			CHECK(full.error().GetCode() == ErrorCode::InvalidState);
			CHECK(pool.GetSize() == 2);
			CHECK(pool.GetCapacity() == 2);

			REQUIRE(pool.Destroy(*first).has_value());
			CHECK(pool.Create("c").has_value());
		}

		TEST_CASE("HandlePool: ForEach visits live objects in slot order")
		{
			HandlePool<Voice> pool;
			const Result<Handle<Voice>> a = pool.Create("a");
			const Result<Handle<Voice>> b = pool.Create("b");
			REQUIRE(pool.Create("c").has_value());
			REQUIRE(pool.Destroy(*b).has_value());
			REQUIRE(pool.Create("d").has_value()); // reuses slot 1

			std::vector<std::string> visited;
			pool.ForEach([&visited](Handle<Voice> handle, Voice& voice)
			{
				visited.push_back(std::format("{}:{}", handle.GetIndex(), voice.Clip));
			});
			CHECK(visited == std::vector<std::string>{ "0:a", "1:d", "2:c" });
			CHECK(a.has_value());
		}

		TEST_CASE("HandlePool: Clear makes every handle stale")
		{
			HandlePool<Voice> pool;
			const Result<Handle<Voice>> a = pool.Create("a");
			const Result<Handle<Voice>> b = pool.Create("b");
			pool.Clear();
			CHECK(pool.GetSize() == 0);
			CHECK_FALSE(pool.IsAlive(*a));
			CHECK_FALSE(pool.IsAlive(*b));

			const Result<Handle<Voice>> reused = pool.Create("c");
			REQUIRE(reused.has_value());
			CHECK(reused->GetGeneration() == 2);
		}

		TEST_CASE("HandlePool: a stale Get fails a verify in every configuration" * doctest::skip(true))
		{
			Test::CheckDeath("Core/StaleHandleGet", "Stale or null handle");
		}

		TEST_CASE("Handle: GetValue and FromValue round-trip and the default handle is null")
		{
			constexpr Handle<Voice> NullHandle;
			static_assert(NullHandle.IsNull());
			static_assert(NullHandle.GetValue() == 0);

			constexpr Handle<Voice> Packed = Handle<Voice>::FromValue(0x0000000300000007ull);
			static_assert(Packed.GetIndex() == 7);
			static_assert(Packed.GetGeneration() == 3);
			static_assert(Packed.GetValue() == 0x0000000300000007ull);
			CHECK_FALSE(Packed.IsNull());
		}
	}

}
