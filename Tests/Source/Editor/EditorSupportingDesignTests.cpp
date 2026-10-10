#include "TestsPCH.h"
#include "Editor/Panels/AutomationPanel.h"
#include "Editor/Panels/ConsolePanel.h"
#include "Editor/Panels/DiagnosticsPanel.h"
#include "Editor/Panels/ProjectSettingsPanel.h"
#include "Editor/Panels/StatsPanel.h"
#include "Editor/Panels/UndoHistoryPanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "Editor/Ui/EditorStyle.h"
#include "Engine/Core/Log.h"
#include "Support/ExpectLog.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <vector>

namespace Engine {

	namespace Test {

		std::vector<ImFontGlyph> SupportingPanelTestUi::CaptureGlyphs(std::string_view text)
		{
			ImFontBaked* font = ImGui::GetFontBaked();
			if (!font)
				return {};
			std::vector<ImFontGlyph> glyphs;
			for (const char character : text)
			{
				const auto codepoint = static_cast<ImWchar>(static_cast<unsigned char>(character));
				if (!font->IsGlyphLoaded(codepoint))
					return {};
				const ImFontGlyph* glyph = font->FindGlyphNoFallback(codepoint);
				if (!glyph)
					return {};
				if (glyph->Visible)
					glyphs.push_back(*glyph);
			}
			return glyphs;
		}

		// Match copied glyphs against actual rendered quads. No current font or window is needed after Render().
		std::optional<ImVec2> SupportingPanelTestUi::FindRenderedText(std::span<const ImFontGlyph> glyphs)
		{
			const ImDrawData* data = ImGui::GetDrawData();
			if (!data || glyphs.empty())
				return std::nullopt;
			for (int listIndex = data->CmdListsCount - 1; listIndex >= 0; --listIndex)
			{
				const ImDrawList& list = *data->CmdLists[listIndex];
				const size_t vertexCount = static_cast<size_t>(list.VtxBuffer.Size);
				for (size_t first = 0; first + glyphs.size() * 4 <= vertexCount; ++first)
				{
					bool match = true;
					for (size_t index = 0; index < glyphs.size(); ++index)
					{
						const ImDrawVert& top = list.VtxBuffer[static_cast<int>(first + index * 4)];
						const ImDrawVert& bottom = list.VtxBuffer[static_cast<int>(first + index * 4 + 2)];
						const ImFontGlyph& glyph = glyphs[index];
						// SmallButton clips glyphs to its tight text bounds, adjusting their UV rectangle.
						// The visible subrectangle must still belong to this exact glyph in the atlas.
						if (top.uv.x < glyph.U0 || top.uv.y < glyph.V0 || bottom.uv.x > glyph.U1 || bottom.uv.y > glyph.V1 || top.uv.x >= bottom.uv.x || top.uv.y >= bottom.uv.y)
						{
							match = false;
							break;
						}
					}
					if (match)
					{
						const ImVec2 top = list.VtxBuffer[static_cast<int>(first)].pos;
						const ImVec2 bottom = list.VtxBuffer[static_cast<int>(first + glyphs.size() * 4 - 2)].pos;
						const ImVec2 center((top.x + bottom.x) * 0.5f, (top.y + bottom.y) * 0.5f);
						if (center.x < 0.0f || center.y < 0.0f || center.x >= data->DisplaySize.x || center.y >= data->DisplaySize.y)
							continue;
						// The log fixture also submits clipped content. Accept only geometry inside its draw command's clip.
						for (const ImDrawCmd& command : list.CmdBuffer)
						{
							if (top.x < command.ClipRect.x || top.y < command.ClipRect.y || bottom.x > command.ClipRect.z || bottom.y > command.ClipRect.w)
								continue;
							for (unsigned int element = 0; element < command.ElemCount; ++element)
							{
								const auto vertex = static_cast<size_t>(list.IdxBuffer[static_cast<int>(command.IdxOffset + element)]) + command.VtxOffset;
								if (vertex == first)
									return center;
							}
						}
					}
				}
			}
			return std::nullopt;
		}

	}

	namespace {

		struct SupportingTextTarget
		{
			float Scale = 1.0f;
			bool SmallButton = false;
			float RenderedFontSize = 0.0f;
			ImVec2 Minimum{};
			ImVec2 Maximum{};
			uint32_t Clicks = 0;

			[[nodiscard]] Status Draw(EditorPanelContext&)
			{
				ImGui::SetWindowFontScale(Scale);
				RenderedFontSize = ImGui::GetFontSize();
				if (SmallButton ? ImGui::SmallButton("Rendered click target") : ImGui::Button("Rendered click target"))
					++Clicks;
				Minimum = ImGui::GetItemRectMin();
				Maximum = ImGui::GetItemRectMax();
				return {};
			}
		};

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
		TEST_CASE("EditorSupportingDesign: text clicks retain the rendered window font after the frame ends")
		{
			Test::UtilityPanelFixture fixture;
			// The fixture prebuilds a legacy atlas; window scaling uses its existing glyphs.
			Utils::ApplyEditorStyle(1.0f);
			SupportingTextTarget target;
			SUBCASE("normal window scale")
			{
			}
			SUBCASE("double window scale")
			{
				target.Scale = 2.0f;
			}
			SUBCASE("small button")
			{
				target.SmallButton = true;
			}
			SUBCASE("small button at double window scale")
			{
				target.SmallButton = true;
				target.Scale = 2.0f;
			}
			const auto position = Test::SupportingPanelTestUi::FindText(fixture, target, "Rendered click target");
			REQUIRE(position.has_value());
			CHECK(ImGui::GetFontBaked() == nullptr);
			CHECK(target.RenderedFontSize == doctest::Approx(ImGui::GetFontSize() * target.Scale));
			CHECK(position->x > target.Minimum.x);
			CHECK(position->x < target.Maximum.x);
			CHECK(position->y > target.Minimum.y);
			CHECK(position->y < target.Maximum.y);
			CHECK_FALSE(Test::SupportingPanelTestUi::FindText(fixture, target, "Absent target").has_value());
			CHECK(target.Clicks == 0);
			Test::SupportingPanelTestUi::ClickText(fixture, target, "Rendered click target");
			CHECK(target.Clicks == 1);
		}

		TEST_CASE("EditorSupportingDesign: console filters compose and clearing only hides the shared log")
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

		TEST_CASE("EditorSupportingDesign: console source and entity actions retain structured context")
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

		TEST_CASE("EditorSupportingDesign: settings navigation retains every reflected category without writing")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			ProjectSettingsPanel panel;
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const auto before = FileSystem::ReadText(fixture.GetEditor().GetProject().GetProjectFile());
			REQUIRE(before);
			CHECK_FALSE(fixture.Draw(panel).contains("Gravity"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "General");
			const std::string categories = fixture.Draw(panel);
			for (const char* category : { "Window", "Simulation", "Physics", "Input", "Rendering", "Scripting", "Export", "Testing" })
				CHECK_MESSAGE(categories.contains(category), category);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Physics");
			const std::string physics = fixture.Draw(panel);
			CHECK(physics.contains("Gravity"));
			CHECK(physics.contains("Collisions"));
			CHECK_FALSE(physics.contains("Start scene"));
			fixture.Pump();
			CHECK(fixture.GetEditor().GetRevision() == revision);
			const auto after = FileSystem::ReadText(fixture.GetEditor().GetProject().GetProjectFile());
			REQUIRE(after);
			CHECK(*after == *before);
		}

		TEST_CASE("EditorSupportingDesign: settings text edits queue one undoable human command")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			ProjectSettingsPanel panel;
			fixture.Draw(panel); // Settle the reflected table layout before measuring its input text.
			const auto before = fixture.GetEditor().GetProject().GetSettings();
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const size_t history = fixture.GetEditor().GetHistory().GetUndoCount();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, before.Name);
			fixture.ReplaceFocusedText(panel, "Supporting human settings");
			CHECK(fixture.GetEditor().GetRevision() == revision);
			fixture.Press(panel, ImGuiKey_Enter);
			CHECK(fixture.GetEditor().GetRevision() == revision);
			fixture.Pump();
			fixture.Draw(panel);
			CHECK(fixture.GetEditor().GetProject().GetSettings().Name == "Supporting human settings");
			CHECK(fixture.GetEditor().GetProject().GetSettings().Window.Title == before.Window.Title);
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() == history + 1);
			const auto entries = fixture.GetEditor().GetHistory().GetEntries(1);
			REQUIRE(entries.size() == 1);
			CHECK(entries.front().Origin == CommandOrigin::User);
			REQUIRE(fixture.GetClient().Call("edit.undo", Json::object()));
			CHECK(fixture.GetEditor().GetProject().GetSettings().Name == before.Name);
		}

		TEST_CASE("EditorSupportingDesign: intervening edits discard the settings draft")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			ProjectSettingsPanel panel;
			fixture.Draw(panel); // Match the initial layout frame used by the legacy settings interaction tests.
			Test::SupportingPanelTestUi::ClickText(fixture, panel, fixture.GetEditor().GetProject().GetSettings().Name);
			fixture.ReplaceFocusedText(panel, "Uncommitted supporting draft");
			REQUIRE(fixture.GetClient().Call("project.setSettings", Json{ { "patch", Json{ { "Name", "Agent settings" } } } }));
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const Test::ExpectLog conflict(LogLevel::Error, "project changed while editing settings");
			CHECK(fixture.Draw(panel).contains("draft discarded"));
			fixture.Press(panel, ImGuiKey_Enter);
			fixture.Pump();
			CHECK(fixture.GetEditor().GetProject().GetSettings().Name == "Agent settings");
			CHECK(fixture.GetEditor().GetRevision() == revision);
			CHECK(conflict.GetMatchCount() == 1);
		}

		TEST_CASE("EditorSupportingDesign: filtered diagnostic fixes remain queued human commands")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			REQUIRE(fixture.GetClient().Call("project.setSettings", Json{ { "patch", Json{ { "StartScene", "Assets/Scenes/OtherMissing.scene" }, { "Export", Json{ { "BuildScenes", Json::array({ "Assets/Scenes/Absent.scene" }) } } } } } }));
			DiagnosticsPanel panel;
			fixture.Draw(panel);
			fixture.Pump();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Search messages, files or codes");
			ImGui::GetIO().AddInputCharactersUTF8("build_scene_missing");
			const std::string filtered = fixture.Draw(panel);
			CHECK(filtered.contains("BUILD_SCENE_MISSING"));
			CHECK_FALSE(filtered.contains("BUILD_START_SCENE_MISSING"));
			const uint64_t revision = fixture.GetEditor().GetRevision();
			fixture.GetContext().AutomationControls.SetPolicy({ .DenyMutations = true });
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Include in fix");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Fix selected");
			CHECK(fixture.GetEditor().GetRevision() == revision);
			CHECK(fixture.GetEditor().GetProject().GetSettings().Export.BuildScenes.size() == 1);
			fixture.Pump();
			fixture.Draw(panel);
			CHECK(fixture.GetEditor().GetProject().GetSettings().Export.BuildScenes.empty());
			const auto entries = fixture.GetEditor().GetHistory().GetEntries(1);
			REQUIRE(entries.size() == 1);
			CHECK(entries.front().Origin == CommandOrigin::User);
			fixture.Pump();
			CHECK_FALSE(fixture.Draw(panel).contains("Export.BuildScenes names"));
		}

		TEST_CASE("EditorSupportingDesign: performance updates pause without changing simulation state")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			StatsPanel panel;
			fixture.Draw(panel);
			fixture.Pump();
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const std::string sampled = fixture.Draw(panel);
			CHECK(sampled.contains("CPU frame"));
			CHECK(sampled.contains("Physics bodies"));
			CHECK(sampled.contains("Unavailable (no active script VM)"));
			CHECK(sampled.contains("Render statistics unavailable"));
			CHECK(fixture.GetEditor().GetRevision() == revision);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Live");
			fixture.Pump(); // finish the sample already in flight
			const std::string paused = fixture.Draw(panel);
			CHECK(paused.contains("Updates paused"));
			REQUIRE(fixture.GetClient().Call("entity.create", Json{ { "name", "After paused sample" } }));
			fixture.Pump();
			CHECK(fixture.Draw(panel) == paused);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Refresh");
			fixture.Pump();
			CHECK(fixture.Draw(panel) != paused);
			CHECK(fixture.Draw(panel).contains("Updates paused"));
		}

		TEST_CASE("EditorSupportingDesign: automation outcome and method filters compose")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			REQUIRE(fixture.GetClient().Call("project.info", Json::object()));
			REQUIRE(fixture.GetClient().Call("entity.create", Json{ { "name", "Allowed" } }));
			AutomationPanel panel;
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Deny agent mutations");
			CHECK(fixture.GetContext().AutomationControls.GetPolicy().DenyMutations);
			const auto denied = fixture.GetClient().Call("entity.create", Json{ { "name", "Denied" } });
			REQUIRE_FALSE(denied);
			CHECK(denied.error().GetCode() == ErrorCode::PermissionDenied);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Search client or method");
			ImGui::GetIO().AddInputCharactersUTF8("ENTITY.CREATE");
			fixture.Draw(panel);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "All requests");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Failed");
			const std::string failures = fixture.Draw(panel);
			CHECK(failures.contains("entity.create"));
			CHECK_FALSE(failures.contains("project.info"));
			CHECK_FALSE(failures.contains("Completed"));
		}

		TEST_CASE("EditorSupportingDesign: history distinguishes saved applied and undone agent changes")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			UndoHistoryPanel panel;
			CHECK(fixture.Draw(panel).contains("Scene matches the saved state"));
			REQUIRE(fixture.GetClient().Call("entity.create", Json{ { "name", "Supporting history target" } }));
			CHECK(fixture.Draw(panel).contains("Unsaved scene changes"));
			CHECK(fixture.Draw(panel).contains("[agent]"));
			const uint64_t revision = fixture.GetEditor().GetRevision();
			const size_t count = fixture.GetEditor().GetHistory().GetUndoCount();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Undo");
			CHECK(fixture.GetEditor().GetRevision() == revision);
			fixture.Pump();
			const std::string undone = fixture.Draw(panel);
			CHECK(undone.contains("Scene matches the saved state"));
			CHECK(undone.contains("Undone"));
			CHECK(undone.contains("Current position"));
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() + 1 == count);
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Redo");
			CHECK(fixture.GetEditor().GetHistory().GetRedoCount() == 1);
			fixture.Pump();
			CHECK(fixture.Draw(panel).contains("Applied"));
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() == count);
		}

		TEST_CASE("EditorSupportingDesign: compact console keeps controls visible at editor scales")
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
