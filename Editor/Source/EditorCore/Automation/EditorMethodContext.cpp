#include "EditorPCH.h"
#include "EditorCore/Automation/EditorMethodContext.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

// M4 contract stub (Roadmap rule 3): stream C (methods) implements the shared resolution rules. The constructor is real,
// so the AutomationServer (stream B) can create contexts while the helpers are stubs.

namespace Engine {

	EditorMethodContext::EditorMethodContext(EditorContext& editor, AutomationServer& server, MethodRequest request)
		: MethodContext(TypeKeyOf<EditorMethodContext>(), std::move(request)), m_Editor(&editor), m_Server(&server)
	{
	}

	Scope<MethodContext> EditorMethodContext::CreateNested(MethodRequest request) const
	{
		ENGINE_CONTRACT_STUB();
		return CreateScope<EditorMethodContext>(*m_Editor, *m_Server, std::move(request));
	}

	Result<Scene*> EditorMethodContext::ResolveTargetScene(SceneTarget /*target*/, bool /*given*/, bool /*mutation*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorMethodContext::ResolveTargetScene is an M4 contract stub");
	}

	Result<Entity> EditorMethodContext::ResolveEntity(Scene& /*scene*/, std::string_view /*reference*/, std::string_view /*pointer*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorMethodContext::ResolveEntity is an M4 contract stub");
	}

	Result<VfsPath> EditorMethodContext::ResolveProjectPath(std::string_view /*path*/, std::string_view /*pointer*/,
		std::string_view /*extension*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EditorMethodContext::ResolveProjectPath is an M4 contract stub");
	}

	EntitySummary EditorMethodContext::MakeEntitySummary(ConstEntity /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
