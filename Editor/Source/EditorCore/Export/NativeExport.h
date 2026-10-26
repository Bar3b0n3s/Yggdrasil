#pragma once

#include "EditorCore/Export/ExportConfiguration.h"
#include "EditorCore/Native/NativeBuild.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Result.h"

#include <filesystem>

// The export of a game with a native module (Docs/Decisions/0030-m19-contract.md decision 33, C-NATIVE-STATIC): its package
// holds a Runtime executable that links the module (Scripts/BuildNative.py --kind runtime), instead of the prebuilt Runtime a
// game without Native/ gets unchanged. Exporter.cpp calls these at three points, which the contract wired: step 1 (Validate)
// refuses first, before anything is cooked or written; step 3a (ExportPhase::NativeBuild) starts the build and polls it; step
// 4 (CopyRuntime) copies the built executable instead of the prebuilt one. Stream N-A implements them. Two more keep
// what must outlive the build's directory, which the export removes: a failed build's log and the exported executable's
// symbols.

namespace Engine {

	class EditorContext;

	// Step 1's refusals of a project with Native/ (HasNativeModuleDirectory): Unsupported on Linux and macOS
	// (SharedLibrary::CheckSupported, naming the platform); PermissionDenied while the user's "Allow native code" preference
	// is off (NativeModuleHost::IsAllowed: an export of a native game builds and, with smokeTest or featureTest, runs its
	// code); NotFound when Native/ holds no source.
	[[nodiscard]] Status CheckNativeExport(const EditorContext& editor);

	// Step 3a: starts the build of the open project's Runtime of `configuration` with its module linked into
	// `outputDirectory` (a new directory beside the export's staging directory, never inside the package), through the
	// editor's NativeModuleHost::StartRuntimeBuild, so tests drive it with a fake backend. Errors: that function's.
	[[nodiscard]] Result<Scope<INativeBuildJob>> StartNativeRuntimeBuild(EditorContext& editor, ExportConfiguration configuration,
		const std::filesystem::path& outputDirectory);

	// The error of a build that did not succeed: CompileFailed "the native module of '<name>' does not build: <first
	// error>", with every diagnostic an issue (located in its source), and the hint to fix them (native.build reports the
	// same diagnostics in the editor's configuration) naming the outcome's Log as the build's whole output.
	[[nodiscard]] Error MakeNativeBuildError(const NativeBuildOutcome& outcome, std::string_view name);

	// Where an export keeps what outlives step 3a's build directory, which the export removes when it ends, below the
	// project's Library/ (never shipped, never committed): the log of the configuration's last failed build, and the symbols
	// of the configuration's last exported native Runtime.
	[[nodiscard]] std::filesystem::path GetNativeExportLogPath(const std::filesystem::path& projectRoot, ExportConfiguration configuration);
	[[nodiscard]] std::filesystem::path GetNativeExportSymbolsDirectory(const std::filesystem::path& projectRoot, ExportConfiguration configuration);

	// Step 3a's failure: the build's log copied to GetNativeExportLogPath (replacing the previous one), and `outcome` with
	// its Log naming the copy, so MakeNativeBuildError's hint names a file that outlives the export. When the copy fails
	// (logged), or the outcome names no log, the outcome is returned unchanged.
	[[nodiscard]] NativeBuildOutcome KeepNativeBuildLog(const std::filesystem::path& projectRoot, ExportConfiguration configuration, NativeBuildOutcome outcome);

	// After the package moved into place: the PDB and the linker map step 3a's linker wrote beside `executable` (its stem
	// with ".pdb" and ".map", as Scripts/Premake/NativeModule.lua links it) copied into GetNativeExportSymbolsDirectory,
	// which they replace: Architecture §2.2's "archived, never shipped" for the executable an export linked itself, as
	// Scripts/Build.py archives the prebuilt Dist Runtime's in bin/Symbols. Returns that directory. Errors: NotFound naming a
	// missing file; Io.
	[[nodiscard]] Result<std::filesystem::path> ArchiveNativeRuntimeSymbols(const std::filesystem::path& projectRoot, ExportConfiguration configuration,
		const std::filesystem::path& executable);

}
