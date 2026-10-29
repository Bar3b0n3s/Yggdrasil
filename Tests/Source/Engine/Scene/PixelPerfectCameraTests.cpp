#include "TestsPCH.h"

#include "Engine/Scene/PixelPerfectCamera.h"

#include <cmath>

// The pixel-perfect camera's view (Docs/Decisions/0032-m21-contract.md decision 6, stream C): skipped until stream C
// implements it.

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("PixelPerfectCamera: the view renders the reference resolution in pixel-art mode at an orthographic size of its height" * doctest::skip(true))
		{
			const PixelPerfectCameraComponent camera{ .ReferenceWidth = 256, .ReferenceHeight = 144, .PixelsPerUnit = 8.0f, .LetterboxColor = glm::vec3(0.1f, 0.2f, 0.3f) };
			const PixelPerfectView view = ComputePixelPerfectView(camera, glm::vec3(0.0f, 0.0f, 5.0f));
			CHECK(view.Output.SceneWidth == 256);
			CHECK(view.Output.SceneHeight == 144);
			CHECK(view.Output.PixelArt);
			CHECK(view.Output.LetterboxColor == glm::vec3(0.1f, 0.2f, 0.3f));
			CHECK(view.OrthographicSize == 9.0f);
			CHECK(view.Position == glm::vec3(0.0f, 0.0f, 5.0f));
		}

		TEST_CASE("PixelPerfectCamera: the camera position snaps the view's left and bottom edges to the pixel grid" * doctest::skip(true))
		{
			// An odd reference width puts the centre on a half pixel: the left edge, x - 21 / 32 (10.5 pixels of 1/16 unit), is
			// what snaps.
			const PixelPerfectCameraComponent camera{ .ReferenceWidth = 21, .ReferenceHeight = 20, .PixelsPerUnit = 16.0f };
			const PixelPerfectView view = ComputePixelPerfectView(camera, glm::vec3(1.01f, -2.02f, 3.0f));
			const float left = (view.Position.x - 21.0f / 32.0f) * 16.0f;
			const float bottom = (view.Position.y - 20.0f / 32.0f) * 16.0f;
			CHECK(left == std::round(left));
			CHECK(bottom == std::round(bottom));
			CHECK(view.Position.x == doctest::Approx(1.0f + 1.0f / 32.0f));
			CHECK(view.Position.z == 3.0f);
		}
	}

}
