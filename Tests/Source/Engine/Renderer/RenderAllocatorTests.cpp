#include "TestsPCH.h"

#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <memory_resource>

// The memory resource of the render snapshot's and the debug draw lists' containers (M18, C18.1, R-ARENA;
// Docs/Decisions/0029-m18-contract.md decision 9), which W18.1 measures a per-frame arena through.

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderSnapshot: a snapshot over a memory resource allocates its containers from it")
		{
			std::pmr::monotonic_buffer_resource arena;
			RenderSnapshot snapshot(&arena);
			snapshot.Meshes.push_back(MeshDrawItem{});
			snapshot.Lights.push_back(LightData{});
			snapshot.Texts.push_back(TextItem{ .Text = "Score" });
			snapshot.PickTable.push_back(UUID(1));
			snapshot.Icons.push_back(RenderIcon{});
			snapshot.DebugDraw.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f));
			snapshot.AnnotationDraw.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f));
			CHECK(snapshot.Meshes.get_allocator().resource() == &arena);
			CHECK(snapshot.Lights.get_allocator().resource() == &arena);
			CHECK(snapshot.Texts.get_allocator().resource() == &arena);
			CHECK(snapshot.PickTable.get_allocator().resource() == &arena);
			CHECK(snapshot.Icons.get_allocator().resource() == &arena);
			CHECK(snapshot.DebugDraw.GetMemoryResource() == &arena);
			CHECK(snapshot.AnnotationDraw.GetMemoryResource() == &arena);

			// A copy allocates from the default resource, so it may outlive the arena; its values are equal.
			const RenderSnapshot copy = snapshot;
			CHECK(copy.Meshes.get_allocator().resource() == std::pmr::get_default_resource());
			CHECK(copy.DebugDraw.GetMemoryResource() == std::pmr::get_default_resource());
			REQUIRE(copy.Texts.size() == 1);
			CHECK(copy.Texts[0].Text == "Score");
			CHECK(copy.DebugDraw == snapshot.DebugDraw);
			// A default snapshot uses the default resource.
			const RenderSnapshot plain;
			CHECK(plain.Meshes.get_allocator().resource() == std::pmr::get_default_resource());
		}

		TEST_CASE("DebugDrawList: a list over a memory resource allocates its commands from it")
		{
			std::pmr::monotonic_buffer_resource arena;
			DebugDrawList list(&arena);
			list.AddSphere(glm::vec3(0.0f), 1.0f, glm::vec4(1.0f));
			CHECK(list.GetMemoryResource() == &arena);
			CHECK(list.GetSize() == 1);
			// Assignment keeps the target's resource.
			DebugDrawList other;
			other = list;
			CHECK(other.GetMemoryResource() == std::pmr::get_default_resource());
			CHECK(other == list);
			list.Clear();
			CHECK(list.GetMemoryResource() == &arena);
		}
	}

}
