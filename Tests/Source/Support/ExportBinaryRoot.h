#pragma once

#include "EditorCore/Export/ExportConfiguration.h"
#include "Engine/Core/Base.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The build output an exporting test hands the exporter as ExportSpecification::BinaryRoot (Architecture §14.2 step 4;
// Docs/Decisions/0024-m15-contract.md decision 2, review finding 1). Every test that runs an export through Poll past
// Validate (ExporterTests, TestingExportTests, ExportResourcesTests, ExportMethodsTests) builds its bin directory here, so
// what M15 makes the exporter require of a build output (a real executable for PatchResources, the notices file, the macOS
// frameworks for Sign) is met in one place, from this build's real Runtime output as Scripts/Build.py leaves it, and a
// stream's tests keep passing when another stream's requirement lands. Implemented by the contract; stream A owns it
// afterwards.

namespace Engine {

	namespace Test {

		class TempDirectory;

		// What MakeExportBinaryRoot writes as Runtime/THIRD_PARTY_NOTICES.txt by default.
		inline constexpr std::string_view DefaultExportNotices = "Third-party notices of this test build\n";

		struct ExportBinaryRootOptions
		{
			// The configuration whose output directory name (Exporter::GetBuildOutputDirectoryName) the files are staged
			// under, whatever configuration this test binary was built in.
			ExportConfiguration Configuration = ExportConfiguration::Release;
			// The text of Runtime/THIRD_PARTY_NOTICES.txt; nullopt leaves the file out (the missing-notices case).
			std::optional<std::string> Notices = std::string(DefaultExportNotices);
		};

		// A bin directory as Scripts/Build.py leaves it, at <directory>/bin, holding <OutputDir>/Runtime/ with a copy of this
		// build's real Runtime executable under its own name (Test::GetBuiltExecutablePath("Runtime")) and a copy of every
		// directory Build.py stages beside it that this build has: Redist/ (Windows, required: the app-local CRT), Frameworks/
		// and Vulkan/ (macOS, from M15 stream A's Build.py). Then Runtime/THIRD_PARTY_NOTICES.txt from `options`. It writes no
		// Shaders/: each test writes the shader files it counts. Returns <directory>/bin. Fails the running test case (REQUIRE)
		// when the Runtime is not built or, on Windows, has no Redist/ (build with Scripts/Build.py).
		[[nodiscard]] std::filesystem::path MakeExportBinaryRoot(const TempDirectory& directory, const ExportBinaryRootOptions& options = {});

	}

}
