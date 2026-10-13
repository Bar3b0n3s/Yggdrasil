#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

// The generated reference (Architecture §2.3 GenerateDocs.py, §13.10; Docs/Decisions/0022-m14-contract.md decision 13),
// frozen by the M14 contract. `Editor --dump-reference <dir>` writes exactly these files and GenerateDocs.py maps them
// into the repository; `--check` fails on any missing, stale or extra file. Generation is pure and deterministic: the
// same registries give the same bytes on every OS (UTF-8, LF line ends, byte-wise sorting, canonical JsonWriter output
// for JSON, Markdown with escaped table cells).

namespace Engine {

	class ImporterRegistry;
	class MethodRegistry;
	class ScriptApiRegistry;
	class TypeRegistry;

	// The frozen registries the reference describes; borrowed for the call.
	struct ReferenceSources
	{
		const TypeRegistry* Types = nullptr;         // the editor's registry, editor method types registered
		const ScriptApiRegistry* Scripts = nullptr;  // RegisterBindings, frozen
		const MethodRegistry* Methods = nullptr;     // RegisterEditorMethods without test hooks, frozen
		const ImporterRegistry* Importers = nullptr; // RegisterBuiltinImporters
	};

	// One generated file: Path is '/'-separated and relative to the dump directory.
	struct GeneratedReferenceFile
	{
		std::string Path{};
		std::string Content{};
	};

	// Every reference file, sorted by Path:
	//   catalog.json                  MethodRegistry::BuildToolCatalog (the MCP catalogue, unchanged format)
	//   Methods.json                  MethodRegistry::BuildMethodCatalog (unchanged format)
	//   Engine.d.luau, ScriptAPI.md   ScriptApiRegistry::GenerateDefinitions and GenerateDocumentation (unchanged)
	//   Components.md                 every component: category, flags, requirements, fields (type, unit, default, range,
	//                                 enum values, description) and its script proxy members
	//   Automation.md                 every method by domain: description, flags, timeout, parameters and result fields
	//                                 (compact; the full schemas stay in Methods.json), examples
	//   Validation.md                 every project.validate code in §13.7 order (ProjectValidator::GetCodeInfos)
	//   FileFormats.md                every file format (Format/Version header, schema link, example) and every source
	//                                 format by importer and extension (ImporterRegistry, SourceFormat::Importable)
	//   Schemas/<Format>.schema.json  one JSON Schema per authored format: AssetMeta, BatchRequest, GameManifest, Material,
	//                                 Prefab, Project, Provenance, Replay, Scene, SoundEffect; only the subset
	//                                 JsonSchema::Validate supports, unknown components and keys allowed where §6 allows them
	// Errors: InvalidArgument for a missing source; InvalidState for a registry that is not frozen; Validation naming every
	// empty or whitespace-only description (§5.4, gate 7: CollectDescriptions and FindMissingDescriptions of
	// DescriptionInventory.h, so the generator and the gate share one walk).
	[[nodiscard]] Result<std::vector<GeneratedReferenceFile>> GenerateReferences(const ReferenceSources& sources);

	// Writes `files` below `directory` (created with its parents), each atomically (a temporary file, then a rename).
	// Errors: Io naming the file.
	[[nodiscard]] Status WriteReferences(std::span<const GeneratedReferenceFile> files, const std::filesystem::path& directory);

	// `Editor --dump-reference <dir>` (EditorApp calls only this; ADR 0022 decision 13 and its ownership section): builds the
	// method registry (RegisterEditorMethods without test hooks, frozen) over `types`, then writes the reference into
	// `directory`. The M14 contract moved the M13 behaviour here unchanged: Methods.json, catalog.json, Engine.d.luau and
	// ScriptAPI.md. Stream D replaces the body with GenerateReferences (adding the built-in ImporterRegistry) and
	// WriteReferences, so the dump writes the whole file set without another EditorApp change. `types` is the editor's frozen
	// registry with the editor method types registered and `scripts` its frozen script API. Errors: those of generating and
	// writing, naming the file.
	[[nodiscard]] Status DumpReferences(const TypeRegistry& types, const ScriptApiRegistry& scripts, const std::filesystem::path& directory);

}
