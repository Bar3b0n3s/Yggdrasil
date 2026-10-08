#include "TestsPCH.h"

#include "EditorCore/Export/Exporter.h"

#include "EditorCore/EditorContext.h"
#include "Support/EditorTestFixture.h"
#include "Support/TempDirectory.h"
#include "Support/WaitUntil.h"

#include <optional>
#include <string>

// The exporter v0 (Architecture §14.1, §14.2). Skipped skeletons of the M7 contract (Docs/Decisions/0012-m7-decisions.md
// decision 14): stream D implements the exporter and removes the skips. The end-to-end exports (paks, executable, CRT,
// determinism, the smoke test per configuration) are tested by Tests/Automation/test_export.py against the built Runtime.

namespace Engine {

	namespace {

		// Polls `exporter` until it resolves. Cooking runs on jobs, so the wait is bounded by Test::WaitUntil's deadline, a
		// failure bound only (a spin count would depend on the machine's speed); nullopt when it did not resolve by then.
		std::optional<Result<ExportReport>> RunToEnd(Exporter& exporter)
		{
			std::optional<Result<ExportReport>> outcome;
			const bool resolved = Test::WaitUntil([&exporter, &outcome]()
			{
				outcome = exporter.Poll();
				return outcome.has_value();
			});
			if (!resolved)
				return std::nullopt;
			return outcome;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("Exporter: names the default output directory after the platform, configuration and project" * doctest::skip(true))
		{
			const std::string directory = Exporter::GetDefaultOutputDirectory("Tetris", ExportConfiguration::Dist);
			CHECK(directory == std::string("Build/") + std::string(Exporter::GetPlatformName()) + "-Dist/Tetris");
#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK(Exporter::GetPlatformName() == "Windows");
			CHECK(Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Release) == "Release-windows-x86_64");
#endif
		}

		TEST_CASE("Exporter: the specification is checked when the export starts" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			ExportSpecification specification;
			specification.BinaryRoot = fixture.GetProjectRoot();
			specification.OutputDirectory = "Assets/Out";
			const Result<Scope<Exporter>> outside = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_FALSE(outside.has_value());
			CHECK(outside.error().GetCode() == ErrorCode::InvalidArgument);

			specification.OutputDirectory.clear();
			specification.BinaryRoot.clear();
			const Result<Scope<Exporter>> noBinaries = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_FALSE(noBinaries.has_value());
			CHECK(noBinaries.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("Exporter: a project without a start scene fails validation and writes nothing" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			ExportSpecification specification;
			specification.BinaryRoot = fixture.GetProjectRoot();
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			CHECK(outcome->error().GetCode() == ErrorCode::Validation);
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", error));
		}

		TEST_CASE("Exporter: configurations and phases have their enumerator names")
		{
			// Implemented by the contract.
			CHECK(ExportConfigurationToString(ExportConfiguration::Debug) == "Debug");
			CHECK(ExportConfigurationToString(ExportConfiguration::Release) == "Release");
			CHECK(ExportConfigurationToString(ExportConfiguration::Dist) == "Dist");
			CHECK(ExportPhaseToString(ExportPhase::Validate) == "Validate");
			CHECK(ExportPhaseToString(ExportPhase::SmokeTest) == "SmokeTest");
			CHECK(ExportPhaseToString(ExportPhase::MoveToOutput) == "MoveToOutput");
			CHECK(ExportPhaseToString(ExportPhase::Done) == "Done");
		}
	}

}
