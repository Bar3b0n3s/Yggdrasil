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
// 4 (CopyRuntime) copies the built executable instead of the prebuilt one. Stream N-A implements them.

namespace Engine {

	class EditorContext;

	// Step 1's refusals of a project with Native/ (HasNativeModuleDirectory): Unsupported on Linux and macOS
	// (SharedLibrary::CheckSupported, naming the platform); PermissionDenied while the user's "Allow native code" preference
	// is off (NativeModuleHost::IsAllowed: an export of a native game builds and, with smokeTest or featureTest, runs its
	// code); NotFound when Native/ holds no source.
	[[nodiscard]] Status CheckNativeExport(const EditorContext& editor);

	// Step 3a: starts the build of the open project's Runtime of `configuration` with its module linked into
	// `outputDirectory` (beside the export's staging directory, never inside the package), through the editor's
	// NativeModuleHost::StartRuntimeBuild, so tests drive it with a fake backend. Errors: that function's.
	[[nodiscard]] Result<Scope<INativeBuildJob>> StartNativeRuntimeBuild(EditorContext& editor, ExportConfiguration configuration,
		const std::filesystem::path& outputDirectory);

	// The error of a build that did not succeed: CompileFailed "the native module of '<name>' does not build: <first
	// error>", with every diagnostic an issue (located in its source), and the hint to fix them (native.build reports the
	// same diagnostics in the editor's configuration).
	[[nodiscard]] Error MakeNativeBuildError(const NativeBuildOutcome& outcome, std::string_view name);

}
