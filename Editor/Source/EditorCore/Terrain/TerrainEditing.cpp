#include "EditorPCH.h"
#include "EditorCore/Terrain/TerrainEditing.h"

// Terrain sculpting and painting (TerrainEditing.h): contract stubs of M26's C26.1, implemented by stream T-E
// (Docs/Decisions/0037-m26-contract.md decision 23).

namespace Engine {

	void ApplyTerrainSculpt(HeightfieldData& /*heightfield*/, const TerrainExtent& /*extent*/, const TerrainStroke& /*stroke*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ApplyTerrainPaint(TerrainSplatImage& /*splat*/, const TerrainExtent& /*extent*/, const TerrainStroke& /*stroke*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<TerrainEditResult> EditTerrain(EditorContext& /*editor*/, OpenSceneId /*scene*/, UUID /*entity*/, const TerrainStroke& /*stroke*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 terrain contract (Docs/Decisions/0037-m26-contract.md decision 23, stream T-E)");
	}

}
