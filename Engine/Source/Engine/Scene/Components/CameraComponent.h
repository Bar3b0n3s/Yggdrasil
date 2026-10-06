#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// Registry enum "ProjectionType".
	enum class ProjectionType : uint8_t
	{
		Perspective,
		Orthographic
	};

	// Registry enum "ClearMode".
	enum class ClearMode : uint8_t
	{
		Skybox,
		Color
	};

	// Registry name "Camera" (Architecture §5.3): looks down local -Z. VerticalFov in degrees (perspective);
	// OrthographicSize is the half height in metres (orthographic); NearClip/FarClip bound the depth range and culling.
	// Exactly one active camera should be Primary (validated by the project validator, M4).
	struct CameraComponent
	{
		ProjectionType Projection = ProjectionType::Perspective;
		float VerticalFov = 60.0f;
		float OrthographicSize = 10.0f;
		float NearClip = 0.1f;
		float FarClip = 1000.0f;
		bool Primary = false;
		ClearMode Clear = ClearMode::Skybox;
		glm::vec3 ClearColor = glm::vec3(0.05f, 0.05f, 0.06f); // linear
	};

}
