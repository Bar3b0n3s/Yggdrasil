#include "TestsPCH.h"

#include "Editor/Private/EditorHostSource.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Socket.h"
#include "Support/DeathTest.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"

#include <charconv>
#include <optional>
#include <string>

namespace Engine {

	// An external source editor stand-in: connects to the test's listener (port in --child-argument), sends its process
	// ID, then stays open until the test closes the connection.
	ENGINE_DEATH_TEST("Editor/ExternalEditorStandIn")
	{
		const std::string& text = Test::GetTestOptions().ChildArgument;
		uint16_t port = 0;
		const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), port);
		if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
		{
			ENGINE_CORE_ERROR("The external editor stand-in needs a port as --child-argument");
			return;
		}
		Result<Socket> connection = Socket::Connect(port, std::chrono::seconds(30));
		if (!connection.has_value())
		{
			ENGINE_CORE_ERROR("The external editor stand-in cannot connect: {}", connection.error());
			return;
		}
		const std::string id = std::to_string(Process::GetCurrentId()) + "\n";
		if (!connection->Send(std::as_bytes(std::span(id.data(), id.size())), std::chrono::seconds(30)).has_value())
			return;
		std::array<std::byte, 1> buffer{};
		static_cast<void>(connection->Receive(buffer, std::chrono::seconds(60)));
	}

	// The source file and preferences every launcher test starts from.
	struct SourceLaunchSetup
	{
		Test::TempDirectory Directory{ "HostSourceLaunch" };
		std::filesystem::path Project = Directory / "Project";
		std::filesystem::path Repository = Directory / "Repository";
		std::filesystem::path Script = Project / "Assets/Scripts/Game.luau";

		SourceLaunchSetup()
		{
			REQUIRE(FileSystem::CreateDirectories(Script.parent_path()).has_value());
			REQUIRE(FileSystem::CreateDirectories(Repository / "Resources").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(Script, {}).has_value());
		}

		[[nodiscard]] EditorSourceRequest Request(EditorPreferences preferences) const
		{
			return { .Source = "project://Assets/Scripts/Game.luau", .Line = 7, .ProjectRoot = Project, .RepositoryRoot = Repository, .Preferences = std::move(preferences), .PreferencesFile = Directory / "UserData" / "Editor.json" };
		}

		// The Tests executable as the external editor, running `deathTest` with --child-argument=`argument`.
		[[nodiscard]] static EditorPreferences TestsEditor(std::string_view deathTest, const std::string& argument)
		{
			EditorPreferences preferences;
			preferences.ExternalEditorExecutable = FileSystem::PathToUtf8(Test::GetTestOptions().ExecutablePath);
			preferences.ExternalEditorArguments = { std::format("--death-test={}", deathTest), "--child-argument=" + argument,
				Test::ChildProcessOption, "--source={path}:{line}" };
			return preferences;
		}
	};

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorHostSource: Console links resolve project URIs and engine source without launching a shell")
		{
			Test::TempDirectory directory("HostSource");
			const auto project = directory / "Project";
			const auto repository = directory / "Repository";
			REQUIRE(FileSystem::CreateDirectories(project / "Assets/Scripts").has_value());
			REQUIRE(FileSystem::CreateDirectories(repository / "Engine/Source").has_value());
			REQUIRE(FileSystem::CreateDirectories(repository / "Resources/Scripting").has_value());
			const auto script = project / "Assets/Scripts/Game {line}.luau";
			const auto cpp = repository / "Engine/Source/App.cpp";
			const auto builtin = repository / "Resources/Scripting/Engine.d.luau";
			for (const auto& file : { script, cpp, builtin })
				REQUIRE(FileSystem::WriteFileAtomic(file, {}).has_value());
			const auto check = [&project, &repository](const std::filesystem::path& input, const std::filesystem::path& expected)
			{
				const auto result = ResolveEditorSourcePath(input, project, repository);
				REQUIRE(result.has_value());
				std::error_code error;
				CHECK(std::filesystem::equivalent(*result, expected, error));
				CHECK_FALSE(error);
			};
			check("project://Assets/Scripts/Game {line}.luau", script);
			check("project:/Assets/Scripts/Game {line}.luau", script);
			check("Assets/Scripts/Game {line}.luau", script);
			check(script, script);
			check("Engine/Source/App.cpp", cpp);
			check(cpp, cpp);
			check("engine://Scripting/Engine.d.luau", builtin);
			CHECK(ResolveEditorSourcePath(cpp, {}, repository).has_value());
			const auto noProject = ResolveEditorSourcePath("project://Assets/Scripts/Game.luau", {}, repository);
			REQUIRE_FALSE(noProject.has_value());
			CHECK(noProject.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("EditorHostSource: missing files directories and mount escapes are rejected")
		{
			Test::TempDirectory directory("HostSourceErrors");
			const auto project = directory / "Project";
			const auto repository = directory / "Repository";
			REQUIRE(FileSystem::CreateDirectories(project).has_value());
			REQUIRE(FileSystem::CreateDirectories(repository / "Resources").has_value());
			const auto outside = directory / "outside.luau";
			REQUIRE(FileSystem::WriteFileAtomic(outside, {}).has_value());
			const auto check = [&project, &repository](const std::filesystem::path& input, ErrorCode expected)
			{
				const auto result = ResolveEditorSourcePath(input, project, repository);
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == expected);
			};
			check("project://../outside.luau", ErrorCode::PermissionDenied);
			check("engine://../../outside.luau", ErrorCode::PermissionDenied);
			check(outside, ErrorCode::PermissionDenied);
			check("project://missing.luau", ErrorCode::NotFound);
			check("project://.", ErrorCode::InvalidArgument);
			check("cache://script.luau", ErrorCode::InvalidArgument);
			check("project:script.luau", ErrorCode::InvalidArgument);
			check({}, ErrorCode::InvalidArgument);
		}

		TEST_CASE("EditorHostSource: native paths keep colons that do not start a URI")
		{
			Test::TempDirectory directory("HostSourceColon");
			const auto repository = directory / "Repository";
			REQUIRE(FileSystem::CreateDirectories(repository / "Resources").has_value());
#if defined(ENGINE_PLATFORM_WINDOWS)
			// Windows names cannot hold ':'; an extended-length path has one after its "\\?\" prefix.
			const auto project = directory / "Project";
			const auto script = project / "Assets/Game.luau";
			REQUIRE(FileSystem::CreateDirectories(script.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(script, {}).has_value());
			std::error_code error;
			const auto absolute = std::filesystem::canonical(script, error);
			REQUIRE_FALSE(error);
			const std::filesystem::path input(L"\\\\?\\" + absolute.native());
#else
			// ':' is an ordinary character in POSIX names.
			const auto project = directory / "Games: 2026" / "Breakout";
			const auto script = project / "Assets/Game.luau";
			REQUIRE(FileSystem::CreateDirectories(script.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(script, {}).has_value());
			const std::filesystem::path input = script;
#endif
			const auto resolved = ResolveEditorSourcePath(input, project, repository);
			REQUIRE_MESSAGE(resolved.has_value(), resolved.error().ToString());
			std::error_code same;
			CHECK(std::filesystem::equivalent(*resolved, script, same));
			CHECK_FALSE(same);
			// A two-letter scheme is still a URI, and an unknown one stays unsupported.
			const auto scheme = ResolveEditorSourcePath("ab://Game.luau", project, repository);
			REQUIRE_FALSE(scheme.has_value());
			CHECK(scheme.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("EditorSourceLauncher: configuration and path failures reach the caller before any launch")
		{
			SourceLaunchSetup setup;
			EditorSourceLauncher launcher;
			const Status unconfigured = launcher.Queue(setup.Request({}));
			REQUIRE_FALSE(unconfigured.has_value());
			CHECK(unconfigured.error().GetCode() == ErrorCode::Unsupported);
			CHECK(unconfigured.error().GetMessageText().contains("ExternalEditorExecutable"));
			CHECK(unconfigured.error().GetMessageText().contains(FileSystem::PathToUtf8(setup.Directory / "UserData" / "Editor.json")));

			EditorPreferences preferences = SourceLaunchSetup::TestsEditor("Support/ReturnsWithoutDying", "0");
			EditorSourceRequest missingSource = setup.Request(preferences);
			missingSource.Source = "project://Assets/Scripts/Missing.luau";
			CHECK(launcher.Queue(missingSource).error().GetCode() == ErrorCode::NotFound);

			EditorPreferences missingEditor = preferences;
			missingEditor.ExternalEditorExecutable = FileSystem::PathToUtf8(setup.Directory / "NoSuchEditor.exe");
			const Status absent = launcher.Queue(setup.Request(missingEditor));
			REQUIRE_FALSE(absent.has_value());
			CHECK(absent.error().GetCode() == ErrorCode::NotFound);

			EditorPreferences noPath = preferences;
			noPath.ExternalEditorArguments = { "--line={line}" };
			CHECK(launcher.Queue(setup.Request(noPath)).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(launcher.GetQueuedCount() == 0);
			REQUIRE(launcher.Pump().has_value());
			CHECK(launcher.GetRunningCount() == 0);
		}

		TEST_CASE("EditorSourceLauncher: a failed launch never drops the other queued launches")
		{
			SourceLaunchSetup setup;
			EditorSourceLauncher launcher;
			// Valid when queued, gone before the safe point starts it.
			const auto vanishing = setup.Directory / "Vanishing.exe";
			REQUIRE(FileSystem::WriteFileAtomic(vanishing, AsBytes("not a program")).has_value());
			EditorPreferences gone = SourceLaunchSetup::TestsEditor("Support/ReturnsWithoutDying", "0");
			gone.ExternalEditorExecutable = FileSystem::PathToUtf8(vanishing);
			REQUIRE(launcher.Queue(setup.Request(gone)).has_value());
			REQUIRE(launcher.Queue(setup.Request(SourceLaunchSetup::TestsEditor("Support/ReturnsWithoutDying", "0"))).has_value());
			CHECK(launcher.GetQueuedCount() == 2);
			REQUIRE(FileSystem::Remove(vanishing).has_value());
			const Status pumped = launcher.Pump();
			REQUIRE_FALSE(pumped.has_value());
			CHECK(pumped.error().GetCode() == ErrorCode::NotFound);
			CHECK(launcher.GetQueuedCount() == 0);
			CHECK(launcher.GetRunningCount() == 1); // the second launch started
		}

		TEST_CASE("EditorSourceLauncher: an external editor keeps running after the launcher is destroyed")
		{
			SourceLaunchSetup setup;
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE_MESSAGE(listener.has_value(), listener.error().ToString());
			std::optional<EditorSourceLauncher> launcher;
			launcher.emplace();
			REQUIRE(launcher->Queue(setup.Request(SourceLaunchSetup::TestsEditor("Editor/ExternalEditorStandIn", std::to_string(listener->GetPort())))).has_value());
			REQUIRE(launcher->Pump().has_value());
			CHECK(launcher->GetRunningCount() == 1);
			Result<Socket> connection = listener->Accept(std::chrono::seconds(60));
			REQUIRE_MESSAGE(connection.has_value(), connection.error().ToString());
			std::string received;
			while (!received.ends_with('\n'))
			{
				std::array<std::byte, 32> buffer{};
				const Result<size_t> count = connection->Receive(buffer, std::chrono::seconds(60));
				REQUIRE(count.has_value());
				REQUIRE(*count != 0);
				for (size_t index = 0; index < *count; ++index)
					received.push_back(static_cast<char>(buffer[index]));
			}
			uint32_t id = 0;
			const auto parsed = std::from_chars(received.data(), received.data() + received.size() - 1, id);
			REQUIRE(parsed.ec == std::errc());
			// Closing the engine editor destroys its launcher: the user's editor and its unsaved work survive.
			launcher.reset();
			CHECK(Process::IsRunning(id));
			connection->Close(); // the stand-in exits on its own
		}
	}

}
