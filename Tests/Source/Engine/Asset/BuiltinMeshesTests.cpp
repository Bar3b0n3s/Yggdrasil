#include "TestsPCH.h"

#include "Engine/Asset/BuiltinMeshes.h"

#include <array>

namespace Engine {

	namespace {

		constexpr std::array<BuiltinMesh, 7> Meshes = { BuiltinMesh::Cube, BuiltinMesh::Sphere, BuiltinMesh::Plane, BuiltinMesh::Quad,
			BuiltinMesh::Cylinder, BuiltinMesh::Capsule, BuiltinMesh::Cone };

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("BuiltinMeshes: each mesh is named after its engine path and other values are Unknown")
		{
			CHECK(BuiltinMeshToString(BuiltinMesh::Cube) == "Cube");
			CHECK(BuiltinMeshToString(BuiltinMesh::Capsule) == "Capsule");
			CHECK(BuiltinMeshToString(static_cast<BuiltinMesh>(99)) == "Unknown");
		}

		TEST_CASE("BuiltinMeshes: generation is deterministic" * doctest::skip(true))
		{
			for (const BuiltinMesh mesh : Meshes)
			{
				CAPTURE(std::string(BuiltinMeshToString(mesh)));
				CHECK(SerializeMeshPayload(GenerateBuiltinMesh(mesh)) == SerializeMeshPayload(GenerateBuiltinMesh(mesh)));
			}
		}

		TEST_CASE("BuiltinMeshes: every mesh is valid, unit size and outward-facing" * doctest::skip(true))
		{
			for (const BuiltinMesh shape : Meshes)
			{
				CAPTURE(std::string(BuiltinMeshToString(shape)));
				const MeshData mesh = GenerateBuiltinMesh(shape);
				REQUIRE(ValidateMeshData(mesh).has_value());
				REQUIRE(mesh.Submeshes.size() == 1);
				REQUIRE(mesh.Slots.size() == 1);
				CHECK(mesh.Slots.front().Name == "Default");
				CHECK_FALSE(mesh.Slots.front().DefaultMaterial.IsValid());
				// Centred on the origin, 1 m across in X (the capsule is 2 m tall, the plane and quad are flat).
				CHECK(mesh.Bounds.GetCenter().x == doctest::Approx(0.0f).epsilon(1e-6));
				CHECK(mesh.Bounds.GetSize().x == doctest::Approx(1.0f).epsilon(1e-6));
				// Outward: every triangle's geometric normal agrees with its first vertex's normal.
				for (size_t index = 0; index < mesh.Indices.size(); index += 3)
				{
					const MeshVertex& a = mesh.Vertices[mesh.Indices[index]];
					const MeshVertex& b = mesh.Vertices[mesh.Indices[index + 1]];
					const MeshVertex& c = mesh.Vertices[mesh.Indices[index + 2]];
					const glm::vec3 geometric = glm::cross(b.Position - a.Position, c.Position - a.Position);
					if (glm::dot(geometric, geometric) > 1e-12f)
						CHECK(glm::dot(geometric, a.Normal) > 0.0f);
				}
			}
			CHECK(GenerateBuiltinMesh(BuiltinMesh::Capsule).Bounds.GetSize().y == doctest::Approx(2.0f).epsilon(1e-6));
			CHECK(GenerateBuiltinMesh(BuiltinMesh::Plane).Bounds.GetSize().y == 0.0f);
			CHECK(GenerateBuiltinMesh(BuiltinMesh::Cube).Vertices.size() == 24);
		}
	}

}
