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

		TEST_CASE("HandlePool: Create reuses the most recently freed slot first")
		{
			HandlePool<Voice> pool;
			std::vector<Handle<Voice>> handles;
			for (const char* clip : { "a", "b", "c", "d" })
			{
				const Result<Handle<Voice>> handle = pool.Create(clip);
				REQUIRE(handle.has_value());
				handles.push_back(*handle);
			}
			REQUIRE(pool.Destroy(handles[1]).has_value());
			REQUIRE(pool.Destroy(handles[3]).has_value());

			const Result<Handle<Voice>> first = pool.Create("e");
			const Result<Handle<Voice>> second = pool.Create("f");
			const Result<Handle<Voice>> third = pool.Create("g");
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			REQUIRE(third.has_value());
			CHECK(first->GetIndex() == 3);
			CHECK(first->GetGeneration() == 2);
			CHECK(second->GetIndex() == 1);
			CHECK(second->GetGeneration() == 2);
			CHECK(third->GetIndex() == 4);
			CHECK(third->GetGeneration() == 1);
			CHECK(pool.GetSize() == 5);
		}

		TEST_CASE("HandlePool: handles that do not match a live object are never alive")
		{
			HandlePool<Voice> pool;
			const Result<Handle<Voice>> live = pool.Create("Jump.sfx");
			REQUIRE(live.has_value());

			// The right index with another generation, an index past every slot, and the null handle.
			const uint64_t generation = live->GetGeneration();
			const Handle<Voice> wrongGeneration = Handle<Voice>::FromValue(((generation + 1) << 32) | live->GetIndex());
			const Handle<Voice> pastTheEnd = Handle<Voice>::FromValue((generation << 32) | 7u);
			for (const Handle<Voice> handle : { wrongGeneration, pastTheEnd, Handle<Voice>() })
			{
				CHECK_FALSE(pool.IsAlive(handle));
				CHECK(pool.TryGet(handle) == nullptr);
				const Status destroyed = pool.Destroy(handle);
				REQUIRE_FALSE(destroyed.has_value());
				CHECK(destroyed.error().GetCode() == ErrorCode::NotFound);
			}

			// A handle rebuilt from its value addresses the same object.
			const Handle<Voice> rebuilt = Handle<Voice>::FromValue(live->GetValue());
			CHECK(rebuilt == *live);
			REQUIRE(pool.TryGet(rebuilt) != nullptr);
			CHECK(pool.TryGet(rebuilt)->Clip == "Jump.sfx");
			CHECK(pool.GetSize() == 1);
		}

		TEST_CASE("HandlePool: const access sees the same objects")
		{
			HandlePool<Voice> pool;
			const Result<Handle<Voice>> handle = pool.Create("Jump.sfx");
			REQUIRE(handle.has_value());
			pool.Get(*handle).Clip = "Land.sfx";

			const HandlePool<Voice>& view = pool;
			CHECK(view.Get(*handle).Clip == "Land.sfx");
			REQUIRE(view.TryGet(*handle) != nullptr);
			CHECK(view.TryGet(*handle)->Clip == "Land.sfx");
			CHECK(view.IsAlive(*handle));

			REQUIRE(pool.Destroy(*handle).has_value());
			CHECK(view.TryGet(*handle) == nullptr);
		}

		TEST_CASE("HandlePool: Destroy runs the object's destructor")
		{
			Ref<int> witness = CreateRef<int>(0);
			HandlePool<Ref<int>> pool;
			const Result<Handle<Ref<int>>> handle = pool.Create(witness);
			REQUIRE(handle.has_value());
			CHECK(witness.use_count() == 2);

			REQUIRE(pool.Destroy(*handle).has_value());
			CHECK(witness.use_count() == 1);

			REQUIRE(pool.Create(witness).has_value());
			pool.Clear();
			CHECK(witness.use_count() == 1);
		}

		TEST_CASE("HandlePool: a stale Get fails a verify in every configuration")
		{
			ENGINE_CHECK_DEATH("Core/StaleHandleGet", "Stale or null handle");
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
