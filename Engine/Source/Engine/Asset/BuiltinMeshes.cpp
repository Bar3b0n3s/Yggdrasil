#include "EnginePCH.h"
#include "Engine/Asset/BuiltinMeshes.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

// Every value is computed in double from DetMath and IEEE-exact operations, then rounded to float once, so the bytes do
// not depend on the C runtime or on how a compiler schedules float arithmetic (§4.12). Angles that are multiples of a
// quarter turn use exact sines and cosines, which keeps the shapes exactly symmetric and their bounds exactly unit size.
//
// Texture coordinates follow glTF: origin at the top left, V growing downwards. Seen from outside, every surface shows
// its texture upright and unmirrored, so the bitangent cross(Normal, Tangent) points to the top of the texture and every
// tangent sign is +1.

namespace Engine {

	namespace {

		// A point of the unit circle: cos and sin of an angle.
		struct CirclePoint
		{
			double Cos = 1.0;
			double Sin = 0.0;
		};

		// A direction or position in double precision.
		struct Vector3
		{
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
		};

		// One row of a surface of revolution around Y, listed top to bottom.
		struct ProfileRow
		{
			double Radius = 0.0; // 0: a pole (or apex) row
			double Y = 0.0;
			double NormalRadial = 0.0; // the unit normal of the profile curve: outward radial and Y components
			double NormalY = 0.0;
			double V = 0.0; // the texture's V coordinate of the row
		};

		// The segment count of the round meshes (BuiltinMeshes.h).
		constexpr uint32_t RoundSegments = 32;
		constexpr uint32_t SphereRings = 16;
		constexpr uint32_t CapsuleHemisphereRings = 8;

		// The radius of every round mesh: 1 m across.
		constexpr double RoundRadius = 0.5;

	}

	namespace Utils {

		// cos and sin of `index / count` of a full turn; exact at multiples of a quarter turn.
		static CirclePoint TurnFraction(uint32_t index, uint32_t count)
		{
			if ((static_cast<uint64_t>(index) * 4) % count == 0)
			{
				switch ((static_cast<uint64_t>(index) * 4 / count) % 4)
				{
					case 0:  return { .Cos = 1.0, .Sin = 0.0 };
					case 1:  return { .Cos = 0.0, .Sin = 1.0 };
					case 2:  return { .Cos = -1.0, .Sin = 0.0 };
					default: return { .Cos = 0.0, .Sin = -1.0 };
				}
			}
			const double angle = 2.0 * std::numbers::pi * static_cast<double>(index) / static_cast<double>(count);
			const SinCosResult<double> result = DetMath::SinCos(angle);
			return { .Cos = result.Cos, .Sin = result.Sin };
		}

		// The outward horizontal direction at the angle of `point` around Y. U grows with the angle; U = 0 faces -Z (the
		// seam is at the back), U = 0.25 faces -X and U = 0.5 faces +Z.
		static Vector3 Outward(const CirclePoint& point)
		{
			return { .X = -point.Sin, .Y = 0.0, .Z = -point.Cos };
		}

		// The direction of growing U at the angle of `point` (the derivative of Outward).
		static Vector3 AlongU(const CirclePoint& point)
		{
			return { .X = -point.Cos, .Y = 0.0, .Z = point.Sin };
		}

		static Vector3 Normalize(const Vector3& vector)
		{
			const double length = std::sqrt(vector.X * vector.X + vector.Y * vector.Y + vector.Z * vector.Z);
			return { .X = vector.X / length, .Y = vector.Y / length, .Z = vector.Z / length };
		}

		// Rounds to float; adding +0 turns -0 into +0, so every zero has one bit pattern.
		static float ToFloat(double value)
		{
			return static_cast<float>(value) + 0.0f;
		}

		static glm::vec3 ToFloat(const Vector3& vector)
		{
			return { ToFloat(vector.X), ToFloat(vector.Y), ToFloat(vector.Z) };
		}

	}

	namespace {

		// Collects the vertices and triangles of one mesh, then gives it its bounds, submesh and slot.
		class MeshBuilder
		{
		public:
			// Adds a vertex with `normal` and `tangent` normalized (sign +1) and returns its index.
			uint32_t AddVertex(const Vector3& position, const Vector3& normal, const Vector3& tangent, double u, double v)
			{
				m_Mesh.Vertices.push_back({
					.Position = Utils::ToFloat(position),
					.Normal = Utils::ToFloat(Utils::Normalize(normal)),
					.Tangent = glm::vec4(Utils::ToFloat(Utils::Normalize(tangent)), 1.0f),
					.TexCoord = { Utils::ToFloat(u), Utils::ToFloat(v) },
				});
				return static_cast<uint32_t>(m_Mesh.Vertices.size() - 1);
			}

			// A counter-clockwise triangle seen from its front.
			void AddTriangle(uint32_t a, uint32_t b, uint32_t c)
			{
				m_Mesh.Indices.insert(m_Mesh.Indices.end(), { a, b, c });
			}

			// A flat quad facing `normal`, 1 m x 1 m, centred on `center`; `alongU` is the direction of growing U and
			// cross(normal, alongU) the top of the texture.
			void AddQuad(const Vector3& center, const Vector3& normal, const Vector3& alongU)
			{
				const Vector3 up = {
					.X = normal.Y * alongU.Z - normal.Z * alongU.Y,
					.Y = normal.Z * alongU.X - normal.X * alongU.Z,
					.Z = normal.X * alongU.Y - normal.Y * alongU.X,
				};
				const auto corner = [&center, &alongU, &up](double u, double v)
				{
					const double right = u - 0.5;
					const double top = 0.5 - v;
					return Vector3{
						.X = center.X + right * alongU.X + top * up.X,
						.Y = center.Y + right * alongU.Y + top * up.Y,
						.Z = center.Z + right * alongU.Z + top * up.Z,
					};
				};
				const uint32_t bottomLeft = AddVertex(corner(0.0, 1.0), normal, alongU, 0.0, 1.0);
				const uint32_t bottomRight = AddVertex(corner(1.0, 1.0), normal, alongU, 1.0, 1.0);
				const uint32_t topRight = AddVertex(corner(1.0, 0.0), normal, alongU, 1.0, 0.0);
				const uint32_t topLeft = AddVertex(corner(0.0, 0.0), normal, alongU, 0.0, 0.0);
				AddTriangle(bottomLeft, bottomRight, topRight);
				AddTriangle(bottomLeft, topRight, topLeft);
			}

			// A surface of revolution around Y with `segments` segments: one vertex row per profile row (a ring with a seam
			// vertex at U = 1, or for a pole one vertex per segment at the segment's middle) and the triangles between
			// consecutive rows. Two consecutive poles are not allowed.
			void AddRevolution(std::span<const ProfileRow> rows, uint32_t segments)
			{
				ENGINE_CORE_ASSERT(rows.size() >= 2, "a surface of revolution needs at least two rows");
				uint32_t previousStart = 0;
				bool previousIsPole = false;
				for (size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
				{
					const ProfileRow& row = rows[rowIndex];
					const bool isPole = row.Radius == 0.0;
					const uint32_t start = static_cast<uint32_t>(m_Mesh.Vertices.size());
					const uint32_t count = isPole ? segments : segments + 1;
					for (uint32_t index = 0; index < count; ++index)
					{
						// A pole vertex sits at its segment's middle, so the segment's triangle gets a tangent and U between
						// its two ring vertices.
						const CirclePoint point = isPole ? Utils::TurnFraction(2 * index + 1, 2 * segments) : Utils::TurnFraction(index, segments);
						const double u = isPole ? (static_cast<double>(index) + 0.5) / segments : static_cast<double>(index) / segments;
						const Vector3 outward = Utils::Outward(point);
						const Vector3 position = { .X = row.Radius * outward.X, .Y = row.Y, .Z = row.Radius * outward.Z };
						const Vector3 normal = { .X = row.NormalRadial * outward.X, .Y = row.NormalY, .Z = row.NormalRadial * outward.Z };
						AddVertex(position, normal, Utils::AlongU(point), u, row.V);
					}

					if (rowIndex > 0)
					{
						ENGINE_CORE_ASSERT(!(isPole && previousIsPole), "two consecutive pole rows");
						for (uint32_t segment = 0; segment < segments; ++segment)
						{
							if (previousIsPole)
							{
								AddTriangle(start + segment, start + segment + 1, previousStart + segment);
							}
							else if (isPole)
							{
								AddTriangle(start + segment, previousStart + segment + 1, previousStart + segment);
							}
							else
							{
								const uint32_t bottomLeft = start + segment;
								const uint32_t topLeft = previousStart + segment;
								AddTriangle(bottomLeft, bottomLeft + 1, topLeft + 1);
								AddTriangle(bottomLeft, topLeft + 1, topLeft);
							}
						}
					}
					previousStart = start;
					previousIsPole = isPole;
				}
			}

			// A disc of radius 0.5 at height `y` facing +Y (`facesUp`) or -Y, textured by planar projection: U grows with X,
			// and the top of the texture is -Z on the upper disc and +Z on the lower one, as on the cube's +Y and -Y faces.
			void AddDisc(double y, bool facesUp, uint32_t segments)
			{
				const Vector3 normal = { .X = 0.0, .Y = facesUp ? 1.0 : -1.0, .Z = 0.0 };
				const Vector3 alongU = { .X = 1.0, .Y = 0.0, .Z = 0.0 };
				const double vPerZ = facesUp ? 1.0 : -1.0;
				const uint32_t center = AddVertex({ .X = 0.0, .Y = y, .Z = 0.0 }, normal, alongU, 0.5, 0.5);
				const uint32_t first = static_cast<uint32_t>(m_Mesh.Vertices.size());
				for (uint32_t index = 0; index < segments; ++index)
				{
					const Vector3 outward = Utils::Outward(Utils::TurnFraction(index, segments));
					const Vector3 position = { .X = RoundRadius * outward.X, .Y = y, .Z = RoundRadius * outward.Z };
					AddVertex(position, normal, alongU, 0.5 + position.X, 0.5 + vPerZ * position.Z);
				}
				for (uint32_t index = 0; index < segments; ++index)
				{
					const uint32_t current = first + index;
					const uint32_t next = first + (index + 1) % segments;
					if (facesUp)
						AddTriangle(center, current, next);
					else
						AddTriangle(center, next, current);
				}
			}

			// The finished mesh: bounds of every vertex, one submesh over every index, one "Default" slot.
			[[nodiscard]] MeshData Finish()
			{
				for (const MeshVertex& vertex : m_Mesh.Vertices)
					m_Mesh.Bounds.Extend(vertex.Position);
				m_Mesh.Submeshes.push_back({
					.IndexOffset = 0,
					.IndexCount = static_cast<uint32_t>(m_Mesh.Indices.size()),
					.MaterialSlot = 0,
					.Bounds = m_Mesh.Bounds,
				});
				m_Mesh.Slots.push_back({ .Name = "Default", .DefaultMaterial = AssetHandle() });
				return std::move(m_Mesh);
			}
		private:
			MeshData m_Mesh;
		};

	}

	namespace Utils {

		static MeshData GenerateCube()
		{
			struct Face
			{
				Vector3 Normal{};
				Vector3 AlongU{};
			};
			// Seen from outside, U runs to the right and the top of the texture is up (+Y), or -Z on top and +Z below.
			constexpr std::array<Face, 6> Faces = { {
				{ .Normal = { 1.0, 0.0, 0.0 }, .AlongU = { 0.0, 0.0, -1.0 } },
				{ .Normal = { -1.0, 0.0, 0.0 }, .AlongU = { 0.0, 0.0, 1.0 } },
				{ .Normal = { 0.0, 1.0, 0.0 }, .AlongU = { 1.0, 0.0, 0.0 } },
				{ .Normal = { 0.0, -1.0, 0.0 }, .AlongU = { 1.0, 0.0, 0.0 } },
				{ .Normal = { 0.0, 0.0, 1.0 }, .AlongU = { 1.0, 0.0, 0.0 } },
				{ .Normal = { 0.0, 0.0, -1.0 }, .AlongU = { -1.0, 0.0, 0.0 } },
			} };
			MeshBuilder builder;
			for (const Face& face : Faces)
			{
				const Vector3 center = { .X = RoundRadius * face.Normal.X, .Y = RoundRadius * face.Normal.Y, .Z = RoundRadius * face.Normal.Z };
				builder.AddQuad(center, face.Normal, face.AlongU);
			}
			return builder.Finish();
		}

		static MeshData GenerateSphere()
		{
			std::array<ProfileRow, SphereRings + 1> rows{};
			for (uint32_t ring = 0; ring <= SphereRings; ++ring)
			{
				// The polar angle runs from 0 (top) to half a turn (bottom).
				const CirclePoint polar = TurnFraction(ring, 2 * SphereRings);
				rows[ring] = {
					.Radius = RoundRadius * polar.Sin,
					.Y = RoundRadius * polar.Cos,
					.NormalRadial = polar.Sin,
					.NormalY = polar.Cos,
					.V = static_cast<double>(ring) / SphereRings,
				};
			}
			MeshBuilder builder;
			builder.AddRevolution(rows, RoundSegments);
			return builder.Finish();
		}

		static MeshData GeneratePlane()
		{
			constexpr uint32_t Cells = 10;
			MeshBuilder builder;
			// Facing +Y: U grows with X and the top of the texture is -Z, so V grows with Z.
			const Vector3 normal = { .X = 0.0, .Y = 1.0, .Z = 0.0 };
			const Vector3 alongU = { .X = 1.0, .Y = 0.0, .Z = 0.0 };
			for (uint32_t row = 0; row <= Cells; ++row)
			{
				for (uint32_t column = 0; column <= Cells; ++column)
				{
					const double u = static_cast<double>(column) / Cells;
					const double v = static_cast<double>(row) / Cells;
					builder.AddVertex({ .X = u - 0.5, .Y = 0.0, .Z = v - 0.5 }, normal, alongU, u, v);
				}
			}
			for (uint32_t row = 0; row < Cells; ++row)
			{
				for (uint32_t column = 0; column < Cells; ++column)
				{
					const uint32_t topLeft = row * (Cells + 1) + column;
					const uint32_t bottomLeft = topLeft + Cells + 1;
					builder.AddTriangle(bottomLeft, bottomLeft + 1, topLeft + 1);
					builder.AddTriangle(bottomLeft, topLeft + 1, topLeft);
				}
			}
			return builder.Finish();
		}

		static MeshData GenerateQuad()
		{
			MeshBuilder builder;
			builder.AddQuad({ 0.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 }, { 1.0, 0.0, 0.0 });
			return builder.Finish();
		}

		static MeshData GenerateCylinder()
		{
			const std::array<ProfileRow, 2> rows = { {
				{ .Radius = RoundRadius, .Y = 0.5, .NormalRadial = 1.0, .NormalY = 0.0, .V = 0.0 },
				{ .Radius = RoundRadius, .Y = -0.5, .NormalRadial = 1.0, .NormalY = 0.0, .V = 1.0 },
			} };
			MeshBuilder builder;
			builder.AddRevolution(rows, RoundSegments);
			builder.AddDisc(0.5, true, RoundSegments);
			builder.AddDisc(-0.5, false, RoundSegments);
			return builder.Finish();
		}

		static MeshData GenerateCapsule()
		{
			// Two hemispheres of radius 0.5 around a 1 m cylinder; V follows the arc length of the profile, so the texture
			// is not stretched along it.
			const double quarterArc = std::numbers::pi * RoundRadius / 2.0;
			const double cylinderLength = 1.0;
			const double length = 2.0 * quarterArc + cylinderLength;
			std::array<ProfileRow, 2 * (CapsuleHemisphereRings + 1)> rows{};
			for (uint32_t ring = 0; ring <= CapsuleHemisphereRings; ++ring)
			{
				// Polar angles from 0 to a quarter turn on the upper hemisphere, from a quarter to half a turn on the lower one.
				const CirclePoint upper = TurnFraction(ring, 4 * CapsuleHemisphereRings);
				const CirclePoint lower = TurnFraction(CapsuleHemisphereRings + ring, 4 * CapsuleHemisphereRings);
				const double arc = quarterArc * static_cast<double>(ring) / CapsuleHemisphereRings;
				rows[ring] = {
					.Radius = RoundRadius * upper.Sin,
					.Y = 0.5 + RoundRadius * upper.Cos,
					.NormalRadial = upper.Sin,
					.NormalY = upper.Cos,
					.V = arc / length,
				};
				rows[CapsuleHemisphereRings + 1 + ring] = {
					.Radius = RoundRadius * lower.Sin,
					.Y = -0.5 + RoundRadius * lower.Cos,
					.NormalRadial = lower.Sin,
					.NormalY = lower.Cos,
					.V = (quarterArc + cylinderLength + arc) / length,
				};
			}
			MeshBuilder builder;
			builder.AddRevolution(rows, RoundSegments);
			return builder.Finish();
		}

		static MeshData GenerateCone()
		{
			// The slope's outward normal: perpendicular to the side from the base rim (0.5, -0.5) to the apex (0, 0.5).
			const double height = 1.0;
			const double slope = std::sqrt(height * height + RoundRadius * RoundRadius);
			const double normalRadial = height / slope;
			const double normalY = RoundRadius / slope;
			const std::array<ProfileRow, 2> rows = { {
				{ .Radius = 0.0, .Y = 0.5, .NormalRadial = normalRadial, .NormalY = normalY, .V = 0.0 },
				{ .Radius = RoundRadius, .Y = -0.5, .NormalRadial = normalRadial, .NormalY = normalY, .V = 1.0 },
			} };
			MeshBuilder builder;
			builder.AddRevolution(rows, RoundSegments);
			builder.AddDisc(-0.5, false, RoundSegments);
			return builder.Finish();
		}

		static MeshData GenerateShape(BuiltinMesh mesh)
		{
			switch (mesh)
			{
				case BuiltinMesh::Cube:     return GenerateCube();
				case BuiltinMesh::Sphere:   return GenerateSphere();
				case BuiltinMesh::Plane:    return GeneratePlane();
				case BuiltinMesh::Quad:     return GenerateQuad();
				case BuiltinMesh::Cylinder: return GenerateCylinder();
				case BuiltinMesh::Capsule:  return GenerateCapsule();
				case BuiltinMesh::Cone:     return GenerateCone();
			}
			ENGINE_CORE_ASSERT(false, "Unknown BuiltinMesh {}", std::to_underlying(mesh));
			return GenerateCube();
		}

	}

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

	MeshData GenerateBuiltinMesh(BuiltinMesh mesh)
	{
		MeshData generated = Utils::GenerateShape(mesh);
		ENGINE_CORE_ASSERT(ValidateMeshData(generated).has_value(), "the built-in mesh '{}' fails ValidateMeshData", BuiltinMeshToString(mesh));
		return generated;
	}

}
