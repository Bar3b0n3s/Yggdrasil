#pragma once

#include "Engine/App/CommandLine.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

// The editor's own command-line options (Architecture §12.1, §13.9), the M4 subset and M6's --bake-engine-assets and
// --engine-cache-dir:
//   Editor [--project <path>] [--read-only] [--automation[=port]] [--automation-test-hooks] [--batch <file.jsonl>]
//          [--upgrade] [--dump-reference <dir>] [--bake-engine-assets] [--engine-cache-dir <dir>]
// plus the engine options every application takes (GetEngineCommandLineOptions: --headless, --frames N,
// --user-data-dir, --renderer vulkan|none and the graphics options) and the Editor executable's screenshot options
// (Editor/EditorApp.h). --renderer is an engine option because the Runtime takes it too (§13.9; ADR 0009 decision 3:
// one definition after the M4/M5 merge, decision 33). --run-tests, --check-scripts, --validate, --export and --timeout
// arrive with their milestones. Parsed in EditorCore so the rules are unit-tested;
// EditorApp applies them.

namespace Engine {

	// What the editor was asked to do.
	struct EditorLaunchOptions
	{
		// --project <path>: a .eproj or a directory holding one; native, absolute or relative to the working directory. Without
		// it the editor starts in the launcher state (§12.1).
		std::optional<std::filesystem::path> Project{};
		// --read-only (§4.13): open the project without the lock, mutations denied.
		bool ReadOnly = false;
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
		// One-shot M13 modes; require --project and share the automation implementations and result schemas.
		bool CheckScripts = false;
		bool RunTests = false;
		std::string TestFilter{};
		// --dump-reference <dir>: write <dir>/Methods.json (MethodRegistry::BuildMethodCatalog) and <dir>/catalog.json
		// (BuildToolCatalog, the MCP catalogue, ADR 0008 decision 9), creating <dir>, and exit with 0 (§2.3 GenerateDocs.py).
		std::optional<std::filesystem::path> DumpReferenceDirectory{};
		// --bake-engine-assets (§7.5): fill the engine cooked cache bin/EngineCache (BakeEngineAssets over
		// engine://EngineAssets.json, with the device's environment baker from M8) and exit: 0 when every entry is baked or up
		// to date (entries without an importer in this build are skipped with a warning), 1 when a bake failed. Needs no
		// project; CI.py's bake stage runs it after the build (§15.8).
		bool BakeEngineAssets = false;
		// --engine-cache-dir <dir>: the engine cooked cache to mount as enginecache:// instead of the checkout's
		// <repo>/bin/EngineCache (ApplicationSpecification::EngineCacheDirectory, created when missing); native, absolute or
		// relative to the working directory. Tests pass a temporary directory, so they never write the checkout's cache.
		std::optional<std::filesystem::path> EngineCacheDirectory{};

		// True when the editor runs a task and exits (batch, upgrade, dump-reference, bake-engine-assets) instead of running
		// until shutdown.
		[[nodiscard]] bool IsOneShot() const
		{
			return BatchFile.has_value() || Upgrade || DumpReferenceDirectory.has_value() || BakeEngineAssets || CheckScripts || RunTests;
		}

		// Whether the automation server listens on TCP and writes a session file (AutomationServerSpecification::Listen):
		// with --automation, or headless unless the run is one-shot, so an MCP editor_launch can never attach to a batch,
		// upgrade or dump-reference run (ADR 0008 decision 14).
		[[nodiscard]] bool ListensForAutomation(bool headless) const { return Automation || (headless && !IsOneShot()); }
	};

	// The options above, for CommandLine::Parse together with GetEngineCommandLineOptions. The views refer to static storage.
	[[nodiscard]] std::span<const CommandLineOption> GetEditorCommandLineOptions();

	// Reads the editor options of `commandLine` (parsed with both option tables). Errors: InvalidArgument naming the option
	// for: an --automation port that is not 1 to 65535; --read-only or --upgrade without --project; --upgrade with --read-only; more than one of --batch, --upgrade,
	// --dump-reference and --bake-engine-assets; --automation or --automation-test-hooks together with --dump-reference,
	// --upgrade or --bake-engine-assets; and --automation-test-hooks without --automation or --batch.
	[[nodiscard]] Result<EditorLaunchOptions> ParseEditorLaunchOptions(const CommandLine& commandLine);

}
