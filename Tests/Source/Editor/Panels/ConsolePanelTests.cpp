#include "TestsPCH.h"
#include "Editor/Panels/ConsolePanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "Editor/Ui/EditorStyle.h"
#include "Engine/Core/Log.h"

#include <string>

namespace Engine {

	namespace {

		template<typename Panel>
		struct SupportingPanelSize
		{
			Panel& Instance;
			ImVec2 Size{};

			[[nodiscard]] Status Draw(EditorPanelContext& context)
			{
				Status result;
				if (ImGui::BeginChild("SupportingDesign", Size))
					result = Instance.Draw(context);
				ImGui::EndChild();
				return result;
			}
		};

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("ConsolePanel: source navigation and entity selection use structured log context")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			const auto created = fixture.GetClient().Call("entity.create", Json{ { "name", "Console target" } });
			REQUIRE(created);
			const auto idText = JsonReader((*created)["entity"]).ReadMember<std::string>("id");
			REQUIRE(idText);
			const auto id = UUID::FromString(*idText);
			REQUIRE(id);
			std::filesystem::path opened;
			uint32_t openedLine = 0;
			fixture.GetContext().OpenSource = [&opened, &openedLine](const std::filesystem::path& path, uint32_t line) -> Status
			{
				opened = path;
				openedLine = line;
				return {};
			};
			ConsolePanel panel;
			Log::GetRingBuffer().Append(LogEntry{ .Tick = {}, .Message = "UtilityNavigationOnly", .File = "Native.cpp", .Line = 9, .EntityId = *id, .ScriptFile = "Source with spaces.luau", .ScriptLine = 42 });
			fixture.Draw(panel);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Search messages");
			ImGui::GetIO().AddInputCharactersUTF8("UtilityNavigationOnly");
			REQUIRE(fixture.Draw(panel).contains("Source with spaces.luau:42"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Open source");
			CHECK(opened == std::filesystem::path("Source with spaces.luau"));
			CHECK(openedLine == 42);
			fixture.GetEditor().SetSelection({});
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Select entity");
			CHECK(fixture.GetEditor().GetSelection().empty());
			fixture.Pump();
			fixture.Draw(panel);
			const auto selection = fixture.GetEditor().GetSelection();
			REQUIRE(selection.size() == 1);
			CHECK(selection[0] == *id);
		}

		TEST_CASE("ConsolePanel: level and logger filters compose and clearing preserves the shared ring")
		{
			Test::UtilityPanelFixture fixture;
			ConsolePanel panel;
			RingBufferSink& ring = Log::GetRingBuffer();
			const uint64_t first = ring.GetNextSeq();
			ring.Append(LogEntry{ .Tick = {}, .Level = LogLevel::Trace, .Logger = LogChannel::Script, .Message = "UtilityTrace", .File = {}, .EntityId = {}, .ScriptFile = {} });
			ring.Append(LogEntry{ .Tick = {}, .Level = LogLevel::Info, .Logger = LogChannel::Engine, .Message = "UtilityEngine", .File = {}, .EntityId = {}, .ScriptFile = {} });
			ring.Append(LogEntry{ .Tick = {}, .Level = LogLevel::Info, .Logger = LogChannel::Script, .Message = "UtilityScript", .File = {}, .EntityId = {}, .ScriptFile = {} });
			fixture.Draw(panel);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "All levels");
			fixture.Press(panel, ImGuiKey_Home);
			fixture.Press(panel, ImGuiKey_DownArrow);
			fixture.Press(panel, ImGuiKey_Enter);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "All sources");
			fixture.Press(panel, ImGuiKey_End);
			fixture.Press(panel, ImGuiKey_Enter);
			const std::string filtered = fixture.Draw(panel);
			CHECK(filtered.contains("UtilityScript"));
			CHECK_FALSE(filtered.contains("UtilityTrace"));
			CHECK_FALSE(filtered.contains("UtilityEngine"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Clear view");
			CHECK_FALSE(fixture.Draw(panel).contains("UtilityScript"));
			const auto retained = ring.Read(LogQuery{ .Cursor = first, .Channels = {}, .Contains = "Utility" });
			CHECK(retained.Entries.size() == 3);
		}

		TEST_CASE("ConsolePanel: text filtering retains matching structured log entries")
		{
			Test::UtilityPanelFixture fixture;
			ConsolePanel panel;
			Log::GetRingBuffer().Append(LogEntry{ .Tick = {}, .Message = "UtilityConsoleVisible", .File = "Example.cpp", .Line = 42, .EntityId = {}, .ScriptFile = {} });
			Log::GetRingBuffer().Append(LogEntry{ .Tick = {}, .Message = "UtilityConsoleHidden", .File = {}, .EntityId = {}, .ScriptFile = {} });
			fixture.Draw(panel);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Search messages");
			ImGui::GetIO().AddInputCharactersUTF8("UtilityConsoleVisible");
			const std::string text = fixture.Draw(panel);
			CHECK(text.contains("UtilityConsoleVisible"));
			CHECK_FALSE(text.contains("UtilityConsoleHidden"));
			CHECK(text.contains("Example.cpp:42"));
		}

		TEST_CASE("ConsolePanel: source and level filter buttons compose and clearing only hides the shared log")
		{
			Test::UtilityPanelFixture fixture;
			ConsolePanel panel;
			auto& ring = Log::GetRingBuffer();
			const uint64_t first = ring.GetNextSeq();
			ring.Append(LogEntry{ .Tick = {}, .Level = LogLevel::Trace, .Logger = LogChannel::Script, .Message = "SupportingTrace", .File = {}, .EntityId = {}, .ScriptFile = {} });
			ring.Append(LogEntry{ .Tick = {}, .Level = LogLevel::Info, .Logger = LogChannel::Engine, .Message = "SupportingEngine", .File = {}, .EntityId = {}, .ScriptFile = {} });
			ring.Append(LogEntry{ .Tick = {}, .Level = LogLevel::Info, .Logger = LogChannel::Script, .Message = "SupportingScript", .File = {}, .EntityId = {}, .ScriptFile = {} });
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "All levels");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Info and above");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "All sources");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Scripts");
			const std::string filtered = fixture.Draw(panel);
			CHECK(filtered.contains("SupportingScript"));
			CHECK_FALSE(filtered.contains("SupportingTrace"));
			CHECK_FALSE(filtered.contains("SupportingEngine"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Clear view");
			CHECK_FALSE(fixture.Draw(panel).contains("SupportingScript"));
			CHECK(ring.Read(LogQuery{ .Cursor = first, .Channels = {}, .Contains = "Supporting" }).Entries.size() == 3);
			ring.Append(LogEntry{ .Tick = {}, .Logger = LogChannel::Script, .Message = "SupportingNewMessage", .File = {}, .EntityId = {}, .ScriptFile = {} });
			CHECK(fixture.Draw(panel).contains("SupportingNewMessage"));
		}

		TEST_CASE("ConsolePanel: source and entity buttons retain structured context")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			const auto created = fixture.GetClient().Call("entity.create", Json{ { "name", "Supporting target" } });
			REQUIRE(created);
			const auto id = JsonReader((*created)["entity"]["id"]).ReadUUID();
			REQUIRE(id);
			std::filesystem::path opened;
			uint32_t openedLine = 0;
			fixture.GetContext().OpenSource = [&opened, &openedLine](const std::filesystem::path& path, uint32_t line) -> Status
			{
				opened = path;
				openedLine = line;
				return {};
			};
			ConsolePanel panel;
			Log::GetRingBuffer().Append(LogEntry{ .Tick = {}, .Message = "SupportingNavigation", .File = "Native.cpp", .Line = 9, .EntityId = *id, .ScriptFile = "Assets/Source with spaces.luau", .ScriptLine = 42 });
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Search messages");
			ImGui::GetIO().AddInputCharactersUTF8("SupportingNavigation");
			CHECK(fixture.Draw(panel).contains("Source with spaces.luau:42"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Open source");
			CHECK(opened == std::filesystem::path("Assets/Source with spaces.luau"));
			CHECK(openedLine == 42);
			fixture.GetEditor().SetSelection({});
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Select entity");
			CHECK(fixture.GetEditor().GetSelection().empty());
			fixture.Pump();
			fixture.Draw(panel);
			const auto selection = fixture.GetEditor().GetSelection();
			REQUIRE(selection.size() == 1);
			CHECK(selection.front() == *id);
		}

		TEST_CASE("ConsolePanel: a compact panel keeps its controls visible at editor scales")
		{
			Test::UtilityPanelFixture fixture;
			float scale = 1.0f;
			SUBCASE("normal scale")
			{
			}
			SUBCASE("large scale")
			{
				scale = 1.5f;
			}
			Utils::ApplyEditorStyle(scale);
			ConsolePanel panel;
			SupportingPanelSize<ConsolePanel> sized{ panel, ImVec2(360.0f * scale, 500.0f * scale) };
			CHECK(Test::SupportingPanelTestUi::FindText(fixture, sized, "All levels").has_value());
			CHECK(Test::SupportingPanelTestUi::FindText(fixture, sized, "All sources").has_value());
			CHECK(Test::SupportingPanelTestUi::FindText(fixture, sized, "Search messages").has_value());
			CHECK(Test::SupportingPanelTestUi::FindText(fixture, sized, "Follow newest").has_value());
			CHECK(Test::SupportingPanelTestUi::FindText(fixture, sized, "Clear view").has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, sized, "Clear view");
			CHECK(fixture.Draw(sized).contains("No messages to show"));
		}
	}

}
