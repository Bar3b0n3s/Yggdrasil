#include "TestsPCH.h"
#include "Engine/Session/PlaySession.h"

#include "Engine/Platform/DisplayModes.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string_view>
#include <vector>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 3): the display calls in a play session.

namespace Engine {

	TEST_SUITE("Session")
	{
		TEST_CASE("PlaySession: the display calls do nothing in a fixed-view session and reach the host in live play")
		{
			// M28 (Docs/Decisions/0043-m28-game-services.md decision 3).
			class DisplayHost final : public IPlaySessionHost
			{
			public:
				ScriptEnvironment GetScriptEnvironment() const override { return { .IsEditor = false, .IsHeadless = false }; }
				Status SetScriptCursorMode(CursorMode) override { return {}; }
				CursorMode GetScriptCursorMode() const override { return CursorMode::Normal; }
				void OnScriptError(const ScriptError&, bool) override {}
				void OnScriptPhase(std::string_view, std::string_view) override {}
				Result<std::vector<DisplayVideoMode>> GetDisplayModes() const override
				{
					return std::vector<DisplayVideoMode>{ { .Width = 1280, .Height = 720, .RefreshRate = 60 }, { .Width = 1920, .Height = 1080, .RefreshRate = 60 } };
				}
				Result<DisplaySettings> GetDisplaySettings() const override { return Current; }
				Status RequestDisplaySettings(const DisplaySettings& settings) override
				{
					Requests.push_back(settings);
					return {};
				}
				DisplaySettings Current{ .WindowMode = DisplayWindowMode::Windowed, .Width = 1280, .Height = 720, .RefreshRate = 0, .VSync = true };
				std::vector<DisplaySettings> Requests{};
			};
			const auto run = [](bool testMode, bool lockstep, DisplayHost& host) -> Json
			{
				Test::ScriptTestFixture fixture;
				PlaySessionSpecification specification{};
				specification.Registry = &fixture.GetTypes();
				specification.Assets = &fixture.GetAssetManager();
				specification.ScriptSchemas = fixture.GetSchemaSnapshot();
				specification.TestMode = testMode;
				specification.ViewWidth = 640;
				specification.ViewHeight = 360;
				specification.Project.Window.VSync = false;
				specification.Host = &host;
				auto created = PlaySession::CreateFromScene(specification, fixture.GetScene());
				REQUIRE(created);
				if (lockstep)
					(*created)->SetLockstep(true, 1);
				const auto evaluated = (*created)->GetScripts()->Evaluate(R"(
local settings = Application.GetDisplaySettings()
local accepted = Application.SetDisplaySettings({ WindowMode = "Exclusive", Width = 1920, Height = 1080, RefreshRate = 60 })
return { #Application.GetDisplayModes(), settings.WindowMode, settings.Width, settings.Height, settings.VSync, accepted }
)",
					{});
				REQUIRE_MESSAGE(evaluated.has_value(), (evaluated ? "" : evaluated.error().ToString()));
				return evaluated->Value.Get();
			};
			DisplayHost suiteHost;
			CHECK(run(true, false, suiteHost) == Json::array({ 0, "Windowed", 640, 360, false, false }));
			DisplayHost lockstepHost;
			CHECK(run(false, true, lockstepHost) == Json::array({ 0, "Windowed", 640, 360, false, false }));
			CHECK(suiteHost.Requests.empty());
			CHECK(lockstepHost.Requests.empty());
			DisplayHost liveHost;
			CHECK(run(false, false, liveHost) == Json::array({ 2, "Windowed", 1280, 720, true, true }));
			REQUIRE(liveHost.Requests.size() == 1);
			CHECK(liveHost.Requests[0] == DisplaySettings{ .WindowMode = DisplayWindowMode::Exclusive, .Width = 1920, .Height = 1080, .RefreshRate = 60, .VSync = true });
		}
	}

}
