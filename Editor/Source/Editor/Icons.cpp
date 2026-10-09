#include "EditorPCH.h"
#include "Editor/Icons.h"

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace Engine {

	namespace {

		struct EditorIconSegment
		{
			glm::vec2 From = glm::vec2(0.0f);
			glm::vec2 To = glm::vec2(0.0f);
		};

	}

	namespace Utils {

		static std::vector<EditorIconSegment> EditorIconSegments(EditorIcon icon)
		{
			std::vector<EditorIconSegment> segments;
			const auto path = [&segments](std::initializer_list<glm::vec2> points)
			{
				if (points.size() < 2)
					return;
				for (auto current = points.begin() + 1; current != points.end(); ++current)
					segments.push_back({ *(current - 1), *current });
			};
			const auto circle = [&segments](const glm::vec2& center, float radius)
			{
				constexpr int Steps = 24;
				for (int step = 0; step < Steps; ++step)
				{
					const float a = static_cast<float>(step) * 2.0f * std::numbers::pi_v<float> / Steps;
					const float b = static_cast<float>(step + 1) * 2.0f * std::numbers::pi_v<float> / Steps;
					segments.push_back({ center + radius * glm::vec2(std::cos(a), std::sin(a)), center + radius * glm::vec2(std::cos(b), std::sin(b)) });
				}
			};
			switch (icon)
			{
				case EditorIcon::Folder:
					path({ { 0.1f, 0.8f }, { 0.1f, 0.2f }, { 0.4f, 0.2f }, { 0.5f, 0.32f }, { 0.9f, 0.32f }, { 0.9f, 0.8f }, { 0.1f, 0.8f }, { 0.2f, 0.45f }, { 0.9f, 0.45f } });
					break;
				case EditorIcon::Scene:
					path({ { 0.1f, 0.8f }, { 0.1f, 0.2f }, { 0.9f, 0.2f }, { 0.9f, 0.8f }, { 0.1f, 0.8f }, { 0.35f, 0.45f }, { 0.55f, 0.68f }, { 0.72f, 0.48f }, { 0.9f, 0.8f } });
					circle({ 0.7f, 0.35f }, 0.06f);
					break;
				case EditorIcon::Prefab:
				case EditorIcon::Mesh:
					path({ { 0.5f, 0.1f }, { 0.85f, 0.3f }, { 0.85f, 0.7f }, { 0.5f, 0.9f }, { 0.15f, 0.7f }, { 0.15f, 0.3f }, { 0.5f, 0.1f } });
					path({ { 0.15f, 0.3f }, { 0.5f, 0.5f }, { 0.85f, 0.3f } });
					path({ { 0.5f, 0.5f }, { 0.5f, 0.9f } });
					if (icon == EditorIcon::Prefab)
						circle({ 0.5f, 0.3f }, 0.08f);
					break;
				case EditorIcon::Material:
					circle({ 0.5f, 0.5f }, 0.36f);
					path({ { 0.3f, 0.72f }, { 0.7f, 0.28f } });
					path({ { 0.43f, 0.81f }, { 0.79f, 0.4f } });
					break;
				case EditorIcon::Texture:
					path({ { 0.15f, 0.15f }, { 0.85f, 0.15f }, { 0.85f, 0.85f }, { 0.15f, 0.85f }, { 0.15f, 0.15f } });
					path({ { 0.15f, 0.5f }, { 0.85f, 0.5f } });
					path({ { 0.5f, 0.15f }, { 0.5f, 0.85f } });
					path({ { 0.15f, 0.15f }, { 0.5f, 0.5f }, { 0.85f, 0.85f } });
					break;
				case EditorIcon::Environment:
					circle({ 0.5f, 0.5f }, 0.37f);
					path({ { 0.13f, 0.5f }, { 0.87f, 0.5f } });
					path({ { 0.5f, 0.13f }, { 0.32f, 0.5f }, { 0.5f, 0.87f }, { 0.68f, 0.5f }, { 0.5f, 0.13f } });
					break;
				case EditorIcon::Audio:
					path({ { 0.13f, 0.38f }, { 0.33f, 0.38f }, { 0.56f, 0.18f }, { 0.56f, 0.82f }, { 0.33f, 0.62f }, { 0.13f, 0.62f }, { 0.13f, 0.38f } });
					path({ { 0.68f, 0.32f }, { 0.77f, 0.5f }, { 0.68f, 0.68f } });
					path({ { 0.8f, 0.2f }, { 0.92f, 0.5f }, { 0.8f, 0.8f } });
					break;
				case EditorIcon::Script:
					path({ { 0.33f, 0.28f }, { 0.12f, 0.5f }, { 0.33f, 0.72f } });
					path({ { 0.67f, 0.28f }, { 0.88f, 0.5f }, { 0.67f, 0.72f } });
					path({ { 0.59f, 0.15f }, { 0.41f, 0.85f } });
					break;
				case EditorIcon::Font:
					path({ { 0.17f, 0.83f }, { 0.5f, 0.17f }, { 0.83f, 0.83f } });
					path({ { 0.3f, 0.59f }, { 0.7f, 0.59f } });
					break;
				case EditorIcon::Replay:
					circle({ 0.5f, 0.5f }, 0.35f);
					path({ { 0.41f, 0.31f }, { 0.7f, 0.5f }, { 0.41f, 0.69f }, { 0.41f, 0.31f } });
					break;
				case EditorIcon::Camera:
					path({ { 0.1f, 0.3f }, { 0.64f, 0.3f }, { 0.64f, 0.7f }, { 0.1f, 0.7f }, { 0.1f, 0.3f } });
					path({ { 0.64f, 0.43f }, { 0.9f, 0.25f }, { 0.9f, 0.75f }, { 0.64f, 0.57f } });
					break;
				case EditorIcon::DirectionalLight:
				case EditorIcon::PointLight:
					circle({ 0.5f, 0.5f }, 0.2f);
					for (int ray = 0; ray < 8; ++ray)
					{
						const float angle = static_cast<float>(ray) * std::numbers::pi_v<float> / 4.0f;
						const glm::vec2 direction(std::cos(angle), std::sin(angle));
						segments.push_back({ glm::vec2(0.5f) + direction * 0.29f, glm::vec2(0.5f) + direction * 0.43f });
					}
					if (icon == EditorIcon::DirectionalLight)
						path({ { 0.42f, 0.45f }, { 0.5f, 0.6f }, { 0.58f, 0.45f } });
					break;
				case EditorIcon::SpotLight:
					path({ { 0.42f, 0.15f }, { 0.58f, 0.15f }, { 0.6f, 0.32f }, { 0.4f, 0.32f }, { 0.42f, 0.15f } });
					path({ { 0.45f, 0.4f }, { 0.15f, 0.85f }, { 0.85f, 0.85f }, { 0.55f, 0.4f } });
					break;
				case EditorIcon::Trigger:
					path({ { 0.5f, 0.1f }, { 0.9f, 0.5f }, { 0.5f, 0.9f }, { 0.1f, 0.5f }, { 0.5f, 0.1f } });
					path({ { 0.57f, 0.29f }, { 0.4f, 0.53f }, { 0.6f, 0.53f }, { 0.43f, 0.75f } });
					break;
			}
			return segments;
		}

	}

	void DrawEditorIcon(ImDrawList& drawList, EditorIcon icon, const glm::vec2& minimum, float size, uint32_t color)
	{
		if (!std::isfinite(size) || size <= 0.0f || !std::isfinite(minimum.x) || !std::isfinite(minimum.y))
			return;
		for (const EditorIconSegment& segment : Utils::EditorIconSegments(icon))
		{
			const glm::vec2 from = minimum + segment.From * size;
			const glm::vec2 to = minimum + segment.To * size;
			drawList.AddLine(ImVec2(from.x, from.y), ImVec2(to.x, to.y), color, 1.5f);
		}
	}

	void AppendEditorIcon(DebugDrawList& drawList, EditorIcon icon, const glm::mat4& world, float size, const glm::vec4& color)
	{
		if (!std::isfinite(size) || size <= 0.0f)
			return;
		for (const EditorIconSegment& segment : Utils::EditorIconSegments(icon))
		{
			const glm::vec2 from = (segment.From - glm::vec2(0.5f)) * size;
			const glm::vec2 to = (segment.To - glm::vec2(0.5f)) * size;
			drawList.AddLine(glm::vec3(world * glm::vec4(from.x, -from.y, 0.0f, 1.0f)),
				glm::vec3(world * glm::vec4(to.x, -to.y, 0.0f, 1.0f)), color);
		}
		if (icon == EditorIcon::Camera || icon == EditorIcon::SpotLight || icon == EditorIcon::DirectionalLight)
		{
			const glm::vec3 origin(world[3]);
			const glm::vec3 end(world * glm::vec4(0.0f, 0.0f, -size, 1.0f));
			drawList.AddArrow(origin, end, size * 0.15f, color);
		}
	}

	EditorIcon AssetTypeToEditorIcon(AssetType type)
	{
		switch (type)
		{
			case AssetType::None:        return EditorIcon::Folder;
			case AssetType::Scene:       return EditorIcon::Scene;
			case AssetType::Prefab:      return EditorIcon::Prefab;
			case AssetType::Mesh:        return EditorIcon::Mesh;
			case AssetType::Material:    return EditorIcon::Material;
			case AssetType::Texture:     return EditorIcon::Texture;
			case AssetType::Environment: return EditorIcon::Environment;
			case AssetType::AudioClip:   return EditorIcon::Audio;
			case AssetType::Script:      return EditorIcon::Script;
			case AssetType::Font:        return EditorIcon::Font;
			case AssetType::Replay:      return EditorIcon::Replay;
		}
		return EditorIcon::Folder;
	}

}
