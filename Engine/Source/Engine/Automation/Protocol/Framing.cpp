#include "EnginePCH.h"
#include "Engine/Automation/Protocol/Framing.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements the frame decoder.

namespace Engine {

	std::string_view FrameErrorKindToString(FrameErrorKind /*kind*/)
	{
		ENGINE_CONTRACT_STUB();
		return "Unknown";
	}

	void FrameDecoder::Append(std::span<const std::byte> /*bytes*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<std::optional<std::string>> FrameDecoder::Next()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "FrameDecoder::Next is an M4 contract stub");
	}

	std::string EncodeFrame(std::string_view /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
