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
		// Docs/Decisions/0036-m25-tetris-pixel-art.md, "The soak": the soak's memory rule measures these bytes, so a block of
		// several MiB, which the heap gives memory of its own (VirtualAlloc), must count while it is held. Other threads of
		// the run may allocate meanwhile, so the release is checked within a margin.
		constexpr size_t BlockBytes = 8 * 1024 * 1024;
		constexpr uint64_t ReleaseMargin = 1024 * 1024;

	#if defined(__SANITIZE_ADDRESS__)
		TEST_CASE("ProcessHeap: the AddressSanitizer build allocates outside the process heap")
		{
			// The hardening stage's AddressSanitizer build (Docs/Decisions/0024-m15-contract.md decision 12): its runtime
			// serves malloc, operator new and HeapAlloc itself from its own allocator, whatever heap a HeapAlloc names, so the
			// measure leaves those allocations out there; the soak never runs on that build.
			const Result<uint64_t> before = ProcessHeap::GetCommittedBytes();
			REQUIRE_MESSAGE(before.has_value(), before.error().ToString());

			auto runtimeBlock = std::make_unique<std::byte[]>(BlockBytes);
			Result<Test::ProcessHeapBlock> heapBlock = Test::AllocateOnProcessHeap(BlockBytes);
			REQUIRE_MESSAGE(heapBlock.has_value(), heapBlock.error().ToString());
			const Result<uint64_t> held = ProcessHeap::GetCommittedBytes();
			REQUIRE(held.has_value());
			CHECK(*held < *before + BlockBytes);
		}
	#else
		TEST_CASE("ProcessHeap: the C runtime allocates from the process heap")
		{
			// The heap whose committed bytes ProcessHeap reports is the one malloc and operator new use (_get_heap_handle).
			CHECK(Test::IsCrtHeapTheProcessHeap());
		}

		TEST_CASE("ProcessHeap: a large block of the process heap raises the committed bytes and its release lowers them")
		{
			// The block comes from the process heap itself (HeapAlloc), whatever the C runtime allocates from.
			const Result<uint64_t> before = ProcessHeap::GetCommittedBytes();
			REQUIRE_MESSAGE(before.has_value(), before.error().ToString());
			CHECK(*before > 0);

			Result<Test::ProcessHeapBlock> block = Test::AllocateOnProcessHeap(BlockBytes);
			REQUIRE_MESSAGE(block.has_value(), block.error().ToString());
			const Result<uint64_t> held = ProcessHeap::GetCommittedBytes();
			REQUIRE(held.has_value());
			CHECK(*held >= *before + BlockBytes);

			block->reset();
			const Result<uint64_t> released = ProcessHeap::GetCommittedBytes();
			REQUIRE(released.has_value());
			CHECK(*released + BlockBytes <= *held + ReleaseMargin);
		}

		TEST_CASE("ProcessHeap: a large allocation raises the committed bytes and its release lowers them")
		{
			// The same through the C runtime (operator new), which allocates from the process heap.
			const Result<uint64_t> before = ProcessHeap::GetCommittedBytes();
			REQUIRE_MESSAGE(before.has_value(), before.error().ToString());

			auto block = std::make_unique<std::byte[]>(BlockBytes);
			const Result<uint64_t> held = ProcessHeap::GetCommittedBytes();
			REQUIRE(held.has_value());
			CHECK(*held >= *before + BlockBytes);

			block.reset();
			const Result<uint64_t> released = ProcessHeap::GetCommittedBytes();
			REQUIRE(released.has_value());
			CHECK(*released + BlockBytes <= *held + ReleaseMargin);
		}
	#endif
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
