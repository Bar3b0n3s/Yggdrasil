#include "EnginePCH.h"
#include "Engine/Asset/MeshData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Utf8.h"

#include <cmath>
#include <limits>
#include <utility>

namespace Engine {

	namespace Utils {

		// How far a normal or a tangent direction may be from unit length (ValidateMeshData).
		static constexpr double UnitLengthTolerance = 1e-3;

		// The fixed sizes of the payload's records, for the size checks before anything is allocated.
		static constexpr size_t SubmeshRecordSize = 3 * sizeof(uint32_t) + 6 * sizeof(float);
		static constexpr size_t MinimumSlotRecordSize = sizeof(uint32_t) + sizeof(uint64_t);

		// The Streams bits this payload version defines a section for (MeshData version 2: the attribute stream, and C23.3's
		// skin and morph sections).
		static constexpr MeshStreams KnownStreams = MeshStreams::Attributes | MeshStreams::Skin | MeshStreams::Morph;

		// How far a skinned vertex's weights may sum from 1 (MeshSkinVertex).
		static constexpr double SkinWeightSumTolerance = 1e-3;

		static bool IsFinite(const glm::vec2& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		static bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		static bool IsFinite(const glm::vec4& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		// Whether `direction` is unit length within UnitLengthTolerance; the squared length is summed in double, so the check
		// does not depend on how a compiler orders float additions.
		static bool IsUnitLength(const glm::vec3& direction)
		{
			const double x = direction.x;
			const double y = direction.y;
			const double z = direction.z;
			const double length = std::sqrt(x * x + y * y + z * z);
			return std::fabs(length - 1.0) <= UnitLengthTolerance;
		}

		static void WriteVector(BinaryWriter& writer, const glm::vec3& value)
		{
			writer.WriteF32(value.x);
			writer.WriteF32(value.y);
			writer.WriteF32(value.z);
		}

		static Result<glm::vec3> ReadVector(BinaryReader& reader)
		{
			ENGINE_TRY_ASSIGN(const float x, reader.ReadF32());
			ENGINE_TRY_ASSIGN(const float y, reader.ReadF32());
			ENGINE_TRY_ASSIGN(const float z, reader.ReadF32());
			return glm::vec3(x, y, z);
		}

		static Result<Aabb> ReadBounds(BinaryReader& reader)
		{
			Aabb bounds;
			ENGINE_TRY_ASSIGN(bounds.Min, ReadVector(reader));
			ENGINE_TRY_ASSIGN(bounds.Max, ReadVector(reader));
			return bounds;
		}

		// Fails with Parse when `count` records of at least `recordSize` bytes cannot fit in what is left of the payload, so a
		// corrupt count is rejected before a container is sized from it.
		static Status CheckRecordCount(const BinaryReader& reader, uint32_t count, size_t recordSize, std::string_view what)
		{
			if (static_cast<uint64_t>(count) * recordSize > reader.GetRemaining())
			{
				return MakeError(ErrorCode::Parse, "mesh payload declares {} {}, more than the {} remaining bytes at offset {} can hold", count, what,
					reader.GetRemaining(), reader.GetPosition());
			}
			return {};
		}

		static Result<MeshData> ReadPayload(std::span<const std::byte> payload)
		{
			BinaryReader reader(payload);
			ENGINE_TRY_ASSIGN(const uint32_t vertexCount, reader.ReadU32());
			ENGINE_TRY_ASSIGN(const uint32_t indexCount, reader.ReadU32());
			ENGINE_TRY_ASSIGN(const uint32_t submeshCount, reader.ReadU32());
			ENGINE_TRY_ASSIGN(const uint32_t slotCount, reader.ReadU32());
			ENGINE_TRY_ASSIGN(const uint32_t streamBits, reader.ReadU32());
			ENGINE_TRY_ASSIGN(const uint32_t reserved, reader.ReadU32());
			const MeshStreams streams = static_cast<MeshStreams>(streamBits);
			if ((streams & ~KnownStreams) != MeshStreams::None)
			{
				return MakeError(ErrorCode::Parse, "mesh payload sets the stream bits {:#x}, which mesh format version {} does not define",
					streamBits & ~std::to_underlying(KnownStreams), MeshData::FormatVersion);
			}
			if (reserved != 0)
				return MakeError(ErrorCode::Parse, "mesh payload has the reserved word {:#x}, not 0", reserved);

			MeshData mesh;
			ENGINE_TRY_ASSIGN(mesh.Bounds, ReadBounds(reader));

			ENGINE_TRY(CheckRecordCount(reader, submeshCount, SubmeshRecordSize, "submeshes"));
			mesh.Submeshes.reserve(submeshCount);
			for (uint32_t index = 0; index < submeshCount; ++index)
			{
				MeshSubmesh submesh;
				ENGINE_TRY_ASSIGN(submesh.IndexOffset, reader.ReadU32());
				ENGINE_TRY_ASSIGN(submesh.IndexCount, reader.ReadU32());
				ENGINE_TRY_ASSIGN(submesh.MaterialSlot, reader.ReadU32());
				ENGINE_TRY_ASSIGN(submesh.Bounds, ReadBounds(reader));
				mesh.Submeshes.push_back(submesh);
			}

			ENGINE_TRY(CheckRecordCount(reader, slotCount, MinimumSlotRecordSize, "material slots"));
			mesh.Slots.reserve(slotCount);
			for (uint32_t index = 0; index < slotCount; ++index)
			{
				MeshMaterialSlot slot;
				ENGINE_TRY_ASSIGN(slot.Name, WithContext(reader.ReadString(), std::format("in the name of material slot {}", index)));
				ENGINE_TRY_ASSIGN(const uint64_t material, reader.ReadU64());
				slot.DefaultMaterial = AssetHandle(material);
				mesh.Slots.push_back(std::move(slot));
			}

			ENGINE_TRY_ASSIGN(mesh.Vertices, reader.ReadArray<MeshVertex>(vertexCount));
			ENGINE_TRY_ASSIGN(mesh.Indices, reader.ReadArray<uint32_t>(indexCount));
			if ((streams & MeshStreams::Attributes) != MeshStreams::None)
			{
				ENGINE_TRY_ASSIGN(mesh.Attributes, reader.ReadArray<MeshVertexAttributes>(vertexCount));
			}
			if ((streams & MeshStreams::Skin) != MeshStreams::None)
			{
				ENGINE_TRY_ASSIGN(mesh.SkinJointCount, reader.ReadU32());
				if (mesh.SkinJointCount == 0 || mesh.SkinJointCount > MaxMeshSkinJoints)
				{
					return MakeError(ErrorCode::Parse, "mesh payload's skin section addresses {} joints, not 1 to {}",
						mesh.SkinJointCount, MaxMeshSkinJoints);
				}
				ENGINE_TRY_ASSIGN(mesh.Skin, reader.ReadArray<MeshSkinVertex>(vertexCount));
			}
			if ((streams & MeshStreams::Morph) != MeshStreams::None)
			{
				ENGINE_TRY_ASSIGN(const uint32_t targetCount, reader.ReadU32());
				if (targetCount == 0 || targetCount > MaxMeshMorphTargets)
				{
					return MakeError(ErrorCode::Parse, "mesh payload's morph section holds {} targets, not 1 to {}", targetCount,
						MaxMeshMorphTargets);
				}
				mesh.MorphTargets.reserve(targetCount);
				for (uint32_t index = 0; index < targetCount; ++index)
				{
					MeshMorphTarget target;
					ENGINE_TRY_ASSIGN(target.Name, WithContext(reader.ReadString(), std::format("in the name of morph target {}", index)));
					ENGINE_TRY_ASSIGN(target.DefaultWeight, reader.ReadF32());
					ENGINE_TRY_ASSIGN(target.Deltas, reader.ReadArray<MeshMorphDelta>(vertexCount));
					mesh.MorphTargets.push_back(std::move(target));
				}
			}
			if (!reader.IsAtEnd())
				return MakeError(ErrorCode::Parse, "mesh payload has {} trailing bytes after offset {}", reader.GetRemaining(), reader.GetPosition());
			return mesh;
		}

	}

	Status ValidateMeshData(const MeshData& mesh)
	{
		constexpr uint64_t MaxCount = std::numeric_limits<uint32_t>::max();
		if (mesh.Vertices.empty())
			return MakeError(ErrorCode::Validation, "the mesh has no vertices");
		if (mesh.Indices.empty())
			return MakeError(ErrorCode::Validation, "the mesh has no indices");
		if (mesh.Submeshes.empty())
			return MakeError(ErrorCode::Validation, "the mesh has no submeshes");
		if (mesh.Vertices.size() > MaxCount || mesh.Indices.size() > MaxCount || mesh.Submeshes.size() > MaxCount || mesh.Slots.size() > MaxCount)
			return MakeError(ErrorCode::Validation, "the mesh has more than {} vertices, indices, submeshes or slots", MaxCount);

		Aabb bounds;
		for (size_t index = 0; index < mesh.Vertices.size(); ++index)
		{
			const MeshVertex& vertex = mesh.Vertices[index];
			if (!Utils::IsFinite(vertex.Position) || !Utils::IsFinite(vertex.Normal) || !Utils::IsFinite(vertex.Tangent) || !Utils::IsFinite(vertex.TexCoord))
				return MakeError(ErrorCode::Validation, "vertex {} has a non-finite value", index);
			if (!Utils::IsUnitLength(vertex.Normal))
				return MakeError(ErrorCode::Validation, "the normal of vertex {} is not unit length", index);
			if (!Utils::IsUnitLength(glm::vec3(vertex.Tangent)))
				return MakeError(ErrorCode::Validation, "the tangent of vertex {} is not unit length", index);
			if (vertex.Tangent.w != 1.0f && vertex.Tangent.w != -1.0f)
				return MakeError(ErrorCode::Validation, "the tangent sign of vertex {} is {}, not +1 or -1", index, vertex.Tangent.w);
			bounds.Extend(vertex.Position);
		}

		for (size_t index = 0; index < mesh.Indices.size(); ++index)
		{
			if (mesh.Indices[index] >= mesh.Vertices.size())
				return MakeError(ErrorCode::Validation, "index {} is {}, beyond the {} vertices", index, mesh.Indices[index], mesh.Vertices.size());
		}

		for (size_t index = 0; index < mesh.Submeshes.size(); ++index)
		{
			const MeshSubmesh& submesh = mesh.Submeshes[index];
			if (submesh.IndexOffset % 3 != 0 || submesh.IndexCount % 3 != 0)
			{
				return MakeError(ErrorCode::Validation, "submesh {} starts at index {} with {} indices; both must be multiples of 3", index,
					submesh.IndexOffset, submesh.IndexCount);
			}
			const uint64_t end = static_cast<uint64_t>(submesh.IndexOffset) + submesh.IndexCount;
			if (end > mesh.Indices.size())
			{
				return MakeError(ErrorCode::Validation, "submesh {} uses indices {} to {}, beyond the {} indices", index, submesh.IndexOffset, end,
					mesh.Indices.size());
			}
			if (submesh.MaterialSlot >= mesh.Slots.size())
			{
				return MakeError(ErrorCode::Validation, "submesh {} uses material slot {}, beyond the {} slots", index, submesh.MaterialSlot,
					mesh.Slots.size());
			}
			if (!Utils::IsFinite(submesh.Bounds.Min) || !Utils::IsFinite(submesh.Bounds.Max))
				return MakeError(ErrorCode::Validation, "the bounds of submesh {} are not finite", index);
			Aabb submeshBounds;
			for (uint64_t position = submesh.IndexOffset; position < end; ++position)
				submeshBounds.Extend(mesh.Vertices[mesh.Indices[static_cast<size_t>(position)]].Position);
			if (submeshBounds != submesh.Bounds)
				return MakeError(ErrorCode::Validation, "the bounds of submesh {} differ from the bounds of the vertices it references", index);
		}

		for (size_t index = 0; index < mesh.Slots.size(); ++index)
		{
			if (mesh.Slots[index].Name.size() > MaxCount || !IsValidUtf8(mesh.Slots[index].Name))
				return MakeError(ErrorCode::Validation, "the name of material slot {} is not valid UTF-8 of at most {} bytes", index, MaxCount);
		}

		if (!Utils::IsFinite(mesh.Bounds.Min) || !Utils::IsFinite(mesh.Bounds.Max))
			return MakeError(ErrorCode::Validation, "the mesh bounds are not finite");
		if (bounds != mesh.Bounds)
			return MakeError(ErrorCode::Validation, "the mesh bounds differ from the bounds of its vertices");

		if (!mesh.Attributes.empty() && mesh.Attributes.size() != mesh.Vertices.size())
		{
			return MakeError(ErrorCode::Validation, "the mesh has {} attribute vertices for its {} vertices; the attribute stream has one per vertex",
				mesh.Attributes.size(), mesh.Vertices.size());
		}
		for (size_t index = 0; index < mesh.Attributes.size(); ++index)
		{
			const MeshVertexAttributes& attributes = mesh.Attributes[index];
			if (!Utils::IsFinite(attributes.TexCoord1) || !Utils::IsFinite(attributes.Color))
				return MakeError(ErrorCode::Validation, "the attributes of vertex {} have a non-finite value", index);
			if (glm::any(glm::lessThan(attributes.Color, glm::vec4(0.0f))) || glm::any(glm::greaterThan(attributes.Color, glm::vec4(1.0f))))
				return MakeError(ErrorCode::Validation, "the colour of vertex {} has a channel outside [0, 1]", index);
		}

		// M23 (C23.3): the skin and the morph targets (MeshData.h).
		if (mesh.Skin.empty())
		{
			if (mesh.SkinJointCount != 0)
			{
				return MakeError(ErrorCode::Validation,
					"the mesh has no skin but addresses {} skin joints; a mesh without a skin addresses 0", mesh.SkinJointCount);
			}
		}
		else
		{
			if (mesh.Skin.size() != mesh.Vertices.size())
			{
				return MakeError(ErrorCode::Validation, "the mesh has {} skin vertices for its {} vertices; the skin has one per vertex", mesh.Skin.size(),
					mesh.Vertices.size());
			}
			if (mesh.SkinJointCount == 0 || mesh.SkinJointCount > MaxMeshSkinJoints)
			{
				return MakeError(ErrorCode::Validation, "the skin of the mesh addresses {} joints, not 1 to {}", mesh.SkinJointCount,
					MaxMeshSkinJoints);
			}
		}
		for (size_t index = 0; index < mesh.Skin.size(); ++index)
		{
			const MeshSkinVertex& skin = mesh.Skin[index];
			double sum = 0.0;
			for (glm::length_t influence = 0; influence < 4; ++influence)
			{
				const uint16_t joint = skin.Joints[static_cast<size_t>(influence)];
				const float weight = skin.Weights[influence];
				if (joint >= mesh.SkinJointCount)
				{
					return MakeError(ErrorCode::Validation, "skin vertex {} names joint {}, beyond the skin's {} joints", index, joint,
						mesh.SkinJointCount);
				}
				if (!std::isfinite(weight) || weight < 0.0f || weight > 1.0f)
					return MakeError(ErrorCode::Validation, "skin vertex {} has a weight outside [0, 1]", index);
				sum += weight;
			}
			if (std::fabs(sum - 1.0) > Utils::SkinWeightSumTolerance)
				return MakeError(ErrorCode::Validation, "the weights of skin vertex {} sum to {}, not 1", index, sum);
		}

		if (mesh.MorphTargets.size() > MaxMeshMorphTargets)
		{
			return MakeError(ErrorCode::Validation, "the mesh has {} morph targets, more than the {} a mesh may have", mesh.MorphTargets.size(),
				MaxMeshMorphTargets);
		}
		for (size_t index = 0; index < mesh.MorphTargets.size(); ++index)
		{
			const MeshMorphTarget& target = mesh.MorphTargets[index];
			if (target.Name.empty() || target.Name.size() > MaxMeshMorphTargetNameBytes || !IsValidUtf8(target.Name))
			{
				return MakeError(ErrorCode::Validation, "morph target {} has a name that is not 1 to {} bytes of valid UTF-8", index,
					MaxMeshMorphTargetNameBytes);
			}
			for (size_t earlier = 0; earlier < index; ++earlier)
			{
				if (mesh.MorphTargets[earlier].Name == target.Name)
					return MakeError(ErrorCode::Validation, "morph target {} is named '{}', as morph target {} is", index, target.Name, earlier);
			}
			if (!std::isfinite(target.DefaultWeight))
				return MakeError(ErrorCode::Validation, "morph target {} '{}' has a default weight that is not a finite number", index, target.Name);
			if (target.Deltas.size() != mesh.Vertices.size())
			{
				return MakeError(ErrorCode::Validation, "morph target {} '{}' has {} deltas for the mesh's {} vertices", index, target.Name, target.Deltas.size(),
					mesh.Vertices.size());
			}
			for (size_t vertex = 0; vertex < target.Deltas.size(); ++vertex)
			{
				const MeshMorphDelta& delta = target.Deltas[vertex];
				if (!Utils::IsFinite(delta.Position) || !Utils::IsFinite(delta.Normal) || !Utils::IsFinite(delta.Tangent))
					return MakeError(ErrorCode::Validation, "morph target {} '{}' has a non-finite delta at vertex {}", index, target.Name, vertex);
			}
		}
		return {};
	}

	MeshStreams GetMeshStreams(const MeshData& mesh)
	{
		MeshStreams streams = MeshStreams::None;
		if (!mesh.Attributes.empty())
			streams = streams | MeshStreams::Attributes;
		if (!mesh.Skin.empty())
			streams = streams | MeshStreams::Skin;
		if (!mesh.MorphTargets.empty())
			streams = streams | MeshStreams::Morph;
		return streams;
	}

	Buffer SerializeMeshPayload(const MeshData& mesh)
	{
		ENGINE_CORE_ASSERT(ValidateMeshData(mesh).has_value(), "SerializeMeshPayload needs a mesh that passes ValidateMeshData");

		BinaryWriter writer;
		writer.WriteU32(static_cast<uint32_t>(mesh.Vertices.size()));
		writer.WriteU32(static_cast<uint32_t>(mesh.Indices.size()));
		writer.WriteU32(static_cast<uint32_t>(mesh.Submeshes.size()));
		writer.WriteU32(static_cast<uint32_t>(mesh.Slots.size()));
		writer.WriteU32(std::to_underlying(GetMeshStreams(mesh)));
		writer.WriteU32(0); // Reserved (kept 0 by C23.3)
		Utils::WriteVector(writer, mesh.Bounds.Min);
		Utils::WriteVector(writer, mesh.Bounds.Max);
		for (const MeshSubmesh& submesh : mesh.Submeshes)
		{
			writer.WriteU32(submesh.IndexOffset);
			writer.WriteU32(submesh.IndexCount);
			writer.WriteU32(submesh.MaterialSlot);
			Utils::WriteVector(writer, submesh.Bounds.Min);
			Utils::WriteVector(writer, submesh.Bounds.Max);
		}
		for (const MeshMaterialSlot& slot : mesh.Slots)
		{
			writer.WriteString(slot.Name);
			writer.WriteU64(slot.DefaultMaterial.GetValue());
		}
		writer.WriteArray(std::span<const MeshVertex>(mesh.Vertices));
		writer.WriteArray(std::span<const uint32_t>(mesh.Indices));
		writer.WriteArray(std::span<const MeshVertexAttributes>(mesh.Attributes));
		if (!mesh.Skin.empty())
		{
			writer.WriteU32(mesh.SkinJointCount);
			writer.WriteArray(std::span<const MeshSkinVertex>(mesh.Skin));
		}
		if (!mesh.MorphTargets.empty())
		{
			writer.WriteU32(static_cast<uint32_t>(mesh.MorphTargets.size()));
			for (const MeshMorphTarget& target : mesh.MorphTargets)
			{
				writer.WriteString(target.Name);
				writer.WriteF32(target.DefaultWeight);
				writer.WriteArray(std::span<const MeshMorphDelta>(target.Deltas));
			}
		}
		return writer.TakeBuffer();
	}

	Result<MeshData> DeserializeMeshPayload(std::span<const std::byte> payload)
	{
		ENGINE_TRY_ASSIGN(MeshData mesh, Utils::ReadPayload(payload));
		ENGINE_TRY(WithContext(ValidateMeshData(mesh), "in a cooked mesh payload"));
		return mesh;
	}

	Buffer CookMesh(const MeshData& mesh, uint32_t importerVersion)
	{
		const Buffer payload = SerializeMeshPayload(mesh);
		return WriteCookedArtifact(AssetType::Mesh, MeshData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<MeshData>> LoadCookedMesh(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::Mesh, MeshData::FormatVersion));
		ENGINE_TRY_ASSIGN(MeshData mesh, DeserializeMeshPayload(artifact.Payload));
		return AssetRef<MeshData>(CreateRef<MeshData>(std::move(mesh)));
	}

}
