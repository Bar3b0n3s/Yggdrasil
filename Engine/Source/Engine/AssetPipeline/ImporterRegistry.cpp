#include "EnginePCH.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"

#include "Engine/AssetPipeline/Importers/AudioImporter.h"
#include "Engine/AssetPipeline/Importers/EnvironmentImporter.h"
#include "Engine/AssetPipeline/Importers/FontImporter.h"
#include "Engine/AssetPipeline/Importers/GltfImporter.h"
#include "Engine/AssetPipeline/Importers/MaterialImporter.h"
#include "Engine/AssetPipeline/Importers/PrefabImporter.h"
#include "Engine/AssetPipeline/Importers/SceneImporter.h"
#include "Engine/AssetPipeline/Importers/SoundEffectImporter.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/Core/Assert.h"

#include <algorithm>

namespace Engine {

	namespace Utils {

		[[maybe_unused]] static bool IsLowerCaseExtension(std::string_view extension)
		{
			if (extension.size() < 2 || extension.front() != '.')
				return false;
			return std::ranges::none_of(extension, [](char character)
			{
				return character >= 'A' && character <= 'Z';
			});
		}

	}

	ImporterRegistry::ImporterRegistry() = default;

	ImporterRegistry::~ImporterRegistry() = default;

	void ImporterRegistry::Register(Scope<IAssetImporter> importer)
	{
		ENGINE_CORE_ASSERT(importer != nullptr, "ImporterRegistry::Register needs an importer");
		ENGINE_CORE_ASSERT(FindById(importer->GetId()) == nullptr, "A second importer with id '{}'", importer->GetId());
		for ([[maybe_unused]] const std::string_view extension : importer->GetExtensions())
		{
			ENGINE_CORE_ASSERT(Utils::IsLowerCaseExtension(extension), "Importer '{}': extension '{}' is not lower case with a leading dot",
				importer->GetId(), extension);
			ENGINE_CORE_ASSERT(FindForExtension(extension) == nullptr, "Importer '{}': extension '{}' is taken by '{}'", importer->GetId(),
				extension, FindForExtension(extension) != nullptr ? FindForExtension(extension)->GetId() : std::string_view());
		}
		const std::string_view id = importer->GetId();
		const auto position = std::ranges::lower_bound(m_Importers, id, std::less<>(), [](const Scope<IAssetImporter>& registered)
		{
			return registered->GetId();
		});
		m_Importers.insert(position, std::move(importer));
	}

	const IAssetImporter* ImporterRegistry::FindById(std::string_view id) const
	{
		const auto found = std::ranges::find_if(m_Importers, [id](const Scope<IAssetImporter>& importer)
		{
			return importer->GetId() == id;
		});
		return found == m_Importers.end() ? nullptr : found->get();
	}

	const IAssetImporter* ImporterRegistry::FindForExtension(std::string_view extension) const
	{
		const auto found = std::ranges::find_if(m_Importers, [extension](const Scope<IAssetImporter>& importer)
		{
			return importer->CanImport(extension);
		});
		return found == m_Importers.end() ? nullptr : found->get();
	}

	std::vector<const IAssetImporter*> ImporterRegistry::GetImporters() const
	{
		std::vector<const IAssetImporter*> importers;
		importers.reserve(m_Importers.size());
		for (const Scope<IAssetImporter>& importer : m_Importers)
			importers.push_back(importer.get());
		return importers;
	}

	std::vector<AssetImporterDescription> ImporterRegistry::Describe() const
	{
		std::vector<AssetImporterDescription> descriptions;
		descriptions.reserve(m_Importers.size());
		for (const Scope<IAssetImporter>& importer : m_Importers)
		{
			AssetImporterDescription description;
			description.Id = std::string(importer->GetId());
			description.MainType = importer->GetMainType();
			description.Version = importer->GetVersion();
			for (const std::string_view extension : importer->GetExtensions())
				description.Extensions.emplace_back(extension);
			descriptions.push_back(std::move(description));
		}
		return descriptions;
	}

	void RegisterBuiltinImporters(ImporterRegistry& registry)
	{
		registry.Register(CreateScope<TextureImporter>());
		registry.Register(CreateScope<GltfImporter>());
		registry.Register(CreateScope<MaterialImporter>());
		registry.Register(CreateScope<SceneImporter>());
		registry.Register(CreateScope<PrefabImporter>());
		registry.Register(CreateScope<FontImporter>());
		registry.Register(CreateScope<EnvironmentImporter>());
		// M12 (Docs/Decisions/0015-m12-decisions.md).
		registry.Register(CreateScope<AudioImporter>());
		registry.Register(CreateScope<SoundEffectImporter>());
	}

	void RegisterAssetPipelineTypes(TypeRegistry& registry)
	{
		// In the order of RegisterBuiltinImporters; the material, scene and prefab importers have no settings.
		TextureImporter::RegisterTypes(registry);
		GltfImporter::RegisterTypes(registry);
		FontImporter::RegisterTypes(registry);
		EnvironmentImporter::RegisterTypes(registry);
		// M12 (Docs/Decisions/0015-m12-decisions.md): the audio import settings and the .sfx description.
		AudioImporter::RegisterTypes(registry);
		SoundEffectImporter::RegisterTypes(registry);
	}

}
