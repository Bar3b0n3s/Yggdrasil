#pragma once

#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <string_view>

// The procedural built-in meshes (Architecture §7.1: engine://Meshes/{Cube, Sphere, Plane, Quad, Cylinder, Capsule, Cone}:
// "deterministic generation, analytic tangents, unit size").

namespace Engine {

	enum class BuiltinMesh : uint8_t
	{
		Cube,     // 1 m edges, centred; 24 vertices (flat faces), one submesh
		Sphere,   // diameter 1 m (radius 0.5), UV sphere of 32 segments x 16 rings
		Plane,    // 1 m x 1 m in XZ facing +Y, 10 x 10 quads (receives shadows and lights smoothly)
		Quad,     // 1 m x 1 m in XY facing +Z, one quad (sprites, UI-like geometry)
		Cylinder, // diameter 1 m, height 1 m along Y, 32 segments, capped
		Capsule,  // diameter 1 m, total height 2 m along Y (CapsuleCollider defaults Radius 0.5, HalfHeight 0.5), 32 segments, 8 rings per hemisphere
		Cone      // base diameter 1 m at y = -0.5, apex at y = 0.5, 32 segments, capped
	};

	// "Cube", "Sphere", ...; the name in engine://Meshes/<Name>. "Unknown" for a value outside the enumeration (a pure
	// name lookup, like FieldTypeToString). Pure and thread-safe.
	[[nodiscard]] std::string_view BuiltinMeshToString(BuiltinMesh mesh);

	// Generates `mesh` (§7.1): centred on the origin, counter-clockwise front faces (glTF), outward normals, analytic unit
	// tangents with sign +1 for the U direction of the texture coordinates, texture coordinates in [0, 1], one submesh with
	// one material slot "Default" whose DefaultMaterial is null (the Default material). Uses only DetMath and IEEE-exact
	// operations, so the bytes are identical on every run, host compiler configuration and platform build of one toolchain
	// ("BuiltinMeshes: generation is deterministic"). The result passes ValidateMeshData.
	[[nodiscard]] MeshData GenerateBuiltinMesh(BuiltinMesh mesh);

}
