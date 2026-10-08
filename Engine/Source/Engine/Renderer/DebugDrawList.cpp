#include "EnginePCH.h"
#include "Engine/Renderer/DebugDrawList.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace Engine {

	namespace Utils {

		// A duration as the list stores it: negative and non-finite values become 0.
		[[nodiscard]] static float SanitizeDuration(float duration)
		{
			return std::isfinite(duration) && duration > 0.0f ? duration : 0.0f;
		}

	}

	void DebugDrawList::AddLine(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, float duration, DebugDepthMode depth)
	{
		Add({ .Shape = DebugLine{ .From = from, .To = to }, .Color = color, .Duration = duration, .Depth = depth, .Remaining = 0.0f });
	}

	void DebugDrawList::AddRay(const glm::vec3& origin, const glm::vec3& direction, float length, const glm::vec4& color, float duration,
		DebugDepthMode depth)
	{
		Add({ .Shape = DebugRay{ .Origin = origin, .Direction = direction, .Length = length },
			.Color = color,
			.Duration = duration,
			.Depth = depth,
			.Remaining = 0.0f });
	}

	void DebugDrawList::AddBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec4& color,
		float duration, DebugDepthMode depth)
	{
		Add({ .Shape = DebugBox{ .Center = center, .HalfExtents = halfExtents, .Rotation = rotation },
			.Color = color,
			.Duration = duration,
			.Depth = depth,
			.Remaining = 0.0f });
	}

	void DebugDrawList::AddSphere(const glm::vec3& center, float radius, const glm::vec4& color, float duration, DebugDepthMode depth)
	{
		Add({ .Shape = DebugSphere{ .Center = center, .Radius = radius }, .Color = color, .Duration = duration, .Depth = depth, .Remaining = 0.0f });
	}

	void DebugDrawList::AddCapsule(const glm::vec3& start, const glm::vec3& end, float radius, const glm::vec4& color, float duration,
		DebugDepthMode depth)
	{
		Add({ .Shape = DebugCapsule{ .Start = start, .End = end, .Radius = radius },
			.Color = color,
			.Duration = duration,
			.Depth = depth,
			.Remaining = 0.0f });
	}

	void DebugDrawList::AddArrow(const glm::vec3& from, const glm::vec3& to, float headSize, const glm::vec4& color, float duration,
		DebugDepthMode depth)
	{
		Add({ .Shape = DebugArrow{ .From = from, .To = to, .HeadSize = headSize },
			.Color = color,
			.Duration = duration,
			.Depth = depth,
			.Remaining = 0.0f });
	}

	void DebugDrawList::AddFrustum(const std::array<glm::vec3, 8>& corners, const glm::vec4& color, float duration, DebugDepthMode depth)
	{
		Add({ .Shape = DebugFrustum{ .Corners = corners }, .Color = color, .Duration = duration, .Depth = depth, .Remaining = 0.0f });
	}

	void DebugDrawList::AddText(const glm::vec3& position, std::string text, float size, const glm::vec4& color, float duration,
		DebugDepthMode depth)
	{
		Add({ .Shape = DebugText{ .Position = position, .Text = std::move(text), .Size = size },
			.Color = color,
			.Duration = duration,
			.Depth = depth,
			.Remaining = 0.0f });
	}

	void DebugDrawList::Add(DebugDrawCommand command)
	{
		if (m_Commands.size() >= MaxCommands)
		{
			++m_Dropped;
			return;
		}
		command.Duration = Utils::SanitizeDuration(command.Duration);
		command.Remaining = command.Duration;
		m_Commands.push_back(std::move(command));
	}

	void DebugDrawList::Append(const DebugDrawList& other)
	{
		if (&other == this)
		{
			// Inserting a vector's own range into it would read from invalidated storage.
			const DebugDrawList copy = other;
			Append(copy);
			return;
		}
		const size_t room = MaxCommands - std::min(m_Commands.size(), MaxCommands);
		const size_t taken = std::min(room, other.m_Commands.size());
		m_Commands.insert(m_Commands.end(), other.m_Commands.begin(), other.m_Commands.begin() + static_cast<std::ptrdiff_t>(taken));
		m_Dropped += other.m_Commands.size() - taken;
	}

	void DebugDrawList::Advance(float deltaSeconds)
	{
		ENGINE_CORE_ASSERT(std::isfinite(deltaSeconds) && deltaSeconds >= 0.0f, "DebugDrawList::Advance needs a finite, non-negative delta, got {}",
			deltaSeconds);
		std::erase_if(m_Commands, [](const DebugDrawCommand& command)
		{
			return command.Remaining <= 0.0f;
		});
		for (DebugDrawCommand& command : m_Commands)
			command.Remaining -= deltaSeconds;
	}

	void DebugDrawList::Clear()
	{
		m_Commands.clear();
		m_Dropped = 0;
	}

}
