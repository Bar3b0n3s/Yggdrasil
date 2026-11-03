#include "EditorPCH.h"
#include "EditorCore/Animation/AnimationPreview.h"

// The editor's animation preview (AnimationPreview.h). Contract stubs of M23's C23.3, implemented by stream D
// (Docs/Decisions/0034-m23-contract.md decision 32): until then no preview starts.

namespace Engine {

	Result<AnimationPreviewState> AnimationPreview::Begin(EditorContext& /*context*/, OpenSceneId /*scene*/, UUID /*entity*/, AssetManager& /*assets*/,
		float /*time*/, bool /*playing*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M23 animation preview contract (Docs/Decisions/0034-m23-contract.md decision 32, stream D)");
	}

	void AnimationPreview::Update(EditorContext& /*context*/, AssetManager& /*assets*/, double /*frameSeconds*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AnimationPreview::End(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
