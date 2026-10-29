#include "TestsPCH.h"

#include "Engine/Renderer/UiDrawList.h"

#include "Engine/Renderer/RenderSnapshot.h"

#include <array>
#include <cstddef>
#include <memory_resource>

// The UI draw list (M22, C22.1; Docs/Decisions/0033-m22-contract.md decision 7): its containers' memory resource, as the
// render snapshot's (M18's R-ARENA), and the clip rectangle's emptiness.

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("UiDrawList: a list over a memory resource allocates its items and annotations from it")
		{
			std::array<std::byte, 4096> storage{};
			std::pmr::monotonic_buffer_resource arena(storage.data(), storage.size(), std::pmr::null_memory_resource());
			UiDrawList list(&arena);
			CHECK(list.Items.get_allocator().resource() == &arena);
			CHECK(list.Annotations.get_allocator().resource() == &arena);
			list.Items.push_back(UiDrawItem{ .Kind = UiDrawKind::Text });
			list.Annotations.push_back(UiAnnotation{ .Outline = true });
			CHECK(list.Items.size() == 1);

			// A copy allocates from the default resource; the default list does too.
			const UiDrawList copy = list;
			CHECK(copy.Items.get_allocator().resource() == std::pmr::get_default_resource());
			CHECK(UiDrawList{}.Items.get_allocator().resource() == std::pmr::get_default_resource());

			// The snapshot gives its Ui list its own resource.
			const RenderSnapshot snapshot(&arena);
			CHECK(snapshot.Ui.Items.get_allocator().resource() == &arena);
			CHECK(snapshot.Ui.Annotations.get_allocator().resource() == &arena);
		}

		TEST_CASE("UiClipRect: a rectangle that holds no pixel is empty")
		{
			CHECK_FALSE(UiClipRect{ .MinX = -2, .MinY = 0, .MaxX = 1, .MaxY = 1 }.IsEmpty());
			CHECK(UiClipRect{ .MinX = 3, .MinY = 0, .MaxX = 3, .MaxY = 5 }.IsEmpty());
			CHECK(UiClipRect{ .MinX = 0, .MinY = 5, .MaxX = 4, .MaxY = 2 }.IsEmpty());
			CHECK(UiClipRect{}.IsEmpty());
		}
	}

}
