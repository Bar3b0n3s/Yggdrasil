#pragma once

#include "EditorCore/Export/ExportConfiguration.h"
#include "Engine/App/CommandLine.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The editor's own command-line options (Architecture §12.1, §13.9), the M4 subset, M6's --bake-engine-assets and
// --engine-cache-dir, M13's --check-scripts, --run-tests and --filter, M14's --id-seed and --junit, M15's export options
// and --strict, and M16's --help (friction F20):
//   Editor [--help | -h]
//   Editor [--project <path>] [--read-only] [--strict] [--automation[=port]] [--automation-test-hooks] [--batch <file.jsonl>
//          [--id-seed <n>]] [--upgrade] [--dump-reference <dir>] [--bake-engine-assets] [--engine-cache-dir <dir>]
//          [--check-scripts] [--run-tests [--filter <pattern>] [--junit <file.xml>]]
//          [--export <dir> --config Debug|Release|Dist [--testing] [--smoke-test] [--export-report <file.json>]]
//          [--export-root <dir>] [--headless-game-view]
// plus the engine options every application takes (GetEngineCommandLineOptions: --headless, --frames N,
// --user-data-dir, --renderer vulkan|none and the graphics options) and the Editor executable's screenshot options
// (Editor/EditorApp.h). --renderer is an engine option because the Runtime takes it too (§13.9; ADR 0009 decision 3:
// one definition after the M4/M5 merge, decision 33). M15 adds --export with --config, --testing, --smoke-test and
// --export-report, and --export-root (Docs/Decisions/0024-m15-contract.md decision 3; stream A parses them, EditorApp
// applies them). Parsed in EditorCore so the rules are unit-tested; EditorApp applies them.

namespace Engine {

	// What the editor was asked to do.
	struct EditorLaunchOptions
	{
		// --help, or -h (ExpandEditorShortOptions): print the usage, every option with its one-line description
		// (CommandLine::FormatUsage), to standard output and exit 0 without starting the editor (Editor/EditorApp.h RunEditor).
		// The other options must still parse (an unknown option stays a usage error), but no other rule is checked and no
		// other member is read.
		bool Help = false;
		// --project <path>: a .eproj or a directory holding one; native, absolute or relative to the working directory. Without
		// it the editor starts in the launcher state (§12.1).
		std::optional<std::filesystem::path> Project{};
		// --read-only (§4.13): open the project without the lock, mutations denied.
		bool ReadOnly = false;
		// --strict (§6 "Strict reader"; the M15 completion review's A024): every project this editor opens, through --project,
		// project.open, project.create or recovery, is read strictly: unknown keys in the .eproj, and unknown keys, fields
		// and components (and unresolvable or mismatching script field values) in its scenes and prefabs, are errors
		// instead of warnings (EditorContextSpecification::StrictUnknowns), and a settings patch
		// (ProjectSettingsCommand::CreateFromPatch: project.setSettings, the inspector) naming a member the settings do not
		// have is refused instead of dropped with a warning. CI's feature, determinism and export runs pass it over the
		// committed FeatureTest. Combines with every other option.
		bool Strict = false;
		// M19 --headless-game-view (Docs/Decisions/0030-m19-contract.md decision 11, R-SOAK), only with the engine option
		// --headless (InvalidArgument otherwise): the headless editor renders the Game view every frame as if its panel were
		// displayed, at the project's Window.Width x Window.Height (viewport.setOptions' gameResolution when one is set), the
		// size lockstep and replay sessions use as view coordinates, so the game camera's passes and post-processing run
		// while it plays and stats.get reports the "game" view. The hardening soak launches its editors with it, so the
		// editor soak renders each scene's game view. Without it a headless editor renders only the views its panels display.
		bool HeadlessGameView = false;
		// --automation[=port]: start the automation server listening (§13.2). --headless listens too, except in one-shot
		// runs (IsOneShot; EditorApp.h, AutomationServerSpecification::Listen).
		bool Automation = false;
		std::optional<uint16_t> AutomationPort{}; // the port after '=', 1 to 65535
		// --automation-test-hooks: register the debug.* methods (Roadmap M4).
		bool AutomationTestHooks = false;
		// --batch <file>: run the file's requests (BatchRunner) and exit with 0 or 1 (§13.9).
		std::optional<std::filesystem::path> BatchFile{};
		// --upgrade: open --project with its asset scan deferred, run project.refreshAssets (the scan) and project.upgrade,
		// with transcript lines (client "cli"), and exit (§13.12; Docs/Decisions/0026-m16-contract.md decision 11).
		bool Upgrade = false;
		// One-shot M13 modes; require --project and share the automation implementations and result schemas.
		bool CheckScripts = false;
		bool RunTests = false;
		std::string TestFilter{};
		// M14 (Architecture §12.1, §15.5; Docs/Decisions/0022-m14-contract.md decision 7): --junit <file.xml>, only with
		// --run-tests, names the JUnit report --run-tests writes; the JSON report goes beside it with the extension replaced
		// by .json. Without it the reports are <working directory>/bin/TestResults/FeatureTest-Editor.xml and .json. Native,
		// absolute or relative to the working directory; parent directories are created. Written whatever the outcome,
		// independent of the automation server's offload directory and its cleanup: from the run's result, from the result
		// test.run published with its error when its cases ran, or, when the run failed before a result existed (a preflight
		// error such as script type errors), as a failed run naming the error (MakeFailedTestRunResult), so a report of an
		// earlier run never survives at these paths.
		std::optional<std::filesystem::path> TestJunitPath{};
		// M14 (decision 8; implemented by the contract, so streams B and C can check their scaffolds from the start): --id-seed
		// <n>, only with --batch: a decimal 64-bit seed whose Random(n).GetState() becomes the editor's id generator state
		// (EditorContextSpecification::IdGeneratorState), so a committed scaffold run gives the same entity ids and asset
		// handles on every machine. Without it the state comes from the OS CSPRNG.
		std::optional<uint64_t> IdSeed{};
		// --dump-reference <dir>: write the whole generated reference into <dir> (DumpReferences, ReferenceGenerator.h: the
		// method and MCP catalogues Methods.json and catalog.json, the Markdown references, Schemas/, Engine.d.luau and
		// ScriptAPI.md), creating <dir>, and exit with 0 (§2.3 GenerateDocs.py, ADR 0022 decision 13).
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
		// M15 (§12.1, §14.2; ADR 0024 decision 3): --export <dir> --config Debug|Release|Dist [--testing] [--smoke-test]
		// [--export-report <file.json>], with --project and without --read-only: export the project into the package
		// directory <dir> (native, absolute or relative to the working directory; ExportSpecification::OutputPath, whose
		// refusal rules protect the project, the checkout, the working directory and the user-data root; replaced as a whole
		// only when it is empty or holds an earlier export) from bin/ of the checkout (ENGINE_REPO_ROOT), then exit 0 when it
		// exported, 1 when the export failed, 3 when the project does not open, its lock is held, or a build output is missing
		// (Exporter::IsMissingBuildOutput; any other NotFound is 1). The export runs headless or windowed, polled once per
		// frame like project.export. --frames (an engine option) is refused with it: the export decides when the run ends.
		std::optional<std::filesystem::path> ExportDirectory{};
		std::optional<ExportConfiguration> ExportConfig{}; // --config, required with --export and only with it
		bool ExportTesting = false;                        // --testing, only with --export (ExportSpecification::Testing)
		bool ExportSmokeTest = false;                      // --smoke-test, only with --export (ExportSpecification::SmokeTest)
		// --export-report <file.json>, only with --export: written whatever the outcome, canonical JSON
		// {"success": true, "result": <project.export's result>} or {"success": false, "error": <the error as the automation
		// protocol reports it: code, message, issues>}; parent directories are created.
		std::optional<std::filesystem::path> ExportReportPath{};
		// --export-root <dir>, not with --export: the directory project.export writes under instead of <Project>/Build/
		// (§13.4; AutomationServerSpecification::ExportRoot, ExportSpecification::ExportRoot); made absolute against the
		// working directory.
		std::optional<std::filesystem::path> ExportRoot{};

		// True when the editor runs a task and exits (batch, upgrade, dump-reference, bake-engine-assets, export) instead of
		// running until shutdown.
		[[nodiscard]] bool IsOneShot() const
		{
			return BatchFile.has_value() || Upgrade || DumpReferenceDirectory.has_value() || BakeEngineAssets || CheckScripts || RunTests
				|| ExportDirectory.has_value();
		}

		// Whether the automation server listens on TCP and writes a session file (AutomationServerSpecification::Listen):
		// with --automation, or headless unless the run is one-shot, so an MCP editor_launch can never attach to a batch,
		// upgrade or dump-reference run (ADR 0008 decision 14).
		[[nodiscard]] bool ListensForAutomation(bool headless) const { return Automation || (headless && !IsOneShot()); }
	};

	// The options above, for CommandLine::Parse together with GetEngineCommandLineOptions. The views refer to static storage.
	[[nodiscard]] std::span<const CommandLineOption> GetEditorCommandLineOptions();

	// `arguments` with "-h", the one single-dash option (CommandLine::Parse accepts none), spelled "--help" wherever it stands
	// as an option: not as the value of a Required option of `options` given as "--name value", and not after "--". Every
	// other argument is kept as it is.
	[[nodiscard]] std::vector<std::string> ExpandEditorShortOptions(std::span<const std::string> arguments, std::span<const CommandLineOption> options);

	// Reads the editor options of `commandLine` (parsed with both option tables). With --help, only Help is set and nothing
	// else is checked. Errors: InvalidArgument naming the option for: an --automation port that is not 1 to 65535; --read-only or --upgrade without --project; --upgrade with --read-only; more than one of --batch, --upgrade,
	// --dump-reference, --bake-engine-assets, --check-scripts, --run-tests and --export; --automation or --automation-test-hooks together with --dump-reference,
	// --upgrade, --bake-engine-assets, --check-scripts, --run-tests or --export; --automation-test-hooks without --automation or --batch; --config other
	// than Debug, Release or Dist; --config, --testing, --smoke-test or --export-report without --export; --export without --config or --project, or with
	// --read-only, --export-root or --frames; and an --export-root that cannot be made absolute.
	[[nodiscard]] Result<EditorLaunchOptions> ParseEditorLaunchOptions(const CommandLine& commandLine);

}
