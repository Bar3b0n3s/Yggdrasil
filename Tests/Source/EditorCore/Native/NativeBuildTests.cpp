#include "TestsPCH.h"
#include "EditorCore/Native/NativeBuild.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Support/Fuzz.h"
#include "Support/TempDirectory.h"

#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		// A record as Scripts/BuildNative.py writes it (decision 30).
		constexpr std::string_view RecordText = R"({
	"Format": "NativeBuild",
	"Version": 1,
	"Kind": "Module",
	"Configuration": "Debug",
	"SourceHash": "0123456789abcdef",
	"Sources": ["Native/Spinner.cpp", "Native/Spinner.h"],
	"Succeeded": false,
	"Output": "",
	"Log": "Build.log",
	"Diagnostics": [
		{
			"Severity": "Error",
			"Code": "NATIVE_COMPILE_ERROR",
			"File": "Native/Spinner.cpp",
			"Line": 3,
			"Column": 5,
			"Message": "an error"
		}
	]
}
)";

		void WriteFixtureFile(const std::filesystem::path& path, std::string_view text)
		{
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size()))).has_value());
		}

		// Decision 30's formula, written out independently of ComputeNativeSourceHash.
		[[nodiscard]] std::string ExpectedSourceHash(std::string_view configuration, std::span<const std::pair<std::string, std::string>> files)
		{
			XXH64Hasher hasher(0);
			hasher.Update(std::format("EngineNativeSources 1\n{}\n", configuration));
			for (const auto& [path, bytes] : files)
			{
				hasher.Update(path);
				hasher.Update("\n");
				hasher.UpdateU64(bytes.size());
				hasher.Update(bytes);
			}
			return std::format("{:016x}", hasher.Digest());
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Docs/Decisions/0030-m19-contract.md decision 26: a game is a native game when it has a Native/ directory. The contract
		// implements the check: live.
		TEST_CASE("NativeBuild: a project is a game with a native module when it has a Native/ directory")
		{
			const Test::TempDirectory directory("NativeBuild");
			CHECK_FALSE(HasNativeModuleDirectory(directory.GetPath()));
			WriteFixtureFile(directory / "Native.txt", "a file, not the directory");
			CHECK_FALSE(HasNativeModuleDirectory(directory.GetPath()));
			REQUIRE(FileSystem::CreateDirectories(directory / "Native").has_value());
			CHECK(HasNativeModuleDirectory(directory.GetPath()));
			CHECK_FALSE(HasNativeModuleDirectory(directory / "Missing"));
		}

		// Decisions 26 and 30 (N-A): the sources are every .cpp and .h below Native/, project-relative, in byte order, and the
		// source hash covers the configuration, NativeApi.h and each source's path and bytes, as Scripts/Lib/native_build.py's.
		TEST_CASE("NativeBuild: the sources are the .cpp and .h files below Native/ and the source hash covers them in order" * doctest::skip(true))
		{
			const Test::TempDirectory directory("NativeBuild");
			const std::filesystem::path project = directory / "Game";
			WriteFixtureFile(project / "Native" / "Spinner.cpp", "#include \"Engine/Native/NativeApi.h\"\n");
			WriteFixtureFile(project / "Native" / "Spinner.h", "#pragma once\n");
			WriteFixtureFile(project / "Native" / "Systems" / "Move.cpp", "// move\n");
			WriteFixtureFile(project / "Native" / "Notes.txt", "not a source\n");
			WriteFixtureFile(project / "Assets" / "Other.cpp", "not below Native/\n");
			const std::filesystem::path header = directory / "NativeApi.h";
			WriteFixtureFile(header, "// the interface\n");

			const Result<std::vector<std::string>> sources = ListNativeSources(project);
			REQUIRE_MESSAGE(sources.has_value(), sources.error().ToString());
			CHECK(*sources == std::vector<std::string>{ "Native/Spinner.cpp", "Native/Spinner.h", "Native/Systems/Move.cpp" });

			const std::array<std::pair<std::string, std::string>, 4> files = { std::pair<std::string, std::string>{ "Engine/Native/NativeApi.h", "// the interface\n" },
				std::pair<std::string, std::string>{ "Native/Spinner.cpp", "#include \"Engine/Native/NativeApi.h\"\n" },
				std::pair<std::string, std::string>{ "Native/Spinner.h", "#pragma once\n" }, std::pair<std::string, std::string>{ "Native/Systems/Move.cpp", "// move\n" } };
			const Result<std::string> debug = ComputeNativeSourceHash(project, ExportConfiguration::Debug, header);
			REQUIRE_MESSAGE(debug.has_value(), debug.error().ToString());
			CHECK(*debug == ExpectedSourceHash("Debug", files));
			const Result<std::string> release = ComputeNativeSourceHash(project, ExportConfiguration::Release, header);
			REQUIRE(release.has_value());
			CHECK(*release == ExpectedSourceHash("Release", files));
			CHECK(*release != *debug);

			WriteFixtureFile(project / "Native" / "Spinner.h", "#pragma once\n// changed\n");
			const Result<std::string> changed = ComputeNativeSourceHash(project, ExportConfiguration::Debug, header);
			REQUIRE(changed.has_value());
			CHECK(*changed != *debug);

			const Result<std::string> missingHeader = ComputeNativeSourceHash(project, ExportConfiguration::Debug, directory / "Missing.h");
			REQUIRE_FALSE(missingHeader.has_value());
			CHECK(missingHeader.error().GetCode() == ErrorCode::NotFound);
			const Result<std::vector<std::string>> none = ListNativeSources(directory / "NoModule");
			REQUIRE(none.has_value());
			CHECK(none->empty());
		}

		// Decision 30 (N-A): the build record reads strictly, what the build wrote and nothing else.
		TEST_CASE("NativeBuild: a build record reads back what the build wrote and refuses anything else" * doctest::skip(true))
		{
			const Result<NativeBuildRecord> record = ParseNativeBuildRecord(RecordText);
			REQUIRE_MESSAGE(record.has_value(), record.error().ToString());
			CHECK(record->Kind == NativeBuildKind::Module);
			CHECK(record->Configuration == ExportConfiguration::Debug);
			CHECK(record->SourceHash == "0123456789abcdef");
			CHECK(record->Sources == std::vector<std::string>{ "Native/Spinner.cpp", "Native/Spinner.h" });
			CHECK_FALSE(record->Succeeded);
			CHECK(record->Log == "Build.log");
			REQUIRE(record->Diagnostics.size() == 1);
			CHECK(record->Diagnostics[0].Severity == DiagnosticSeverity::Error);
			CHECK(record->Diagnostics[0].Code == "NATIVE_COMPILE_ERROR");
			CHECK(record->Diagnostics[0].Line == 3);

			std::string unknown(RecordText);
			unknown.replace(unknown.find("\"Log\""), 5, "\"Logs\"");
			const Result<NativeBuildRecord> refused = ParseNativeBuildRecord(unknown);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
			std::string succeeded(RecordText);
			succeeded.replace(succeeded.find("false"), 5, "true");
			CHECK_FALSE(ParseNativeBuildRecord(succeeded).has_value());
			CHECK(ParseNativeBuildRecord("{").error().GetCode() == ErrorCode::Parse);
		}

		// Decision 30 (N-A): the record is a file the editor reads, so it has a fuzz target (NativeBuildRecord in
		// Tests/Data/Fuzz/Targets.json, added by the W19.1 integration).
		TEST_CASE("NativeBuild: 1,000 seeded mutations of a build record never crash" * doctest::test_suite(Test::FuzzSuite) * doctest::skip(true))
		{
			const Test::FuzzRun run = Test::RunFuzzCase(0x4E42A, 1000, [](Random& random, uint64_t /*index*/)
			{
				std::string mutated(RecordText);
				Test::MutateText(mutated, random);
				const Result<NativeBuildRecord> record = ParseNativeBuildRecord(mutated);
				if (!record.has_value())
				{
					CHECK((record.error().GetCode() == ErrorCode::Parse || record.error().GetCode() == ErrorCode::Validation));
					return false;
				}
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}
	}

}
