#include "TestsPCH.h"

#include "Engine/Renderer/DebugDrawList.h"

#include "Support/DeathTest.h"

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <variant>

// The debug draw list (Architecture §8.10), implemented by the M8 contract because M11's collider visualization and M13's
// scripts emit into it (Docs/Decisions/0013-m8-decisions.md decision 6).

namespace Engine {

	ENGINE_DEATH_TEST("Renderer/DebugDrawListNegativeAdvance")
	{
		DebugDrawList list;
		list.Advance(-1.0f);
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("DebugDrawList: every primitive is recorded in order with its colour, duration and depth mode")
		{
			DebugDrawList list;
			const glm::vec4 red(1.0f, 0.0f, 0.0f, 1.0f);
			list.AddLine(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), red);
			list.AddRay(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 2.0f, red, 0.5f, DebugDepthMode::OnTop);
			list.AddBox(glm::vec3(1.0f), glm::vec3(0.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), red);
			list.AddSphere(glm::vec3(2.0f), 0.25f, red);
			list.AddCapsule(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 0.5f, red);
			list.AddArrow(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.2f, red);
			list.AddFrustum(std::array<glm::vec3, 8>{}, red);
			list.AddText(glm::vec3(0.0f, 2.0f, 0.0f), "Label", 20.0f, red, 1.0f);
			REQUIRE(list.GetSize() == 8);
			CHECK(std::holds_alternative<DebugLine>(list.GetCommands()[0].Shape));
			CHECK(std::holds_alternative<DebugRay>(list.GetCommands()[1].Shape));
			CHECK(std::holds_alternative<DebugBox>(list.GetCommands()[2].Shape));
			CHECK(std::holds_alternative<DebugSphere>(list.GetCommands()[3].Shape));
			CHECK(std::holds_alternative<DebugCapsule>(list.GetCommands()[4].Shape));
			CHECK(std::holds_alternative<DebugArrow>(list.GetCommands()[5].Shape));
			CHECK(std::holds_alternative<DebugFrustum>(list.GetCommands()[6].Shape));
			REQUIRE(std::holds_alternative<DebugText>(list.GetCommands()[7].Shape));
			CHECK(std::get<DebugText>(list.GetCommands()[7].Shape).Text == "Label");
			CHECK(std::get<DebugRay>(list.GetCommands()[1].Shape).Length == 2.0f);
			CHECK(list.GetCommands()[1].Depth == DebugDepthMode::OnTop);
			CHECK(list.GetCommands()[1].Duration == 0.5f);
			CHECK(list.GetCommands()[1].Remaining == 0.5f);
			CHECK(list.GetCommands()[0].Depth == DebugDepthMode::Tested);
			CHECK(list.GetCommands()[0].Color == red);
		}

		TEST_CASE("DebugDrawList: negative and non-finite durations are stored as 0")
		{
			DebugDrawList list;
			list.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f), -2.0f);
			list.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f), std::numeric_limits<float>::quiet_NaN());
			list.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f), std::numeric_limits<float>::infinity());
			for (const DebugDrawCommand& command : list.GetCommands())
			{
				CHECK(command.Duration == 0.0f);
				CHECK(command.Remaining == 0.0f);
			}
		}

		TEST_CASE("DebugDrawList: Advance keeps a command for its duration and a zero-duration command until the next step")
		{
			DebugDrawList list;
			list.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f));        // one step
			list.AddLine(glm::vec3(0.0f), glm::vec3(2.0f), glm::vec4(1.0f), 0.06f); // more than three steps of 1/60 s
			constexpr float Step = 1.0f / 60.0f;
			// The first step's Advance removes the zero-duration command, which was extracted once since it was added.
			list.Advance(Step);
			REQUIRE(list.GetSize() == 1);
			CHECK(std::get<DebugLine>(list.GetCommands()[0].Shape).To == glm::vec3(2.0f));
			list.Advance(Step);
			list.Advance(Step);
			CHECK(list.GetSize() == 1);
			list.Advance(Step);
			CHECK(list.GetSize() == 1); // 0.06 - 3 steps > 0 at the start of the fourth step
			list.Advance(Step);
			CHECK(list.IsEmpty());
		}

		TEST_CASE("DebugDrawList: Append keeps order and remaining times, and appending a list to itself duplicates it")
		{
			DebugDrawList persistent;
			persistent.AddSphere(glm::vec3(0.0f), 1.0f, glm::vec4(1.0f), 2.0f);
			persistent.Advance(0.5f);
			DebugDrawList view;
			view.AddBox(glm::vec3(0.0f), glm::vec3(1.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(1.0f));
			view.Append(persistent);
			REQUIRE(view.GetSize() == 2);
			CHECK(std::holds_alternative<DebugBox>(view.GetCommands()[0].Shape));
			CHECK(view.GetCommands()[1].Remaining == 1.5f);
			view.Append(view);
			CHECK(view.GetSize() == 4);
			CHECK(view.GetCommands()[2] == view.GetCommands()[0]);
			const DebugDrawList copy = view;
			CHECK(copy == view);
			view.Clear();
			CHECK(view.IsEmpty());
		}

		TEST_CASE("DebugDrawList: commands past the limit are dropped and counted")
		{
			DebugDrawList list;
			for (size_t index = 0; index < DebugDrawList::MaxCommands + 3; ++index)
				list.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f));
			CHECK(list.GetSize() == DebugDrawList::MaxCommands);
			CHECK(list.GetDroppedCount() == 3);
			DebugDrawList other;
			other.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f));
			list.Append(other);
			CHECK(list.GetDroppedCount() == 4);
			list.Clear();
			CHECK(list.GetDroppedCount() == 0);
		}

		TEST_CASE("DebugDrawList: Advance asserts a finite, non-negative delta")
		{
			ENGINE_CHECK_DEATH("Renderer/DebugDrawListNegativeAdvance", "DebugDrawList::Advance needs a finite, non-negative delta");
		}
	}

}
