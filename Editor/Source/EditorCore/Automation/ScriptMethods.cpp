#include "EditorPCH.h"
#include "EditorCore/Automation/ScriptMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

	namespace {

		Result<EditorScriptService*> RequireScriptService(EditorMethodContext& context)
		{
			EditorScriptService* service = context.GetEditor().GetScriptService();
			if (service == nullptr)
				return MakeError(ErrorCode::InvalidState, "the editor script service is unavailable");
			return service;
		}

		// Decision 2 of Docs/Decisions/0035-m24-contract.md: a graph is a Script asset, but its source is a .graph document
		// that only the graph methods read and edit, so the script source methods refuse it at `pointer`, naming them.
		Status RefuseGraphSource(std::string_view path, std::string_view pointer, std::string_view hint)
		{
			if (!path.ends_with(GraphFileExtension))
				return {};
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
				std::format("'{}' is a visual-scripting graph, not a Luau script", path), std::string(hint)));
		}

		// A content hash as the results report it, the form a replay's start fingerprint records (16 lowercase hex digits);
		// empty for none.
		std::string FormatContentHash(const std::optional<uint64_t>& hash)
		{
			return hash.has_value() ? UUID(*hash).ToString() : std::string();
		}

		constexpr std::string_view GraphEditHint = "read a graph with graph.read and change it with graph.addNode, graph.connect, graph.setValues, "
												   "graph.setMembers and the other graph edits";

		void AddScriptDefaults(Json& schema, const ScriptFieldSchema& field)
		{
			schema["default"] = field.DefaultValue.Get();
			if (field.Meta.Step.has_value())
				schema["x-step"] = *field.Meta.Step;
			if (field.Element != nullptr)
				AddScriptDefaults(schema["items"], *field.Element);
		}

	}

	namespace Automation {

		Result<ScriptCreateResult> ScriptCreate(EditorMethodContext& context, const ScriptCreateParams& params)
		{
			ENGINE_TRY(RefuseGraphSource(params.Path, "/path", "script.create makes Luau scripts; graph.create makes a graph"));
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveAssetsPath(context, params.Path, "/path", ".luau"));
			ENGINE_TRY_ASSIGN(EditorScriptService * service, RequireScriptService(context));
			const auto written = service->Create(path, params.Template);
			if (!written)
				return std::unexpected(Utils::LocateAtParam(written.error(), "/path"));
			return ScriptCreateResult{ .Script = Utils::MakeAssetSummary(context.GetEditor().GetAssets(), written->Script),
				.Diagnostics = written->Diagnostics,
				.UndoIndex = ToAutomationCounter(written->UndoIndex) };
		}

		Result<ScriptReadResult> ScriptRead(EditorMethodContext& context, const ScriptReadParams& params)
		{
			ENGINE_TRY(RefuseGraphSource(params.Path, "/path", GraphEditHint));
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveAssetsPath(context, params.Path, "/path", ".luau"));
			ENGINE_TRY_ASSIGN(EditorScriptService * service, RequireScriptService(context));
			const auto source = service->Read(path);
			if (!source)
				return std::unexpected(Utils::LocateAtParam(source.error(), "/path"));
			ENGINE_TRY_ASSIGN(const AssetHandle script, Utils::ResolveAssetParam(context, path.GetPath(), "/path"));
			const AssetMetadata* metadata = context.GetEditor().GetAssets().GetMetadata(script);
			ENGINE_VERIFY(metadata != nullptr, "The registry holds the metadata of the script '{}' it resolved", path.GetPath());
			const auto hash = service->GetCookedHash(path, *source, *metadata);
			if (!hash)
				return std::unexpected(Utils::LocateAtParam(hash.error(), "/path"));
			return ScriptReadResult{ .Script = Utils::MakeAssetSummary(context.GetEditor().GetAssets(), script),
				.Source = *source,
				.LineCount = CountScriptLines(*source),
				.Hash = FormatContentHash(*hash) };
		}

		Result<ScriptWriteResult> ScriptWrite(EditorMethodContext& context, const ScriptWriteParams& params)
		{
			ENGINE_TRY(RefuseGraphSource(params.Path, "/path", GraphEditHint));
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveAssetsPath(context, params.Path, "/path", ".luau"));
			ENGINE_TRY_ASSIGN(EditorScriptService * service, RequireScriptService(context));
			const auto written = service->Write(path, params.Source);
			if (!written)
				return std::unexpected(Utils::LocateAtParam(written.error(), "/path"));
			VariantValue changedLines;
			if (written->ChangedLines.has_value())
			{
				ENGINE_TRY_ASSIGN(Json serialized, context.SerializeResult(*written->ChangedLines));
				changedLines = VariantValue(std::move(serialized));
			}
			return ScriptWriteResult{ .Script = Utils::MakeAssetSummary(context.GetEditor().GetAssets(), written->Script),
				.Diagnostics = written->Diagnostics,
				.UndoIndex = ToAutomationCounter(written->UndoIndex),
				.LineCount = CountScriptLines(params.Source),
				.Hash = FormatContentHash(written->CookedHash),
				.ChangedLines = std::move(changedLines) };
		}

		Result<ScriptCheckResult> ScriptCheck(EditorMethodContext& context, const ScriptCheckParams& params)
		{
			ENGINE_TRY_ASSIGN(EditorScriptService * service, RequireScriptService(context));
			std::vector<VfsPath> paths;
			for (size_t index = 0; index < params.Paths.size(); ++index)
			{
				ENGINE_TRY(RefuseGraphSource(params.Paths[index], std::format("/paths/{}", index), "a graph's findings are graph.read's and project.validate's"));
				ENGINE_TRY_ASSIGN(VfsPath path, Utils::ResolveAssetsPath(context, params.Paths[index], std::format("/paths/{}", index), ".luau"));
				paths.push_back(std::move(path));
			}
			auto checked = service->Check(paths);
			if (!checked)
			{
				for (size_t index = 0; index < paths.size(); ++index)
					if (checked.error().GetLocation().File == paths[index].ToString() || checked.error().GetLocation().File == paths[index].GetPath())
						return std::unexpected(Utils::LocateAtParam(checked.error(), std::format("/paths/{}", index)));
				return std::unexpected(checked.error());
			}
			const bool passed = std::ranges::none_of(checked->Diagnostics, [](const ScriptDiagnostic& diagnostic)
			{
				return diagnostic.Severity == DiagnosticSeverity::Error;
			});
			return ScriptCheckResult{ .Paths = std::move(checked->Paths), .Diagnostics = std::move(checked->Diagnostics), .Passed = passed };
		}

		Result<ScriptFieldsResult> ScriptFields(EditorMethodContext& context, const ScriptFieldsParams& params)
		{
			ENGINE_TRY_ASSIGN(EditorScriptService * service, RequireScriptService(context));
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Script, "/script"));
			const auto loaded = service->GetFields(handle);
			if (!loaded)
				return std::unexpected(Utils::LocateAtParam(loaded.error(), "/script"));
			const AssetRef<ScriptData>& script = *loaded;
			ENGINE_TRY_ASSIGN(const auto schema, ScriptFieldSchemaSource::Create({ { handle, script } }));
			ScriptFieldsResult result{ .Script = Utils::MakeAssetSummary(context.GetEditor().GetAssets(), handle), .Kind = script->Kind, .Name = script->Name };
			for (const ScriptFieldSchema& field : script->Fields)
			{
				ENGINE_TRY_ASSIGN(const FieldInfo* reflected, schema->FindField(handle, field.Name));
				// A Script component's Fields value in params: an Entity field also takes an id prefix or an entity path (ADR 0030
				// decision 7).
				Json valueSchema = JsonSchema::ForField(*reflected, EntityReferenceSchema::IdOrPath);
				AddScriptDefaults(valueSchema, field);
				result.Fields.push_back({ .Name = field.Name, .Type = field.Type, .Default = field.DefaultValue, .Tooltip = field.Tooltip, .Schema = VariantValue(std::move(valueSchema)) });
			}
			return result;
		}

	}

	void RegisterEditorScriptMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<ScriptTemplate>("ScriptTemplate", "The shipped template used to create a source file.")
			.Entry(ScriptTemplate::Behaviour, "Behaviour", "An attachable behaviour class.")
			.Entry(ScriptTemplate::Module, "Module", "A reusable required module.")
			.Entry(ScriptTemplate::Test, "Test", "A Test.Suite source.");
		registry.Enum<ScriptKind>("ScriptKind", "Authenticated kind of the script's load-time return value.")
			.Entry(ScriptKind::Behaviour, "Behaviour", "An attachable Script.Define class.")
			.Entry(ScriptKind::Module, "Module", "A required module.")
			.Entry(ScriptKind::TestSuite, "TestSuite", "A Test.Suite definition.");
		auto fieldTypes = registry.Enum<FieldType>("FieldType", "The underlying reflected value kind.");
		for (const FieldType type : std::array{ FieldType::Bool, FieldType::Int32, FieldType::UInt32, FieldType::Float,
				 FieldType::Vec2, FieldType::Vec3, FieldType::Vec4, FieldType::Quat, FieldType::Color3, FieldType::Color4, FieldType::Bool3,
				 FieldType::String, FieldType::EntityRef, FieldType::AssetRef, FieldType::Enum, FieldType::Array, FieldType::Struct, FieldType::Map, FieldType::Variant })
			fieldTypes.Entry(type, FieldTypeToString(type), std::format("A reflected {} value.", FieldTypeToString(type)));
		registry.Struct<ScriptDiagnostic>("ScriptDiagnostic", "A script finding with its full authored source range.")
			.Field("severity", &ScriptDiagnostic::Severity, "Error or Warning.")
			.Field("code", &ScriptDiagnostic::Code, "Stable diagnostic code.")
			.Field("file", &ScriptDiagnostic::File, "Authored source path, including required modules.")
			.Field("line", &ScriptDiagnostic::Line, "One-based starting line, zero when unknown.")
			.Field("column", &ScriptDiagnostic::Column, "One-based UTF-8 starting byte column, zero when unknown.")
			.Field("message", &ScriptDiagnostic::Message, "Actionable finding.")
			.Field("endLine", &ScriptDiagnostic::EndLine, "Exclusive ending line, zero when unknown.")
			.Field("endColumn", &ScriptDiagnostic::EndColumn, "Exclusive ending UTF-8 byte column, zero when unknown.");
		registry.Struct<ScriptCreateParams>("ScriptCreateParams", "Create a script from a shipped template.")
			.Field("path", &ScriptCreateParams::Path, "New .luau file below Assets.")
			.Field("template", &ScriptCreateParams::Template, "Behaviour, Module or Test.");
		registry.Struct<ScriptCreateResult>("ScriptCreateResult", "Created script, findings and undo step.")
			.Field("script", &ScriptCreateResult::Script, "Created script identity.")
			.Field("diagnostics", &ScriptCreateResult::Diagnostics, "Complete compile, extraction and type findings.")
			.Field("undoIndex", &ScriptCreateResult::UndoIndex, "Undo step, zero during dry run.");
		registry.Struct<ScriptReadParams>("ScriptReadParams", "Read a script's authored source.")
			.Field("path", &ScriptReadParams::Path, "Existing .luau path below Assets.");
		registry.Struct<ScriptReadResult>("ScriptReadResult", "Exact source bytes, script identity, line count and content hash.")
			.Field("script", &ScriptReadResult::Script, "Script identity.")
			.Field("source", &ScriptReadResult::Source, "Exact UTF-8 source, preserving authored newlines.")
			.Field("lineCount", &ScriptReadResult::LineCount,
				"The source's lines: each \\n ends one and a last line without one counts too (0 for an empty source); a \\r before a "
				"\\n belongs to its line. They are the lines diagnostics number.")
			.Field("hash", &ScriptReadResult::Hash,
				"The source's content hash, 16 lowercase hex digits: the XXH64 of the artifact the editor cooks from these bytes as this "
				"script (the artifact also holds its path), the value a replay's start fingerprint records for it (its Assets entry's "
				"Hash). Any changed byte changes it, so two reads of the script with the same hash hold the same source. Empty when the "
				"source does not cook (a compile or load-time error, an unresolved require).");
		registry.Struct<ScriptWriteParams>("ScriptWriteParams", "Replace or create script source.")
			.Field("path", &ScriptWriteParams::Path, "A .luau file below Assets.")
			.Field("source", &ScriptWriteParams::Source, "Exact UTF-8 source; empty is permitted.");
		registry.Struct<ScriptLineRange>("ScriptLineRange", "A range of a script's lines, as a diagnostic's: from line up to the exclusive endLine.")
			.Field("line", &ScriptLineRange::Line, "One-based first line; for an empty range, the line it sits before.")
			.Field("endLine", &ScriptLineRange::EndLine, "Exclusive ending line; equal to line for an empty range, which holds no line.");
		registry.Struct<ScriptChangedLines>("ScriptChangedLines", "The single span of lines a script.write changed, in the replaced source and in the written one.")
			.Field("before", &ScriptChangedLines::Before, "The replaced source's lines in the span; empty where lines were only added.")
			.Field("after", &ScriptChangedLines::After, "The written source's lines in the span; empty where lines were only removed.");
		registry.Struct<ScriptWriteResult>("ScriptWriteResult", "Written source, findings, undo step, line count, content hash and changed lines.")
			.Field("script", &ScriptWriteResult::Script, "Stable script identity.")
			.Field("diagnostics", &ScriptWriteResult::Diagnostics, "Findings do not discard the written source.")
			.Field("undoIndex", &ScriptWriteResult::UndoIndex, "Undo step; zero for unchanged bytes or dry run.")
			.Field("lineCount", &ScriptWriteResult::LineCount, "The written source's lines, counted as script.read's lineCount.")
			.Field("hash", &ScriptWriteResult::Hash,
				"The written source's content hash, as script.read reports it: the value a replay's start fingerprint records for this "
				"script while these bytes are its source. Empty when they do not cook.")
			.Field("changedLines", &ScriptWriteResult::ChangedLines,
				"{before: {line, endLine}, after: {line, endLine}} (ScriptChangedLines): a single span from the first to the last "
				"changed line, not a diff. It holds the lines that differ from the source the write replaced once the lines both share "
				"at their start and at their end are set aside, compared with their line ends, as before (the replaced source's lines) "
				"and after (the written source's); unchanged lines between two edits lie inside it. A range is empty (line equal to "
				"endLine) where nothing was removed or added; a new script's before is empty at line 1 and its after the whole file; "
				"null when the bytes are unchanged.");
		registry.Struct<ScriptCheckParams>("ScriptCheckParams", "Check scripts without changing files or play state.")
			.Field("paths", &ScriptCheckParams::Paths, "Scripts to check; omitted or empty checks all project scripts.");
		registry.Struct<ScriptCheckResult>("ScriptCheckResult", "Checked roots and transitive diagnostics.")
			.Field("paths", &ScriptCheckResult::Paths, "Unique checked roots in canonical order.")
			.Field("diagnostics", &ScriptCheckResult::Diagnostics, "Unique findings with complete ranges.")
			.Field("passed", &ScriptCheckResult::Passed, "No finding has Error severity.");
		registry.Struct<ScriptFieldsParams>("ScriptFieldsParams", "Inspect an extracted script schema.")
			.Field("script", &ScriptFieldsParams::Script, "Script asset handle or path.");
		registry.Struct<ScriptFieldSummary>("ScriptFieldSummary", "One persistent script field and its recursive schema.")
			.Field("name", &ScriptFieldSummary::Name, "Authored field name.")
			.Field("type", &ScriptFieldSummary::Type, "Reflected value kind.")
			.Field("default", &ScriptFieldSummary::Default, "Default reflected JSON value.")
			.Field("tooltip", &ScriptFieldSummary::Tooltip, "Authored help text.")
			.Field("schema", &ScriptFieldSummary::Schema, "Complete field JSON Schema, including nested defaults and metadata; an Entity field also takes an id prefix of 6 to 15 hex digits or an entity path in component data.");
		registry.Struct<ScriptFieldsResult>("ScriptFieldsResult", "Current immutable script schema.")
			.Field("script", &ScriptFieldsResult::Script, "Script identity.")
			.Field("kind", &ScriptFieldsResult::Kind, "Behaviour, Module or TestSuite.")
			.Field("name", &ScriptFieldsResult::Name, "Declared class/suite name; empty for Module.")
			.Field("fields", &ScriptFieldsResult::Fields, "Fields in canonical name order; empty for Module and TestSuite.");
	}

	void RegisterEditorScriptMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "script.create", .Description = "Creates a script from a shipped template as one undoable source/meta edit.", .RequiredParams = { "path", "template" }, .ExposeAsTool = true, .Mutates = true, .SupportsDryRun = true, .AllowedInBatch = true, .Examples = { { .Description = "Create a behaviour.", .Params = Json{ { "path", "Assets/Scripts/Ball.luau" }, { "template", "Behaviour" } } } } }, &Automation::ScriptCreate);
		methods.Add({ .Name = "script.read", .Description = "Reads exact script source and its stable asset identity, with its line count and content hash (the hash a replay's start fingerprint records for the script), so two reads with the same hash hold the same source.", .RequiredParams = { "path" }, .ExposeAsTool = true, .AllowedInBatch = true, .Examples = { { .Description = "Read Ball.", .Params = Json{ { "path", "Assets/Scripts/Ball.luau" } } } } }, &Automation::ScriptRead);
		methods.Add({ .Name = "script.write", .Description = "Writes source as one undoable edit and returns full compile/type diagnostics; invalid source remains saved. The result also gives the written source's line count and content hash (as script.read reports them) and changedLines, a single span from the first to the last changed line against the replaced source (its old and new line ranges, not a diff; null for unchanged bytes), so an edit can be checked to change only the lines it meant to. A dry run reports the same.", .RequiredParams = { "path", "source" }, .ExposeAsTool = true, .Mutates = true, .SupportsDryRun = true, .AllowedInBatch = true, .Examples = { { .Description = "Write a module.", .Params = Json{ { "path", "Assets/Scripts/Values.luau" }, { "source", "return { Value = 1 }" } } } } }, &Automation::ScriptWrite);
		methods.Add({ .Name = "script.check", .Description = "Checks selected or all project scripts without writing or reloading code.", .ExposeAsTool = true, .AllowedInBatch = true, .Examples = { { .Description = "Check all project scripts.", .Params = Json::object() } } }, &Automation::ScriptCheck);
		methods.Add({ .Name = "script.fields", .Description = "Inspects the current extracted fields, defaults and recursive metadata of a script asset.", .RequiredParams = { "script" }, .AllowedInBatch = true, .Examples = { { .Description = "Inspect Ball's fields.", .Params = Json{ { "script", "Assets/Scripts/Ball.luau" } } } } }, &Automation::ScriptFields);
	}

}
