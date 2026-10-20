#include "TestsPCH.h"
#include "EditorCore/Native/NativeBuild.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Support/Fuzz.h"
#include "Support/TempDirectory.h"
#include "Support/WaitUntil.h"

#include <array>
#include <chrono>
#include <format>
#include <optional>
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
		TEST_CASE("NativeBuild: the sources are the .cpp and .h files below Native/ and the source hash covers them in order")
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
			// The values Scripts/Lib/native_build.py's source_hash computes for the same files, which
			// Tests/Automation/test_build_native.py pins too: the editor and BuildNative.py agree on when a build is current.
			CHECK(*debug == "a3230069ed62f267");
			CHECK(*release == "6fddebbbd4d34c08");

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
		TEST_CASE("NativeBuild: a build record reads back what the build wrote and refuses anything else")
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

		// Decision 30 (N-A): the editor's backend runs Scripts/BuildNative.py by fixed paths with fixed arguments and resolves
		// to the record it writes, also for a build that cannot start (a Native/ without a source: a NATIVE_BUILD_ERROR, not
		// an error); a cancelled build resolves as Cancelled. Linux and macOS start no build (decision 34).
		TEST_CASE("NativeBuild: the editor's backend runs Scripts/BuildNative.py and resolves to the record it writes")
		{
			const Scope<INativeBuildBackend> backend = CreateNativeBuildBackend();
			const Test::TempDirectory directory("NativeBuild");
			const std::filesystem::path project = directory / "Game";
			REQUIRE(FileSystem::CreateDirectories(project / "Native").has_value());
			const NativeBuildRequest request{
				.ProjectRoot = project,
				.Kind = NativeBuildKind::Module,
				.Configuration = ExportConfiguration::Debug,
				.OutputDirectory = directory / "Build",
			};
#if defined(ENGINE_PLATFORM_WINDOWS)
			NativeBuildRequest relative = request;
			relative.ProjectRoot = "Game";
			const Result<Scope<INativeBuildJob>> refused = backend->Start(relative);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);

			Result<Scope<INativeBuildJob>> job = backend->Start(request);
			REQUIRE_MESSAGE(job.has_value(), job.error().ToString());
			std::optional<Result<NativeBuildOutcome>> outcome;
			REQUIRE(Test::WaitUntil([&job, &outcome]()
			{
				outcome = (*job)->Poll();
				return outcome.has_value();
			}, std::chrono::seconds(120)));
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			const NativeBuildOutcome& built = **outcome;
			CHECK_FALSE(built.Succeeded);
			CHECK(built.Output.empty());
			CHECK(built.SourceHash.size() == 16);
			REQUIRE(built.Diagnostics.size() == 1);
			CHECK(built.Diagnostics[0].Code == "NATIVE_BUILD_ERROR");
			CHECK(built.Diagnostics[0].Severity == DiagnosticSeverity::Error);
			CHECK(FileSystem::Exists(built.Log));
			const std::optional<Result<NativeBuildOutcome>> again = (*job)->Poll();
			REQUIRE(again.has_value());
			REQUIRE(again->has_value());
			CHECK((*again)->Diagnostics.size() == built.Diagnostics.size());

			NativeBuildRequest cancelledRequest = request;
			cancelledRequest.OutputDirectory = directory / "Cancelled";
			Result<Scope<INativeBuildJob>> cancelled = backend->Start(cancelledRequest);
			REQUIRE_MESSAGE(cancelled.has_value(), cancelled.error().ToString());
			(*cancelled)->Cancel();
			const std::optional<Result<NativeBuildOutcome>> stopped = (*cancelled)->Poll();
			REQUIRE(stopped.has_value());
			REQUIRE_FALSE(stopped->has_value());
			CHECK(stopped->error().GetCode() == ErrorCode::Cancelled);
#else
			const Result<Scope<INativeBuildJob>> unsupported = backend->Start(request);
			REQUIRE_FALSE(unsupported.has_value());
			CHECK(unsupported.error().GetCode() == ErrorCode::Unsupported);
#endif
		}

		// Decision 30 (N-A): the record is a file the editor reads, so it has a fuzz target (NativeBuildRecord in
		// Tests/Data/Fuzz/Targets.json, added by the W19.1 integration).
		TEST_CASE("NativeBuild: 1,000 seeded mutations of a build record never crash" * doctest::test_suite(Test::FuzzSuite))
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
