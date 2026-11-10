#include "TestsPCH.h"
#include "EditorCore/Viewport/EditorGrid2D.h"

#include "EditorCore/Viewport/EditorCamera.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Support/SceneTestFixture.h"

#include <set>
#include <variant>

// The scene view's 2D grid (Docs/Decisions/0032-m21-contract.md decision 14, stream D).

namespace Engine {

	namespace {

		CameraData MakeGridCamera(const ExplicitRenderCamera& camera, uint32_t width, uint32_t height)
		{
			Test::SceneTestFixture fixture;
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(),
				{ .Camera = RenderCameraSource::Explicit, .ExplicitCamera = camera, .Width = width, .Height = height });
			REQUIRE_MESSAGE(snapshot.has_value(), (snapshot ? std::string() : snapshot.error().ToString()));
			return snapshot->Camera;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorGrid2D: the 2D camera's grid lies in the plane z = 0 with the X axis red and the Y axis green")
		{
			// Make2D's camera over (2.25, 1.25) with a half height of 5 at 16:9: one-metre cells, the view spanning y in
			// [-3.75, 6.25] and x in about [-6.6, 11.1].
			const Result<ExplicitRenderCamera> flat = EditorCamera::Make2D({ .Position = glm::vec3(2.25f, 1.25f, 9.0f), .Target = glm::vec3(2.25f, 1.25f, 0.0f), .Projection = RenderProjection::Orthographic, .OrthographicSize = 5.0f });
			REQUIRE(flat.has_value());
			DebugDrawList lines;
			REQUIRE(AppendEditorGrid2D(MakeGridCamera(*flat, 640, 360), lines).has_value());
			REQUIRE_FALSE(lines.GetCommands().empty());
			std::set<float> verticals;
			std::set<float> horizontals;
			bool xAxis = false;
			bool yAxis = false;
			for (const DebugDrawCommand& command : lines.GetCommands())
			{
				const DebugLine* line = std::get_if<DebugLine>(&command.Shape);
				REQUIRE(line != nullptr);
				CHECK(line->From.z == 0.0f);
				CHECK(line->To.z == 0.0f);
				CHECK(command.Color.a >= 0.0f);
				CHECK(command.Color.a <= 0.8f);
				if (line->From.x == line->To.x)
				{
					verticals.insert(line->From.x);
					yAxis |= line->From.x == 0.0f && command.Color.g > command.Color.r;
				}
				else
				{
					REQUIRE(line->From.y == line->To.y);
					horizontals.insert(line->From.y);
					xAxis |= line->From.y == 0.0f && command.Color.r > command.Color.g;
				}
			}
			CHECK(xAxis);
			CHECK(yAxis);
			CHECK(*horizontals.begin() == -3.0f);
			CHECK(*horizontals.rbegin() == 6.0f);
			CHECK(horizontals.size() == 10);
			CHECK(*verticals.begin() == -6.0f);
			CHECK(*verticals.rbegin() == 11.0f);
			CHECK(verticals.size() == 18);

			// Zooming out ten times makes the cells ten times larger.
			ExplicitRenderCamera wide = *flat;
			wide.OrthographicSize = 50.0f;
			DebugDrawList coarse;
			REQUIRE(AppendEditorGrid2D(MakeGridCamera(wide, 640, 360), coarse).has_value());
			std::set<float> rows;
			for (const DebugDrawCommand& command : coarse.GetCommands())
			{
				const DebugLine& line = std::get<DebugLine>(command.Shape);
				if (line.From.y == line.To.y)
					rows.insert(line.From.y);
			}
			CHECK(rows == std::set<float>{ -40.0f, -30.0f, -20.0f, -10.0f, 0.0f, 10.0f, 20.0f, 30.0f, 40.0f, 50.0f });
		}

		TEST_CASE("EditorGrid2D: a camera that sees no part of the plane z = 0 adds no line")
		{
			// A perspective camera above the plane looking away from it, up the Z axis.
			DebugDrawList lines;
			REQUIRE(AppendEditorGrid2D(MakeGridCamera({ .Position = glm::vec3(0.0f, 0.0f, 10.0f), .Target = glm::vec3(0.0f, 0.5f, 20.0f) }, 640, 360), lines).has_value());
			CHECK(lines.GetCommands().empty());
		}
	}

}
