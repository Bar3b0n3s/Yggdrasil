#include "EditorPCH.h"
#include "EditorCore/EditorActions.h"

namespace Engine {

	EditorActions::EditorActions(EditorContext& /*context*/, AutomationServer& /*server*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	EditorActions::~EditorActions()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<uint64_t> EditorActions::Submit(std::string_view /*method*/, const Json& /*params*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	void EditorActions::Pump()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<std::optional<Result<Json>>> EditorActions::TakeResult(uint64_t /*ticket*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Status EditorActions::Cancel(uint64_t /*ticket*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
