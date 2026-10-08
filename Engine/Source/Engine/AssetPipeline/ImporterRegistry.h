#pragma once

#include "Engine/Asset/AssetRegistry.h"
#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <span>
#include <string_view>
#include <vector>

namespace Engine {

	class TypeRegistry;

	// The editor's importers (Architecture §3 rule 4, §7.4). Built once at startup (Register on one thread), then only read:
	// every const member is thread-safe. Not copyable.
	class ImporterRegistry
	{
	public:
		ImporterRegistry();
		~ImporterRegistry();

		ImporterRegistry(const ImporterRegistry&) = delete;
		ImporterRegistry& operator=(const ImporterRegistry&) = delete;

		// Adds `importer` (non-null, asserted). A repeated id or an extension another importer takes is a programmer error
		// (asserted), as is an extension that is not lower case with a leading dot.
		void Register(Scope<IAssetImporter> importer);

		// The importer with id `id` (exact), or nullptr.
		[[nodiscard]] const IAssetImporter* FindById(std::string_view id) const;
		// The importer that takes `extension` (with the dot, ASCII case-insensitive), or nullptr.
		[[nodiscard]] const IAssetImporter* FindForExtension(std::string_view extension) const;

		// Every importer, sorted by id.
		[[nodiscard]] std::vector<const IAssetImporter*> GetImporters() const;

		// What AssetRegistry::Scan needs to know about the importers, sorted by id.
		[[nodiscard]] std::vector<AssetImporterDescription> Describe() const;
	private:
		std::vector<Scope<IAssetImporter>> m_Importers; // sorted by id
	};

	// Registers the built-in importers (§7.4): M6's TextureImporter, GltfImporter, MaterialImporter, SceneImporter,
	// PrefabImporter and FontImporter, and M8's EnvironmentImporter. AudioImporter and SoundEffectImporter (M12), ScriptImporter
	// and ReplayImporter (M13) join here with their milestones.
	void RegisterBuiltinImporters(ImporterRegistry& registry);

	// Registers every importer's settings struct and enum (§5.4: "every *ImportSettings" is a reflected struct) in the order
	// of RegisterBuiltinImporters, then M12's AudioImportSettings and the .sfx description structs (SoundEffectImporter.h).
	// The editor's RegisterEditorMethodTypes calls it (asset.getImportSettings and asset.setImportSettings validate against
	// these types); the registry suite's registry includes it.
	void RegisterAssetPipelineTypes(TypeRegistry& registry);

}
