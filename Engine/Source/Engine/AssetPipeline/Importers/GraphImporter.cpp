#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/GraphImporter.h"

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"
#include "Engine/Core/Hash.h"
#include "Engine/Platform/ThreadClock.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/LoadTimeVm.h"
#include "Engine/Scripting/Sandbox.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <algorithm>
#include <format>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// GraphImporter (Docs/Decisions/0035-m24-contract.md decision 9): stream G-B implements Import, and stream G-A registers the
// importer (RegisterBuiltinImporters, Asset/SourceFormats.cpp's row).

namespace Engine {

	namespace Utils {

		constexpr std::string_view GraphCompileErrorCode = "SCRIPT_COMPILE_ERROR";

		// A graph requires no module (decision 2), so its text's reader refuses every read.
		class GraphModuleReader final : public IScriptModuleReader
		{
		public:
			Result<std::string> ReadModule(const VfsPath& path) override
			{
				return MakeError(ErrorCode::Validation, "a graph requires no module ('{}')", path.ToString());
			}
		};

		// "node <Id> (<Kind>): " for the node whose code line `line` of the text is, or "".
		[[nodiscard]] static std::string GetNodePrefix(const GraphDocument& graph, const GraphTranspileResult& text, uint32_t line)
		{
			const uint32_t node = FindGraphNodeAtLine(text, line);
			const GraphNode* found = node != 0 ? FindGraphNode(graph, node) : nullptr;
			return found != nullptr ? std::format("node {} ({}): ", found->Id, found->Kind) : std::string();
		}

		static void SortFindings(std::vector<ScriptDiagnostic>& findings)
		{
			std::ranges::stable_sort(findings, [](const ScriptDiagnostic& left, const ScriptDiagnostic& right)
			{
				return std::tie(left.File, left.Line, left.Column, left.Code, left.Message, left.EndLine, left.EndColumn, left.Severity)
					< std::tie(right.File, right.Line, right.Column, right.Code, right.Message, right.EndLine, right.EndColumn, right.Severity);
			});
		}

		// One GRAPH_INVALID finding of the graph itself, at line and column 0, its message naming the pointer and the node
		// (IScriptDiagnosticsProvider.h's ScriptDiagnostic).
		[[nodiscard]] static ScriptDiagnostic MakeGraphFinding(std::string_view file, std::string_view pointer, std::string_view message)
		{
			return { .Severity = DiagnosticSeverity::Error,
				.Code = std::string(GraphInvalidCode),
				.File = std::string(file),
				.Line = 0,
				.Column = 0,
				.Message = pointer.empty() ? std::string(message) : std::format("{}: {}", pointer, message),
				.EndLine = 0,
				.EndColumn = 0 };
		}

		// A document that does not read: one finding per located issue, or the error's own message.
		static void AppendDocumentFailure(ScriptImportCheck& check, const Error& error, std::string_view file)
		{
			if (error.GetIssues().empty())
			{
				const ErrorLocation& location = error.GetLocation();
				std::string message = error.GetMessageText();
				if (location.Line != 0)
					message = std::format("line {}, column {}: {}", location.Line, location.Column, message);
				check.Diagnostics.push_back(MakeGraphFinding(file, location.JsonPointer.value_or(std::string()), message));
				return;
			}
			for (const ErrorIssue& issue : error.GetIssues())
				check.Diagnostics.push_back(MakeGraphFinding(file, issue.JsonPointer, issue.Message));
		}

		// The text's compile, load-time or extraction failure as one located compile finding naming its node, unless the
		// checker already reported a compile error on that line (ScriptImporter's rule). Infrastructure failures are not
		// script findings.
		static void AppendTextFailure(ScriptImportCheck& check, const Error& error, std::string_view file, const GraphDocument& graph, const GraphTranspileResult& text)
		{
			const ErrorCode code = error.GetCode();
			if (code == ErrorCode::Io || code == ErrorCode::PermissionDenied || code == ErrorCode::InvalidState || code == ErrorCode::Unsupported)
				return;
			const ErrorLocation& location = error.GetLocation();
			const uint32_t line = location.Line != 0 ? location.Line : 1;
			ScriptDiagnostic finding{ .Severity = DiagnosticSeverity::Error,
				.Code = std::string(GraphCompileErrorCode),
				.File = std::string(file),
				.Line = line,
				.Column = location.Line != 0 && location.Column != 0 ? location.Column : 1,
				.Message = GetNodePrefix(graph, text, line) + error.GetMessageText(),
				.EndLine = 0,
				.EndColumn = 0 };
			if (std::ranges::any_of(check.Diagnostics, [&finding](const ScriptDiagnostic& existing)
			{
				return existing.Code == finding.Code && existing.File == finding.File && existing.Line == finding.Line;
			}))
				return;
			check.Diagnostics.push_back(std::move(finding));
			SortFindings(check.Diagnostics);
		}

	}

	std::span<const std::string_view> GraphImporter::GetExtensions() const
	{
		static constexpr std::string_view Extensions[] = { GraphFileExtension };
		return Extensions;
	}

	Result<ImportResult> GraphImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const VfsPath& source = context.GetSourcePath();
		const std::string file(source.GetPath());
		ScriptImportCheck check;
		check.SourceHash = XXH64(context.GetSourceBytes());
		IScriptDiagnosticsProvider* provider = context.GetScriptDiagnostics();
		if (provider != nullptr)
		{
			check.Performed = true;
			check.EnvironmentHash = provider->GetEnvironmentHash();
		}
		// Published before anything can fail, then again with each attempt's findings (ScriptImporter's rule).
		context.SetScriptCheck(check);
		const auto located = [&file](Error error)
		{
			ErrorLocation location = error.GetLocation();
			if (location.File.empty())
				location.File = file;
			return std::unexpected(std::move(error).WithLocation(std::move(location)));
		};

		const TypeRegistry& types = context.GetRegistry();
		if (types.FindStruct("Graph") == nullptr)
			return located(Error(ErrorCode::Unsupported, "the import's type registry has no graph types (RegisterGraphTypes)"));
		GraphLoadReport report;
		Result<GraphDocument> graph = GraphFromText(AsStringView(context.GetSourceBytes()), types, report, true);
		if (!graph.has_value())
		{
			Utils::AppendDocumentFailure(check, graph.error(), file);
			context.SetScriptCheck(check);
			return located(std::move(graph).error());
		}

		// The script API the library is built from and the load-time VM binds: the context's shared pure API, as a .luau import's
		// VM borrows it (Docs/Decisions/0041-script-check-performance.md decision 3), or, in a context without one (a test or a
		// tool), one built for this import, which both then use.
		Scope<PureScriptApi> ownApi;
		ScriptApiRegistry* api = context.GetLoadTimeScriptApi();
		if (api == nullptr)
		{
			ENGINE_TRY_ASSIGN(ownApi, PureScriptApi::Create());
			api = &ownApi->GetApi();
		}
		ENGINE_TRY_ASSIGN(const Scope<GraphNodeLibrary> library, GraphNodeLibrary::Create(*api));
		const std::vector<GraphIssue> issues = ValidateGraph(*graph, *library);
		if (!issues.empty())
		{
			std::vector<ErrorIssue> errorIssues;
			for (const GraphIssue& issue : issues)
			{
				check.Diagnostics.push_back(Utils::MakeGraphFinding(file, issue.JsonPointer, issue.Message));
				errorIssues.push_back({ .JsonPointer = issue.JsonPointer, .Message = issue.Message, .Hint = {}, .Suggestions = {} });
			}
			context.SetScriptCheck(check);
			return located(Error(ErrorCode::Validation, std::format("graph '{}' does not validate: {} issue{}", file, issues.size(), issues.size() == 1 ? "" : "s"))
					.WithIssues(std::move(errorIssues)));
		}
		Result<GraphTranspileResult> text = TranspileGraph(*graph, *library, file);
		if (!text.has_value())
		{
			Utils::AppendDocumentFailure(check, text.error(), file);
			context.SetScriptCheck(check);
			return located(std::move(text).error());
		}

		Utils::GraphModuleReader modules;
		const ScriptCheckRequest request{ source, text->Luau, &modules };
		if (provider != nullptr)
		{
			check.Diagnostics = provider->CheckScript(request);
			for (ScriptDiagnostic& diagnostic : check.Diagnostics)
			{
				if (diagnostic.File == file && diagnostic.Line != 0)
					diagnostic.Message = Utils::GetNodePrefix(*graph, *text, diagnostic.Line) + diagnostic.Message;
			}
			context.SetScriptCheck(check);
		}
		const auto fail = [&context, &check, &file, &graph, &text, &located](Error error) -> Result<ImportResult>
		{
			Utils::AppendTextFailure(check, error, file, *graph, *text);
			context.SetScriptCheck(check);
			const uint32_t line = error.GetLocation().Line;
			if (const std::string prefix = Utils::GetNodePrefix(*graph, *text, line); !prefix.empty())
				return located(std::move(error).WithContext(prefix.substr(0, prefix.size() - 2)));
			return located(std::move(error));
		};
		// The load-time VM's budget counts this import thread's CPU time, as ScriptImporter's does, and it binds the API above.
		Result<Ref<const ScriptData>> extracted = LoadTimeVm::Extract(request, LoadTimeVmSpecification{ .ClockSeconds = &ThreadClock::GetCpuSeconds, .Api = api });
		if (!extracted.has_value())
			return fail(std::move(extracted).error());
		const ScriptData& script = **extracted;
		if (script.Kind != ScriptKind::Behaviour || script.Name != graph->Name)
			return fail(Error(ErrorCode::Validation, std::format("the graph's text does not define the Behaviour '{}'", graph->Name)));
		ENGINE_TRY_ASSIGN(Buffer cooked, CookScript(script, Version));

		ImportResult result;
		result.Artifacts.push_back({ metadata.Handle, AssetType::Script, {}, std::move(cooked) });
		for (const AssetHandle handle : FindGraphAssetLiterals(*graph, *library).Handles)
		{
			if (handle != metadata.Handle)
				result.Dependencies.push_back(handle);
		}
		return result;
	}

}
