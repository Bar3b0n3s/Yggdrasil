#include "EnginePCH.h"
#include "Engine/Asset/AssetLoaderRegistry.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/ReplayData.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Assert.h"

#include <algorithm>
#include <format>

namespace Engine {

	namespace {

		// A loader over one payload reader of the data types: `LoadFunction(cooked, context)` returns the typed asset.
		template<AssetDataType T, Result<AssetRef<T>> (*LoadFunction)(std::span<const std::byte>, const AssetLoadContext&)>
		class PayloadLoader final : public IAssetLoader
		{
		public:
			[[nodiscard]] AssetType GetType() const override { return T::StaticType; }

			[[nodiscard]] Result<AssetRef<Asset>> Load(std::span<const std::byte> cooked, const AssetLoadContext& context) const override
			{
				Result<AssetRef<T>> loaded = LoadFunction(cooked, context);
				if (!loaded.has_value())
					return std::unexpected(std::move(loaded).error().WithContext(std::format("while loading asset {}", context.Handle.ToString())));
				return AssetRef<Asset>(std::move(*loaded));
			}
		};

	}

	namespace Utils {

		static Result<AssetRef<SceneData>> LoadScene(std::span<const std::byte> cooked, const AssetLoadContext& /*context*/)
		{
			return LoadCookedScene(cooked);
		}

		static Result<AssetRef<PrefabData>> LoadPrefab(std::span<const std::byte> cooked, const AssetLoadContext& /*context*/)
		{
			return LoadCookedPrefab(cooked);
		}

		static Result<AssetRef<MeshData>> LoadMesh(std::span<const std::byte> cooked, const AssetLoadContext& /*context*/)
		{
			return LoadCookedMesh(cooked);
		}

		static Result<AssetRef<MaterialData>> LoadMaterial(std::span<const std::byte> cooked, const AssetLoadContext& context)
		{
			ENGINE_CORE_ASSERT(context.Registry != nullptr, "Loading a material needs the type registry");
			return LoadCookedMaterial(cooked, *context.Registry);
		}

		static Result<AssetRef<TextureData>> LoadTexture(std::span<const std::byte> cooked, const AssetLoadContext& /*context*/)
		{
			return LoadCookedTexture(cooked);
		}

		static Result<AssetRef<FontData>> LoadFont(std::span<const std::byte> cooked, const AssetLoadContext& /*context*/)
		{
			return LoadCookedFont(cooked);
		}

		static Result<AssetRef<EnvironmentData>> LoadEnvironment(std::span<const std::byte> cooked, const AssetLoadContext& /*context*/)
		{
			return LoadCookedEnvironment(cooked);
		}

		static Result<AssetRef<AudioClipData>> LoadAudioClip(std::span<const std::byte> cooked, const AssetLoadContext& /*context*/)
		{
			return LoadCookedAudioClip(cooked);
		}

		static Result<AssetRef<ScriptData>> LoadScript(std::span<const std::byte> cooked, const AssetLoadContext& /*context*/)
		{
			return LoadCookedScript(cooked);
		}

		static Result<AssetRef<ReplayData>> LoadReplay(std::span<const std::byte> cooked, const AssetLoadContext& /*context*/)
		{
			return LoadCookedReplay(cooked);
		}

	}

	AssetLoaderRegistry::AssetLoaderRegistry() = default;

	AssetLoaderRegistry::~AssetLoaderRegistry() = default;

	void AssetLoaderRegistry::Register(Scope<IAssetLoader> loader)
	{
		ENGINE_CORE_ASSERT(loader != nullptr, "AssetLoaderRegistry::Register needs a loader");
		const AssetType type = loader->GetType();
		ENGINE_CORE_ASSERT(type != AssetType::None, "A loader reads a type");
		ENGINE_CORE_ASSERT(Find(type) == nullptr, "A second loader for AssetType {}", AssetTypeToString(type));
		const auto position = std::ranges::lower_bound(m_Loaders, type, std::less<>(), [](const Scope<IAssetLoader>& registered)
		{
			return registered->GetType();
		});
		m_Loaders.insert(position, std::move(loader));
	}

	const IAssetLoader* AssetLoaderRegistry::Find(AssetType type) const
	{
		const auto found = std::ranges::find_if(m_Loaders, [type](const Scope<IAssetLoader>& loader)
		{
			return loader->GetType() == type;
		});
		return found == m_Loaders.end() ? nullptr : found->get();
	}

	std::vector<AssetType> AssetLoaderRegistry::GetTypes() const
	{
		std::vector<AssetType> types;
		types.reserve(m_Loaders.size());
		for (const Scope<IAssetLoader>& loader : m_Loaders)
			types.push_back(loader->GetType());
		return types;
	}

	Result<AssetRef<Asset>> AssetLoaderRegistry::Load(std::span<const std::byte> cooked, const AssetLoadContext& context) const
	{
		Result<CookedArtifactView> view = ReadCookedArtifact(cooked);
		if (!view.has_value())
			return std::unexpected(std::move(view).error().WithContext(std::format("while loading asset {}", context.Handle.ToString())));
		const IAssetLoader* loader = Find(view->Header.Type);
		if (loader == nullptr)
		{
			return std::unexpected(Error(ErrorCode::Unsupported,
				std::format("no loader for AssetType {} (asset {})", AssetTypeToString(view->Header.Type), context.Handle.ToString()))
					.WithHint("this build cannot load the asset's type yet"));
		}
		return loader->Load(cooked, context);
	}

	void RegisterBuiltinLoaders(AssetLoaderRegistry& registry)
	{
		registry.Register(CreateScope<PayloadLoader<SceneData, &Utils::LoadScene>>());
		registry.Register(CreateScope<PayloadLoader<PrefabData, &Utils::LoadPrefab>>());
		registry.Register(CreateScope<PayloadLoader<MeshData, &Utils::LoadMesh>>());
		registry.Register(CreateScope<PayloadLoader<MaterialData, &Utils::LoadMaterial>>());
		registry.Register(CreateScope<PayloadLoader<TextureData, &Utils::LoadTexture>>());
		registry.Register(CreateScope<PayloadLoader<FontData, &Utils::LoadFont>>());
		registry.Register(CreateScope<PayloadLoader<EnvironmentData, &Utils::LoadEnvironment>>());
		// M12 (Docs/Decisions/0015-m12-decisions.md).
		registry.Register(CreateScope<PayloadLoader<AudioClipData, &Utils::LoadAudioClip>>());
		registry.Register(CreateScope<PayloadLoader<ScriptData, &Utils::LoadScript>>());
		registry.Register(CreateScope<PayloadLoader<ReplayData, &Utils::LoadReplay>>());
	}

}
