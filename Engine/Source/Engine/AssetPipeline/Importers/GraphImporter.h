#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

// GraphImporter (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decision 9; stream G-B): a .graph file to a Script
// asset. Graphs are Script assets told apart by their importer (AssetMetadata::Importer "Graph", decision 2), so its output is
// the same cooked ScriptData a .luau gives, and nothing downstream of the import knows a graph.

namespace Engine {

	// Imports .graph into ScriptData (Asset/ScriptData.h), as ScriptImporter imports .luau, in order:
	//   1. GraphFromText strictly (the context's registry, on which RegisterGraphTypes ran), then a GraphNodeLibrary built
	//      from the context's shared pure script API (ImportContext::GetLoadTimeScriptApi, the PureScriptApi every script
	//      import's load-time VM borrows: Docs/Decisions/0041-script-check-performance.md decision 3), or from a PureScriptApi
	//      built for this import when the context has none, then ValidateGraph: any issue fails the import with Validation,
	//      each issue located (JSON pointer, node) and published as one GRAPH_INVALID finding of the script check below.
	//   2. TranspileGraph with the source's project-relative path, then the load-time extraction of that text
	//      (LoadTimeVm::Extract with the .graph path and the text as the request's source, which the extraction accepts as a
	//      script source path, IsScriptSourcePath in Asset/ScriptData.h; its VM binds the API of step 1, as ScriptImporter's
	//      binds the context's), which must give a Behaviour named the graph's Name:
	//      the cooked ScriptData's source map describes the transpiled text under the .graph path, so a runtime error names
	//      "<path>:<line>" of that text (decision 8), and the script engine instantiates, caches and hot-reloads it as a .luau
	//      behaviour.
	//   3. The script check, published through ImportContext::SetScriptCheck before anything can fail, as ScriptImporter
	//      publishes it: SourceHash the XXH64 of the .graph bytes, EnvironmentHash the diagnostics provider's (so the cache
	//      re-imports every graph when the script API's declarations change, which is also when the library can change:
	//      EditorAssetManager's IsScriptCheckCurrent rule for Script assets), and the diagnostics of ValidateGraph (GRAPH_INVALID,
	//      line and column 0, the message naming the node and the pointer: IScriptDiagnosticsProvider.h's ScriptDiagnostic) and
	//      of the provider's check of the transpiled text under the .graph path (SCRIPT_TYPE_ERROR, SCRIPT_COMPILE_ERROR at their
	//      lines of the text, each message prefixed "node <Id> (<Kind>): " when FindGraphNodeAtLine names one).
	//   4. Import.Dependencies: FindGraphAssetLiterals' handles (every non-null Asset literal, as a component's references are
	//      listed), sorted and unique. A literal Assets.Load path is no dependency: the exporter scans the graph's transpiled text
	//      for its literal and computed loads as it scans a .luau source (decision 24), so a pruned export keeps a literal
	//      load's target and a missing, built-in, sub-asset or excluded target behaves as a Luau script's.
	// Pure function of the source bytes, the importer version and what it read and looked up; identical bytes in every run and
	// configuration. No settings, no sub-assets, no main thread. Errors: Parse and Validation from the document and the
	// library (located); the extraction's CompileFailed, Script or Timeout (located at the text's line, the node named);
	// Validation when the text does not define a Behaviour named Name; Unsupported when the context's registry lacks the graph
	// types. No partial artifact.
	class GraphImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Graph";
		// Bumped whenever the transpiler's output for an existing graph changes (decision 10).
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Script; }
		// ".graph" (GraphFileExtension).
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }
		[[nodiscard]] bool RequiresMainThread() const override { return false; }

		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
