#include "EditorPCH.h"
#include "EditorCore/EditorContext.h"

#include "EditorCore/Commands/CompositeCommand.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Platform/SecureRandom.h"

// M4 contract stub (Roadmap rule 3): stream A (commands) implements the editor state. Construction and the trivial
// accessors are real, so other streams can build an EditorContext while its behaviour is stubbed.

namespace Engine {

	EditorContext::EditorContext(ConstructionKey /*key*/, EngineContext& engine, const EditorContextSpecification& specification,
		const Random::State& idGeneratorState)
		: m_Engine(&engine), m_Specification(specification), m_IdGenerator(UUIDGenerator::CreateRandom(idGeneratorState)), m_History(specification.HistoryLimits)
	{
	}

	EditorContext::~EditorContext() = default;

	Result<Scope<EditorContext>> EditorContext::Create(EngineContext& engine, const EditorContextSpecification& specification)
	{
		if (specification.IdGeneratorState.has_value())
			return CreateScope<EditorContext>(ConstructionKey(), engine, specification, *specification.IdGeneratorState);
		Result<Random::State> generated = SecureRandom::GenerateState();
		if (!generated)
			return std::unexpected(std::move(generated).error().WithContext("seeding the editor's id generator"));
		return CreateScope<EditorContext>(ConstructionKey(), engine, specification, *generated);
	}

	uint64_t EditorContext::GetRevision() const
	{
		return m_RevisionBase + (m_Scene != nullptr ? m_Scene->GetRevision() : 0);
	}

	const TypeRegistry& EditorContext::GetTypeRegistry() const
	{
		return m_Engine->GetTypeRegistry();
	}

	VirtualFileSystem& EditorContext::GetVfs() const
	{
		return m_Engine->GetVfs();
	}

	const LoadedProject& EditorContext::GetProject() const
	{
		ENGINE_CORE_ASSERT(m_Project != nullptr, "EditorContext::GetProject without an open project");
		return *m_Project;
	}

	bool EditorContext::IsReadOnly() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Status EditorContext::OpenProject(Scope<LoadedProject> /*project*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorContext::OpenProject is an M4 contract stub");
	}

	void EditorContext::CloseProject()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status EditorContext::ApplyProjectSettings(const Json& /*document*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorContext::ApplyProjectSettings is an M4 contract stub");
	}

	Scene& EditorContext::GetScene()
	{
		ENGINE_CORE_ASSERT(m_Scene != nullptr, "EditorContext::GetScene without an open scene");
		return *m_Scene;
	}

	const Scene& EditorContext::GetScene() const
	{
		ENGINE_CORE_ASSERT(m_Scene != nullptr, "EditorContext::GetScene without an open scene");
		return *m_Scene;
	}

	Scope<Scene> EditorContext::CreateScene(std::string name)
	{
		return Scene::Create({ .Name = std::move(name), .Seed = 0, .Registry = &GetTypeRegistry(), .IdGenerator = &m_IdGenerator, .Runtime = false });
	}

	void EditorContext::SetScene(Scope<Scene> /*scene*/, std::optional<VfsPath> /*path*/, bool /*dirty*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void EditorContext::CloseScene()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool EditorContext::IsSceneDirty() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void EditorContext::MarkSceneSaved(const VfsPath& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<uint64_t> EditorContext::Execute(Scope<Command> /*command*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorContext::Execute is an M4 contract stub");
	}

	CommandHistory& EditorContext::GetHistory()
	{
		ENGINE_CONTRACT_STUB();
		return m_History;
	}

	const CommandHistory& EditorContext::GetHistory() const
	{
		ENGINE_CONTRACT_STUB();
		return m_History;
	}

	void EditorContext::SetSelection(std::vector<UUID> /*selection*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status EditorContext::WriteProjectFile(const VfsPath& /*path*/, std::span<const std::byte> /*data*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorContext::WriteProjectFile is an M4 contract stub");
	}

	void EditorContext::SetWriteAttribution(std::optional<WriteAttribution> /*attribution*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	const ProvenanceRecorder* EditorContext::GetProvenance() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	void EditorContext::AppendEvent(EngineEvent /*event*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void EditorContext::RequestShutdown(int /*exitCode*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	struct EditorTransaction::State
	{
		EditorContext* Context = nullptr; // documented back-reference: outlives the transaction
		std::string Label{};
		EditorTransaction* Outermost = nullptr; // the transaction this one joined; null for the outermost
		std::vector<Scope<Command>> Executed{}; // the outermost's executed commands, oldest first
		size_t JoinedFirst = 0;                 // when joined: the index of this one's first command in the outermost's list
		size_t JoinedCount = 0;                 // when joined: how many commands this one appended there
		uint64_t RevisionBefore = 0;            // EditorContext::GetRevision before the first command
	};

	EditorTransaction::EditorTransaction(EditorContext& context, std::string label)
		: m_State(CreateScope<State>())
	{
		ENGINE_CONTRACT_STUB();
		m_State->Context = &context;
		m_State->Label = std::move(label);
	}

	EditorTransaction::~EditorTransaction()
	{
		ENGINE_CONTRACT_STUB();
	}

	uint64_t EditorTransaction::Commit()
	{
		ENGINE_CONTRACT_STUB();
		m_IsOpen = false;
		return 0;
	}

	Status EditorTransaction::Rollback()
	{
		ENGINE_CONTRACT_STUB();
		m_IsOpen = false;
		return MakeError(ErrorCode::Unsupported, "EditorTransaction::Rollback is an M4 contract stub");
	}

	bool EditorTransaction::IsJoined() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Outermost != nullptr;
	}

	Status EditorTransaction::ExecuteAndAppend(Scope<Command> /*command*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorTransaction::ExecuteAndAppend is an M4 contract stub");
	}

	size_t EditorTransaction::GetCommandCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	struct EditorDryRunScope::State
	{
		EditorContext* Context = nullptr; // documented back-reference: outlives the dry run
	};

	EditorDryRunScope::EditorDryRunScope(ConstructionKey /*key*/, EditorContext& context)
		: m_State(CreateScope<State>())
	{
		m_State->Context = &context;
	}

	Result<Scope<EditorDryRunScope>> EditorDryRunScope::Begin(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorDryRunScope::Begin is an M4 contract stub");
	}

	EditorDryRunScope::~EditorDryRunScope()
	{
		ENGINE_CONTRACT_STUB();
	}

	std::vector<std::string> EditorDryRunScope::GetChangedPaths() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
