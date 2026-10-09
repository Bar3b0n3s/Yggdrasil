#include "EnginePCH.h"
#include "Engine/Renderer/AsyncPicker.h"

namespace Engine {

	struct AsyncPicker::State
	{
	};

	AsyncPicker::AsyncPicker(GraphicsDevice&)
	{
		ENGINE_CONTRACT_STUB();
	}

	AsyncPicker::~AsyncPicker() = default;

	Result<PickTicket> AsyncPicker::Request(nvrhi::ITexture&, std::span<const UUID>, uint64_t, const PickRequest&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	Result<std::optional<PickResult>> AsyncPicker::Poll(PickTicket, uint64_t, uint64_t)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	void AsyncPicker::CancelAll()
	{
		ENGINE_CONTRACT_STUB();
	}

}
