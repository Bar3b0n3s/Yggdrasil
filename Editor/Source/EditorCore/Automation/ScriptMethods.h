#pragma once

#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Reuse Asset's complete §11.9 finding, including EndLine/EndColumn. Register it once as ScriptDiagnostic with
	// camelCase keys. Runtime/compile findings with no known range retain zero end positions rather than inventing one.
	using ScriptDiagnosticSummary = ScriptDiagnostic;

	struct ScriptCreateParams
	{
		std::string Path{};                                  // Required project path below Assets, ending in .luau.
		ScriptTemplate Template = ScriptTemplate::Behaviour; // Required; Behaviour, Module, Test.
	};

	struct ScriptCreateResult
	{
		AssetSummary Script{};
		std::vector<ScriptDiagnosticSummary> Diagnostics{};
		uint32_t UndoIndex = 0;
	};

	struct ScriptReadParams
	{
		std::string Path{};
	};

	struct ScriptReadResult
	{
		AssetSummary Script{};
		std::string Source{}; // Exact UTF-8 bytes, with authored newlines; large responses use normal offloading.
	};

	struct ScriptWriteParams
	{
		std::string Path{};
		std::string Source{}; // Required; an empty source is valid input and is written exactly.
	};

	struct ScriptWriteResult
	{
		AssetSummary Script{};
		std::vector<ScriptDiagnosticSummary> Diagnostics{};
		uint32_t UndoIndex = 0;
	};

	struct ScriptCheckParams
	{
		std::vector<std::string> Paths{}; // Omitted/empty: every project script; requires are checked transitively.
	};

	struct ScriptCheckResult
	{
		std::vector<std::string> Paths{};
		std::vector<ScriptDiagnosticSummary> Diagnostics{};
		bool Passed = false; // No Error-severity diagnostic; warnings remain in diagnostics.
	};

	struct ScriptFieldsParams
	{
		std::string Script{}; // Required Script AssetRef, resolved through the common asset resolver.
	};

	struct ScriptFieldSummary
	{
		std::string Name{};
		FieldType Type = FieldType::Float; // Registry enum, describing the underlying reflected value type.
		VariantValue Default{};            // Full reflected JSON value, including null references.
		std::string Tooltip{};
		// A complete JSON Schema value from the reflected field plus its persistent ScriptFieldSchema metadata.
		// Includes enum choices, asset type, numeric limits/step and recursive array element metadata/defaults.
		// Variant is intentional: JSON Schema is polymorphic, not an untyped substitute for diagnostics.
		VariantValue Schema{};
	};

	struct ScriptFieldsResult
	{
		AssetSummary Script{};
		ScriptKind Kind = ScriptKind::Module;
		std::string Name{};                       // Script.Define/Test.Suite's name, empty for a Module.
		std::vector<ScriptFieldSummary> Fields{}; // Canonical name order; empty for Module/TestSuite.
	};

	namespace Automation {

		// All handlers are main-thread, delegate to EditorScriptService and preserve owned result data. Paths use
		// ResolveProjectPath then the service's Assets/.luau confinement. Errors name /path, /paths/i or /script.
		// Create/write effects go through AssetEditCommand and the editor's write/provenance path, one undo step each.
		// Dry runs compile/check inside the overlay and leave no source, meta, watcher notification or provenance.
		// Existing sources are AlreadyExists for create; writes may create. Syntax/type findings are result diagnostics,
		// while file/permission/infrastructure failures are Result errors. Read/fields return NotFound for absent assets.
		[[nodiscard]] Result<ScriptCreateResult> ScriptCreate(EditorMethodContext& context, const ScriptCreateParams& params);
		[[nodiscard]] Result<ScriptReadResult> ScriptRead(EditorMethodContext& context, const ScriptReadParams& params);
		[[nodiscard]] Result<ScriptWriteResult> ScriptWrite(EditorMethodContext& context, const ScriptWriteParams& params);
		[[nodiscard]] Result<ScriptCheckResult> ScriptCheck(EditorMethodContext& context, const ScriptCheckParams& params);
		[[nodiscard]] Result<ScriptFieldsResult> ScriptFields(EditorMethodContext& context, const ScriptFieldsParams& params);

	}

	// Registers ScriptTemplate, ScriptKind and FieldType once (none currently has a reflected enum registration);
	// DiagnosticSeverity reuses RegisterAutomationTypes. Registers full diagnostic ranges and nested results.
	void RegisterEditorScriptMethodTypes(TypeRegistry& registry);
	// Five editor-only methods, none in the launcher. create/read/write/check are tools; fields uses engine_call.
	// create/write: Mutates, SupportsDryRun, AllowedInBatch. read/check/fields: pure reads, AllowedInBatch, no dry run.
	void RegisterEditorScriptMethods(MethodRegistry& methods);

}
