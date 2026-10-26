#pragma once

#include "EditorCore/Export/ExportConfiguration.h"
#include "EditorCore/Native/NativeBuild.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Native/NativeApi.h"
#include "Engine/Native/NativeComponents.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// The editor's side of a game's native module (Docs/Decisions/0030-m19-contract.md decisions 29 to 31, C-NATIVE-DLL): the
// "Allow native code" preference that gates it, the module's builds below <Project>/Library/Native/Builds/, and the loaded
// module, which the editor hot-reloads. EditorContext owns one for its lifetime (EditorContext::GetNativeModule); the
// native.* methods (EditorCore/Automation/NativeMethods.h), the Native panel, every play session the editor starts
// (PlaySessionSpecification::Native from GetLoadedTypes) and the exporter (decision 33) go through it. Main thread only.
//
// The preference (decision 29) is the user's: "AllowNativeCode" in user://Editor.json (EditorPreferences), off by default,
// changed only by the Native panel's checkbox, never by an automation method: no method takes it as a param, writes
// user://Editor.json or changes the editor's preferences otherwise, and native.status only reports it. While it is off,
// StartBuild and Load refuse with PermissionDenied, no project opening loads a module, and the exporter refuses a project
// with Native/ (decision 33); turning it off unloads a loaded module at the next frame boundary without a play session.
//
// Loading (decision 31). A load takes the newest successful build of the editor's own configuration whose SourceHash is
// current, loads its library (INativeLibraryLoader: SharedLibrary on Windows), registers the module
// (BuildNativeModuleTypes, which keeps the library loaded) and only then replaces the loaded module: a load that fails at
// any step leaves the previous module loaded and the scene unchanged. It canonicalizes the open scene's native components
// against the new types (CanonicalizeNativeComponents, the migration by field name of decision 27): a reload whose layout
// hash equals the replaced module's rewrites nothing, so the scene stays byte-identical, and a load whose layout changed
// (every first load, one after an unload included) migrates every component and marks the scene modified when its
// canonical form changed. A migration is not an undo step, like a load's migrations, and a load whose layout changed
// clears the undo history (CommandHistory), whose entity snapshots hold the previous layout's values or, recorded without
// a module, the values as written, which undo would restore whole; a reload whose layout is unchanged keeps it. The
// previous module's types are released last, which unloads its library once nothing holds them. Loads happen only at
// frame boundaries without a play session (Play or Simulate): native.load is refused while one runs, and so is a project
// opening's load while one runs, which never happens.
// A project opening loads the newest current build when the preference is on and the platform supported, and builds
// nothing (decision 31: a project with Native/ and no current build opens without its module; native.build then
// native.load load it, and a --strict editor refuses its scenes' native components through NATIVE_COMPONENT_UNREGISTERED,
// which is why the games suite builds the module before it starts a strict editor, decision 35).
//
// Linux and macOS (decision 34, ADR 0028 decision 10): StartBuild and Load refuse with SharedLibrary::CheckSupported's
// Unsupported naming the platform, and the module is never loaded, as while the preference is off; a project without
// Native/ is unaffected.

namespace Engine {

	class EditorContext;
	class NativeModuleTypes;

	// What loading a built library gives the host: the module's entry and what keeps its code loaded.
	struct NativeLoadedLibrary
	{
		NativeModuleEntryPoint Entry{};
		Ref<const void> Keepalive{}; // the SharedLibrary; null for a module that needs nothing kept (a test's)
	};

	// Loads a built library. The platform's (CreateNativeLibraryLoader) loads it with SharedLibrary and reads
	// EngineNativeModuleApiVersion and EngineNativeModuleRegister (ENGINE_NATIVE_MODULE); tests inject one that returns
	// in-process registration functions (EditorContextSpecification::NativeLoader).
	class INativeLibraryLoader
	{
	public:
		virtual ~INativeLibraryLoader() = default;

		// Errors: those of SharedLibrary::Load and FindSymbol (Unsupported on Linux and macOS, NotFound for a library that
		// exports neither entry function).
		[[nodiscard]] virtual Result<NativeLoadedLibrary> Load(const std::filesystem::path& library) = 0;
	};

	// The platform's loader (see INativeLibraryLoader).
	[[nodiscard]] Scope<INativeLibraryLoader> CreateNativeLibraryLoader();

	// A successful module build below <Project>/Library/Native/Builds/ (its NativeBuild.json, decision 30).
	struct NativeBuildInfo
	{
		std::filesystem::path Directory{}; // the build's directory
		std::filesystem::path Library{};   // its shared library
		std::string SourceHash{};
		ExportConfiguration Configuration = ExportConfiguration::Debug;
		bool Current = false; // SourceHash equals the sources' hash now
	};

	// The host's state, as native.status reports it (decision 32).
	struct NativeModuleStatus
	{
		bool Allowed = false;                         // the user's "Allow native code" preference (decision 29)
		bool Supported = false;                       // SharedLibrary::CheckSupported succeeded: this platform builds and loads modules
		std::vector<std::string> Sources{};           // ListNativeSources; empty: the project has no native module
		std::string SourceHash{};                     // ComputeNativeSourceHash of the sources now; empty without sources
		std::optional<NativeBuildInfo> LatestBuild{}; // the newest successful build of the editor's configuration
		bool Building = false;                        // a build runs (StartBuild until PollBuild resolves)
		// The last build this editor ran for the open project, successful or not, with its diagnostics.
		std::optional<NativeBuildOutcome> LastBuild{};
		Ref<const NativeModuleTypes> Loaded{}; // the loaded module's types; null when none is loaded
		std::string LoadedSourceHash{};        // the loaded build's SourceHash; empty when none is loaded
	};

	// What a load did (native.load, decision 31).
	struct NativeLoadReport
	{
		bool Reloaded = false;      // a loaded module was replaced
		bool LayoutChanged = false; // the layout hash differs from the replaced module's (true for a first load)
		bool UndoCleared = false;   // a load whose layout changed (a first load included) cleared the open scene's undo history
		NativeSceneReport Scene{};  // the open scene's canonicalization; Scene.Changed: the scene is now modified
	};

	struct NativeModuleHostSpecification
	{
		Ref<INativeBuildBackend> Build{};   // non-null (asserted)
		Ref<INativeLibraryLoader> Loader{}; // non-null (asserted)
		// The editor's own configuration (Debug or Release; Dist has no editor): the configuration of the module builds it
		// starts and loads, whose code is compiled with the editor's flags.
		ExportConfiguration EditorConfiguration = ExportConfiguration::Debug;
		// The NativeApi.h module builds compile against (<repository>/Engine/Source/Engine/Native/NativeApi.h through
		// ENGINE_REPO_ROOT), part of every source hash.
		std::filesystem::path ApiHeader{};
	};

	// The editor's specification: `build` and `loader`, or the platform's (CreateNativeBuildBackend,
	// CreateNativeLibraryLoader) when null; the editor's own configuration (ENGINE_DEBUG: Debug, else Release); the
	// repository's NativeApi.h (ENGINE_REPO_ROOT, which every configuration that builds EditorCore defines).
	[[nodiscard]] NativeModuleHostSpecification MakeEditorNativeModuleHostSpecification(Ref<INativeBuildBackend> build, Ref<INativeLibraryLoader> loader);

	class NativeModuleHost
	{
	public:
		explicit NativeModuleHost(NativeModuleHostSpecification specification);
		~NativeModuleHost();

		NativeModuleHost(const NativeModuleHost&) = delete;
		NativeModuleHost& operator=(const NativeModuleHost&) = delete;

		// --- The preference (decision 29) -----------------------------------------------------------------------------
		[[nodiscard]] bool IsAllowed() const;
		// Reads AllowNativeCode from user://Editor.json (ReadEditorPreferences): EditorContext calls it when the editor starts
		// and when a project opens. A missing file is off; a malformed one is off and its error returned. Never writes.
		[[nodiscard]] Status ReadPreference(const EditorContext& editor);
		// The Native panel's checkbox, the only writer: queued without I/O, applied by Update.
		void QueuePreferenceChange(bool allowed);
		// Once per frame at the safe point (EditorContext::Update): applies a queued preference change (WriteEditorPreferences,
		// which keeps every other member of user://Editor.json) and, while the preference is off and no play session runs,
		// unloads a loaded module (its components stay in the scene as written).
		void Update(EditorContext& editor);

		// --- Project lifecycle -----------------------------------------------------------------------------------------
		// EditorContext's OpenProject, before the project's scenes load: with the preference on and the platform supported,
		// loads the newest current build (see the file comment). Errors: a load's, which leave the project open without
		// its module (EditorContext logs them as a warning).
		[[nodiscard]] Status OnProjectOpened(EditorContext& editor);
		// EditorContext's CloseProject: cancels a running build and unloads the module.
		void OnProjectClosed();

		// --- Status, build, load (decisions 30 to 32) ---------------------------------------------------------------------
		[[nodiscard]] Result<NativeModuleStatus> GetStatus(const EditorContext& editor) const;
		// Starts a module build of the editor's configuration into a new <Project>/Library/Native/Builds/<sequence>/ (the
		// sequence one more than the highest there, at least 8 digits). Errors: Unsupported on Linux and macOS;
		// PermissionDenied while the preference is off (hint: the user turns on Allow native code in the Native panel; no
		// method can); InvalidState without a project, for a read-only project, or while a build runs; NotFound for a project
		// without native sources (hint: native.write); the backend's.
		[[nodiscard]] Status StartBuild(EditorContext& editor);
		// The running build's outcome once it resolved, nullopt before. A successful build becomes LatestBuild; builds older
		// than LatestBuild and the loaded one are removed (a failure to remove one is logged). Every outcome but a
		// cancellation becomes LastBuild.
		[[nodiscard]] std::optional<Result<NativeBuildOutcome>> PollBuild(EditorContext& editor);
		// Cancels a running build (its outcome is then Cancelled).
		void CancelBuild();
		// An export's build (decision 33; NativeExport.h StartNativeRuntimeBuild): the open project's Runtime of
		// `configuration` with its module linked, into `outputDirectory`, through the host's backend. The job is the caller's:
		// Building stays false and the outcome becomes neither LatestBuild nor LastBuild, but IsRuntimeBuildRunning is true
		// until the job resolves or is destroyed. Errors: StartBuild's refusals (Unsupported, PermissionDenied, InvalidState
		// without a project, NotFound without sources) and the backend's.
		[[nodiscard]] Result<Scope<INativeBuildJob>> StartRuntimeBuild(EditorContext& editor, ExportConfiguration configuration,
			const std::filesystem::path& outputDirectory);
		// Whether a job StartRuntimeBuild returned still runs: native.write refuses then, as while a module build runs
		// (decision 32), since the build compiles the sources it linted.
		[[nodiscard]] bool IsRuntimeBuildRunning() const;
		// Loads, or reloads, the newest current build (see the file comment). Errors: Unsupported on Linux and macOS;
		// PermissionDenied while the preference is off; InvalidState without a project, while a play session or a build runs,
		// or without a current successful build (hint: native.build); the loader's and BuildNativeModuleTypes' (the previous
		// module stays loaded); Validation from canonicalizing the open scene (an error there leaves the previous module
		// loaded and the scene unchanged).
		[[nodiscard]] Result<NativeLoadReport> Load(EditorContext& editor);

		// The loaded module's types, null when none is loaded: what every play session the editor starts runs.
		[[nodiscard]] Ref<const NativeModuleTypes> GetLoadedTypes() const;
	private:
		// The specification, the preference, the queued change, the running build, the latest and last builds and the loaded
		// module (NativeModuleHost.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
