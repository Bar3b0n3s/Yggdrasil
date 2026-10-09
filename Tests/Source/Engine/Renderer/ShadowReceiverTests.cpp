#include "TestsPCH.h"

#include "Engine/Asset/TextureData.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <cmath>

namespace Engine {

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Shadows: a planar receiver never shadows itself and retains a nearby occluder")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 128, 128);
			for (const bool spot : { false, true })
				for (const RenderProjection projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
					for (uint32_t normalMode = 0; normalMode < 3; ++normalMode)
						for (uint32_t shadowMapSize : { 256U, 1024U })
						{
							CAPTURE(spot);
							CAPTURE(normalMode);
							CAPTURE(shadowMapSize);
							auto material = CreateRef<MaterialData>();
							if (normalMode == 1)
							{
								auto texture = CreateRef<TextureData>();
								texture->Width = texture->Height = 1;
								texture->Mips = { { .Width = 1, .Height = 1, .Offset = 0, .Size = 4 } };
								texture->Pixels = { std::byte{ 128 }, std::byte{ 230 }, std::byte{ 205 }, std::byte{ 255 } };
								fixture.Assets().Publish(UUID(8912), texture);
								material->NormalMap = TypedAssetHandle<AssetType::Texture>(UUID(8912));
							}
							fixture.Assets().Publish(UUID(8911), material);
							// The plane's vertex normals may be smoothly interpolated, as on a low-poly curved surface.
							const auto source = fixture.Assets().GetOrPlaceholder<MeshData>(UUID(8910));
							auto mesh = CreateRef<MeshData>(*source);
							for (auto& vertex : mesh->Vertices)
								vertex.Normal = normalMode == 2 ? glm::normalize(glm::vec3(vertex.Position.x * 0.35f, vertex.Position.y * 0.35f, 1)) : glm::vec3(0, 0, 1);
							fixture.Assets().Publish(UUID(8910), mesh);
							CAPTURE(static_cast<uint32_t>(projection));
							auto snapshot = fixture.Snapshot(projection);
							snapshot.Quality.ShadowMapSize = shadowMapSize;
							snapshot.Meshes[0].World = glm::scale(glm::translate(glm::mat4(1), glm::vec3(0, 0, -4)), glm::vec3(10));
							snapshot.Environment.FallbackColor = glm::vec3(0);
							LightData light;
							light.Type = spot ? RenderLightType::Spot : RenderLightType::Directional;
							light.Entity = UUID(30);
							light.Position = glm::vec3(-3, 4, 0);
							light.Direction = glm::normalize(glm::vec3(0, 0, -4) - light.Position);
							light.Intensity = spot ? 12.0f : 0.25f;
							light.Range = 20;
							light.InnerConeAngle = 45;
							light.OuterConeAngle = 60;
							light.CastShadows = true;
							light.CascadeCount = 4;
							light.ShadowDistance = 20;
							light.LightAngle = 2;
							light.SourceRadius = 0.12f;
							light.DepthBias = 0.5f;
							light.NormalBias = 0.5f;
							snapshot.Lights = { light };
							const auto shadowed = fixture.Render(snapshot);
							auto control = snapshot;
							control.Lights[0].CastShadows = false;
							const auto unshadowed = fixture.Render(control);
							uint32_t selfShadowed = 0;
							int largest = 0;
							for (uint32_t y = 4; y < 124; ++y)
								for (uint32_t x = 4; x < 124; ++x)
								{
									const int difference = Test::SceneRendererIntegrationFixture::Channel(unshadowed, x, y)
										- Test::SceneRendererIntegrationFixture::Channel(shadowed, x, y);
									largest = std::max(largest, difference);
									selfShadowed += difference > 1 ? 1U : 0U;
								}
							CAPTURE(selfShadowed);
							CAPTURE(largest);
							CHECK(selfShadowed == 0);
							CHECK(largest <= 1);

							// A separate plane 0.2 m in front of the receiver must still cast a shadow; correcting receiver slope
							// must not erase contact shadows by applying the whole search radius as a depth bias.
							auto blocker = snapshot.Meshes[0];
							blocker.Entity = UUID(31);
							blocker.PickId = 0;
							blocker.World = glm::scale(glm::translate(glm::mat4(1), glm::vec3(0, 0, -3.8f)), glm::vec3(0.45f));
							snapshot.Meshes.push_back(blocker);
							const auto occluded = fixture.Render(snapshot);
							control = snapshot;
							control.Meshes[0].CastShadows = false;
							const auto blockerOnly = fixture.Render(control);
							// Keeping the plane in the caster list must not invent blockers or darken the PCF footprint,
							// including pixels whose broad blocker search sees the nearby real occluder.
							int receiverDifference = 0;
							for (uint32_t y = 4; y < 124; ++y)
								for (uint32_t x = 4; x < 124; ++x)
									receiverDifference = std::max(receiverDifference, std::abs(Test::SceneRendererIntegrationFixture::Channel(blockerOnly, x, y) - Test::SceneRendererIntegrationFixture::Channel(occluded, x, y)));
							CAPTURE(receiverDifference);
							CHECK(receiverDifference <= 1);
							control = snapshot;
							control.Lights[0].CastShadows = false;
							const auto clear = fixture.Render(control);
							control = snapshot;
							for (auto& item : control.Meshes)
								item.CastShadows = false;
							CHECK(fixture.Render(control).Pixels == clear.Pixels);
							uint32_t genuinelyShadowed = 0;
							for (uint32_t y = 4; y < 124; ++y)
								for (uint32_t x = 4; x < 124; ++x)
									genuinelyShadowed += Test::SceneRendererIntegrationFixture::Channel(clear, x, y)
												- Test::SceneRendererIntegrationFixture::Channel(occluded, x, y)
											> 12
										? 1U
										: 0U;
							CAPTURE(genuinelyShadowed);
							CHECK(genuinelyShadowed > 20);
						}
		}
	}

}
