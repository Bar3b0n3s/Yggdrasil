#include "EditorPCH.h"
#include "EditorCore/Autosave/Autosave.h"

namespace Engine {

	Autosave::Autosave(EditorContext& /*context*/, const AutosaveSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Autosave::~Autosave()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<AutosaveWriteResult> Autosave::Update(double /*nowSeconds*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Status Autosave::Publish()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Result<AutosaveWriteResult> Autosave::Save(AutosaveReason /*reason*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	AutosaveFatalResult Autosave::WriteFatalSnapshot() noexcept
	{
		ENGINE_CONTRACT_STUB();
		return AutosaveFatalResult::Disabled;
	}

	Result<std::optional<AutosaveRecoveryInfo>> Autosave::FindRecovery() const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Status Autosave::Recover(const AutosaveRecoveryInfo& /*recovery*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Status Autosave::DiscardSavedRecovery()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	bool Autosave::Reset()
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
