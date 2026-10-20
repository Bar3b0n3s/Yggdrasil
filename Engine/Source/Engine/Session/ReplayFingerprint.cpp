#include "EnginePCH.h"
#include "Engine/Session/ReplayFingerprint.h"

namespace Engine {

	Result<ReplayFingerprint> ComputeReplayFingerprint(const Scene& scene, const ScriptFieldSchemaSource* schemas, AssetManager* assets)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(scene);
		static_cast<void>(schemas);
		static_cast<void>(assets);
		return std::unexpected(Error(ErrorCode::Unsupported, "the replay start fingerprint (Docs/Decisions/0030-m19-contract.md decision 2, stream B)"));
	}

	std::vector<std::string> DescribeReplayFingerprintChanges(const ReplayFingerprint& recorded, const ReplayFingerprint& observed, size_t limit)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(recorded);
		static_cast<void>(observed);
		static_cast<void>(limit);
		return {};
	}

}
