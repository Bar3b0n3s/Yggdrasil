#include "TestsPCH.h"
#include "Engine/Scripting/TrackingAllocator.h"

#include "Engine/Core/Random.h"
#include "Engine/Scripting/Sandbox.h"

#include <doctest/doctest.h>

#include <format>
#include <limits>

namespace Engine {

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Allocator: soft limit raises a script error and the VM stays usable" * doctest::skip())
		{
			Random random(17);
			SandboxSpecification specification{};
			specification.MemoryLimitMB = 1;
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/Scripts/Breach.luau");
			REQUIRE(path.has_value());
			const Status breach = (*sandbox)->RunSource(*path,
				"local retained = string.rep('x', 2 * 1024 * 1024)\nwhile true do retained = retained end");
			REQUIRE_FALSE(breach.has_value());
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Kind == ScriptErrorKind::Memory);
			CHECK(error->Script == "Assets/Scripts/Breach.luau");
			CHECK_FALSE((*sandbox)->IsStopped());
			auto healthy = VfsPath::Parse("project://Assets/Scripts/Healthy.luau");
			REQUIRE(healthy.has_value());
			CHECK((*sandbox)->RunSource(*healthy, "assert(1 + 1 == 2); return {}").has_value());
			CHECK((*sandbox)->GetMemoryState().SoftBreachCount == 1);
			// ScriptEngine's instance tests additionally assert the faulting instance receives no later callbacks.
		}

		TEST_CASE("Allocator: second breach stops the session with a structured error" * doctest::skip())
		{
			Random random(17);
			SandboxSpecification specification{};
			specification.MemoryLimitMB = 1;
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			for (const std::string_view name : { "First", "Second" })
			{
				auto path = VfsPath::Parse(std::format("project://Assets/Scripts/{}.luau", name));
				REQUIRE(path.has_value());
				CHECK_FALSE((*sandbox)->RunSource(*path,
										  "local retained = string.rep('x', 2 * 1024 * 1024)\nwhile true do retained = retained end")
						.has_value());
			}
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Kind == ScriptErrorKind::Memory);
			CHECK((*sandbox)->IsStopped());
			CHECK((*sandbox)->GetMemoryState().SoftBreachCount == 2);
			auto path = VfsPath::Parse("project://Assets/Scripts/AfterStop.luau");
			REQUIRE(path.has_value());
			const Status stopped = (*sandbox)->RunSource(*path, "return {}");
			REQUIRE_FALSE(stopped.has_value());
			CHECK(stopped.error().GetCode() == ErrorCode::InvalidState);
			// The host integration asserts OnScriptError(error, true) requests stop after protected unwinding.
		}

		TEST_CASE("TrackingAllocator: accounting ignores new-allocation tags and counts a pending breach once" * doctest::skip())
		{
			auto allocator = TrackingAllocator::Create(1);
			REQUIRE(allocator.has_value());
			constexpr size_t Soft = 1024 * 1024;
			CHECK((*allocator)->GetState().HardLimitBytes == Soft + TrackingAllocator::HeadroomBytes);
			void* block = (*allocator)->Reallocate(nullptr, 123, Soft + 1);
			REQUIRE(block != nullptr);
			CHECK((*allocator)->GetState().UsedBytes == Soft + 1);
			CHECK((*allocator)->CheckInterrupt(0) == ScriptMemoryAction::Continue);
			CHECK((*allocator)->CheckInterrupt(3) == ScriptMemoryAction::Continue);
			CHECK((*allocator)->CheckInterrupt(-1) == ScriptMemoryAction::RecoverInstance);
			CHECK((*allocator)->CheckInterrupt(-1) == ScriptMemoryAction::RecoverInstance);
			CHECK((*allocator)->GetState().SoftBreachCount == 1);
			CHECK((*allocator)->Reallocate(block, Soft + 1, 0) == nullptr);
			REQUIRE((*allocator)->FinishRecovery().has_value());
			CHECK((*allocator)->GetState().UsedBytes == 0);
			CHECK((*allocator)->GetState().PeakBytes == Soft + 1);
			block = (*allocator)->Reallocate(nullptr, 0, Soft + 1);
			REQUIRE(block != nullptr);
			CHECK((*allocator)->CheckInterrupt(-1) == ScriptMemoryAction::StopSession);
			CHECK((*allocator)->Reallocate(block, Soft + 1, 0) == nullptr);
		}

		TEST_CASE("TrackingAllocator: reaching hard limit refuses growth and keeps the old block accounted" * doctest::skip())
		{
			auto allocator = TrackingAllocator::Create(1);
			REQUIRE(allocator.has_value());
			void* block = (*allocator)->Reallocate(nullptr, 0, 32);
			REQUIRE(block != nullptr);
			const size_t hard = static_cast<size_t>((*allocator)->GetState().HardLimitBytes);
			CHECK((*allocator)->Reallocate(block, 32, hard) == nullptr);
			CHECK((*allocator)->GetState().UsedBytes == 32);
			CHECK((*allocator)->GetState().MustStop);
			CHECK((*allocator)->CheckInterrupt(0) == ScriptMemoryAction::Continue);
			CHECK((*allocator)->CheckInterrupt(-1) == ScriptMemoryAction::StopSession);
			CHECK((*allocator)->Reallocate(block, 32, 0) == nullptr);
		}

		TEST_CASE("TrackingAllocator: failed recovery and size overflow latch terminal memory failure" * doctest::skip())
		{
			auto allocator = TrackingAllocator::Create(1);
			REQUIRE(allocator.has_value());
			constexpr size_t BreachSize = 1024 * 1024 + 1;
			void* block = (*allocator)->Reallocate(nullptr, 0, BreachSize);
			REQUIRE(block != nullptr);
			CHECK_FALSE((*allocator)->FinishRecovery().has_value());
			CHECK((*allocator)->GetState().MustStop);
			CHECK((*allocator)->Reallocate(block, BreachSize, 0) == nullptr);
			auto overflow = TrackingAllocator::Create(1);
			REQUIRE(overflow.has_value());
			CHECK((*overflow)->Reallocate(nullptr, 0, std::numeric_limits<size_t>::max()) == nullptr);
			CHECK((*overflow)->GetState().UsedBytes == 0);
			CHECK((*overflow)->GetState().MustStop);
			const auto invalid = TrackingAllocator::Create(0);
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
