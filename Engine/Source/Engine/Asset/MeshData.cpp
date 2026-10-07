#include "EnginePCH.h"
#include "Engine/Asset/MeshData.h"

// M6 contract stub (Roadmap rule 3): stream F (built-ins, GPU cache, shipped assets) implements the mesh payload.

namespace Engine {

	Status ValidateMeshData(const MeshData& /*mesh*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ValidateMeshData is an M6 contract stub");
	}

	Buffer SerializeMeshPayload(const MeshData& /*mesh*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<MeshData> DeserializeMeshPayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "DeserializeMeshPayload is an M6 contract stub");
	}

	Buffer CookMesh(const MeshData& /*mesh*/, uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<AssetRef<MeshData>> LoadCookedMesh(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "LoadCookedMesh is an M6 contract stub");
	}

}
