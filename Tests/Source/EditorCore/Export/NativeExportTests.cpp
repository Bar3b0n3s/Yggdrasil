#include "TestsPCH.h"
#include "EditorCore/Export/NativeExport.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/FileSystem.h"
#include "Support/EditorTestFixture.h"
#include "Support/FakeNativeBackends.h"
#include "Support/TempDirectory.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		// Docs/Decisions/0030-m19-contract.md decision 33 (N-A): a native build that does not succeed fails the export with
		// CompileFailed, naming the game and its first error and located there, with every diagnostic an issue and the build's
		// log in the hint.
		TEST_CASE("NativeExport: a failed native build is CompileFailed naming its first error, with every diagnostic an issue")
		{
			NativeBuildOutcome outcome;
			outcome.Log = std::filesystem::path("Build") / "Build.log";
			outcome.Diagnostics = {
				NativeBuildDiagnostic{ .Severity = DiagnosticSeverity::Warning, .Code = "NATIVE_BUILD_ERROR", .File = {}, .Line = 0, .Column = 0, .Message = "MSB8029: a warning" },
				NativeBuildDiagnostic{ .Severity = DiagnosticSeverity::Error, .Code = "NATIVE_COMPILE_ERROR", .File = "Native/Spinner.cpp", .Line = 12, .Column = 9, .Message = "C2065: 'speed': undeclared identifier" },
				NativeBuildDiagnostic{ .Severity = DiagnosticSeverity::Error, .Code = "NATIVE_LINK_ERROR", .File = {}, .Line = 0, .Column = 0, .Message = "LNK2019: unresolved external symbol" },
			};
			const Error error = MakeNativeBuildError(outcome, "Spinners");
			CHECK(error.GetCode() == ErrorCode::CompileFailed);
			CHECK(error.GetMessageText().contains("'Spinners'"));
			CHECK(error.GetMessageText().contains("Native/Spinner.cpp:12:9"));
			CHECK(error.GetLocation().File == "Native/Spinner.cpp");
			CHECK(error.GetLocation().Line == 12);
			CHECK(error.GetLocation().Column == 9);
			CHECK(error.GetHint().contains("Build/Build.log"));
			REQUIRE(error.GetIssues().size() == outcome.Diagnostics.size());
			for (size_t index = 0; index < outcome.Diagnostics.size(); ++index)
			{
				CAPTURE(index);
				CHECK(error.GetIssues()[index].Message.contains(outcome.Diagnostics[index].Code));
				CHECK(error.GetIssues()[index].Message.contains(outcome.Diagnostics[index].Message));
			}

			// A build that failed without a diagnostic of its own still fails the export, saying so.
			const Error bare = MakeNativeBuildError(NativeBuildOutcome{}, "Spinners");
			CHECK(bare.GetCode() == ErrorCode::CompileFailed);
			CHECK(bare.GetIssues().empty());
		}

		// What outlives step 3a's build directory, which the export removes when it ends: a failed build's log, copied below the
		// project's Library/ so the error's hint names a file that exists, and the exported executable's PDB and linker map,
		// archived there (Architecture §2.2: archived, never shipped), each replacing the configuration's previous one.
		TEST_CASE("NativeExport: a failed build's log and an exported Runtime's symbols are kept below Library/, replacing the previous ones")
		{
			const Test::TempDirectory temporary("NativeExportKeep");
			const std::filesystem::path project = temporary.GetPath() / "Game";
			const std::filesystem::path build = temporary.GetPath() / "Build";
			REQUIRE(FileSystem::CreateDirectories(project).has_value());
			REQUIRE(FileSystem::CreateDirectories(build).has_value());
			const auto write = [](const std::filesystem::path& path, std::string_view text)
			{
				REQUIRE(FileSystem::WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size()))).has_value());
			};
			const auto read = [](const std::filesystem::path& path)
			{
				const Result<std::string> text = FileSystem::ReadText(path);
				REQUIRE(text.has_value());
				return *text;
			};

			NativeBuildOutcome outcome;
			outcome.Log = build / "Build.log";
			write(outcome.Log, "the whole output\n");
			const NativeBuildOutcome kept = KeepNativeBuildLog(project, ExportConfiguration::Dist, outcome);
			CHECK(kept.Log == GetNativeExportLogPath(project, ExportConfiguration::Dist));
			CHECK(kept.Log.parent_path().parent_path().parent_path() == project / "Library");
			CHECK(read(kept.Log) == "the whole output\n");
			CHECK(MakeNativeBuildError(kept, "Spinners").GetHint().contains(FileSystem::PathToUtf8(kept.Log)));
			// An outcome without a log, as a build that never ran gives, is returned as it is.
			CHECK(KeepNativeBuildLog(project, ExportConfiguration::Dist, NativeBuildOutcome{}).Log.empty());

			const std::filesystem::path executable = build / "Runtime.exe";
			write(executable, "executable");
			write(build / "Runtime.pdb", "first symbols");
			const Result<std::filesystem::path> missing = ArchiveNativeRuntimeSymbols(project, ExportConfiguration::Dist, executable);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(missing.error().GetMessageText().contains("Runtime.map"));

			write(build / "Runtime.map", "first map");
			const Result<std::filesystem::path> archived = ArchiveNativeRuntimeSymbols(project, ExportConfiguration::Dist, executable);
			REQUIRE_MESSAGE(archived.has_value(), archived.error().ToString());
			CHECK(*archived == GetNativeExportSymbolsDirectory(project, ExportConfiguration::Dist));
			CHECK(read(*archived / "Runtime.pdb") == "first symbols");
			CHECK(read(*archived / "Runtime.map") == "first map");
			CHECK_FALSE(FileSystem::Exists(*archived / "Runtime.exe"));

			// The next export's symbols replace them; another configuration's are its own.
			write(*archived / "Stale.pdb", "stale");
			write(build / "Runtime.pdb", "second symbols");
			REQUIRE(ArchiveNativeRuntimeSymbols(project, ExportConfiguration::Dist, executable).has_value());
			CHECK(read(*archived / "Runtime.pdb") == "second symbols");
			CHECK_FALSE(FileSystem::Exists(*archived / "Stale.pdb"));
			CHECK(GetNativeExportSymbolsDirectory(project, ExportConfiguration::Release) != *archived);
		}

		// Decision 33 (N-A): step 1's refusal of a game with Native/ comes before anything else is checked: Unsupported on Linux
		// and macOS, and on Windows PermissionDenied while the user's preference is off, even for a Native/ without sources.
		// Without Native/ there is nothing to refuse (the exporter never calls it then).
		TEST_CASE("NativeExport: a game with Native/ is refused while native code is not allowed, before its sources are read")
		{
			Test::EditorTestFixture fixture("NativeExport", {}, nullptr, std::nullopt, false,
				Test::EditorTestNativeBackends{ .Build = CreateRef<Test::FakeNativeBuildBackend>(), .Loader = CreateRef<Test::FakeNativeLibraryLoader>() });
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			REQUIRE(FileSystem::CreateDirectories(fixture.GetProjectRoot() / "Native").has_value());
			REQUIRE_FALSE(editor.GetNativeModule().IsAllowed());
			const Status refused = CheckNativeExport(editor);
			REQUIRE_FALSE(refused.has_value());
#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK(refused.error().GetCode() == ErrorCode::PermissionDenied);
			CHECK(refused.error().GetHint().contains("Allow native code"));
#else
			CHECK(refused.error().GetCode() == ErrorCode::Unsupported);
#endif
		}
	}

}
