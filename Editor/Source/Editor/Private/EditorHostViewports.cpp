#include "EditorPCH.h"
#include "Editor/Private/EditorHostViewports.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Platform/Window.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderAnnotations.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"

#include <algorithm>
#include <cmath>

namespace Engine {

	EditorHostViewports::EditorHostViewports(EditorContext& editor, GizmoController& gizmos)
		: m_Editor(editor), m_Gizmos(gizmos)
	{
	}
	EditorHostViewports::~EditorHostViewports()
	{
		Retire();
	}
	size_t EditorHostViewports::Index(ViewportView view)
	{
		return view == ViewportView::Game ? 1 : 0;
	}
	Status EditorHostViewports::Initialize(GraphicsDevice& device, SceneRendererPipelines& pipelines, GpuResourceCache& cache, ImGuiRenderer& imgui)
	{
		for (View& view : m_Views)
		{
			ENGINE_TRY_ASSIGN(view.Renderer, SceneRenderer::Create(device, pipelines, cache, m_Editor.GetAssets(), { .Width = 640, .Height = 360 }));
		}
		m_ImGui = &imgui;
		return {};
	}
	void EditorHostViewports::Cancel(View& view)
	{
		if (view.Renderer)
			view.Renderer->CancelPicks();
		view.Queued.reset();
		view.Pending.clear();
	}
	void EditorHostViewports::Invalidate()
	{
		for (View& view : m_Views)
		{
			Cancel(view);
			view.Source = nullptr;
		}
		m_Gizmos.Cancel();
		SetGameInputFocused(false);
	}
	void EditorHostViewports::Retire()
	{
		for (View& view : m_Views)
		{
			Cancel(view);
			if (m_ImGui && view.Image.Texture != 0)
				m_ImGui->RemoveTexture(view.Image.Texture);
			view.Image = {};
			view.RegisteredTexture = nullptr;
			view.Renderer.reset();
		}
		m_ImGui = nullptr;
	}
	void EditorHostViewports::SetUnavailable()
	{
		Invalidate();
		for (size_t i = 0; i < m_Views.size(); ++i)
		{
			View& view = m_Views[i];
			if (m_ImGui && view.Image.Texture != 0)
				m_ImGui->RemoveTexture(view.Image.Texture);
			view.Image = {};
			view.RegisteredTexture = nullptr;
			const Status status = m_Editor.GetViewportState().SetPixelSize(i == 0 ? ViewportView::Scene : ViewportView::Game, glm::uvec2(0));
			ENGINE_VERIFY(status.has_value(), "valid viewport extent");
		}
	}
	bool EditorHostViewports::IsLayoutReady() const
	{
		if (!m_Editor.HasProject())
			return true;
		for (size_t i = 0; i < m_Views.size(); ++i)
		{
			const auto panel = i == 0 ? EditorPanel::SceneViewport : EditorPanel::GameViewport;
			const auto panels = m_Editor.GetUiState().GetOpenPanels();
			if (std::ranges::find(panels, panel) == panels.end())
				continue;
			const View& view = m_Views[i];
			if (view.RequestedSize.x != view.Image.Width || view.RequestedSize.y != view.Image.Height)
				return false;
		}
		return true;
	}
	SceneRenderer* EditorHostViewports::GetRenderer(ViewportView view) const
	{
		return m_Views[Index(view)].Renderer.get();
	}
	EditorViewportImage EditorHostViewports::GetImage(ViewportView view) const
	{
		return m_Views[Index(view)].Image;
	}
	void EditorHostViewports::SetRectangle(ViewportView view, const EditorViewportRect& rectangle)
	{
		glm::uvec2 size(0);
		const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
		if (std::isfinite(rectangle.Size.x) && std::isfinite(rectangle.Size.y) && rectangle.Size.x > 0 && rectangle.Size.y > 0
			&& std::isfinite(scale.x) && std::isfinite(scale.y) && scale.x > 0 && scale.y > 0)
		{
			size = glm::uvec2(static_cast<uint32_t>(std::clamp(std::floor(rectangle.Size.x * scale.x), 1.0f, 8192.0f)),
				static_cast<uint32_t>(std::clamp(std::floor(rectangle.Size.y * scale.y), 1.0f, 8192.0f)));
			if (view == ViewportView::Game && m_Editor.GetViewportState().GetGameResolution().x != 0)
				size = m_Editor.GetViewportState().GetGameResolution();
		}
		View& state = m_Views[Index(view)];
		if (state.RequestedSize != size)
			Cancel(state);
		state.RequestedSize = size;
	}
	Status EditorHostViewports::ConfigureSceneSnapshot(RenderSnapshot& snapshot, const EditorViewportOptions& options, std::span<const UUID> selection)
	{
		snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Picking;
		if (options.Grid)
			snapshot.Flags |= RenderViewFlags::Grid;
		if (options.Colliders)
			snapshot.Flags |= RenderViewFlags::Colliders;
		if (options.Icons)
			snapshot.Flags |= RenderViewFlags::Icons;
		if (options.Wireframe)
			snapshot.Flags |= RenderViewFlags::Wireframe;
		if (!selection.empty())
			snapshot.Flags |= RenderViewFlags::Selection;
		snapshot.SelectedEntities.assign(selection.begin(), selection.end());
		return {};
	}
	Result<RenderSnapshot> EditorHostViewports::Extract(ViewportView view, uint32_t width, uint32_t height)
	{
		PlaySession* session = m_Editor.GetPlay().GetSession();
		const Scene* preview = view == ViewportView::Scene ? m_Gizmos.GetPreviewScene() : nullptr;
		const Scene* scene = preview ? preview : session ? &session->GetScene()
			: m_Editor.HasScene()                        ? &m_Editor.GetScene()
														 : nullptr;
		if (!scene)
			return RenderSnapshot{};
		RenderExtractionRequest request{ .Camera = view == ViewportView::Scene ? RenderCameraSource::Explicit : RenderCameraSource::Primary,
			.ExplicitCamera = m_Editor.GetSceneViewCamera(),
			.Width = width,
			.Height = height };
		request.Flags = RenderViewFlags::Picking;
		RenderSnapshot options;
		if (view == ViewportView::Scene)
		{
			const SceneTarget target = session ? SceneTarget::Play : SceneTarget::Edit;
			const std::span<const UUID> selected = m_Editor.GetSelectionTarget() == target ? m_Editor.GetSelection() : std::span<const UUID>();
			ENGINE_TRY(ConfigureSceneSnapshot(options, m_Editor.GetViewportState().GetOptions(), selected));
			request.Flags = options.Flags;
			request.SelectedEntities = std::move(options.SelectedEntities);
		}
		if (m_Editor.HasProject())
		{
			const auto& rendering = m_Editor.GetProject().GetSettings().Rendering;
			request.Quality = { .ShadowMapSize = rendering.ShadowMapSize, .SsaoHalfResolution = rendering.SsaoHalfResolution };
		}
		if (!session && !preview)
			TransformSystem::Update(m_Editor.GetScene());
		ENGINE_TRY_ASSIGN(RenderSnapshot snapshot, session && !preview ? session->ExtractView(request) : ExtractRenderSnapshot(*scene, request));
		snapshot.SceneRevision = scene->GetRevision();
		if (view == ViewportView::Scene)
			snapshot.DebugView = m_Editor.GetViewportState().GetDebugView();
		if (m_Editor.HasProject())
		{
			const auto& physics = m_Editor.GetProject().GetSettings().Physics;
			ENGINE_TRY_ASSIGN(const PhysicsLayerTable layers, PhysicsLayerTable::Create(physics.Layers, physics.Collisions));
			ENGINE_TRY(AppendRenderAnnotations(*scene, m_Editor.GetAssets(), layers, session && !preview ? &session->GetPhysics() : nullptr, snapshot));
		}
		return snapshot;
	}
	Status EditorHostViewports::RequestPick(const EditorViewportClick& click)
	{
		if (click.View != ViewportView::Scene && click.View != ViewportView::Game)
			return MakeError(ErrorCode::InvalidArgument, "Unknown viewport");
		View& view = m_Views[Index(click.View)];
		if (!view.Renderer || view.Image.Texture == 0)
			return MakeError(ErrorCode::Unsupported, "This viewport has no picking image");
		const EditorViewportImage& image = view.Image;
		if (click.FrameIndex != image.FrameIndex || click.SceneRevision != image.SceneRevision || click.ViewGeneration != image.Generation
			|| click.Sequence <= view.LatestSequence || image.Generation != view.Renderer->GetViewGeneration())
			return MakeError(ErrorCode::Conflict, "The clicked image is no longer current");
		if (click.Pixel.X >= image.Width || click.Pixel.Y >= image.Height)
			return MakeError(ErrorCode::InvalidArgument, "The click is outside the image");
		view.LatestSequence = click.Sequence;
		view.Queued = click;
		return {};
	}
	Status EditorHostViewports::Poll(View& view, uint64_t frameIndex)
	{
		for (auto item = view.Pending.begin(); item != view.Pending.end();)
		{
			auto result = view.Renderer->PollPick(item->Ticket, frameIndex);
			if (!result)
			{
				if (result.error().GetCode() != ErrorCode::Cancelled && result.error().GetCode() != ErrorCode::NotFound)
					return std::unexpected(std::move(result).error());
				item = view.Pending.erase(item);
				continue;
			}
			if (!result->has_value())
			{
				++item;
				continue;
			}
			const PickResult picked = **result;
			const EditorViewportClick click = item->Click;
			item = view.Pending.erase(item);
			PlaySession* session = m_Editor.GetPlay().GetSession();
			Scene* scene = session ? &session->GetScene() : m_Editor.HasScene() ? &m_Editor.GetScene()
																				: nullptr;
			if (!scene || scene != view.Source || scene->GetRevision() != picked.SceneRevision
				|| picked.Sequence != view.LatestSequence || picked.FrameIndex != click.FrameIndex || picked.SceneRevision != click.SceneRevision
				|| picked.ViewGeneration != view.Renderer->GetViewGeneration())
				continue;
			const SceneTarget target = session ? SceneTarget::Play : SceneTarget::Edit;
			std::vector<UUID> selection;
			if (picked.Entity.IsValid())
			{
				if (!scene->FindEntityByID(picked.Entity))
					continue;
				if (click.ExtendSelection && m_Editor.GetSelectionTarget() == target)
					selection.assign(m_Editor.GetSelection().begin(), m_Editor.GetSelection().end());
				if (std::ranges::find(selection, picked.Entity) == selection.end())
					selection.push_back(picked.Entity);
			}
			ENGINE_TRY(m_Editor.SetSelection(selection, target));
		}
		return {};
	}
	Status EditorHostViewports::Render(RenderContext& context)
	{
		for (size_t i = 0; i < m_Views.size(); ++i)
		{
			View& view = m_Views[i];
			if (!view.Renderer)
				continue;
			const ViewportView kind = i == 0 ? ViewportView::Scene : ViewportView::Game;
			const auto panel = i == 0 ? EditorPanel::SceneViewport : EditorPanel::GameViewport;
			const glm::uvec2 size = m_Editor.HasProject() && std::ranges::find(m_Editor.GetUiState().GetOpenPanels(), panel) != m_Editor.GetUiState().GetOpenPanels().end() ? view.RequestedSize : glm::uvec2(0);
			if (size.x == 0 || size.y == 0)
			{
				if (view.Image.Texture != 0)
				{
					m_ImGui->RemoveTexture(view.Image.Texture);
					Cancel(view);
				}
				view.Image = {};
				view.RegisteredTexture = nullptr;
				ENGINE_TRY(m_Editor.GetViewportState().SetPixelSize(kind, glm::uvec2(0)));
				continue;
			}
			ENGINE_TRY_ASSIGN(RenderSnapshot snapshot, Extract(kind, size.x, size.y));
			PlaySession* session = m_Editor.GetPlay().GetSession();
			const Scene* source = session ? &session->GetScene() : m_Editor.HasScene() ? &m_Editor.GetScene()
																					   : nullptr;
			// Cancel before polling: a proportional resize keeps the camera matrices equal, but the old pixel no longer
			// identifies the image about to be rendered. Resize increments the renderer generation only after this point.
			if (view.Image.Width != size.x || view.Image.Height != size.y
				|| view.Source != source || view.Image.SceneRevision != snapshot.SceneRevision || view.Image.Camera.View != snapshot.Camera.View
				|| view.Image.Camera.Projection != snapshot.Camera.Projection || view.Flags != snapshot.Flags || m_Gizmos.IsDragging())
				Cancel(view);
			view.Source = source;
			view.Flags = snapshot.Flags;
			ENGINE_TRY(Poll(view, context.FrameIndex));
			ENGINE_TRY(view.Renderer->Resize(size.x, size.y));
			snapshot.FrameIndex = context.FrameIndex;
			view.Recorded = true;
			const Status rendered = view.Renderer->Render(*context.CommandList, snapshot);
			nvrhi::ITexture* texture = view.Renderer->GetFinalTexture();
			if (texture != view.RegisteredTexture)
			{
				if (view.Image.Texture != 0)
					m_ImGui->RemoveTexture(view.Image.Texture);
				view.Image.Texture = m_ImGui->AddTexture(*texture);
				view.RegisteredTexture = texture;
			}
			view.Image.Width = size.x;
			view.Image.Height = size.y;
			view.Image.Generation = view.Renderer->GetViewGeneration();
			view.Image.FrameIndex = context.FrameIndex;
			view.Image.SceneRevision = snapshot.SceneRevision;
			view.Image.Camera = snapshot.Camera;
			ENGINE_TRY(m_Editor.GetViewportState().SetPixelSize(kind, size));
			ENGINE_TRY(rendered);
		}
		return {};
	}
	Status EditorHostViewports::Submitted(uint64_t frameIndex, uint64_t submissionId)
	{
		std::optional<Error> failure;
		for (View& view : m_Views)
		{
			if (!view.Renderer || !view.Recorded)
				continue;
			view.Renderer->OnSubmitted(frameIndex, submissionId);
			view.Recorded = false;
			if (!view.Queued)
				continue;
			const EditorViewportClick click = *view.Queued;
			view.Queued.reset();
			if (click.FrameIndex != frameIndex || click.ViewGeneration != view.Renderer->GetViewGeneration())
				continue;
			const auto ticket = view.Renderer->RequestPick({ .X = click.Pixel.X, .Y = click.Pixel.Y, .FrameIndex = click.FrameIndex, .SceneRevision = click.SceneRevision, .Sequence = click.Sequence, .ViewGeneration = click.ViewGeneration });
			if (!ticket)
			{
				if (!failure && ticket.error().GetCode() != ErrorCode::Cancelled && ticket.error().GetCode() != ErrorCode::Conflict)
					failure = ticket.error();
				continue;
			}
			view.Pending.push_back({ .Ticket = *ticket, .Click = click });
		}
		return failure ? Status(std::unexpected(*failure)) : Status{};
	}
	void EditorHostViewports::SetGameInputFocused(bool focused)
	{
		if (m_GameFocused && !focused)
		{
			const EditorPlayController& play = m_Editor.GetPlay();
			if (Window* window = m_Editor.GetEngine().GetWindow(); window != nullptr && play.IsPlaying())
				window->SetCursorMode(CursorMode::Normal);
			if (PlaySession* session = play.GetSession(); session != nullptr && !play.IsLiveInputSuppressed())
				session->GetInput().QueueReleaseAll(session->GetTick());
		}
		m_GameFocused = focused;
	}

}
