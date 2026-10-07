#include "EnginePCH.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"

// M6 contract stub (Roadmap rule 3): stream A (asset core and registry) implements the importer registry; it registers
// the importers of streams B and C and their settings types.

namespace Engine {

	ImporterRegistry::ImporterRegistry() = default;

	ImporterRegistry::~ImporterRegistry() = default;

	void ImporterRegistry::Register(Scope<IAssetImporter> /*importer*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	const IAssetImporter* ImporterRegistry::FindById(std::string_view /*id*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const IAssetImporter* ImporterRegistry::FindForExtension(std::string_view /*extension*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::vector<const IAssetImporter*> ImporterRegistry::GetImporters() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<AssetImporterDescription> ImporterRegistry::Describe() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void RegisterBuiltinImporters(ImporterRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterAssetPipelineTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
