#include "TestsPCH.h"

#include "EditorCore/Automation/AutomationTypes.h"

#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/StructInfo.h"
#include "Support/EditorTestFixture.h"

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("AutomationTypes: counters saturate at UINT32_MAX instead of wrapping")
		{
			static_assert(ToAutomationCounter(0) == 0);
			static_assert(ToAutomationCounter(UINT32_MAX) == UINT32_MAX);
			static_assert(ToAutomationCounter(uint64_t(UINT32_MAX) + 1) == UINT32_MAX);
			CHECK(ToAutomationCounter(42) == 42);
			CHECK(ToAutomationCounter(UINT64_MAX) == UINT32_MAX);
		}

		TEST_CASE("AutomationTypes: the shared structs and the enums automation names are registered" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("AutomationTypes");
			const TypeRegistry& types = fixture.GetEngine().GetTypeRegistry();
			const StructInfo* summary = types.FindStruct("EntitySummary");
			REQUIRE(summary != nullptr);
			CHECK(summary->FindField("id") != nullptr);
			CHECK(summary->FindField("name") != nullptr);
			CHECK(summary->FindField("path") != nullptr);
			REQUIRE(types.FindStruct("NoParams") != nullptr);
			CHECK(types.FindStruct("NoParams")->GetFields().empty());
			for (const std::string_view name : { "SceneTarget", "CommandOrigin", "DiagnosticSeverity", "LogLevel", "LogChannel", "EngineEventType" })
			{
				INFO(std::string(name));
				CHECK(types.FindEnum(name) != nullptr);
			}
			const EnumInfo* target = types.FindEnum("SceneTarget");
			REQUIRE(target != nullptr);
			const EnumEntry* play = target->FindByNameIgnoreCase("play");
			REQUIRE(play != nullptr);
			CHECK(play->Name == "Play");
		}
	}

}
