#include "EditorPCH.h"
#include "EditorCore/Inspector/ReflectedEditController.h"

namespace Engine {

	ReflectedEditController::ReflectedEditController(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	ReflectedEditController::~ReflectedEditController()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ReflectedEditController::Begin(const InspectorEditTarget& /*target*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Status ReflectedEditController::Preview(const Value& /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Result<Value> ReflectedEditController::GetPreview() const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Result<uint64_t> ReflectedEditController::Commit()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	void ReflectedEditController::Cancel()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool ReflectedEditController::IsEditing() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
