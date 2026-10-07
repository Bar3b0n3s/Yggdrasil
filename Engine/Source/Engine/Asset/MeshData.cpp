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
		return {};
	}

	Buffer SerializeMeshPayload(const MeshData& mesh)
	{
		ENGINE_CORE_ASSERT(ValidateMeshData(mesh).has_value(), "SerializeMeshPayload needs a mesh that passes ValidateMeshData");

		BinaryWriter writer;
		writer.WriteU32(static_cast<uint32_t>(mesh.Vertices.size()));
		writer.WriteU32(static_cast<uint32_t>(mesh.Indices.size()));
		writer.WriteU32(static_cast<uint32_t>(mesh.Submeshes.size()));
		writer.WriteU32(static_cast<uint32_t>(mesh.Slots.size()));
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
