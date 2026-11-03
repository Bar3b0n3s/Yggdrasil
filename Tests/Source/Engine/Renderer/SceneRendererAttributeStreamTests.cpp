#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/MaterialConstants.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// M23 (C23.1, C-MESH2; Docs/Decisions/0034-m23-contract.md decisions 9 and 10, stream M): MeshData version 2's attribute
// stream drawn through vertex binding 1 of the mesh pipelines. The vertex colour always multiplies the base colour (RGBA),
// each map whose Material version 2 UV set is UV1 samples the second texture coordinates, and a Mask material's alpha
// test, in the forward passes, the overdraw view and the shadow casters alike, reads the base colour's UV set and the
// vertex colour's alpha (the selection mask's: SelectionPassTests.cpp).

namespace Engine {

	namespace {

		constexpr AssetHandle AttributeMesh{ 0x5A7E000000000001ULL };
		constexpr AssetHandle TwoTexelTexture{ 0x5A7E000000000002ULL };
		constexpr AssetHandle ProbeMaterial{ 0x5A7E000000000003ULL };
		constexpr AssetHandle SwappedMesh{ 0x5A7E000000000004ULL };
		constexpr AssetHandle AlphaTexelTexture{ 0x5A7E000000000005ULL };
		// Every map's UV1 bit of MaterialConstants::Flags.
		constexpr uint32_t MaterialFlagsUV1 = MaterialFlagBaseColorUV1 | MaterialFlagMetallicRoughnessUV1 | MaterialFlagNormalUV1 | MaterialFlagOcclusionUV1
			| MaterialFlagEmissiveUV1;

		// The integration fixture's quad (positions (+-1, +-1, 0)) with every vertex's TexCoord at `texCoord` and the attribute
		// stream's TexCoord1 and colour equal on every vertex, so the interpolated values are those.
		AssetRef<MeshData> MakeAttributeQuad(const glm::vec2& texCoord, const glm::vec2& texCoord1, const glm::vec4& color)
		{
			auto mesh = CreateRef<MeshData>();
			for (const glm::vec3& position : { glm::vec3(-1, -1, 0), glm::vec3(1, -1, 0), glm::vec3(1, 1, 0), glm::vec3(-1, 1, 0) })
			{
				mesh->Vertices.push_back({ .Position = position, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = texCoord });
				mesh->Attributes.push_back({ .TexCoord1 = texCoord1, .Color = color });
			}
			mesh->Indices = { 0, 1, 2, 0, 2, 3 };
			mesh->Bounds = { { -1, -1, 0 }, { 1, 1, 0 } };
			mesh->Submeshes = { { .IndexCount = 6, .Bounds = mesh->Bounds } };
			mesh->Slots = { {} };
			REQUIRE_MESSAGE(ValidateMeshData(*mesh).has_value(), ValidateMeshData(*mesh).error().ToString());
			return mesh;
		}

		// A 2x1 RGBA8 texture of the eight bytes `texels` (sampled at u 0.25 and 0.75, the texels' centres).
		AssetRef<TextureData> MakeTwoTexelTexture(const std::array<int, 8>& texels)
		{
			auto texture = CreateRef<TextureData>();
			texture->Format = TextureFormat::RGBA8Unorm;
			texture->Width = 2;
			texture->Height = 1;
			texture->Mips.push_back({ .Width = 2, .Height = 1, .Offset = 0, .Size = 8 });
			for (const int value : texels)
				texture->Pixels.push_back(static_cast<std::byte>(value));
			return texture;
		}

		// The left texel red, the right one blue.
		AssetRef<TextureData> MakeTwoTexelTexture()
		{
			return MakeTwoTexelTexture({ 255, 0, 0, 255, 0, 0, 255, 255 });
		}

		// White, the left texel transparent and the right one opaque.
		AssetRef<TextureData> MakeAlphaTexelTexture()
		{
			return MakeTwoTexelTexture({ 255, 255, 255, 0, 255, 255, 255, 255 });
		}

		// The centre pixel (on the fixture's quad) as RGB.
		glm::ivec3 GetCentre(const Image& image)
		{
			return { Test::SceneRendererIntegrationFixture::Channel(image, 32, 32, 0), Test::SceneRendererIntegrationFixture::Channel(image, 32, 32, 1),
				Test::SceneRendererIntegrationFixture::Channel(image, 32, 32, 2) };
		}

		bool IsNear(const glm::ivec3& a, const glm::ivec3& b, int tolerance)
		{
			return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
		}

		// One map of Material version 2, a view that shows it and the flag its UV set sets.
		struct UvSetMap
		{
			std::string_view Name{};
			RenderDebugView View = RenderDebugView::Lit;
			uint32_t Flag = 0;
			// Binds the two-texel texture `texture` to the map, sampled with `set`, with factors that show it.
			void (*Bind)(MaterialData& material, TypedAssetHandle<AssetType::Texture> texture, TextureCoordinateSet set) = nullptr;
		};

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SceneRenderer: the vertex colour multiplies the base colour, and a UV1 map samples the second texture coordinates"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.DebugView = RenderDebugView::Albedo;
			const auto render = [&fixture, &snapshot](AssetHandle mesh, const MaterialData& material)
			{
				fixture.Assets().Publish(ProbeMaterial, CreateRef<MaterialData>(material));
				snapshot.Meshes.front().Mesh = mesh;
				snapshot.Meshes.front().Materials = { ProbeMaterial };
				++snapshot.FrameIndex;
				return GetCentre(fixture.Render(snapshot));
			};

			// A vertex colour of (1, 0.5, 0.25) on a white material shows as that base colour factor on a mesh without the stream.
			fixture.Assets().Publish(AttributeMesh, MakeAttributeQuad(glm::vec2(0.0f), glm::vec2(0.0f), glm::vec4(1.0f, 0.5f, 0.25f, 1.0f)));
			const glm::ivec3 painted = render(AttributeMesh, MaterialData());
			MaterialData tinted;
			tinted.BaseColor = glm::vec4(1.0f, 0.5f, 0.25f, 1.0f);
			const glm::ivec3 factor = render(UUID(8910), tinted);
			CAPTURE(painted);
			CAPTURE(factor);
			CHECK(painted.x == 255);
			CHECK(painted.y < 220);
			CHECK(painted.z < painted.y);
			CHECK(IsNear(painted, factor, 1));
			// The mesh without the stream draws as before: its default stream is opaque white.
			CHECK(render(UUID(8910), MaterialData()) == glm::ivec3(255, 255, 255));

			// The base colour map samples TexCoord (u 0.25: red) with UV0 and TexCoord1 (u 0.75: blue) with UV1.
			fixture.Assets().Publish(TwoTexelTexture, MakeTwoTexelTexture());
			fixture.Assets().Publish(AttributeMesh, MakeAttributeQuad(glm::vec2(0.25f, 0.5f), glm::vec2(0.75f, 0.5f), glm::vec4(1.0f)));
			MaterialData mapped;
			mapped.BaseColorMap = TypedAssetHandle<AssetType::Texture>(TwoTexelTexture);
			CHECK(render(AttributeMesh, mapped) == glm::ivec3(255, 0, 0));
			mapped.BaseColorUVSet = TextureCoordinateSet::UV1;
			CHECK(render(AttributeMesh, mapped) == glm::ivec3(0, 0, 255));
			CHECK((fixture.Cache().GetMaterial(ProbeMaterial).Flags & MaterialFlagsUV1) == MaterialFlagBaseColorUV1);
			// The UV transform applies to both sets: an offset of half the texture turns UV1's blue into red.
			mapped.UVOffset = glm::vec2(0.5f, 0.0f);
			CHECK(render(AttributeMesh, mapped) == glm::ivec3(255, 0, 0));

			// Every map: its UV set sets its own flag and no other, and the shader samples the map with the set that flag
			// names, so the quad with the map on UV1 renders as the quad whose two sets are swapped with the map on UV0, and
			// not as the quad with the map on UV0. Each map shows in a view of its own: the base colour in Albedo, the
			// metallic-roughness map's metallic (blue) in Metallic, the normal map in Normals, the emissive map in Emissive and
			// the occlusion map (red: the left texel unoccluded, the right one fully occluded) in the lit image, lit by the
			// ambient alone.
			fixture.Assets().Publish(SwappedMesh, MakeAttributeQuad(glm::vec2(0.75f, 0.5f), glm::vec2(0.25f, 0.5f), glm::vec4(1.0f)));
			snapshot.Lights.clear();
			snapshot.Environment.FallbackColor = glm::vec3(1.0f);
			const std::array<UvSetMap, 5> maps = { {
				{ "BaseColor", RenderDebugView::Albedo, MaterialFlagBaseColorUV1,
					[](MaterialData& material, TypedAssetHandle<AssetType::Texture> texture, TextureCoordinateSet set)
			{
				material.BaseColorMap = texture;
				material.BaseColorUVSet = set;
			} },
				{ "MetallicRoughness", RenderDebugView::Metallic, MaterialFlagMetallicRoughnessUV1,
					[](MaterialData& material, TypedAssetHandle<AssetType::Texture> texture, TextureCoordinateSet set)
			{
				material.MetallicRoughnessMap = texture;
				material.Metallic = 1.0f;
				material.MetallicRoughnessUVSet = set;
			} },
				{ "Normal", RenderDebugView::Normals, MaterialFlagNormalUV1,
					[](MaterialData& material, TypedAssetHandle<AssetType::Texture> texture, TextureCoordinateSet set)
			{
				material.NormalMap = texture;
				material.NormalUVSet = set;
			} },
				{ "Occlusion", RenderDebugView::Lit, MaterialFlagOcclusionUV1,
					[](MaterialData& material, TypedAssetHandle<AssetType::Texture> texture, TextureCoordinateSet set)
			{
				material.OcclusionMap = texture;
				material.OcclusionStrength = 1.0f;
				material.OcclusionUVSet = set;
			} },
				{ "Emissive", RenderDebugView::Emissive, MaterialFlagEmissiveUV1,
					[](MaterialData& material, TypedAssetHandle<AssetType::Texture> texture, TextureCoordinateSet set)
			{
				material.EmissiveMap = texture;
				material.Emissive = glm::vec3(1.0f);
				material.EmissiveStrength = 1.0f;
				material.EmissiveUVSet = set;
			} },
			} };
			const TypedAssetHandle<AssetType::Texture> twoTexel(TwoTexelTexture);
			for (const UvSetMap& map : maps)
			{
				CAPTURE(std::string(map.Name));
				snapshot.DebugView = map.View;
				MaterialData second;
				map.Bind(second, twoTexel, TextureCoordinateSet::UV1);
				const glm::ivec3 onSecond = render(AttributeMesh, second);
				CHECK((fixture.Cache().GetMaterial(ProbeMaterial).Flags & MaterialFlagsUV1) == map.Flag);
				MaterialData first;
				map.Bind(first, twoTexel, TextureCoordinateSet::UV0);
				const glm::ivec3 swapped = render(SwappedMesh, first);
				CHECK((fixture.Cache().GetMaterial(ProbeMaterial).Flags & MaterialFlagsUV1) == 0);
				const glm::ivec3 onFirst = render(AttributeMesh, first);
				CAPTURE(onSecond);
				CAPTURE(swapped);
				CAPTURE(onFirst);
				CHECK(onSecond == swapped);
				CHECK_FALSE(IsNear(onSecond, onFirst, 32));
			}
		}

		TEST_CASE("SceneRenderer: a Mask material's alpha test reads the base colour's UV set and multiplies the vertex alpha in the forward, overdraw and shadow passes"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			MaterialData mask;
			mask.AlphaMode = AlphaMode::Mask;
			mask.AlphaCutoff = 0.5f;
			mask.Emissive = glm::vec3(1.0f, 0.0f, 0.0f);
			fixture.Assets().Publish(ProbeMaterial, CreateRef<MaterialData>(mask));
			const glm::vec4 opaqueWhite(1.0f);
			const glm::vec4 faint(1.0f, 1.0f, 1.0f, 0.25f);

			// In view, unlit: the faint quad is cut away (the clear colour shows), the opaque one emits red.
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.Camera.ClearColor = glm::vec3(0.0f, 0.0f, 1.0f);
			snapshot.Environment.FallbackColor = glm::vec3(0.0f);
			snapshot.Meshes.front().Mesh = AttributeMesh;
			snapshot.Meshes.front().Materials = { ProbeMaterial };
			fixture.Assets().Publish(AttributeMesh, MakeAttributeQuad(glm::vec2(0.0f), glm::vec2(0.0f), faint));
			const glm::ivec3 cut = GetCentre(fixture.Render(snapshot));
			fixture.Assets().Publish(AttributeMesh, MakeAttributeQuad(glm::vec2(0.0f), glm::vec2(0.0f), opaqueWhite));
			++snapshot.FrameIndex;
			const glm::ivec3 kept = GetCentre(fixture.Render(snapshot));
			CAPTURE(cut);
			CAPTURE(kept);
			CHECK(cut == glm::ivec3(0, 0, 255));
			CHECK(kept.x > 200);
			CHECK(kept.z < 50);

			// The alpha test samples the base colour map with the base colour's UV set: the quad's TexCoord (u 0.25) reads the
			// transparent texel, its TexCoord1 (u 0.75) the opaque one. The overdraw view counts only the fragments the test
			// keeps (a black pixel counts none), and the faint vertex alpha cuts the opaque texel away there too.
			fixture.Assets().Publish(AlphaTexelTexture, MakeAlphaTexelTexture());
			MaterialData textured = mask;
			textured.BaseColorMap = TypedAssetHandle<AssetType::Texture>(AlphaTexelTexture);
			const auto renderMask = [&fixture, &snapshot](const MaterialData& material, const glm::vec4& color, RenderDebugView view)
			{
				fixture.Assets().Publish(AttributeMesh, MakeAttributeQuad(glm::vec2(0.25f, 0.5f), glm::vec2(0.75f, 0.5f), color));
				fixture.Assets().Publish(ProbeMaterial, CreateRef<MaterialData>(material));
				snapshot.DebugView = view;
				++snapshot.FrameIndex;
				return GetCentre(fixture.Render(snapshot));
			};
			CHECK(renderMask(textured, opaqueWhite, RenderDebugView::Lit) == glm::ivec3(0, 0, 255));
			CHECK(renderMask(textured, opaqueWhite, RenderDebugView::Overdraw) == glm::ivec3(0, 0, 0));
			textured.BaseColorUVSet = TextureCoordinateSet::UV1;
			const glm::ivec3 secondSet = renderMask(textured, opaqueWhite, RenderDebugView::Lit);
			CAPTURE(secondSet);
			CHECK(secondSet.x > 200);
			CHECK(secondSet.z < 50);
			CHECK(renderMask(textured, opaqueWhite, RenderDebugView::Overdraw) != glm::ivec3(0, 0, 0));
			CHECK(renderMask(textured, faint, RenderDebugView::Overdraw) == glm::ivec3(0, 0, 0));
			snapshot.DebugView = RenderDebugView::Lit;

			// Out of view (behind the camera, on the sun's ray to the receiver's centre): the faint caster casts no shadow.
			fixture.Assets().Publish(ProbeMaterial, CreateRef<MaterialData>(mask));
			fixture.Assets().Publish(AttributeMesh, MakeAttributeQuad(glm::vec2(0.0f), glm::vec2(0.0f), opaqueWhite));
			RenderSnapshot shadow = fixture.Snapshot();
			shadow.FrameIndex = snapshot.FrameIndex; // each render below takes the next frame
			shadow.Environment.FallbackColor = glm::vec3(0.0f);
			shadow.Meshes[0].World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(4.0f, 4.0f, 1.0f));
			shadow.Meshes[0].CastShadows = false;
			MeshDrawItem caster = shadow.Meshes[0];
			caster.Entity = UUID(40);
			caster.PickId = 2;
			caster.CastShadows = true;
			caster.Mesh = AttributeMesh;
			caster.Materials = { ProbeMaterial };
			caster.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 2.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
			shadow.Meshes.push_back(caster);
			LightData sun;
			sun.Direction = glm::vec3(0.0f, 0.0f, -1.0f);
			sun.Intensity = 0.5f;
			sun.CastShadows = true;
			sun.ShadowDistance = 10.0f;
			sun.CascadeCount = 1;
			sun.DepthBias = sun.NormalBias = 0.0f;
			sun.LightAngle = 0.0f;
			sun.Entity = UUID(22);
			shadow.Lights = { sun };
			const auto centreRed = [&fixture, &shadow]()
			{
				++shadow.FrameIndex;
				return Test::SceneRendererIntegrationFixture::Channel(fixture.Render(shadow), 32, 32);
			};
			const int shadowed = centreRed();
			fixture.Assets().Publish(AttributeMesh, MakeAttributeQuad(glm::vec2(0.0f), glm::vec2(0.0f), faint));
			const int unshadowed = centreRed();
			shadow.Meshes[1].CastShadows = false;
			const int lit = centreRed();
			CAPTURE(shadowed);
			CAPTURE(unshadowed);
			CAPTURE(lit);
			CHECK(lit > shadowed + 100);
			CHECK(std::abs(unshadowed - lit) <= 1);

			// The casters' alpha test reads the base colour's UV set too: the transparent texel on UV0 casts no shadow, the
			// opaque one on UV1 does.
			shadow.Meshes[1].CastShadows = true;
			fixture.Assets().Publish(AttributeMesh, MakeAttributeQuad(glm::vec2(0.25f, 0.5f), glm::vec2(0.75f, 0.5f), opaqueWhite));
			MaterialData casterMask = mask;
			casterMask.BaseColorMap = TypedAssetHandle<AssetType::Texture>(AlphaTexelTexture);
			fixture.Assets().Publish(ProbeMaterial, CreateRef<MaterialData>(casterMask));
			const int firstSet = centreRed();
			casterMask.BaseColorUVSet = TextureCoordinateSet::UV1;
			fixture.Assets().Publish(ProbeMaterial, CreateRef<MaterialData>(casterMask));
			const int secondSetShadow = centreRed();
			CAPTURE(firstSet);
			CAPTURE(secondSetShadow);
			CHECK(std::abs(firstSet - lit) <= 1);
			CHECK(std::abs(secondSetShadow - shadowed) <= 1);
		}
	}

}
