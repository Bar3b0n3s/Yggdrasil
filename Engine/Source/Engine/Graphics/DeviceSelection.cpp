#include "EnginePCH.h"
#include "Engine/Graphics/DeviceSelection.h"

// M5 contract stub (Roadmap rule 3): stream A (loader, device, selection, creation wrappers, host image upload) implements
// the pure selection functions of Architecture §8.1 ("DeviceSelection: scoring table").

namespace Engine {

	DeviceRejection GetDeviceRejection(const DeviceCandidate& /*candidate*/, bool /*requirePresent*/)
	{
		ENGINE_CONTRACT_STUB();
		return DeviceRejection::None;
	}

	std::optional<int32_t> ScoreDevice(const DeviceCandidate& /*candidate*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	Result<DeviceSelection> SelectDevice(std::span<const DeviceCandidate> /*candidates*/, const DeviceSelectionRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SelectDevice is not implemented yet");
	}

	nvrhi::Format GetBloomFormat(const DeviceCandidate& /*candidate*/)
	{
		ENGINE_CONTRACT_STUB();
		return nvrhi::Format::RGBA16_FLOAT;
	}

	std::string GetDeviceClass(uint32_t /*vendorID*/, uint32_t /*driverVersion*/, VkDriverId /*driverID*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string_view DeviceRejectionToString(DeviceRejection /*rejection*/)
	{
		ENGINE_CONTRACT_STUB();
		return "Unknown";
	}

	std::string_view GpuDeviceTypeToString(GpuDeviceType /*type*/)
	{
		ENGINE_CONTRACT_STUB();
		return "Unknown";
	}

}
