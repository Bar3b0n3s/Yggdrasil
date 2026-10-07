#pragma once

#include "Engine/App/CommandLine.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

// The editor's own command-line options (Architecture §12.1, §13.9), the M4 subset:
//   Editor [--project <path>] [--read-only] [--renderer vulkan|none] [--automation[=port]] [--automation-test-hooks]
//          [--batch <file.jsonl>] [--upgrade] [--dump-reference <dir>]
// plus the engine options every application takes (--headless, --frames N, --user-data-dir, GetEngineCommandLineOptions).
// --run-tests, --check-scripts, --validate, --export, --bake-engine-assets, --gpu-validation, --vulkan-api and --timeout
// arrive with their milestones. Parsed in EditorCore so the rules are unit-tested; EditorApp applies them.

namespace Engine {

	// --renderer (§13.9). Before the Graphics milestone (M5) the editor renders nothing either way; Vulkan is the default, so
	// later milestones change behaviour, not the command line.
	enum class EditorRenderer : uint8_t
	{
		Vulkan,
		None
	};

	// "vulkan" or "none" (the command-line and session.info spelling).
	[[nodiscard]] std::string_view EditorRendererToString(EditorRenderer renderer);

	// What the editor was asked to do.
	struct EditorLaunchOptions
	{
		// --project <path>: a .eproj or a directory holding one; native, absolute or relative to the working directory. Without
		// it the editor starts in the launcher state (§12.1).
		std::optional<std::filesystem::path> Project{};
		// --read-only (§4.13): open the project without the lock, mutations denied.
		bool ReadOnly = false;
		EditorRenderer Renderer = EditorRenderer::Vulkan;
		// --automation[=port]: start the automation server listening (§13.2). --headless listens too, except in one-shot
		// runs (IsOneShot; EditorApp.h, AutomationServerSpecification::Listen).
		bool Automation = false;
		std::optional<uint16_t> AutomationPort{}; // the port after '=', 1 to 65535
		// --automation-test-hooks: register the debug.* methods (Roadmap M4).
		bool AutomationTestHooks = false;
		// --batch <file>: run the file's requests (BatchRunner) and exit with 0 or 1 (§13.9).
		std::optional<std::filesystem::path> BatchFile{};
		// --upgrade: run project.upgrade on --project, with transcript lines (client "cli"), and exit (§13.12).
		bool Upgrade = false;
		// --dump-reference <dir>: write <dir>/Methods.json (MethodRegistry::BuildMethodCatalog) and <dir>/catalog.json
		// (BuildToolCatalog, the MCP catalogue, ADR 0008 decision 9), creating <dir>, and exit with 0 (§2.3 GenerateDocs.py).
		std::optional<std::filesystem::path> DumpReferenceDirectory{};

		// True when the editor runs a task and exits (batch, upgrade, dump-reference) instead of running until shutdown.
		[[nodiscard]] bool IsOneShot() const { return BatchFile.has_value() || Upgrade || DumpReferenceDirectory.has_value(); }

		// Whether the automation server listens on TCP and writes a session file (AutomationServerSpecification::Listen):
		// with --automation, or headless unless the run is one-shot, so an MCP editor_launch can never attach to a batch,
		// upgrade or dump-reference run (ADR 0008 decision 14).
		[[nodiscard]] bool ListensForAutomation(bool headless) const { return Automation || (headless && !IsOneShot()); }
	};

	// The options above, for CommandLine::Parse together with GetEngineCommandLineOptions. The views refer to static storage.
	[[nodiscard]] std::span<const CommandLineOption> GetEditorCommandLineOptions();

	// Reads the editor options of `commandLine` (parsed with both option tables). Errors: InvalidArgument naming the option
	// for: a --renderer value other than vulkan or none (any ASCII case accepted); an --automation port that is not 1 to
	// 65535; --read-only or --upgrade without --project; --upgrade with --read-only; more than one of --batch, --upgrade and
	// --dump-reference; --automation or --automation-test-hooks together with --dump-reference or --upgrade; and
	// --automation-test-hooks without --automation or --batch.
	[[nodiscard]] Result<EditorLaunchOptions> ParseEditorLaunchOptions(const CommandLine& commandLine);

}
