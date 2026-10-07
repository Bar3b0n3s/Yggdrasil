#include "EnginePCH.h"
#include "Engine/AssetPipeline/Private/TangentGenerator.h"

#include "Engine/Core/Assert.h"

#include <glm/geometric.hpp>
#include <mikktspace.h>

#include <cmath>
#include <limits>

namespace Engine {

	namespace {

		// What the MikkTSpace callbacks read and write (SMikkTSpaceContext::m_pUserData).
		struct MikkTSpaceMesh
		{
			const Utils::TangentGeneratorInput* Input = nullptr; // the caller's mesh; outlives genTangSpaceDefault
			std::vector<glm::vec4>* Tangents = nullptr;          // one entry per corner, written by SetCornerTangent
		};

	}

	namespace Utils {

		// The squared-length window inside which a returned tangent counts as unit length. MikkTSpace normalizes the
		// tangents it determines and leaves the others at zero (degenerate texture mapping).
		static constexpr float MinUnitLengthSquared = 0.998f;
		static constexpr float MaxUnitLengthSquared = 1.002f;

		static MikkTSpaceMesh& GetMesh(const SMikkTSpaceContext* context)
		{
			return *static_cast<MikkTSpaceMesh*>(context->m_pUserData);
		}

		static size_t GetCorner(int face, int vertex)
		{
			return static_cast<size_t>(face) * 3 + static_cast<size_t>(vertex);
		}

		static uint32_t GetVertexIndex(const SMikkTSpaceContext* context, int face, int vertex)
		{
			return GetMesh(context).Input->Indices[GetCorner(face, vertex)];
		}

		static int GetFaceCount(const SMikkTSpaceContext* context)
		{
			return static_cast<int>(GetMesh(context).Input->Indices.size() / 3);
		}

		static int GetVertexCountOfFace(const SMikkTSpaceContext* /*context*/, int /*face*/)
		{
			return 3;
		}

		static void GetPosition(const SMikkTSpaceContext* context, float position[], int face, int vertex)
		{
			const glm::vec3& value = GetMesh(context).Input->Positions[GetVertexIndex(context, face, vertex)];
			position[0] = value.x;
			position[1] = value.y;
			position[2] = value.z;
		}

		static void GetNormal(const SMikkTSpaceContext* context, float normal[], int face, int vertex)
		{
			const glm::vec3& value = GetMesh(context).Input->Normals[GetVertexIndex(context, face, vertex)];
			normal[0] = value.x;
			normal[1] = value.y;
			normal[2] = value.z;
		}

		static void GetTexCoord(const SMikkTSpaceContext* context, float texCoord[], int face, int vertex)
		{
			const glm::vec2& value = GetMesh(context).Input->TexCoords[GetVertexIndex(context, face, vertex)];
			texCoord[0] = value.x;
			texCoord[1] = value.y;
		}

		static void SetCornerTangent(const SMikkTSpaceContext* context, const float tangent[], float sign, int face, int vertex)
		{
			// MikkTSpace's sign is for a bottom-left texture origin; glTF's origin is the top left, which mirrors v.
			(*GetMesh(context).Tangents)[GetCorner(face, vertex)] = glm::vec4(tangent[0], tangent[1], tangent[2], sign < 0.0f ? 1.0f : -1.0f);
		}

		static bool IsUnitTangent(const glm::vec4& tangent)
		{
			const glm::vec3 direction(tangent);
			if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z))
				return false;
			const float lengthSquared = glm::dot(direction, direction);
			return lengthSquared >= MinUnitLengthSquared && lengthSquared <= MaxUnitLengthSquared;
		}

		Result<std::vector<glm::vec4>> GenerateCornerTangents(const TangentGeneratorInput& input)
		{
			ENGINE_CORE_ASSERT(input.Indices.size() % 3 == 0, "Tangent generation needs a triangle list");
			ENGINE_CORE_ASSERT(input.Normals.size() == input.Positions.size() && input.TexCoords.size() == input.Positions.size(),
				"Tangent generation needs one normal and one texture coordinate per position");
			if (input.Indices.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
			{
				return MakeError(ErrorCode::ImportFailed, "{} triangles are more than tangent generation (MikkTSpace) can address",
					input.Indices.size() / 3);
			}

			std::vector<glm::vec4> tangents(input.Indices.size(), glm::vec4(0.0f));
			MikkTSpaceMesh mesh{ .Input = &input, .Tangents = &tangents };
			SMikkTSpaceInterface callbacks{};
			callbacks.m_getNumFaces = &GetFaceCount;
			callbacks.m_getNumVerticesOfFace = &GetVertexCountOfFace;
			callbacks.m_getPosition = &GetPosition;
			callbacks.m_getNormal = &GetNormal;
			callbacks.m_getTexCoord = &GetTexCoord;
			callbacks.m_setTSpaceBasic = &SetCornerTangent;
			callbacks.m_setTSpace = nullptr;
			SMikkTSpaceContext context{};
			context.m_pInterface = &callbacks;
			context.m_pUserData = &mesh;
			if (genTangSpaceDefault(&context) == 0)
				return MakeError(ErrorCode::ImportFailed, "tangent generation (MikkTSpace) ran out of memory for {} triangles", input.Indices.size() / 3);

			for (size_t corner = 0; corner < tangents.size(); ++corner)
			{
				if (!IsUnitTangent(tangents[corner]))
					tangents[corner] = MakePerpendicularTangent(input.Normals[input.Indices[corner]]);
				else
					tangents[corner] = glm::vec4(glm::normalize(glm::vec3(tangents[corner])), tangents[corner].w);
			}
			return tangents;
		}

		glm::vec4 MakePerpendicularTangent(const glm::vec3& normal)
		{
			const glm::vec3 magnitude = glm::abs(normal);
			glm::vec3 axis(1.0f, 0.0f, 0.0f);
			if (magnitude.y < magnitude.x && magnitude.y <= magnitude.z)
				axis = glm::vec3(0.0f, 1.0f, 0.0f);
			else if (magnitude.z < magnitude.x && magnitude.z < magnitude.y)
				axis = glm::vec3(0.0f, 0.0f, 1.0f);
			const glm::vec3 tangent = glm::normalize(axis - normal * glm::dot(axis, normal));
			return glm::vec4(tangent, 1.0f);
		}

	}

}
