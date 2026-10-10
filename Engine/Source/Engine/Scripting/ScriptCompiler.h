#pragma once

#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <cstdint>
#include <string_view>

namespace Engine {

	enum class ScriptCompileMode : uint8_t
	{
		Chunk,
		Expression,
		ExpressionOrChunk
	};

	struct ScriptCompileRequest
	{
		// Optional diagnostic origin: empty for fileless eval, otherwise canonical below project://Assets (a real
		// .replay is valid). No existence check, .luau suffix requirement or implicit require-resolver operation.
		VfsPath Path{};
		std::string_view Source{}; // exact authored UTF-8, borrowed for Compile only
		ScriptCompileMode Mode = ScriptCompileMode::Chunk;
		// Optional stable Luau diagnostic name, borrowed for Compile only: @Assets/... for a file or =label for a
		// synthetic chunk, e.g. =replay/<handle>/Expect/0. Empty selects @Path.GetPath(), or =eval when Path is empty.
		std::string_view ChunkName{};
		// RFC 6901 pointer inside the diagnostic document, e.g. /Expect/0/Luau or /code; empty for a standalone chunk.
		// Borrowed for Compile only. Diagnostic line/column remain one-based WITHIN Source, not JSON text.
		std::string_view JsonPointer{};
	};

	struct ScriptCompilation
	{
		Buffer Bytecode{};
		ScriptSourceMap SourceMap{};
	};

	// The only source-to-bytecode boundary used by AssetPipeline and Scripting. No public Luau types or compiler
	// options; engine producers share exactly one policy. Independent calls are thread-safe after process startup.
	class ScriptCompiler
	{
	public:
		// Requires InitializeScriptingRuntime before any compile/analysis worker starts. No global flag mutation here.
		// Uses the pinned public compiler API: default optimizationLevel 1, debugLevel 1, typeInfoLevel/coverageLevel 0,
		// and mutableGlobals containing math. This excludes builtin math fastcalls/import assumptions and folding,
		// including aliases even when source optimize/native directives raise the upstream optimization level.
		// Such directives retain upstream semantics; no native code generator runs. Sandbox also clears safeenv.
		// Preserves the documented ^ operator semantics (§4.12); no AST/opcode rewriting. Non-trivial powers require
		// the cross-configuration determinism gate; math.pow uses DetMath. Actual mismatches must be reported.
		// Chunk compiles Source verbatim. Expression must parse as exactly one complete expression (trailing comments
		// allowed), then compiles "return (\n" + Source + "\n)". ExpressionOrChunk tries that complete expression FIRST,
		// otherwise a complete chunk; a prefix parse is never enough. A chunk already containing return is not wrapped.
		// The public parser API selects the mode without execution; no AST/opcode transformation or ^ lowering.
		// SourceMap records the selected ChunkName, Path.GetPath() (possibly empty), JsonPointer, and GeneratedPrefixLines
		// (1 for the expression wrapper, 0 for verbatim chunks). XXH64, byte length and LineOffsets always describe the
		// AUTHORED Source. Compile/runtime locations subtract the prefix once; unknown/generated-only runtime positions
		// stay 0. Compile errors in a generated suffix anchor at authored EOF with wrapper context; never underflow.
		// No filesystem read or RequireResolver call.
		// Compile errors carry the origin file/pointer and one-based authored line/column. Identical inputs
		// produce identical bytes/maps across Debug/Release. Error-encoded compiler output is never successful bytecode.
		// Errors: Validation for invalid UTF-8/path or excessive size; CompileFailed with source location for syntax;
		// InvalidState before process initialization. Dist: Unsupported before any compiler call (no compiler reference).
		[[nodiscard]] static Result<ScriptCompilation> Compile(const ScriptCompileRequest& request);
	};

}
