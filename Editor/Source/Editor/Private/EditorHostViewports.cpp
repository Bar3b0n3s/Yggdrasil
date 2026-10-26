#include "EditorPCH.h"
#include "Editor/Private/EditorHostViewports.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Window.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderAnnotations.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"

#include <algorithm>
#include <cmath>
#include <utility>

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
			view.HoldsSceneResources = false;
		}
		m_Device = &device;
		m_Pipelines = &pipelines;
		m_Cache = &cache;
		m_ImGui = &imgui;
		// After the ImGui renderer and the views' renderers: released before them (GpuRecreation.h).
		m_Recreation = device.GetRecreation().Register({
			.Name = "EditorHostViewports",
			.Release = [this]()
		{
			ReleaseGpuObjects();
		},
			.Recreate = {},
		});
		return {};
	}
	void EditorHostViewports::ReleaseGpuObjects()
	{
		// The recovery releases everything, so the lost frame's trim is not needed.
		m_TrimAfterSubmission = false;
		for (View& view : m_Views)
		{
			// The renderer resolves its own pending picks as cancelled in its release; the host's tickets, its queued click
			// and its recorded frame name a submission that never completes.
			view.Queued.reset();
			view.Pending.clear();
			view.Recorded = false;
			if (m_ImGui && view.Image.Texture != 0)
			{
				m_ImGui->RemoveTexture(view.Image.Texture);
				++m_Withdrawals;
			}
			view.Image = {};
			view.RegisteredTexture = nullptr;
			view.HoldsSceneResources = false;
		}
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
		m_Recreation.Reset();
		m_TrimAfterSubmission = false;
		for (View& view : m_Views)
		{
			Cancel(view);
			if (m_ImGui && view.Image.Texture != 0)
			{
				m_ImGui->RemoveTexture(view.Image.Texture);
				++m_Withdrawals;
			}
			view.Image = {};
			view.RegisteredTexture = nullptr;
			view.Renderer.reset();
			view.HoldsSceneResources = false;
		}
		m_ImGui = nullptr;
		m_Device = nullptr;
		m_Pipelines = nullptr;
		m_Cache = nullptr;
	}
	void EditorHostViewports::SetUnavailable()
	{
		Invalidate();
		m_Unavailable = true;
		for (size_t i = 0; i < m_Views.size(); ++i)
		{
			View& view = m_Views[i];
			if (m_ImGui && view.Image.Texture != 0)
			{
				m_ImGui->RemoveTexture(view.Image.Texture);
				++m_Withdrawals;
			}
			view.Image = {};
			view.RegisteredTexture = nullptr;
			const Status status = m_Editor.GetViewportState().SetPixelSize(i == 0 ? ViewportView::Scene : ViewportView::Game, glm::uvec2(0));
			ENGINE_VERIFY(status.has_value(), "valid viewport extent");
		}
	}
	void EditorHostViewports::SetAvailable()
	{
		m_Unavailable = false;
	}
	Status EditorHostViewports::ForceGameView()
	{
		// A native window displays the panels the user arranged; only a headless window (or none, in process) has no
		// Game panel on screen to decide for.
		if (m_Editor.GetEngine().GetWindow() != nullptr && GlfwLibrary::GetMode() != WindowMode::Headless)
			return MakeError(ErrorCode::InvalidState, "the Game view is forced only in a headless editor (--headless-game-view needs --headless): a windowed editor renders the Game view its panel displays");
		m_GameForced = true;
		return {};
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
			// A size that ran out of memory renders at the size the view stays at (M18, C18.4 decision 44).
			const bool atFailedSize = view.FailedSize == view.RequestedSize && view.Renderer != nullptr
				&& view.Image.Width == view.Renderer->GetWidth() && view.Image.Height == view.Renderer->GetHeight();
			if (!atFailedSize && (view.RequestedSize.x != view.Image.Width || view.RequestedSize.y != view.Image.Height))
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
		const ImVec2 scale = ImGui::GetCurrentContext() != nullptr ? ImGui::GetIO().DisplayFramebufferScale : ImVec2(1.0f, 1.0f);
		if (std::isfinite(rectangle.Size.x) && std::isfinite(rectangle.Size.y) && rectangle.Size.x > 0 && rectangle.Size.y > 0
			&& std::isfinite(scale.x) && std::isfinite(scale.y) && scale.x > 0 && scale.y > 0)
		{
			size = glm::uvec2(static_cast<uint32_t>(std::clamp(std::floor(rectangle.Size.x * scale.x), 1.0f, 8192.0f)),
				static_cast<uint32_t>(std::clamp(std::floor(rectangle.Size.y * scale.y), 1.0f, 8192.0f)));
			if (view == ViewportView::Game && m_Editor.GetViewportState().GetGameResolution().x != 0)
				size = m_Editor.GetViewportState().GetGameResolution();
		}
		if (view == ViewportView::Game)
			m_GameRectangle = rectangle;
		// A view without area is not interactive (ADR 0018): a gizmo drag in it ends without writing anything.
		if (view == ViewportView::Scene && size == glm::uvec2(0) && m_Gizmos.IsDragging())
			m_Gizmos.Cancel();
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
		// Only the scene view picks (SceneViewportPanel); the game view renders like the game, without an EntityId target.
		request.Flags = RenderViewFlags::None;
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
		if (!view.Renderer || view.Image.Texture == 0 || !view.Image.Picking)
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
		if (m_Device == nullptr)
			return {};
		PlaySession* session = m_Editor.GetPlay().GetSession();
		const bool sceneChanged = m_ShownScenes.Update(m_Editor.HasScene() ? &m_Editor.GetScene() : nullptr,
			session ? &session->GetScene() : nullptr, m_Editor.GetEngine().GetEventLog());
		// One view's failure (a partial render error, an extraction or pick failure) never keeps the other from rendering.
		std::optional<Error> failure;
		for (size_t i = 0; i < m_Views.size(); ++i)
		{
			Status rendered = RenderView(i, context, sceneChanged);
			if (rendered)
				continue;
			// A Gpu error comes first: the host trims for it after the frame (M18, C18.4 decision 44).
			if (!failure || (failure->GetCode() != ErrorCode::Gpu && rendered.error().GetCode() == ErrorCode::Gpu))
				failure = std::move(rendered).error();
		}
		// M18 (C18.4, decision 44): a view whose lazily created target or binding set ran out of memory (a Gpu error on a
		// device that is not lost) asks for one trim after the frame's submission (Submitted), however many views failed.
		m_TrimAfterSubmission = failure.has_value() && IsGpuOutOfMemory(*failure, *m_Device);
		return failure ? Status(std::unexpected(std::move(*failure))) : Status{};
	}
	Status EditorHostViewports::RenderView(size_t index, RenderContext& context, bool sceneChanged)
	{
		View& view = m_Views[index];
		const ViewportView kind = index == 0 ? ViewportView::Scene : ViewportView::Game;
		const auto panel = index == 0 ? EditorPanel::SceneViewport : EditorPanel::GameViewport;
		glm::uvec2 size = m_Editor.HasProject() && std::ranges::find(m_Editor.GetUiState().GetOpenPanels(), panel) != m_Editor.GetUiState().GetOpenPanels().end() ? view.RequestedSize : glm::uvec2(0);
		// A forced Game view (--headless-game-view) renders at the size lockstep and replay sessions use as view coordinates,
		// or at the fixed game resolution when one is set, whatever its panel does; at most 8192 pixels a side, as a panel.
		if (kind == ViewportView::Game && m_GameForced && m_Editor.HasProject())
		{
			const glm::uvec2 resolution = m_Editor.GetViewportState().GetGameResolution();
			size = glm::min(resolution.x != 0 ? resolution : GetDeterministicViewSize(m_Editor.GetProject().GetSettings().Window), glm::uvec2(8192));
		}
		if (size.x == 0 || size.y == 0)
		{
			if (view.Image.Texture != 0)
			{
				m_ImGui->RemoveTexture(view.Image.Texture);
				Cancel(view);
			}
			view.Image = {};
			view.RegisteredTexture = nullptr;
			// A hidden view renders nothing, so its renderer would never drop the bindings that keep the previous scene's
			// textures alive (they go one render after their last use): release it and recreate it when displayed again.
			if (sceneChanged && view.HoldsSceneResources)
			{
				Cancel(view);
				view.Renderer.reset();
				view.HoldsSceneResources = false;
				view.Source = nullptr;
			}
			return m_Editor.GetViewportState().SetPixelSize(kind, glm::uvec2(0));
		}
		if (!view.Renderer)
		{
			ENGINE_TRY_ASSIGN(view.Renderer, SceneRenderer::Create(*m_Device, *m_Pipelines, *m_Cache, m_Editor.GetAssets(), { .Width = size.x, .Height = size.y }));
			view.FailedSize = glm::uvec2(0);
		}
		// M18 (C18.4, decision 44): a new size is a request. When it runs out of memory after the renderer's trim and retry,
		// the view goes on at its previous size, shown scaled into the panel, and the size is asked for again only once the
		// requested size changes. Resizing cancels the renderer's picks (a new generation), as the size check below would.
		if (size != view.FailedSize)
		{
			const Status resized = view.Renderer->Resize(size.x, size.y);
			if (!resized)
			{
				if (resized.error().GetCode() != ErrorCode::OutOfMemory)
					return resized;
				ENGINE_WARN("The {} view stays at {}x{}: its {}x{} targets could not be created: {}", kind == ViewportView::Scene ? "scene" : "game",
					view.Renderer->GetWidth(), view.Renderer->GetHeight(), size.x, size.y, resized.error());
				view.FailedSize = size;
			}
			else
			{
				view.FailedSize = glm::uvec2(0);
			}
		}
		const glm::uvec2 rendered(view.Renderer->GetWidth(), view.Renderer->GetHeight());
		ENGINE_TRY_ASSIGN(RenderSnapshot snapshot, Extract(kind, rendered.x, rendered.y));
		PlaySession* session = m_Editor.GetPlay().GetSession();
		const Scene* source = session ? &session->GetScene() : m_Editor.HasScene() ? &m_Editor.GetScene()
																				   : nullptr;
		// Cancel before polling: a proportional resize keeps the camera matrices equal, but the old pixel no longer
		// identifies the image about to be rendered. A Resize above has already cancelled the renderer's picks and
		// incremented its generation; this cancels the view's queued and pending clicks too.
		if (view.Image.Width != rendered.x || view.Image.Height != rendered.y
			|| view.Source != source || view.Image.SceneRevision != snapshot.SceneRevision || view.Image.Camera.View != snapshot.Camera.View
			|| view.Image.Camera.Projection != snapshot.Camera.Projection || view.Flags != snapshot.Flags || m_Gizmos.IsDragging())
			Cancel(view);
		view.Source = source;
		view.Flags = snapshot.Flags;
		ENGINE_TRY(Poll(view, context.FrameIndex));
		snapshot.FrameIndex = context.FrameIndex;
		view.Recorded = true;
		view.HoldsSceneResources = true;
		Status recorded = view.Renderer->Render(*context.CommandList, snapshot);
		// M18 (C18.4, decision 44): a debug-view switch that ran out of memory recorded the view in the debug view its renderer
		// stayed on (the one it last rendered, or Lit when a shader reload or a recovery has dropped that one's pipelines),
		// which the renderer reported once; the setting goes back to that view, so the next frames do not try again, and a
		// later switch does.
		if (!recorded && recorded.error().GetCode() == ErrorCode::OutOfMemory)
		{
			if (kind == ViewportView::Scene)
				ENGINE_TRY(m_Editor.GetViewportState().SetDebugView(view.Renderer->GetRenderedDebugView()));
			recorded = {};
		}
		nvrhi::ITexture* texture = view.Renderer->GetFinalTexture();
		if (texture != view.RegisteredTexture)
		{
			if (view.Image.Texture != 0)
				m_ImGui->RemoveTexture(view.Image.Texture);
			view.Image.Texture = m_ImGui->AddTexture(*texture);
			view.RegisteredTexture = texture;
		}
		view.Image.Width = rendered.x;
		view.Image.Height = rendered.y;
		view.Image.Generation = view.Renderer->GetViewGeneration();
		view.Image.FrameIndex = context.FrameIndex;
		view.Image.SceneRevision = snapshot.SceneRevision;
		view.Image.Camera = snapshot.Camera;
		view.Image.Picking = HasFlag(snapshot.Flags, RenderViewFlags::Picking);
		// A frame forced while minimized renders the images for the UI capture only (ADR 0018: screenshot captures do not
		// publish viewport extents); the views stay unavailable until the window is restored.
		if (!m_Unavailable)
			ENGINE_TRY(m_Editor.GetViewportState().SetPixelSize(kind, rendered));
		return recorded;
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
			// A frame that lost the device ran nothing a pick could read, and the recovery's release cancels the view's picks:
			// the click is dropped, since a pick's objects cannot be created on the lost device, and that refusal is the loss,
			// not a failure to report (decision 28).
			if (m_Device != nullptr && m_Device->IsLost())
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
		// M18 (C18.4, decision 44): after the frame's submission and the views' notifications, the trim an out-of-memory view
		// render asked for. It waits for the device's last submission before any owner drops what it can rebuild, and does
		// nothing on a lost device.
		if (std::exchange(m_TrimAfterSubmission, false) && m_Device != nullptr)
			m_Device->TrimUnused();
		return failure ? Status(std::unexpected(*failure)) : Status{};
	}
	bool EditorHostViewports::IsCursorLocked() const
	{
		const Window* window = m_Editor.GetEngine().GetWindow();
		return window != nullptr && window->GetCursorMode() == CursorMode::Locked;
	}
	std::optional<Event> EditorHostViewports::ToGameEvent(const Event& event)
	{
		if (const auto* move = std::get_if<MouseMoveEvent>(&event))
			m_Pointer = move->Position;
		if (!m_GameFocused)
			return std::nullopt;
		if (const auto* move = std::get_if<MouseMoveEvent>(&event))
		{
			const std::optional<glm::vec2> position = MapToViewCoordinates(move->Position, m_GameRectangle.Min, m_GameRectangle.Size, m_GameRectangle.ImageSize);
			if (!position)
				return std::nullopt;
			return Event(MouseMoveEvent{ .Position = *position });
		}
		if (const auto* button = std::get_if<MouseButtonEvent>(&event))
		{
			// A press in the letterbox bars, or anywhere off the game image, is not a press on the game.
			if (button->Action == ButtonAction::Pressed && !IsCursorLocked() && (!m_Pointer || !ToViewportPixel(m_GameRectangle, *m_Pointer)))
				return std::nullopt;
			return event;
		}
		if (std::holds_alternative<KeyEvent>(event) || std::holds_alternative<CharEvent>(event) || std::holds_alternative<MouseScrollEvent>(event)
			|| std::holds_alternative<GamepadEvent>(event))
			return event;
		return std::nullopt;
	}
	void EditorHostViewports::SetGameInputFocused(bool focused)
	{
		// Game keys must not also navigate ImGui, which can move focus away and synthesize game-key releases.
		// This also restores editor navigation when the view closes, the native window blurs or Play stops.
		if (ImGui::GetCurrentContext() != nullptr)
		{
			ImGuiIO& io = ImGui::GetIO();
			if (focused)
				io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
			else
				io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			// A Locked cursor's unbounded virtual position means nothing to the UI: presses must not land on other panels.
			if (focused && IsCursorLocked())
				io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
			else
				io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
		}
		const bool captured = focused && !m_GameFocused;
		if (m_GameFocused && !focused)
		{
			const EditorPlayController& play = m_Editor.GetPlay();
			if (Window* window = m_Editor.GetEngine().GetWindow(); window != nullptr && play.IsPlaying())
				window->SetCursorMode(CursorMode::Normal);
			// Native input never reaches a lockstep session (its owner injects all of its input), so a UI focus change
			// must not inject releases into it either.
			if (PlaySession* session = play.GetSession(); session != nullptr && !play.IsLiveInputSuppressed() && !session->IsLockstep())
				session->GetInput().QueueReleaseAll(session->GetTick());
		}
		m_GameFocused = focused;
		if (!captured)
			return;
		// The capturing press itself is not game input; the game's cursor starts where that press happened.
		std::optional<glm::vec2> pointer = m_Pointer;
		if (!pointer && ImGui::GetCurrentContext() != nullptr && ImGui::IsMousePosValid())
			pointer = glm::vec2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
		if (!pointer)
			return;
		if (const std::optional<Event> move = ToGameEvent(MouseMoveEvent{ .Position = *pointer }))
			m_Editor.GetPlay().OnInputEvent(*move, true);
	}

}
