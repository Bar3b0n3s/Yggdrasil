#pragma once

#include "EditorCore/Export/Exporter.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

// project.export (Architecture §13.5 "project", §14.2), the editor's export method: a pending operation (§13.2) over the
// Exporter (EditorCore/Export/Exporter.h). Its own file, as ScreenshotMethods is for the screenshot theme, so the project
// domain's other methods stay in ProjectMethods (Docs/Decisions/0012-m7-decisions.md decision 14). Conventions as in
// MethodRegistry.h; frozen by the M7 contract.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class PendingOperation;
	class TypeRegistry;

	// project.export {config, outDir?, smokeTest?, testing?} (§13.5).
	struct ProjectExportParams
	{
		ExportConfiguration Config = ExportConfiguration::Release; // required; registry enum "ExportConfiguration"
		// The project-relative output directory under "Build/"; empty: Build/<Platform>-<Config>/<Name> (§14.1).
		std::string OutDir{};
		// Run the staged package's executable with --headless --frames 300 --expect-no-errors before it reaches the output
		// directory and require exit code 0 (§14.2 step 7; Exporter.h step 6 has the per-configuration options and the Dist
		// export from an editor without a device, which reports smokeTestRan false with a warning).
		bool SmokeTest = false;
		// A testing export (§7.6, M15): Unsupported at /testing when present.
		bool Testing = false;
	};

	// One file of the package (ExportedFile).
	struct ExportedFileSummary
	{
		std::string Path{}; // relative to outputDirectory
		uint32_t Size = 0;  // bytes (ToAutomationCounter)
		std::string Hash{}; // XXH64, 16 lowercase hex digits ("xxh64")
	};

	// The export report (§14.2 step 8).
	struct ProjectExportResult
	{
		std::string OutputDirectory{}; // absolute native path
		std::string Executable{};      // absolute native path
		ExportConfiguration Config = ExportConfiguration::Release;
		std::vector<ExportedFileSummary> Files{};
		uint32_t GameAssetCount = 0;
		uint32_t EngineEntryCount = 0;
		std::vector<std::string> Warnings{};
		bool SmokeTestRan = false;
		int32_t SmokeTestExitCode = 0;
		float Seconds = 0.0f;
	};

	namespace Automation {

		// project.export (pending): Exporter::Start with the params and AutomationServerSpecification::ExportBinaryRoot, polled
		// once per frame; a disconnect cancels it (Exporter::Cancel). Errors: Unsupported at /testing when present;
		// InvalidParams for an outDir outside "Build/"; those of Exporter::Start and Exporter::Poll.
		[[nodiscard]] Result<Scope<PendingOperation>> ProjectExport(EditorMethodContext& context, const ProjectExportParams& params);

	}

	// Registers ExportConfiguration, ProjectExportParams, ExportedFileSummary and ProjectExportResult.
	void RegisterExportMethodTypes(TypeRegistry& registry);

	// Registers project.export: a tool (§13.8 project_export), Mutates (it writes under the project's Build/, which read-only
	// editors refuse), pending with a timeout of 900 s, no dry run (§13.4), not a batch op, not available in the launcher
	// state, not AvailableInRuntime.
	void RegisterExportMethods(MethodRegistry& methods);

}
