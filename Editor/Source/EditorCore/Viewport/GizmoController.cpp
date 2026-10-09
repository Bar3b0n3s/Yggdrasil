#include "EditorPCH.h"
#include "EditorCore/Viewport/GizmoController.h"

namespace Engine {

	GizmoController::GizmoController(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	GizmoController::~GizmoController()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status GizmoController::Begin(std::span<const UUID> /*entities*/, const glm::mat4& /*worldPivot*/, const GizmoSettings& /*settings*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Status GizmoController::Update(const glm::mat4& /*worldPivot*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Result<uint64_t> GizmoController::End()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	void GizmoController::Cancel()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool GizmoController::IsDragging() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	const Scene* GizmoController::GetPreviewScene() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

}
