#include "EnginePCH.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"

#include "Engine/AssetPipeline/AssetHotReloader.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Jobs/JobSystem.h"

// M6 contract stub (Roadmap rule 3): stream A (asset core and registry) implements the editor's asset manager. The
// contract builds the state the accessors return (the registry, the graph, the catalogue and the writer), so the streams
// that write through it compile and link against a real object.

namespace Engine {

	struct EditorAssetManager::State
	{
		explicit State(const EditorAssetManagerSpecification& specification)
			: Specification(specification), Writer(*specification.Vfs)
		{
		}

		EditorAssetManagerSpecification Specification;
		AssetRegistry Registry;
		AssetDependencyGraph Graph;
		BuiltinAssetCatalog Builtins;
		AssetWriter Writer;
	};

	EditorAssetManager::EditorAssetManager(const EditorAssetManagerSpecification& specification)
	{
		ENGINE_ASSERT(specification.Vfs != nullptr && specification.Jobs != nullptr && specification.MainThread != nullptr
				&& specification.Events != nullptr && specification.Registry != nullptr && specification.IdGenerator != nullptr
				&& specification.Importers != nullptr && specification.Loaders != nullptr,
			"EditorAssetManager needs every service of its specification");
		m_State = CreateScope<State>(specification);
	}

	EditorAssetManager::~EditorAssetManager() = default;

	Result<AssetRef<Asset>> EditorAssetManager::Load(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorAssetManager::Load is an M6 contract stub");
	}

	JobHandle<AssetRef<Asset>> EditorAssetManager::LoadAsync(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Specification.Jobs->Submit([]() -> Result<AssetRef<Asset>>
		{
			return MakeError(ErrorCode::Unsupported, "EditorAssetManager::LoadAsync is an M6 contract stub");
		});
	}

	AssetState EditorAssetManager::GetState(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return AssetState::Unloaded;
	}

	const AssetMetadata* EditorAssetManager::GetMetadata(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	AssetType EditorAssetManager::GetAssetType(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return AssetType::None;
	}

	std::optional<AssetHandle> EditorAssetManager::Resolve(std::string_view /*reference*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::string EditorAssetManager::GetReferencePath(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void EditorAssetManager::WaitIdle()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<AssetRefreshReport> EditorAssetManager::OpenProject(const AssetProjectSpecification& /*project*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorAssetManager::OpenProject is an M6 contract stub");
	}

	void EditorAssetManager::CloseProject()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool EditorAssetManager::HasProject() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	const AssetRegistry& EditorAssetManager::GetRegistry() const
	{
		return m_State->Registry;
	}

	const AssetDependencyGraph& EditorAssetManager::GetDependencyGraph() const
	{
		return m_State->Graph;
	}

	const BuiltinAssetCatalog& EditorAssetManager::GetBuiltins() const
	{
		return m_State->Builtins;
	}

	Result<AssetRefreshReport> EditorAssetManager::Refresh()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorAssetManager::Refresh is an M6 contract stub");
	}

	Result<AssetImportOutcome> EditorAssetManager::Reimport(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorAssetManager::Reimport is an M6 contract stub");
	}

	JobHandle<AssetImportOutcome> EditorAssetManager::ReimportAsync(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Specification.Jobs->Submit([]() -> Result<AssetImportOutcome>
		{
			return MakeError(ErrorCode::Unsupported, "EditorAssetManager::ReimportAsync is an M6 contract stub");
		});
	}

	Result<AssetMetadata> EditorAssetManager::CreateMetadata(const VfsPath& /*source*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorAssetManager::CreateMetadata is an M6 contract stub");
	}

	AssetMetadata EditorAssetManager::CreateDependencyMetadata(AssetHandle /*owner*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<AssetHandle> EditorAssetManager::GetPathDependents(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<VariantValue> EditorAssetManager::MergeImportSettings(std::string_view /*importerId*/, const VariantValue& /*base*/,
		const Json& /*patch*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorAssetManager::MergeImportSettings is an M6 contract stub");
	}

	AssetWriter& EditorAssetManager::GetWriter()
	{
		return m_State->Writer;
	}

	void EditorAssetManager::SetWriteObserver(WriteObserver /*observer*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void EditorAssetManager::Update(double /*nowSeconds*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void EditorAssetManager::SetReloadsDeferred(bool /*deferred*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool EditorAssetManager::AreReloadsDeferred() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void EditorAssetManager::SetExternalChangeListener(ExternalChangeListener /*listener*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	AssetHotReloader* EditorAssetManager::GetHotReloader()
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	void EditorAssetManager::BeginDryRun()
	{
		ENGINE_CONTRACT_STUB();
	}

	void EditorAssetManager::EndDryRun()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool EditorAssetManager::IsDryRun() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
