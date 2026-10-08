#include "TestsPCH.h"

#include "EditorCore/Automation/ExportMethods.h"

#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

// project.export in process (Architecture §13.5, §14.2). Skipped skeletons of the M7 contract
// (Docs/Decisions/0012-m7-decisions.md decision 14): stream D implements and registers the method and removes the skips.

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ExportMethods: project.export refuses testing exports and output directories outside Build" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ExportMethods");
			const Json testing = fixture.Request("project.export", Json{ { "config", "release" }, { "testing", true } });
			CHECK(testing["error"]["code"] == Json(-32009));
			CHECK(testing["error"]["data"]["issues"][0]["pointer"] == Json("/testing"));

			const Json outside = fixture.Request("project.export", Json{ { "config", "release" }, { "outDir", "Assets/Game" } });
			CHECK(outside["error"]["code"] == Json(-32602));
			CHECK(outside["error"]["data"]["issues"][0]["pointer"] == Json("/outDir"));
		}

		TEST_CASE("ExportMethods: project.export needs its config and supports no dry run" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ExportMethods");
			const Json missing = fixture.Request("project.export", Json::object());
			CHECK(missing["error"]["code"] == Json(-32602));
			const Json dryRun = fixture.Request("project.export", Json{ { "config", "dist" }, { "dryRun", true } });
			CHECK(dryRun["error"]["code"] == Json(-32009));
		}
	}

}
