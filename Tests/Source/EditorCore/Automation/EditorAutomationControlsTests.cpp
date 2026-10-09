#include "TestsPCH.h"
#include "EditorCore/Automation/EditorAutomationControls.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorPreferences.h"
#include "Engine/Automation/Protocol/SessionFile.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/Socket.h"
#include "Support/AutomationTestClient.h"

#include <doctest/doctest.h>

#include <cmath>
#include <set>

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("EditorAutomationControls: malformed preferences leave a running listener unchanged")
		{
			Test::EditorTestFixture fixture("ControlsMalformed");
			auto specification = Test::MakeTestServerSpecification();
			specification.SessionsDirectory = fixture.GetDirectory() / "Sessions";
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			EditorAutomationControls controls(fixture.GetEditor(), client.GetServer());
			REQUIRE(controls.SetAllowAiAutomation(true));
			const uint16_t port = client.GetServer().GetPort();
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			const std::string broken = "{broken preferences";
			REQUIRE(fixture.GetEditor().GetVfs().WriteFileAtomic(*path, std::as_bytes(std::span(broken.data(), broken.size()))));
			const Status failed = controls.SetAllowAiAutomation(false);
			REQUIRE_FALSE(failed);
			CHECK(failed.error().GetCode() == ErrorCode::Parse);
			CHECK(client.GetServer().GetPort() == port);
			CHECK(fixture.GetEditor().GetVfs().ReadText(*path).value_or("") == broken);
			CHECK(client.Call("session.info", Json::object()));
		}

		TEST_CASE("EditorAutomationControls: a paused agent receives Busy while human edits proceed")
		{
			Test::AutomationFixture fixture("ControlsPause");
			EditorContext& editor = fixture.GetEditor();
			AutomationServer& server = fixture.GetClient().GetServer();
			EditorAutomationControls controls(editor, server);
			EditorActions actions(editor, server);
			controls.SetPolicy({ .Paused = true });
			REQUIRE(controls.GetPolicy().Paused);
			const Json denied = fixture.Request("entity.create", Json{ { "name", "Denied" } });
			REQUIRE(denied.contains("error"));
			CHECK(denied["error"]["code"] == Json(std::to_underlying(RpcErrorCode::Busy)));
			const Json readDenied = fixture.Request("scene.tree", Json::object());
			REQUIRE(readDenied.contains("error"));
			CHECK(readDenied["error"]["code"] == Json(std::to_underlying(RpcErrorCode::Busy)));
			CHECK(fixture.Call("session.info", Json::object()));
			CHECK(fixture.Call("rpc.discover", Json::object()));
			const uint64_t revision = editor.GetRevision();
			const auto ticket = actions.Submit("entity.create", Json{ { "name", "Human" } });
			REQUIRE(ticket);
			CHECK(editor.GetRevision() == revision);
			actions.Pump();
			const auto result = actions.TakeResult(*ticket);
			REQUIRE(result);
			REQUIRE(result->has_value());
			REQUIRE(**result);
			CHECK(editor.GetRevision() > revision);
			const auto history = editor.GetHistory().GetEntries(1);
			REQUIRE(history.size() == 1);
			CHECK(history.front().Origin == CommandOrigin::User);
			CHECK_FALSE(history.front().Label.starts_with("[agent]"));
			controls.SetPolicy({});
			CHECK(fixture.Call("entity.create", Json{ { "name", "Agent" } }));
		}

		TEST_CASE("EditorAutomationControls: denied mutations include optional fixes and file writes")
		{
			Test::AutomationFixture fixture("ControlsDeny");
			EditorContext& editor = fixture.GetEditor();
			REQUIRE(fixture.Call("project.setSettings", Json{ { "patch", Json{ { "Export", Json{ { "BuildScenes", Json::array({ "Assets/Scenes/Missing.scene" }) } } } } } }));
			EditorAutomationControls controls(editor, fixture.GetClient().GetServer());
			controls.SetPolicy({ .DenyMutations = true });
			REQUIRE(controls.GetPolicy().DenyMutations);
			const uint64_t revision = editor.GetRevision();
			for (const auto& [method, params] : std::vector<std::pair<std::string, Json>>{
					 { "entity.create", Json{ { "name", "Blocked" } } },
					 { "project.validate", Json{ { "fix", true } } },
					 { "project.setSettings", Json{ { "patch", Json{ { "Window", Json{ { "Title", "Blocked" } } } } } } } })
			{
				INFO(method);
				const auto denied = fixture.Call(method, params);
				REQUIRE_FALSE(denied);
				CHECK(denied.error().GetCode() == ErrorCode::PermissionDenied);
			}
			CHECK(fixture.Call("project.validate", Json::object()));
			CHECK(fixture.Call("project.setSettings", Json{ { "patch", Json{ { "Window", Json{ { "Title", "Sandbox" } } } } }, { "dryRun", true } }));
			CHECK(editor.GetRevision() == revision);
			const auto file = VfsPath::Parse("project://Assets/Denied.txt");
			REQUIRE(file);
			const std::string bytes = "must not be written by an agent";
			auto command = ProjectSettingsCommand::CreateFromPatch(editor, Json{ { "Window", Json{ { "Title", "Boundary" } } } }, "Boundary");
			REQUIRE(command);
			editor.SetWriteAttribution(WriteAttribution{ .Method = "test.boundary", .Client = "test" });
			const auto deniedCommand = editor.Execute(std::move(*command));
			const auto deniedWrite = editor.WriteProjectFile(*file, std::as_bytes(std::span(bytes.data(), bytes.size())));
			editor.SetWriteAttribution(std::nullopt);
			REQUIRE_FALSE(deniedCommand);
			REQUIRE_FALSE(deniedWrite);
			CHECK(deniedCommand.error().GetCode() == ErrorCode::PermissionDenied);
			CHECK(deniedWrite.error().GetCode() == ErrorCode::PermissionDenied);
			CHECK_FALSE(editor.GetVfs().Exists(*file));
			CHECK(editor.GetRevision() == revision);
			REQUIRE(editor.WriteProjectFile(*file, std::as_bytes(std::span(bytes.data(), bytes.size()))));
			CHECK(editor.GetVfs().Exists(*file));
		}

		TEST_CASE("EditorAutomationControls: listener preference preserves explicit launch intent")
		{
			for (const bool explicitOptIn : { false, true })
			{
				Test::EditorTestFixture fixture("ControlsListener");
				auto specification = Test::MakeTestServerSpecification();
				specification.Listen = explicitOptIn;
				specification.SessionsDirectory = fixture.GetDirectory() / "Sessions";
				Test::AutomationTestClient client(fixture.GetEditor(), specification);
				EditorAutomationControls controls(fixture.GetEditor(), client.GetServer());
				const auto sessionFile = SessionFile::GetPath(specification.SessionsDirectory, Process::GetCurrentId());
				const ClientId existingClient = client.GetClient();
				REQUIRE(controls.SetAllowAiAutomation(true));
				CHECK(client.GetServer().GetPort() != 0);
				CHECK(FileSystem::Exists(sessionFile));
				const auto allowed = ReadEditorPreferences(fixture.GetEditor().GetVfs());
				REQUIRE(allowed);
				CHECK(allowed->AllowAiAutomation);
				REQUIRE(controls.SetAllowAiAutomation(false));
				CHECK((client.GetServer().GetPort() != 0) == explicitOptIn);
				CHECK(FileSystem::Exists(sessionFile) == explicitOptIn);
				CHECK(client.GetClient() == existingClient);
				CHECK(client.Call("session.info", Json::object()));
				const auto disabled = ReadEditorPreferences(fixture.GetEditor().GetVfs());
				REQUIRE(disabled);
				CHECK_FALSE(disabled->AllowAiAutomation);
			}
		}

		TEST_CASE("EditorAutomationControls: request activity is bounded and records failure timings")
		{
			Test::EditorTestFixture fixture("ControlsActivity");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto now = std::chrono::steady_clock::time_point{};
			auto specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::microseconds(125);
				return now;
			};
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			EditorAutomationControls controls(fixture.GetEditor(), client.GetServer());
			for (uint32_t index = 0; index < 270; ++index)
				REQUIRE(client.Call("project.info", Json::object()));
			const auto begin = now;
			const auto failure = client.Call("entity.get", Json{ { "entity", "000000000000dead" } });
			REQUIRE_FALSE(failure);
			const double elapsed = std::chrono::duration<double, std::milli>(now - begin).count();
			const auto activity = controls.GetRecentRequests();
			REQUIRE(activity.size() == 256);
			CHECK(activity.front().Method == "entity.get");
			CHECK(activity.front().Failed);
			CHECK(activity.front().Completed);
			CHECK(activity.front().DurationMilliseconds > 0.0);
			CHECK(activity.front().DurationMilliseconds <= elapsed);
			std::set<std::string> ids;
			for (const auto& request : activity)
			{
				CHECK(request.Completed);
				CHECK(std::isfinite(request.DurationMilliseconds));
				CHECK(request.DurationMilliseconds >= 0.0);
				CHECK_FALSE(request.Client.empty());
				CHECK(ids.insert(request.RequestId).second);
			}
		}

		TEST_CASE("EditorAutomationControls: a listener shutdown failure restores preference bytes and retains clients")
		{
			Test::EditorTestFixture fixture("ControlsStopRollback");
			auto specification = Test::MakeTestServerSpecification();
			specification.SessionsDirectory = fixture.GetDirectory() / "Sessions";
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			EditorAutomationControls controls(fixture.GetEditor(), client.GetServer());
			REQUIRE(controls.SetAllowAiAutomation(true));
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			const auto previous = fixture.GetEditor().GetVfs().ReadText(*path);
			REQUIRE(previous);
			const auto file = SessionFile::GetPath(specification.SessionsDirectory, Process::GetCurrentId());
			const uint16_t port = client.GetServer().GetPort();
			REQUIRE(FileSystem::Remove(file));
			REQUIRE(FileSystem::CreateDirectories(file));
			REQUIRE(FileSystem::WriteFileAtomic(file / "Keep", {}));
			const auto denied = controls.SetAllowAiAutomation(false);
			REQUIRE_FALSE(denied);
			CHECK(denied.error().GetCode() == ErrorCode::Io);
			CHECK(client.GetServer().GetPort() == port);
			CHECK(fixture.GetEditor().GetVfs().ReadText(*path).value_or("") == *previous);
			CHECK(FileSystem::Exists(file / "Keep"));
			CHECK(client.Call("session.info", Json::object()));
			REQUIRE(FileSystem::Remove(file / "Keep"));
			REQUIRE(FileSystem::Remove(file));
			REQUIRE(controls.SetAllowAiAutomation(false));
			CHECK(client.GetServer().GetPort() == 0);
			CHECK(client.Call("session.info", Json::object()));
		}

		TEST_CASE("EditorAutomationControls: a listener startup failure restores preference bytes")
		{
			Test::EditorTestFixture fixture("ControlsRollback");
			auto specification = Test::MakeTestServerSpecification();
			specification.SessionsDirectory = fixture.GetDirectory() / "Sessions";
			REQUIRE(FileSystem::WriteFileAtomic(specification.SessionsDirectory, {}));
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			EditorAutomationControls controls(fixture.GetEditor(), client.GetServer());
			VirtualFileSystem& vfs = fixture.GetEditor().GetVfs();
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			const std::string original = R"({"Format":"EditorPreferences","Version":1,"Future":7,"AllowAiAutomation":false})";
			REQUIRE(vfs.WriteFileAtomic(*path, std::as_bytes(std::span(original.data(), original.size()))));
			const Status failed = controls.SetAllowAiAutomation(true);
			REQUIRE_FALSE(failed);
			CHECK(failed.error().GetCode() == ErrorCode::Io);
			CHECK(client.GetServer().GetPort() == 0);
			const auto restored = vfs.ReadText(*path);
			REQUIRE(restored);
			CHECK(*restored == original);
			REQUIRE(vfs.Remove(*path));
			const Status missingFailed = controls.SetAllowAiAutomation(true);
			REQUIRE_FALSE(missingFailed);
			CHECK(missingFailed.error().GetCode() == ErrorCode::Io);
			CHECK_FALSE(vfs.Exists(*path));
		}
	}

}
