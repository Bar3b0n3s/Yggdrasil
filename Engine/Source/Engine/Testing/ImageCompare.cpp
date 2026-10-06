#include "EnginePCH.h"
#include "Engine/Testing/ImageCompare.h"

// M5 contract stub (Roadmap rule 3): stream E (readback, ImageCompare, golden harness, screenshots) implements the
// comparison, the difference image and the smoke-mode statistics of Architecture §15.4.

namespace Engine {

	Result<ImageCompareResult> CompareImages(const Image& /*actual*/, const Image& /*expected*/, const ImageCompareThresholds& /*thresholds*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CompareImages is not implemented yet");
	}

	Result<ImageStatistics> ComputeImageStatistics(const Image& /*image*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ComputeImageStatistics is not implemented yet");
	}

}
