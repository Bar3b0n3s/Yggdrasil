#include "TestsPCH.h"

#include "Editor/Private/EditorHostPreferences.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Engine {

	static void WriteHostPreferencesText(VirtualFileSystem& vfs, std::string_view text)
	{
		const auto path = VfsPath::Parse("user://Editor.json");
		REQUIRE(path);
		REQUIRE(vfs.WriteFileAtomic(*path, AsBytes(text)));
	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorHostPreferences: a queued automation change applies at the safe point and publishes success")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			EditorHostPreferences preferences;
			REQUIRE(preferences.Load(vfs));
			CHECK_FALSE(preferences.GetAutomationState().Allowed);
			REQUIRE(preferences.QueueAutomationChange(true));
			CHECK(preferences.GetAutomationState().Pending);
			CHECK_FALSE(preferences.GetAutomationState().Allowed);
			CHECK(preferences.QueueAutomationChange(false).error().GetCode() == ErrorCode::InvalidState);
			std::vector<bool> applied;
			const auto apply = [&applied](bool allowed) -> Status
			{
				applied.push_back(allowed);
				return {};
			};
			REQUIRE(preferences.Pump(apply));
			CHECK(applied == std::vector<bool>{ true });
			const EditorAutomationPreferenceState state = preferences.GetAutomationState();
			CHECK(state.Allowed);
			CHECK_FALSE(state.Pending);
			CHECK_FALSE(state.Failure.has_value());
			CHECK(preferences.GetPreferences().AllowAiAutomation);
			REQUIRE(preferences.Pump(apply)); // nothing queued
			CHECK(applied.size() == 1);
		}

		TEST_CASE("EditorHostPreferences: a failed automation change clears pending and publishes the failure")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			EditorHostPreferences preferences;
			REQUIRE(preferences.Load(vfs));
			REQUIRE(preferences.QueueAutomationChange(true));
			const Status failed = preferences.Pump([](bool) -> Status
			{
				return MakeError(ErrorCode::Io, "injected listener failure");
			});
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::Io);
			EditorAutomationPreferenceState state = preferences.GetAutomationState();
			CHECK_FALSE(state.Pending); // the checkbox is usable again
			CHECK_FALSE(state.Allowed);
			REQUIRE(state.Failure.has_value());
			CHECK(state.Failure->GetCode() == ErrorCode::Io);
			CHECK_FALSE(preferences.GetPreferences().AllowAiAutomation);
			REQUIRE(preferences.QueueAutomationChange(true)); // retrying is allowed and clears the old failure
			state = preferences.GetAutomationState();
			CHECK(state.Pending);
			CHECK_FALSE(state.Failure.has_value());
		}

		TEST_CASE("EditorHostPreferences: malformed or newer preferences start with defaults and report the failure")
		{
			for (const std::string_view text : { "{broken", R"({"Format":"EditorPreferences","Version":2,"AllowAiAutomation":true})",
					 R"({"Format":"EditorPreferences","Version":1,"AllowAiAutomation":"yes"})" })
			{
				INFO(std::string(text));
				VirtualFileSystem vfs;
				REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
				WriteHostPreferencesText(vfs, text);
				EditorHostPreferences preferences;
				const Status loaded = preferences.Load(vfs);
				REQUIRE_FALSE(loaded.has_value());
				CHECK_FALSE(preferences.GetPreferences().AllowAiAutomation);
				CHECK(preferences.GetPreferences().ExternalEditorExecutable.empty());
				const EditorAutomationPreferenceState state = preferences.GetAutomationState();
				CHECK_FALSE(state.Allowed);
				CHECK(state.Failure.has_value());
				bool listened = true;
				REQUIRE(preferences.StartListening([&listened](bool allowed) -> Status
				{
					listened = allowed;
					return {};
				}));
				CHECK_FALSE(listened);
				const auto path = VfsPath::Parse("user://Editor.json");
				REQUIRE(path);
				CHECK(vfs.ReadText(*path).value_or(std::string()) == text); // left for the user to repair
			}
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			WriteHostPreferencesText(vfs, R"({"Format":"EditorPreferences","Version":1,"AllowAiAutomation":true})");
			EditorHostPreferences preferences;
			REQUIRE(preferences.Load(vfs));
			CHECK(preferences.GetAutomationState().Allowed);
			CHECK_FALSE(preferences.GetAutomationState().Failure.has_value());
		}

		TEST_CASE("EditorHostPreferences: a listener that cannot start leaves the preference off and reports why")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			WriteHostPreferencesText(vfs, R"({"Format":"EditorPreferences","Version":1,"AllowAiAutomation":true})");
			EditorHostPreferences preferences;
			REQUIRE(preferences.Load(vfs));
			const Status started = preferences.StartListening([](bool allowed) -> Status
			{
				CHECK(allowed);
				return MakeError(ErrorCode::Io, "cannot write the session file");
			});
			REQUIRE_FALSE(started.has_value());
			CHECK(started.error().GetCode() == ErrorCode::Io);
			const EditorAutomationPreferenceState state = preferences.GetAutomationState();
			CHECK_FALSE(state.Allowed);
			CHECK_FALSE(state.Pending);
			REQUIRE(state.Failure.has_value());
			CHECK(state.Failure->GetMessageText().contains("session file"));
		}

		TEST_CASE("EditorHostPreferences: a queued external editor is written at the safe point keeping the other members")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			const std::string_view original = R"({"Format":"EditorPreferences","Version":1,"AllowAiAutomation":true,"FutureMember":{"Precise":1e200}})";
			WriteHostPreferencesText(vfs, original);
			const auto before = JsonReader::Parse(original);
			REQUIRE(before);
			EditorHostPreferences preferences;
			REQUIRE(preferences.Load(vfs));
			CHECK(preferences.GetExternalEditorState().Executable.empty());
			REQUIRE(preferences.QueueExternalEditorChange("code", { "--goto", "{path}:{line}" }));
			// Queued only: the snapshot and the file keep the saved preference until the safe point.
			EditorExternalEditorState state = preferences.GetExternalEditorState();
			CHECK(state.Pending);
			CHECK(state.Executable.empty());
			CHECK(ReadEditorPreferences(vfs)->ExternalEditorExecutable.empty());
			CHECK(preferences.QueueExternalEditorChange("vim", {}).error().GetCode() == ErrorCode::InvalidState);
			REQUIRE(preferences.PumpExternalEditor(vfs));
			state = preferences.GetExternalEditorState();
			CHECK_FALSE(state.Pending);
			CHECK_FALSE(state.Failure.has_value());
			CHECK(state.Executable == "code");
			CHECK(state.Arguments == std::vector<std::string>{ "--goto", "{path}:{line}" });
			const Result<EditorPreferences> stored = ReadEditorPreferences(vfs);
			REQUIRE(stored);
			CHECK(stored->ExternalEditorExecutable == "code");
			CHECK(stored->ExternalEditorArguments == std::vector<std::string>{ "--goto", "{path}:{line}" });
			CHECK(stored->AllowAiAutomation);
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			const std::string text = vfs.ReadText(*path).value_or(std::string());
			const auto after = JsonReader::Parse(text);
			REQUIRE(after);
			CHECK((*after)["FutureMember"] == (*before)["FutureMember"]);
			REQUIRE(preferences.PumpExternalEditor(vfs)); // nothing queued
			CHECK(vfs.ReadText(*path).value_or(std::string()) == text);
			// An empty executable without arguments clears the preference.
			REQUIRE(preferences.QueueExternalEditorChange("", {}));
			REQUIRE(preferences.PumpExternalEditor(vfs));
			CHECK(preferences.GetExternalEditorState().Executable.empty());
			CHECK(ReadEditorPreferences(vfs)->ExternalEditorArguments.empty());
		}

		TEST_CASE("EditorHostPreferences: external editor changes are validated before they queue")
		{
			EditorHostPreferences preferences;
			const std::string withNul = std::string("co") + static_cast<char>(0) + "de";
			const std::string notUtf8 = std::string("code") + static_cast<char>(0xFF);
			for (const auto& [executable, arguments] : std::vector<std::pair<std::string, std::vector<std::string>>>{
					 { "", { "{path}" } },
					 { "code", { "--new-window" } },
					 { withNul, {} },
					 { notUtf8, {} },
					 { "code", { notUtf8, "{path}" } },
				 })
			{
				CAPTURE(executable);
				const Status queued = preferences.QueueExternalEditorChange(executable, arguments);
				REQUIRE_FALSE(queued.has_value());
				CHECK(queued.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK_FALSE(preferences.GetExternalEditorState().Pending);
			}
			CHECK(preferences.QueueExternalEditorChange("code", { "-g", "{path}:{line}" }));
		}

		TEST_CASE("EditorHostPreferences: a failed external editor write keeps the snapshot and publishes the failure")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			EditorHostPreferences preferences;
			REQUIRE(preferences.Load(vfs));
			WriteHostPreferencesText(vfs, "{broken");
			REQUIRE(preferences.QueueExternalEditorChange("code", {}));
			const Status failed = preferences.PumpExternalEditor(vfs);
			REQUIRE_FALSE(failed.has_value());
			EditorExternalEditorState state = preferences.GetExternalEditorState();
			CHECK_FALSE(state.Pending);
			CHECK(state.Executable.empty());
			REQUIRE(state.Failure.has_value());
			CHECK(state.Failure->GetCode() == failed.error().GetCode());
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			CHECK(vfs.ReadText(*path).value_or(std::string()) == "{broken"); // left for the user to repair
			WriteHostPreferencesText(vfs, R"({"Format":"EditorPreferences","Version":1})");
			REQUIRE(preferences.QueueExternalEditorChange("code", {})); // retrying clears the old failure
			CHECK_FALSE(preferences.GetExternalEditorState().Failure.has_value());
			REQUIRE(preferences.PumpExternalEditor(vfs));
			state = preferences.GetExternalEditorState();
			CHECK(state.Executable == "code");
			CHECK_FALSE(state.Failure.has_value());
		}

		TEST_CASE("EditorHostPreferences: recent projects are a snapshot that changes only when refreshed")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			EditorHostPreferences preferences;
			REQUIRE(preferences.GetRecentProjects().has_value());
			CHECK(preferences.GetRecentProjects()->empty());
			std::error_code error;
			const std::filesystem::path project = std::filesystem::temp_directory_path(error) / "EngineTests" / "Recent" / "Game.eproj";
			REQUIRE_FALSE(error);
			REQUIRE(ProjectManager::AddRecentProject(vfs, project));
			CHECK(preferences.GetRecentProjects()->empty());
			preferences.RefreshRecentProjects(vfs);
			REQUIRE(preferences.GetRecentProjects().has_value());
			CHECK(*preferences.GetRecentProjects() == std::vector<std::filesystem::path>{ project });
			WriteHostPreferencesText(vfs, "{broken");
			CHECK(*preferences.GetRecentProjects() == std::vector<std::filesystem::path>{ project });
			preferences.RefreshRecentProjects(vfs);
			REQUIRE_FALSE(preferences.GetRecentProjects().has_value());
			CHECK(preferences.GetRecentProjects().error().GetCode() == ErrorCode::Parse);
		}
	}

}
