#include "TestsPCH.h"
#include "EditorCore/EditorPreferences.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/ChildProcess.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorPreferences: missing preferences use defaults")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			const auto preferences = ReadEditorPreferences(vfs);
			REQUIRE(preferences);
			CHECK_FALSE(preferences->AllowAiAutomation);
			CHECK(preferences->ExternalEditorExecutable.empty());
			CHECK(preferences->ExternalEditorArguments.empty());
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			CHECK_FALSE(vfs.Exists(*path));
			VirtualFileSystem unmounted;
			const Status write = WriteEditorPreferences(unmounted, {});
			REQUIRE_FALSE(write);
			CHECK(write.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("EditorPreferences: automation preference preserves recent projects and unknown fields")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			const std::string original = R"({"Format":"EditorPreferences","Version":1,"RecentProjects":["D:/A.eproj","/space name/B.eproj"],"Future":{"Exact":9007199254740993,"Fraction":1.23456789012345,"Large":1e200,"Array":[null,false,"x"]}})";
			REQUIRE(vfs.WriteFileAtomic(*path, std::as_bytes(std::span(original.data(), original.size()))));
			const auto before = JsonReader::Parse(original);
			REQUIRE(before);
			for (const bool allowed : { true, false })
			{
				EditorPreferences preferences;
				preferences.AllowAiAutomation = allowed;
				preferences.ExternalEditorExecutable = "C:/Tools/Editor app.exe";
				preferences.ExternalEditorArguments = { "--goto", "{path}:{line}", "$(literal) & ; \"quoted\"" };
				REQUIRE(WriteEditorPreferences(vfs, preferences));
				const auto loaded = ReadEditorPreferences(vfs);
				REQUIRE(loaded);
				CHECK(loaded->AllowAiAutomation == allowed);
				CHECK(loaded->ExternalEditorExecutable == preferences.ExternalEditorExecutable);
				CHECK(loaded->ExternalEditorArguments == preferences.ExternalEditorArguments);
				const auto text = vfs.ReadText(*path);
				REQUIRE(text);
				const auto after = JsonReader::Parse(*text);
				REQUIRE(after);
				CHECK((*after)["RecentProjects"] == (*before)["RecentProjects"]);
				CHECK((*after)["Future"] == (*before)["Future"]);
				REQUIRE(WriteEditorPreferences(vfs, preferences));
				const auto repeated = vfs.ReadText(*path);
				REQUIRE(repeated);
				CHECK(*repeated == *text);
			}
		}

		TEST_CASE("EditorPreferences: malformed preferences do not get overwritten")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			for (const std::string text : {
					 "{broken", "[]", R"({"Format":"Other","Version":1})", R"({"Format":"EditorPreferences","Version":2})",
					 R"({"Format":"EditorPreferences","Version":1,"RecentProjects":[1]})",
					 R"({"Format":"EditorPreferences","Version":1,"AllowAiAutomation":"true"})",
					 R"({"Format":"EditorPreferences","Version":1,"ExternalEditorExecutable":[]})",
					 R"({"Format":"EditorPreferences","Version":1,"ExternalEditorArguments":[true]})",
					 R"({"Format":"EditorPreferences","Version":1,"AllowAiAutomation":true,"AllowAiAutomation":false})" })
			{
				INFO(text);
				REQUIRE(vfs.WriteFileAtomic(*path, std::as_bytes(std::span(text.data(), text.size()))));
				CHECK_FALSE(ReadEditorPreferences(vfs));
				CHECK_FALSE(WriteEditorPreferences(vfs, EditorPreferences{ .AllowAiAutomation = true }));
				const auto unchanged = vfs.ReadText(*path);
				REQUIRE(unchanged);
				CHECK(*unchanged == text);
			}
		}

		TEST_CASE("EditorPreferences: a failed atomic write preserves previous preferences")
		{
			VirtualFileSystem vfs;
			auto mount = CreateScope<MemoryMount>();
			MemoryMount& memory = *mount;
			REQUIRE(vfs.Mount("user", std::move(mount)));
			REQUIRE(WriteEditorPreferences(vfs, {}));
			EditorPreferences invalid;
			invalid.ExternalEditorExecutable = std::string(1, static_cast<char>(0xff));
			const Status invalidUtf8 = WriteEditorPreferences(vfs, invalid);
			REQUIRE_FALSE(invalidUtf8);
			CHECK(invalidUtf8.error().GetCode() == ErrorCode::Validation);
			memory.SetAccess(MountAccess::ReadOnly);
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			const auto before = vfs.ReadText(*path);
			REQUIRE(before);
			const Status changed = WriteEditorPreferences(vfs, EditorPreferences{ .AllowAiAutomation = true });
			REQUIRE_FALSE(changed);
			CHECK(changed.error().GetCode() == ErrorCode::PermissionDenied);
			const auto after = vfs.ReadText(*path);
			REQUIRE(after);
			CHECK(*after == *before);
		}

		TEST_CASE("EditorPreferences: source editor arguments are passed without a shell")
		{
			Test::TempDirectory directory("SourceEditorArguments");
			EditorPreferences preferences;
			preferences.ExternalEditorExecutable = FileSystem::PathToUtf8(Test::GetTestOptions().ExecutablePath);
			preferences.ExternalEditorArguments = { "--child-process", "--no-skip", "--test-case=EditorPreferences: receives source argv", "--child-argument={path}:{line}" };
			const auto source = directory / "a & $(echo) {line}.cpp";
			const auto specification = BuildEditorSourceProcessSpecification(preferences, source, 42);
			REQUIRE(specification);
			REQUIRE(specification->Arguments.size() == preferences.ExternalEditorArguments.size());
			CHECK(specification->Arguments.back() == "--child-argument=" + FileSystem::PathToUtf8(source) + ":42");
			const auto child = Process::Run(*specification, std::chrono::seconds(60));
			REQUIRE(child);
			CHECK(child->ExitCode == 0);
			CHECK(child->StandardError.contains("Source argv: [" + FileSystem::PathToUtf8(source) + ":42]"));
			preferences.ExternalEditorArguments = { "--goto", "{path}:{line}", "--other={path}" };
			const auto zeroLine = BuildEditorSourceProcessSpecification(preferences, source, 0);
			REQUIRE(zeroLine);
			CHECK(zeroLine->Arguments[1] == FileSystem::PathToUtf8(source) + ":1");
			CHECK(zeroLine->Arguments[2] == "--other=" + FileSystem::PathToUtf8(source));
			preferences.ExternalEditorArguments.clear();
			const auto defaults = BuildEditorSourceProcessSpecification(preferences, source, 8);
			REQUIRE(defaults);
			CHECK(defaults->Arguments == std::vector<std::string>{ FileSystem::PathToUtf8(source) });
			preferences.ExternalEditorArguments = { "no file placeholder" };
			CHECK_FALSE(BuildEditorSourceProcessSpecification(preferences, source, 42));
			preferences.ExternalEditorArguments = { std::string("nul\0{path}", 10) };
			CHECK_FALSE(BuildEditorSourceProcessSpecification(preferences, source, 42));
			preferences.ExternalEditorExecutable.clear();
			CHECK_FALSE(BuildEditorSourceProcessSpecification(preferences, source, 42));
		}

		// Child target: the parent checks the exact bytes received by the real executable, including shell metacharacters.
		TEST_CASE("EditorPreferences: receives source argv" * doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			ENGINE_CORE_INFO("Source argv: [{}]", Test::GetTestOptions().ChildArgument);
		}
	}

}
