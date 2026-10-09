#include "EditorPCH.h"
#include "EditorCore/Viewport/GizmoController.h"

#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace Engine {

	namespace Utils {

		static Scene* GizmoLiveScene(EditorContext& context)
		{
			if (PlaySession* session = context.GetPlay().GetSession())
				return &session->GetScene();
			return context.HasScene() ? &context.GetScene() : nullptr;
		}

		static bool GizmoFiniteMatrix(const glm::mat4& matrix)
		{
			for (int column = 0; column < 4; ++column)
			{
				for (int row = 0; row < 4; ++row)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		static Result<glm::dmat4> GizmoInverse(const glm::mat4& matrix)
		{
			if (!GizmoFiniteMatrix(matrix) || matrix[0][3] != 0.0f || matrix[1][3] != 0.0f || matrix[2][3] != 0.0f || matrix[3][3] != 1.0f)
				return MakeError(ErrorCode::Validation, "Gizmo transforms must be finite affine matrices");
			glm::dmat3 normalized(matrix);
			for (int column = 0; column < 3; ++column)
			{
				const double length = glm::length(normalized[column]);
				if (length == 0.0)
					return MakeError(ErrorCode::Validation, "A gizmo transform has a noninvertible axis");
				normalized[column] /= length;
			}
			if (std::abs(glm::determinant(normalized)) < 1.0e-6)
				return MakeError(ErrorCode::Validation, "A gizmo transform is noninvertible");
			return glm::inverse(glm::dmat4(matrix));
		}

		static Result<TransformComponent> GizmoDecompose(const glm::mat4& matrix)
		{
			ENGINE_TRY(GizmoInverse(matrix));
			for (int column = 0; column < 3; ++column)
			{
				for (int other = column + 1; other < 3; ++other)
				{
					const glm::dvec3 a = glm::normalize(glm::dvec3(matrix[column]));
					const glm::dvec3 b = glm::normalize(glm::dvec3(matrix[other]));
					if (std::abs(glm::dot(a, b)) > 1.0e-5)
						return MakeError(ErrorCode::Validation, "Gizmo transform would introduce shear");
				}
			}
			Result<TransformDecomposition> result = TransformSystem::DecomposeMatrix(matrix);
			if (!result)
				return MakeError(ErrorCode::Validation, "Gizmo transform has invalid scale or rotation: {}", result.error().ToString());
			return TransformComponent{ result->Translation, result->Rotation, result->Scale };
		}

		static bool GizmoSamePose(const TransformComponent& a, const TransformComponent& b)
		{
			return a.Translation == b.Translation && a.Rotation == b.Rotation && a.Scale == b.Scale;
		}

		static Status GizmoValidateSettings(const GizmoSettings& settings)
		{
			if ((settings.Operation != GizmoOperation::Translate && settings.Operation != GizmoOperation::Rotate && settings.Operation != GizmoOperation::Scale)
				|| (settings.Space != GizmoSpace::Local && settings.Space != GizmoSpace::World)
				|| !std::isfinite(settings.TranslationSnap) || settings.TranslationSnap <= 0.0f
				|| !std::isfinite(settings.RotationSnap) || settings.RotationSnap <= 0.0f
				|| !std::isfinite(settings.ScaleSnap) || settings.ScaleSnap <= 0.0f)
				return MakeError(ErrorCode::InvalidArgument, "Gizmo operation, space and positive snapping increments must be valid");
			return {};
		}

		static Result<glm::mat4> GizmoSnapPivot(const glm::mat4& base, const glm::mat4& candidate, const GizmoSettings& settings)
		{
			ENGINE_TRY_ASSIGN(const TransformComponent original, GizmoDecompose(base));
			ENGINE_TRY_ASSIGN(TransformComponent pose, GizmoDecompose(candidate));
			switch (settings.Operation)
			{
				case GizmoOperation::Translate:
				{
					const glm::mat3 basis = settings.Space == GizmoSpace::Local ? glm::mat3_cast(original.Rotation) : glm::mat3(1.0f);
					glm::dvec3 delta = glm::transpose(glm::dmat3(basis)) * (glm::dvec3(pose.Translation) - glm::dvec3(original.Translation));
					for (int axis = 0; axis < 3; ++axis)
						delta[axis] = std::round(delta[axis] / settings.TranslationSnap) * settings.TranslationSnap;
					pose.Translation = glm::vec3(glm::dvec3(original.Translation) + glm::dmat3(basis) * delta);
					break;
				}
				case GizmoOperation::Rotate:
				{
					glm::dquat delta = glm::normalize(glm::dquat(pose.Rotation) * glm::conjugate(glm::dquat(original.Rotation)));
					if (delta.w < 0.0)
						delta = -delta;
					const glm::dvec3 vector(delta.x, delta.y, delta.z);
					const double length = glm::length(vector);
					if (length > 1.0e-12)
					{
						const double degrees = 2.0 * std::atan2(length, delta.w) * 180.0 / std::numbers::pi;
						const double snapped = std::round(degrees / settings.RotationSnap) * settings.RotationSnap * std::numbers::pi / 180.0;
						pose.Rotation = glm::quat(glm::angleAxis(snapped, vector / length) * glm::dquat(original.Rotation));
					}
					break;
				}
				case GizmoOperation::Scale:
				{
					for (int axis = 0; axis < 3; ++axis)
					{
						const double factor = static_cast<double>(pose.Scale[axis]) / original.Scale[axis];
						pose.Scale[axis] = static_cast<float>((1.0 + std::round((factor - 1.0) / settings.ScaleSnap) * settings.ScaleSnap) * original.Scale[axis]);
					}
					break;
				}
			}
			return TransformSystem::ComputeLocalMatrix(pose);
		}

	}

	struct GizmoController::DragState
	{
		struct RootPose
		{
			UUID Id{};
			glm::mat4 World = glm::mat4(1.0f);
			glm::dmat4 ParentInverse = glm::dmat4(1.0);
			TransformComponent Before{};
			TransformComponent After{};
		};
		// The preview has its own generator so serialization/preview edits never consume live random state.
		explicit DragState(const UUIDGenerator& ids)
			: Ids(ids)
		{
		}
		UUIDGenerator Ids;
		Scope<Scene> Preview{};
		std::vector<RootPose> Roots{};
		GizmoSettings Settings{};
		glm::mat4 Pivot = glm::mat4(1.0f);
		glm::dmat4 PivotInverse = glm::dmat4(1.0);
		uint64_t EditorRevision = 0;
		uint64_t SceneRevision = 0;
		uint64_t PlaySerial = 0;
		bool Playing = false;
	};

	GizmoController::GizmoController(EditorContext& context)
		: m_Context(&context)
	{
	}

	GizmoController::~GizmoController() = default;

	bool GizmoController::HasCurrentSource() const
	{
		if (!m_Drag || m_Context->GetRevision() != m_Drag->EditorRevision || m_Context->GetPlay().IsPlaying() != m_Drag->Playing)
			return false;
		const PlaySession* session = m_Context->GetPlay().GetSession();
		if (session && session->GetSerial() != m_Drag->PlaySerial)
			return false;
		const Scene* scene = Utils::GizmoLiveScene(*m_Context);
		if (!scene || scene->GetRevision() != m_Drag->SceneRevision)
			return false;
		// Runtime systems may write transforms directly without advancing the authored revision.
		for (const DragState::RootPose& root : m_Drag->Roots)
		{
			const ConstEntity entity = scene->FindEntityByID(root.Id);
			if (!entity || !Utils::GizmoSamePose(entity.GetComponent<TransformComponent>(), root.Before)
				|| TransformSystem::ComputeWorldMatrix(entity) != root.World)
				return false;
		}
		return true;
	}

	Status GizmoController::Begin(std::span<const UUID> entities, const glm::mat4& worldPivot, const GizmoSettings& settings)
	{
		if (m_Drag)
			return MakeError(ErrorCode::InvalidState, "A gizmo drag is already active");
		Scene* scene = Utils::GizmoLiveScene(*m_Context);
		if (!scene)
			return MakeError(ErrorCode::InvalidState, "A gizmo drag needs an open scene");
		if (m_Context->IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "A read-only scene cannot be manipulated");
		if (entities.empty())
			return MakeError(ErrorCode::InvalidArgument, "A gizmo drag needs a nonempty selection");
		ENGINE_TRY(Utils::GizmoValidateSettings(settings));
		Result<glm::dmat4> inverse = Utils::GizmoInverse(worldPivot);
		if (!inverse)
			return MakeError(ErrorCode::InvalidArgument, "The gizmo pivot must be finite, affine and invertible");
		auto drag = CreateScope<DragState>(m_Context->GetIdGenerator());
		drag->Settings = settings;
		drag->Pivot = worldPivot;
		drag->PivotInverse = *inverse;
		drag->EditorRevision = m_Context->GetRevision();
		drag->SceneRevision = scene->GetRevision();
		drag->Playing = m_Context->GetPlay().IsPlaying();
		if (const PlaySession* session = m_Context->GetPlay().GetSession())
		{
			drag->PlaySerial = session->GetSerial();
		}
		std::vector<UUID> selected(entities.begin(), entities.end());
		std::ranges::sort(selected);
		selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
		for (const UUID id : selected)
		{
			if (!scene->FindEntityByID(id).IsValid())
				return MakeError(ErrorCode::NotFound, "The gizmo selection contains an unknown entity {}", id);
		}
		for (const UUID id : selected)
		{
			const ConstEntity entity = scene->FindEntityByID(id);
			bool hasSelectedAncestor = false;
			for (ConstEntity ancestor = entity.GetParent(); ancestor.IsValid(); ancestor = ancestor.GetParent())
				hasSelectedAncestor |= std::binary_search(selected.begin(), selected.end(), ancestor.GetUUID());
			if (hasSelectedAncestor)
				continue;
			const ConstEntity parent = entity.GetParent();
			const glm::mat4 parentWorld = parent ? TransformSystem::ComputeWorldMatrix(parent) : glm::mat4(1.0f);
			ENGINE_TRY_ASSIGN(const glm::dmat4 parentInverse, Utils::GizmoInverse(parentWorld));
			const TransformComponent before = entity.GetComponent<TransformComponent>();
			drag->Roots.push_back({ id, TransformSystem::ComputeWorldMatrix(entity), parentInverse, before, before });
		}
		ENGINE_TRY_ASSIGN(const Json document, SceneSerializer::ToJson(*scene));
		drag->Preview = Scene::Create({ .Registry = &m_Context->GetTypeRegistry(), .IdGenerator = &drag->Ids });
		LoadReport report;
		ENGINE_TRY(SceneSerializer::FromJson(*drag->Preview, document, {}, report));
		TransformSystem::Update(*drag->Preview);
		m_Drag = std::move(drag);
		return {};
	}

	Status GizmoController::Update(const glm::mat4& worldPivot)
	{
		if (!m_Drag)
			return MakeError(ErrorCode::InvalidState, "There is no active gizmo drag");
		if (!HasCurrentSource())
		{
			Cancel();
			return MakeError(ErrorCode::Conflict, "The scene or play session changed during the gizmo drag");
		}
		ENGINE_TRY(Utils::GizmoInverse(worldPivot));
		glm::mat4 pivot = worldPivot;
		if (m_Drag->Settings.Snap)
		{
			ENGINE_TRY_ASSIGN(pivot, Utils::GizmoSnapPivot(m_Drag->Pivot, worldPivot, m_Drag->Settings));
		}
		const glm::dmat4 delta = glm::dmat4(pivot) * m_Drag->PivotInverse;
		std::vector<TransformComponent> poses;
		poses.reserve(m_Drag->Roots.size());
		for (const DragState::RootPose& root : m_Drag->Roots)
		{
			if (pivot == m_Drag->Pivot)
			{
				poses.push_back(root.Before);
				continue;
			}
			ENGINE_TRY_ASSIGN(const TransformComponent pose, Utils::GizmoDecompose(glm::mat4(root.ParentInverse * delta * glm::dmat4(root.World))));
			poses.push_back(pose);
		}
		for (size_t index = 0; index < poses.size(); ++index)
		{
			DragState::RootPose& root = m_Drag->Roots[index];
			root.After = poses[index];
			m_Drag->Preview->FindEntityByID(root.Id).Patch<TransformComponent>([&root](TransformComponent& transform)
			{
				transform = root.After;
			});
		}
		TransformSystem::Update(*m_Drag->Preview);
		return {};
	}

	Result<uint64_t> GizmoController::End()
	{
		if (!m_Drag)
			return MakeError(ErrorCode::InvalidState, "There is no active gizmo drag");
		if (!HasCurrentSource())
		{
			Cancel();
			return MakeError(ErrorCode::Conflict, "The scene or play session changed during the gizmo drag");
		}
		if (m_Context->IsReadOnly())
		{
			Cancel();
			return MakeError(ErrorCode::PermissionDenied, "A read-only scene cannot be manipulated");
		}
		Scope<DragState> drag = std::move(m_Drag);
		const bool changed = std::ranges::any_of(drag->Roots, [](const DragState::RootPose& root)
		{
			return !Utils::GizmoSamePose(root.Before, root.After);
		});
		if (!changed)
			return uint64_t{ 0 };
		Scene& scene = *Utils::GizmoLiveScene(*m_Context);
		SceneEdit edit(*m_Context, "Transform selection");
		for (const DragState::RootPose& root : drag->Roots)
		{
			if (Utils::GizmoSamePose(root.Before, root.After))
				continue;
			const Entity entity = scene.FindEntityByID(root.Id);
			entity.Patch<TransformComponent>([&root](TransformComponent& transform)
			{
				transform = root.After;
			});
			if (PlaySession* session = m_Context->GetPlay().GetSession())
				session->MarkTeleported(entity);
		}
		return edit.Commit();
	}

	void GizmoController::Cancel()
	{
		m_Drag.reset();
	}

	bool GizmoController::IsDragging() const
	{
		return m_Drag != nullptr;
	}

	const Scene* GizmoController::GetPreviewScene() const
	{
		return HasCurrentSource() ? m_Drag->Preview.get() : nullptr;
	}

}
