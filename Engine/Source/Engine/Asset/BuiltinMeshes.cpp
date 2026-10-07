#include "EnginePCH.h"
#include "Engine/Asset/BuiltinMeshes.h"

// M6 contract stub (Roadmap rule 3): stream F (built-ins, GPU cache, shipped assets) generates the built-in meshes.

namespace Engine {

	std::string_view BuiltinMeshToString(BuiltinMesh mesh)
	{
		switch (mesh)
		{
			case BuiltinMesh::Cube:     return "Cube";
			case BuiltinMesh::Sphere:   return "Sphere";
			case BuiltinMesh::Plane:    return "Plane";
			case BuiltinMesh::Quad:     return "Quad";
			case BuiltinMesh::Cylinder: return "Cylinder";
			case BuiltinMesh::Capsule:  return "Capsule";
			case BuiltinMesh::Cone:     return "Cone";
		}
		return "Unknown";
	}

	MeshData GenerateBuiltinMesh(BuiltinMesh /*mesh*/)
	{
		ENGINE_CONTRACT_STUB();
		return MeshData();
	}

}
