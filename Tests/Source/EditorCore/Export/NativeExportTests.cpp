#include "TestsPCH.h"
#include "EditorCore/Export/NativeExport.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/FileSystem.h"
#include "Support/EditorTestFixture.h"
#include "Support/FakeNativeBackends.h"

#include <filesystem>
#include <string>
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
