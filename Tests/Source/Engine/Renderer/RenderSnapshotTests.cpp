#include "TestsPCH.h"

#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>

// The render snapshot's reverse-Z projection (Architecture §8.3; Docs/Decisions/0012-m7-decisions.md decision 6).

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderSnapshot: the reverse-Z projection maps the near plane to depth 1 for both projection kinds")
		{
			// §8.3: perspective m[0][0] = f / aspect, m[1][1] = f, m[2][3] = -1, m[3][2] = near, infinite far plane.
			const glm::mat4 perspective = ComputeReverseZProjection(RenderProjection::Perspective, 90.0f, 10.0f, 0.1f, 1000.0f, 200, 100);
			CHECK(perspective[0][0] == doctest::Approx(0.5f));
			CHECK(perspective[1][1] == doctest::Approx(1.0f));
			CHECK(perspective[2][3] == doctest::Approx(-1.0f));
			CHECK(perspective[3][2] == doctest::Approx(0.1f));
			CHECK(perspective[2][2] == 0.0f);
			// The near plane maps to depth 1.
			const glm::vec4 nearPoint = perspective * glm::vec4(0.0f, 0.0f, -0.1f, 1.0f);
			CHECK(nearPoint.z / nearPoint.w == doctest::Approx(1.0f));

			// Orthographic: d = (far + zView) / (far - near), 1 at the near plane, 0 at the far plane.
			const glm::mat4 orthographic = ComputeReverseZProjection(RenderProjection::Orthographic, 60.0f, 5.0f, 1.0f, 11.0f, 100, 100);
			CHECK((orthographic * glm::vec4(0.0f, 0.0f, -1.0f, 1.0f)).z == doctest::Approx(1.0f));
			CHECK((orthographic * glm::vec4(0.0f, 0.0f, -11.0f, 1.0f)).z == doctest::Approx(0.0f));
			CHECK((orthographic * glm::vec4(0.0f, 5.0f, -2.0f, 1.0f)).y == doctest::Approx(1.0f));
		}

		TEST_CASE("RenderSnapshot: perspective depth falls towards 0 with distance and the aspect ratio scales x only")
		{
			// An infinite far plane: depth is near / distance, positive for every point in front of the camera.
			const glm::mat4 perspective = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.5f, 100.0f, 320, 180);
			const glm::vec4 far = perspective * glm::vec4(0.0f, 0.0f, -1.0e6f, 1.0f);
			CHECK(far.z / far.w == doctest::Approx(0.5e-6f));
			CHECK(far.z / far.w > 0.0f);
			const glm::vec4 middle = perspective * glm::vec4(0.0f, 0.0f, -2.0f, 1.0f);
			CHECK(middle.z / middle.w == doctest::Approx(0.25f));
			// tan(30 degrees) = 1 / sqrt(3): f = sqrt(3), divided by the aspect ratio 16:9 on x.
			CHECK(perspective[1][1] == doctest::Approx(1.7320508f));
			CHECK(perspective[0][0] == doctest::Approx(1.7320508f * 9.0f / 16.0f));
			// Clip-space +Y is up: a point above the axis has a positive clip y (NVRHI's viewport flips it, §8.3).
			CHECK((perspective * glm::vec4(0.0f, 1.0f, -2.0f, 1.0f)).y > 0.0f);

			// Orthographic: x is scaled by the half width OrthographicSize * aspect.
			const glm::mat4 orthographic = ComputeReverseZProjection(RenderProjection::Orthographic, 60.0f, 4.0f, 0.1f, 50.0f, 200, 100);
			CHECK((orthographic * glm::vec4(8.0f, 0.0f, -1.0f, 1.0f)).x == doctest::Approx(1.0f));
			CHECK((orthographic * glm::vec4(0.0f, 4.0f, -1.0f, 1.0f)).y == doctest::Approx(1.0f));
			CHECK(orthographic[3][3] == 1.0f);
		}

		TEST_CASE("RenderSnapshot: debug views have their viewport.screenshot names, parsed ignoring case")
		{
			for (const RenderDebugView view : { RenderDebugView::Lit, RenderDebugView::Albedo, RenderDebugView::Normals, RenderDebugView::Roughness,
					 RenderDebugView::Metallic, RenderDebugView::Emissive })
			{
				const std::string_view name = RenderDebugViewToString(view);
				CAPTURE(std::string(name));
				CHECK(ParseRenderDebugView(name) == view);
			}
			CHECK(RenderDebugViewToString(RenderDebugView::Albedo) == "Albedo");
			CHECK(ParseRenderDebugView("NORMALS") == RenderDebugView::Normals);
			CHECK(ParseRenderDebugView("emissive") == RenderDebugView::Emissive);
			// Names are available to contract consumers before their rendering passes are implemented.
			CHECK(ParseRenderDebugView("AO") == RenderDebugView::AO);
			CHECK(ParseRenderDebugView("ShadowCascades") == RenderDebugView::ShadowCascades);
			CHECK(ParseRenderDebugView("overdraw") == RenderDebugView::Overdraw);
			CHECK_FALSE(ParseRenderDebugView("").has_value());
			CHECK_FALSE(ParseRenderDebugView("Albedo ").has_value());
		}

		TEST_CASE("RenderSnapshot: a default snapshot has no texts, no debug primitives and the Lit view")
		{
			const RenderSnapshot snapshot;
			CHECK(snapshot.Texts.empty());
			CHECK(snapshot.DebugDraw.IsEmpty());
			CHECK(snapshot.DebugView == RenderDebugView::Lit);
			const TextItem text;
			CHECK(text.Size == 32.0f);
			CHECK(text.Space == RenderTextSpace::Screen);
			CHECK(text.Alignment == RenderTextAlignment::Center);
			CHECK(text.Anchor == glm::vec2(0.5f));
			CHECK_FALSE(text.Font.IsValid());
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("DebugViews: debug palettes have exact display-encoded colors")
		{
			CHECK(GetOverdrawDebugColor(0) == glm::vec3(0));
			CHECK(GetOverdrawDebugColor(1) == glm::vec3(0, 0, 1));
			CHECK(GetOverdrawDebugColor(2) == glm::vec3(0, 1, 1));
			CHECK(GetOverdrawDebugColor(3) == glm::vec3(0, 1, 0));
			CHECK(GetOverdrawDebugColor(4) == glm::vec3(1, 1, 0));
			CHECK(GetOverdrawDebugColor(5) == glm::vec3(1, 0, 0));
			CHECK(GetOverdrawDebugColor(6) == glm::vec3(1, 0, 0));
			CHECK(GetShadowCascadeDebugColor(0) == glm::vec3(1, 0, 0));
			CHECK(GetShadowCascadeDebugColor(1) == glm::vec3(0, 1, 0));
			CHECK(GetShadowCascadeDebugColor(2) == glm::vec3(0, 0, 1));
			CHECK(GetShadowCascadeDebugColor(3) == glm::vec3(1, 1, 0));
		}
	}

}
