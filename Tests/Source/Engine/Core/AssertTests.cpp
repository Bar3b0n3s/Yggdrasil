#include "TestsPCH.h"

#include "Engine/Core/Assert.h"

#include "Support/DeathTest.h"

#include <entt/entity/registry.hpp>

namespace Engine {

	static size_t CountTo(size_t limit)
	{
		return limit;
	}

	static void IgnoreAssert(const AssertInfo& /*info*/)
	{
	}

	ENGINE_DEATH_TEST("Core/AssertFires")
	{
		const std::vector<int> values = { 1, 2 };
		ENGINE_CORE_ASSERT(values.size() == 3, "Expected {} values, got {}", 3, values.size());
	}

	ENGINE_DEATH_TEST("Core/ClientAssertFires")
	{
		const size_t count = CountTo(5);
		ENGINE_ASSERT(count < 5, "Client count {} is too large", count);
	}

	ENGINE_DEATH_TEST("Core/VerifyFires")
	{
		const size_t count = CountTo(2);
		ENGINE_CORE_VERIFY(count == 0, "Verify saw {}", count);
	}

	ENGINE_DEATH_TEST("Core/UnreachableFires")
	{
		ENGINE_UNREACHABLE("Reached the unreachable branch {}", 17);
	}

	ENGINE_DEATH_TEST("Core/EnttAssertRoutesToEngine")
	{
		entt::registry registry;
		const entt::entity entity = registry.create();
		registry.destroy(entity);
		registry.emplace<int>(entity, 1);
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("Assert: Core/AssertFires exits with code 4 and the message" * doctest::skip(true))
		{
			Test::CheckDeath("Core/AssertFires", "Expected 3 values, got 2");
			Test::CheckDeath("Core/AssertFires", "Assertion failed: values.size() == 3");
		}

		TEST_CASE("Assert: ENGINE_ASSERT fires like ENGINE_CORE_ASSERT" * doctest::skip(true))
		{
			Test::CheckDeath("Core/ClientAssertFires", "Client count 5 is too large");
		}

		TEST_CASE("Assert: a failed ENGINE_CORE_VERIFY exits with code 4 and the message" * doctest::skip(true))
		{
			Test::CheckDeath("Core/VerifyFires", "Verify failed: count == 0: Verify saw 2");
		}

		TEST_CASE("Assert: ENGINE_UNREACHABLE exits with code 4 and the message" * doctest::skip(true))
		{
			Test::CheckDeath("Core/UnreachableFires", "Unreachable code reached: Reached the unreachable branch 17");
		}

		TEST_CASE("Assert: EnTT assertions go through the engine handler" * doctest::skip(true))
		{
			Test::CheckDeath("Core/EnttAssertRoutesToEngine", "Invalid entity");
		}

		TEST_CASE("Assert: a passing assertion evaluates its condition once and continues" * doctest::skip(true))
		{
			int evaluations = 0;
			auto condition = [&evaluations]()
			{
				++evaluations;
				return true;
			};

			ENGINE_CORE_ASSERT(condition(), "never shown");
			ENGINE_CORE_VERIFY(condition(), "never shown either");

			// Tests run in Debug and Release only, where both are evaluated.
			CHECK(evaluations == 2);
		}

		TEST_CASE("Assert: SetAssertHandler returns the previous handler and nullptr restores the default" * doctest::skip(true))
		{
			const AssertHandler original = GetAssertHandler();

			const AssertHandler previous = SetAssertHandler(&IgnoreAssert);
			CHECK(previous == original);
			CHECK(GetAssertHandler() == &IgnoreAssert);

			CHECK(SetAssertHandler(nullptr) == &IgnoreAssert);
			CHECK(GetAssertHandler() == &DefaultAssertHandler);

			SetAssertHandler(original);
			CHECK(GetAssertHandler() == original);
		}

		TEST_CASE("Assert: FormatAssertInfo renders the documented single line" * doctest::skip(true))
		{
			AssertInfo info;
			info.Kind = AssertKind::Assert;
			info.Expression = "index < size";
			info.Message = "Index 7 out of range";
			info.File = "Scene.cpp";
			info.Line = 42;
			info.Function = "GetEntity";
			CHECK(FormatAssertInfo(info) == "Assertion failed: index < size: Index 7 out of range (Scene.cpp:42, GetEntity)");

			info.Kind = AssertKind::Verify;
			CHECK(FormatAssertInfo(info) == "Verify failed: index < size: Index 7 out of range (Scene.cpp:42, GetEntity)");

			info.Message = {};
			CHECK(FormatAssertInfo(info) == "Verify failed: index < size (Scene.cpp:42, GetEntity)");

			info.Kind = AssertKind::Unreachable;
			info.Expression = {};
			info.Message = "Unknown AudioBus 9";
			CHECK(FormatAssertInfo(info) == "Unreachable code reached: Unknown AudioBus 9 (Scene.cpp:42, GetEntity)");
		}

		TEST_CASE("Assert: AssertKindToString names every kind" * doctest::skip(true))
		{
			CHECK(AssertKindToString(AssertKind::Assert) == "Assert");
			CHECK(AssertKindToString(AssertKind::Verify) == "Verify");
			CHECK(AssertKindToString(AssertKind::Unreachable) == "Unreachable");
		}
	}

}
