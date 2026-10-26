#include "TestsPCH.h"
#include "EditorCore/EditorPreferences.h"

#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/ChildProcess.h"
#include "Support/Fuzz.h"
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
			CHECK_FALSE(preferences->AllowNativeCode);
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

		// Docs/Decisions/0030-m19-contract.md decision 29 (N-C): the user's Allow native code preference is read and written with
		// the others, absent means off, and the file's other writers (the AI automation preference, the recent projects) keep
		// it as the file holds it.
		TEST_CASE("EditorPreferences: Allow native code is read and written, and the file's other writers keep it")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			const std::string original = R"({"Format":"EditorPreferences","Version":1,"AllowNativeCode":true,"Future":{"Kept":[1,2]}})";
			REQUIRE(vfs.WriteFileAtomic(*path, std::as_bytes(std::span(original.data(), original.size()))));
			const auto read = ReadEditorPreferences(vfs);
			REQUIRE(read);
			CHECK(read->AllowNativeCode);
			CHECK_FALSE(read->AllowAiAutomation);

			// The AI automation preference's writer reads the file first, so the native preference survives it.
			EditorPreferences automation = *read;
			automation.AllowAiAutomation = true;
			REQUIRE(WriteEditorPreferences(vfs, automation));
			REQUIRE(ProjectManager::AddRecentProject(vfs, std::filesystem::path("D:/Games/Native/Native.eproj")));
			const auto kept = ReadEditorPreferences(vfs);
			REQUIRE(kept);
			CHECK(kept->AllowNativeCode);
			CHECK(kept->AllowAiAutomation);
			const auto text = vfs.ReadText(*path);
			REQUIRE(text);
			const auto document = JsonReader::Parse(*text);
			REQUIRE(document);
			const auto future = JsonReader::Parse(R"({"Kept":[1,2]})");
			REQUIRE(future);
			CHECK((*document)["Future"] == *future);

			EditorPreferences off = *kept;
			off.AllowNativeCode = false;
			REQUIRE(WriteEditorPreferences(vfs, off));
			const auto stored = ReadEditorPreferences(vfs);
			REQUIRE(stored);
			CHECK_FALSE(stored->AllowNativeCode);
			CHECK(stored->AllowAiAutomation);
		}

		TEST_CASE("EditorPreferences: the shared writer keeps every number exactly and rejects text that is not UTF-8")
		{
			const auto document = JsonReader::Parse(R"({"Format":"EditorPreferences","Version":1,"Future":{"Exact":9007199254740993,"Fraction":1.23456789012345,"Large":1e200,"Tiny":5e-324}})");
			REQUIRE(document);
			const auto text = SerializeEditorPreferences(*document);
			REQUIRE(text);
			CHECK(text->ends_with("\n"));
			const auto reparsed = JsonReader::Parse(*text);
			REQUIRE(reparsed);
			CHECK(*reparsed == *document);
			Json invalid = *document;
			invalid["Future"]["Name"] = std::string(1, static_cast<char>(0xff));
			const auto rejected = SerializeEditorPreferences(invalid);
			REQUIRE_FALSE(rejected);
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
			Json invalidKey = *document;
			invalidKey["Future"][std::string(1, static_cast<char>(0xfe))] = 1;
			CHECK(SerializeEditorPreferences(invalidKey).error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("EditorPreferences: 1,000 seeded mutations of Editor.json never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// user://Editor.json lives in a memory mount, so no iteration touches a file. The seeds: preferences the writer
			// stored with every member set, and a file of a newer editor with recent projects and unknown members.
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			const auto path = VfsPath::Parse("user://Editor.json");
			REQUIRE(path);
			EditorPreferences full;
			full.AllowAiAutomation = true;
			full.AllowNativeCode = true;
			full.ExternalEditorExecutable = "C:/Tools/Editor app.exe";
			full.ExternalEditorArguments = { "--goto", "{path}:{line}" };
			REQUIRE(WriteEditorPreferences(vfs, full));
			const auto written = vfs.ReadText(*path);
			REQUIRE(written);
			const std::array<std::string, 2> seeds = { *written,
				R"({"Format":"EditorPreferences","Version":1,"RecentProjects":["D:/A.eproj","/space name/B.eproj"],"Future":{"Exact":9007199254740993,"Fraction":1.23456789012345,"Large":1e200,"Array":[null,false,"x"]}})" };
			const Test::FuzzRun run = Test::RunFuzzCase(0xED17, 1000, [&vfs, &path, &seeds](Random& random, uint64_t /*index*/)
			{
				std::string mutated = seeds[static_cast<size_t>(random.RangeInt(0, 1))];
				Test::MutateText(mutated, random);
				REQUIRE(vfs.WriteFileAtomic(*path, std::as_bytes(std::span(mutated.data(), mutated.size()))));
				const auto loaded = ReadEditorPreferences(vfs);
				if (!loaded)
				{
					const ErrorCode code = loaded.error().GetCode();
					INFO("error: ", loaded.error().ToString());
					CHECK((code == ErrorCode::Parse || code == ErrorCode::Validation || code == ErrorCode::UnsupportedVersion));
					return false;
				}
				// Preferences that read write back and read again unchanged.
				REQUIRE(WriteEditorPreferences(vfs, *loaded));
				const auto reread = ReadEditorPreferences(vfs);
				REQUIRE(reread);
				CHECK(reread->AllowAiAutomation == loaded->AllowAiAutomation);
				CHECK(reread->AllowNativeCode == loaded->AllowNativeCode);
				CHECK(reread->ExternalEditorExecutable == loaded->ExternalEditorExecutable);
				CHECK(reread->ExternalEditorArguments == loaded->ExternalEditorArguments);
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
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
					 R"({"Format":"EditorPreferences","Version":1,"AllowNativeCode":1})",
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
			// An external editor's output is never kept (it may run for hours); this test captures it to see the argv.
			CHECK_FALSE(specification->CaptureOutput);
			ProcessSpecification observed = *specification;
			observed.CaptureOutput = true;
			const auto child = Process::Run(observed, std::chrono::seconds(60));
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
