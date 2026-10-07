#pragma once

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The engine cooked cache (Architecture §7.5): engine resources (the Default font's SDF atlas in M6; the built-in HDRI bakes
// and the generated blue-noise texture from M8) are cooked with the project cache's keys into bin/EngineCache/<handle>/<key>.bin,
// mounted as enginecache:// in development builds and shared by every configuration (bakes do not depend on it). It is
// filled by `Editor --headless --bake-engine-assets` (CI.py's bake stage, after the build, §15.8) and by any editor on
// first use; `--renderer none` editors and exports reuse it and never need a GPU, as long as the bakes exist.

namespace Engine {

	class IEnvironmentBaker;
	class ImporterRegistry;
	class JobSystem;
	class TypeRegistry;
	class VirtualFileSystem;

	// The generator of a Generated built-in (BuiltinAssetSource::Generated), provided by the module that owns its code (M8:
	// the Renderer's "BlueNoise", §8.4; Asset cannot call it, §3). Deterministic: identical bytes on every run, host and
	// configuration; no GPU.
	struct EngineAssetGenerator
	{
		using Function = Result<Buffer> (*)();

		std::string_view Id{};       // the catalogue entry's "Generator"
		uint32_t Version = 0;        // >= 1; part of the cache key like an importer's version: bump it when the output changes
		Function Generate = nullptr; // the complete cooked artifact (CookedHeader + payload) of the entry's type; never null
	};

	struct EngineBakeSpecification
	{
		// engine:// (Resources, read-only) and enginecache:// (bin/EngineCache, read-write) must be mounted; never null.
		VirtualFileSystem* Vfs = nullptr;
		const ImporterRegistry* Importers = nullptr;   // never null
		const TypeRegistry* Registry = nullptr;        // frozen; never null
		JobSystem* Jobs = nullptr;                     // imports of different assets run as jobs; never null
		IEnvironmentBaker* EnvironmentBaker = nullptr; // null without a GPU: environment bakes are then skipped
		// The generators of Generated entries, sorted by Id, each once (empty before M8); must outlive the call.
		std::span<const EngineAssetGenerator> Generators{};
	};

	// What one bake run did, each list sorted by handle.
	struct EngineBakeReport
	{
		std::vector<AssetHandle> Baked{};    // imported and stored now
		std::vector<AssetHandle> UpToDate{}; // a valid entry under the current key existed
		// Entries that could not be baked here, each as a diagnostic: an importer or generator this build lacks (Environment
		// before M8) or an importer that needs a GPU without a baker (Warning); an import or generation that failed, or File
		// settings the importer's settings struct rejects (Error, AssetImportFailedCode).
		std::vector<AssetDiagnostic> Skipped{};
	};

	// Bakes every File entry of `catalog` whose importer is registered and every Generated entry whose generator is in the
	// specification; keeps a valid entry, and otherwise imports (or generates) and stores it in enginecache://. Built-ins
	// have no .meta: the catalogue fixes the handle, the importer and the settings. Keys (AssetCache::ComputeKey):
	//   - File: the file's bytes under engine://, the importer id and version, the complete settings (the importer's
	//     defaults with the entry's Settings merged over them, canonical) and EngineCookVersion;
	//   - Generated: no source bytes, the generator id and version, {} and EngineCookVersion.
	// Procedural built-ins are never cached (the Asset module generates them in code). Deterministic: the same inputs leave
	// byte-identical files. Errors: InvalidArgument for a specification without the mounts; Io when the cache cannot be
	// written. A failed import is reported in the result, not as an error, so one broken resource does not hide the
	// others; Editor --bake-engine-assets exits 1 when the report has an Error diagnostic.
	[[nodiscard]] Result<EngineBakeReport> BakeEngineAssets(const EngineBakeSpecification& specification, const BuiltinAssetCatalog& catalog);

	// The cooked artifacts of one File or Generated entry from enginecache://, baking it first when its entry is missing,
	// stale or corrupted (the "first use" path of EditorAssetManager). The main artifact comes first. Errors: NotFound for a
	// Procedural entry; Unsupported when its importer is not registered or needs a GPU this run lacks, or its generator is
	// not in the specification (with the hint "run Editor --headless --bake-engine-assets on a machine with a GPU"); the
	// import's, the generator's and the cache's read errors. A bake that cannot be stored is logged at Warn and still
	// returned: the cache is an optimisation (BakeEngineAssets, the explicit bake, reports the store's error).
	[[nodiscard]] Result<std::vector<Buffer>> GetOrBakeEngineAsset(const EngineBakeSpecification& specification, const BuiltinAssetEntry& entry);

}
