#pragma once

#include "EditorCore/Export/ExportConfiguration.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The native build (Docs/Decisions/0030-m19-contract.md decision 30, C-NATIVE-STATIC): a game's <Game>/Native/ sources
// compiled with the first-party settings (precise floating point, warnings as errors) after the determinism lint, either
// into the editor's shared library (NativeBuildKind::Module, which the editor loads and hot-reloads) or, for an export, into
// the game's Runtime executable with the module linked in (NativeBuildKind::Runtime). The work is Scripts/BuildNative.py's
// (stream N-A), which generates the build with premake from the same ApplyFirstPartySettings() and Use<Lib>() functions as
// the engine, compiles and links with MSBuild and the vendor cache under the machine-wide build lock, and writes the build
// record NativeBuild.json. The editor starts it as one process by fixed paths with fixed arguments (§13.2 as amended by
// decision 33): the interpreter Scripts/Generate.py recorded (ENGINE_PYTHON_EXECUTABLE) running
// <repository>/Scripts/BuildNative.py --project <root> --kind module|runtime --config <Debug|Release|Dist> --out <dir> --json,
// never an argument a request supplies. Windows backends only (ADR 0028 decision 10): on Linux and macOS
// CreateNativeBuildBackend's Start refuses with SharedLibrary::CheckSupported's Unsupported, naming the platform, and no build
// is written there.

namespace Engine {

	// The project directory of a game's native module (decision 26), and the build records' file name.
	inline constexpr std::string_view NativeSourceDirectoryName = "Native";
	inline constexpr std::string_view NativeBuildRecordFileName = "NativeBuild.json";

	// What a build produces.
	enum class NativeBuildKind : uint8_t
	{
		Module, // the editor's shared library (Debug or Release, the editor's own configuration)
		Runtime // the game's Runtime executable of an export's configuration, the module linked in
	};

	// One build.
	struct NativeBuildRequest
	{
		std::filesystem::path ProjectRoot{}; // absolute: the project whose Native/ is built
		NativeBuildKind Kind = NativeBuildKind::Module;
		ExportConfiguration Configuration = ExportConfiguration::Debug;
		// Absolute, missing or empty: the build writes its output and NativeBuild.json there (the editor: a new
		// <Project>/Library/Native/Builds/<sequence>/; an export: a new numbered directory beside its staging directory).
		std::filesystem::path OutputDirectory{};
	};

	// One finding of a build, located in a source when it has a location.
	struct NativeBuildDiagnostic
	{
		DiagnosticSeverity Severity = DiagnosticSeverity::Error;
		// "NATIVE_LINT_ERROR" (the determinism lint or the include rule, decision 35), "NATIVE_FORMAT_WARNING" (a Warning that
		// fails nothing: a source that is not clang-format clean, at its first differing line, or a format check that could not
		// run; Format.py --check fails the gate on such a source), "NATIVE_COMPILE_ERROR", "NATIVE_COMPILE_WARNING" (an error
		// under warnings-as-errors, reported as found), "NATIVE_LINK_ERROR" or "NATIVE_BUILD_ERROR" (anything else: the
		// toolchain or the build outputs missing, the build lock not taken).
		std::string Code{};
		std::string File{};  // project-relative ("Native/Spin.cpp"); empty without a location
		uint32_t Line = 0;   // 1-based; 0 without a location
		uint32_t Column = 0; // 1-based; 0 without a column
		std::string Message{};
	};

	// What a build produced (NativeBuild.json, decision 30).
	struct NativeBuildOutcome
	{
		bool Succeeded = false;
		std::filesystem::path Output{};                   // the library or the executable; empty when the build failed
		std::string SourceHash{};                         // ComputeNativeSourceHash of the sources it compiled; empty when it failed before hashing them
		std::vector<NativeBuildDiagnostic> Diagnostics{}; // in the order the build found them
		std::filesystem::path Log{};                      // the build's whole output, beside Output
		double Seconds = 0.0;                             // wall-clock, for the report only
	};

	// A build record, NativeBuild.json in a build's output directory (decision 30), as the host and the backend read it:
	//     { "Format": "NativeBuild", "Version": 1, "Kind": "Module" | "Runtime", "Configuration": "Debug" | "Release" | "Dist",
	//       "SourceHash": <16 hex, "" when a failed build could not hash the sources>, "Sources": [<project-relative path>, ...],
	//       "Succeeded": <bool>,
	//       "Output": <file name in the directory, "" when it failed>, "Log": <file name>,
	//       "Diagnostics": [ { "Severity": "Error" | "Warning", "Code", "File", "Line", "Column", "Message" }, ... ] }
	// written canonically by Scripts/BuildNative.py.
	struct NativeBuildRecord
	{
		NativeBuildKind Kind = NativeBuildKind::Module;
		ExportConfiguration Configuration = ExportConfiguration::Debug;
		std::string SourceHash{};
		std::vector<std::string> Sources{};
		bool Succeeded = false;
		std::string Output{};
		std::string Log{};
		std::vector<NativeBuildDiagnostic> Diagnostics{};
	};

	// Reads a build record. The record is written by the build, never by hand, so its reader is strict like a .meta file's:
	// errors are Parse (line and column) for invalid JSON, and Validation located at its pointer for anything else than the
	// layout above (a missing or unknown member, a value of the wrong type, a SourceHash that is not 16 lowercase hex digits
	// or is empty for a successful build, a Version other than 1, an Output that is not a plain file name or is empty for a
	// successful build). Pure; fuzzed.
	[[nodiscard]] Result<NativeBuildRecord> ParseNativeBuildRecord(std::string_view text);

	// One running build. Main thread; Poll once per frame (a pending operation's Poll). Destroying a job that still runs
	// cancels it, so no build outlives its owner.
	class INativeBuildJob
	{
	public:
		virtual ~INativeBuildJob() = default;

		// nullopt while the build runs; then its outcome (a failed compile is a Succeeded false outcome, not an error), or an
		// error when the build could not run or its record cannot be read. Called again after it resolved: the same result.
		[[nodiscard]] virtual std::optional<Result<NativeBuildOutcome>> Poll() = 0;
		// Ends a running build (its process is killed) and resolves it as Cancelled. Nothing after it resolved.
		virtual void Cancel() = 0;
	};

	// Starts builds. The editor's NativeModuleHost owns one; tests inject a fake (EditorContextSpecification::NativeBuild).
	class INativeBuildBackend
	{
	public:
		virtual ~INativeBuildBackend() = default;

		// Errors: Unsupported on Linux and macOS (SharedLibrary::CheckSupported); InvalidState when the editor's workspace was generated
		// without an interpreter (ENGINE_PYTHON_EXECUTABLE empty: run python Scripts/Generate.py); those of Process::Spawn.
		[[nodiscard]] virtual Result<Scope<INativeBuildJob>> Start(const NativeBuildRequest& request) = 0;
	};

	// The platform's backend: the process of the file comment on Windows; Unsupported from Start on Linux and macOS.
	[[nodiscard]] Scope<INativeBuildBackend> CreateNativeBuildBackend();

	// Whether the project at `projectRoot` is a game with a native module (decision 26): it has a Native/ directory, whatever
	// it holds (a Native/ without sources is a module that does not build). A status that cannot be read counts as none.
	[[nodiscard]] bool HasNativeModuleDirectory(const std::filesystem::path& projectRoot);

	// Every .cpp and .h file below <projectRoot>/Native/ (decision 26), project-relative with forward slashes ("Native/Spin.cpp",
	// "Native/Systems/Move.cpp"), sorted by their UTF-8 bytes. Empty when the project has no Native/ directory. Errors: Io.
	// These are every file a build reads from the project: a source includes only NativeApi.h and the module's own .h files,
	// and any other file below Native/ is a lint finding that refuses the build (NATIVE_LINT_ERROR, decision 35), so no
	// compiled input escapes the source hash.
	[[nodiscard]] Result<std::vector<std::string>> ListNativeSources(const std::filesystem::path& projectRoot);

	// The source hash of a module build (decision 30): XXH64 (seed 0), 16 lowercase hex digits, over the UTF-8 text
	// "EngineNativeSources 1\n<Configuration>\n", then for `apiHeader` (the NativeApi.h the module compiles against) and for
	// each of ListNativeSources' files in order: its path ("Engine/Native/NativeApi.h" for the header, the project-relative
	// path for a source), "\n", its size as 8 little-endian bytes and its bytes. Scripts/BuildNative.py computes the same
	// value (Scripts/Lib/native_build.py source_hash), which a test pins. A build is current when its record's SourceHash
	// equals the value now. Errors: Io; NotFound for a missing header.
	[[nodiscard]] Result<std::string> ComputeNativeSourceHash(const std::filesystem::path& projectRoot, ExportConfiguration configuration,
		const std::filesystem::path& apiHeader);

}
