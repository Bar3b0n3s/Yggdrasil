#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	// Imports .luau (including .test.luau) into ScriptData. LoadTimeVm::Extract supplies engine-compiled bytecode,
	// authenticated Behaviour/Module/TestSuite kind, field descriptors, source map and unresolved require edges.
	// Each import owns its isolated VM, source-reader cache and 250 ms whole-graph budget. Concurrent calls are safe
	// after main-thread scripting process initialization; no live scene, Session or EditorCore service is accessed.
	class ScriptImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Script";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Script; }
		// ".luau"; TestSuite classification is by the returned value, never the filename.
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }
		[[nodiscard]] bool RequiresMainThread() const override { return false; }

		// SourceBytes is authoritative, even when it differs from the VFS. Required source uses IScriptModuleReader
		// backed by ReadDependency and a per-import byte cache; FindAsset resolves each edge to a standalone Script
		// handle. Both read hashes and handle dependencies are recorded. ListDependencyFiles remains the empty base
		// implementation: requires are standalone assets, not owned dependency-file metas. No sub-assets or settings.
		// Checks the same source graph through optional context.GetScriptDiagnostics; type errors do not fail cooking.
		// Publishes ScriptImportCheck through context.SetScriptCheck before extraction/cooking can fail, retaining the
		// exact root SourceHash, the provider's GetEnvironmentHash fingerprint and complete diagnostic ranges. A missing
		// provider records Performed=false. The manager reads context.GetScriptCheck for latest-attempt admission and
		// cache metadata, independently retaining the previous artifact after a failed import. Import itself never
		// mutates the registry or live VM.
		// Errors: located CompileFailed/Script/Timeout from extraction, VFS errors, NotFound for unresolved handles,
		// Validation for a non-Script dependency or invalid schema, and cooked-format errors. No partial artifact.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
