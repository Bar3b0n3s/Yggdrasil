#include "TestsPCH.h"

#include "Engine/Platform/ProcessHeap.h"

#include "Support/PlatformProbes.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace Engine {

	TEST_SUITE("Platform")
	{
#if defined(ENGINE_PLATFORM_WINDOWS)
		TEST_CASE("ProcessHeap: the C runtime allocates from the process heap")
		{
			// The heap whose committed bytes ProcessHeap reports is the one malloc and operator new use (_get_heap_handle).
			CHECK(Test::IsCrtHeapTheProcessHeap());
		}

		TEST_CASE("ProcessHeap: a large allocation raises the committed bytes and its release lowers them")
		{
			// Docs/Decisions/0036-m25-tetris-pixel-art.md, "The soak": the soak's memory rule measures these bytes, so a
			// block of several MiB, which the heap gives memory of its own (VirtualAlloc), must count while it is held.
			// Other threads of the run may allocate meanwhile, so the release is checked within a margin.
			constexpr size_t BlockBytes = 8 * 1024 * 1024;
			constexpr uint64_t Margin = 1024 * 1024;
			const Result<uint64_t> before = ProcessHeap::GetCommittedBytes();
			REQUIRE_MESSAGE(before.has_value(), before.error().ToString());
			CHECK(*before > 0);

			auto block = std::make_unique<std::byte[]>(BlockBytes);
			const Result<uint64_t> held = ProcessHeap::GetCommittedBytes();
			REQUIRE(held.has_value());
			CHECK(*held >= *before + BlockBytes);

			block.reset();
			const Result<uint64_t> released = ProcessHeap::GetCommittedBytes();
			REQUIRE(released.has_value());
			CHECK(*released + BlockBytes <= *held + Margin);
		}
#else
		TEST_CASE("ProcessHeap: Linux and macOS have no measure")
		{
			const Result<uint64_t> committed = ProcessHeap::GetCommittedBytes();
			REQUIRE_FALSE(committed.has_value());
			CHECK(committed.error().GetCode() == ErrorCode::Unsupported);
		}
#endif
	}

}
