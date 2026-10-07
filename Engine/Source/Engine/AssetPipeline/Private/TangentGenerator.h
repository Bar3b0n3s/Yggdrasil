#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

// MikkTSpace tangent generation for the glTF importer (Architecture §7.4, Vendor/MikkTSpace/VENDOR.md). This header is
// the only interface to MikkTSpace: TangentGenerator.cpp is the one first-party file that includes <mikktspace.h>
// (Scripts/ModuleRules.json RestrictedThirdParty), so no MikkTSpace type leaves it.

namespace Engine {

	namespace Utils {

		// An indexed triangle list. Every span must outlive the call; Indices.size() is a multiple of 3 and every index is
		// below the attribute count (asserted: the importer validates its input first).
		struct TangentGeneratorInput
		{
			std::span<const glm::vec3> Positions{};
			std::span<const glm::vec3> Normals{};   // unit length
			std::span<const glm::vec2> TexCoords{}; // glTF convention: origin at the top left
			std::span<const uint32_t> Indices{};
		};

		// MikkTSpace tangents for each triangle corner of `input` (Indices.size() entries, corner 3 * t + k of triangle t),
		// in the glTF convention of MeshVertex::Tangent: xyz is the unit tangent along +u, w the bitangent sign with
		// bitangent = cross(normal, xyz) * w for a top-left texture origin. MikkTSpace itself assumes a bottom-left origin
		// (OpenGL), so the sign it reports is negated, as glTF viewers do. A corner whose tangent MikkTSpace cannot
		// determine (no non-degenerate texture mapping in its smoothing group) gets a unit tangent perpendicular to its
		// normal (MakePerpendicularTangent). Corners of one vertex share a tangent unless MikkTSpace splits them; the caller
		// splits vertices where they differ (MikkTSpace results are per corner, never per index). Deterministic for one
		// toolchain and C runtime (VENDOR.md). Errors: ImportFailed when MikkTSpace fails (an allocation) or the triangle
		// count exceeds what its int-based interface can address.
		[[nodiscard]] Result<std::vector<glm::vec4>> GenerateCornerTangents(const TangentGeneratorInput& input);

		// A unit tangent perpendicular to the unit vector `normal`, with sign +1: the coordinate axis least aligned with the
		// normal (the first of X, Y, Z on a tie), made perpendicular to it (Gram-Schmidt) and normalized, so an upward or
		// forward normal gets +X. Used when a primitive has no texture coordinates (ASSET_TANGENTS_APPROXIMATED) or tangent
		// generation is off. Pure.
		[[nodiscard]] glm::vec4 MakePerpendicularTangent(const glm::vec3& normal);

	}

}
