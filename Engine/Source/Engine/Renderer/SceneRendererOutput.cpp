#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/EditorOverlay.h"
#include "Engine/Renderer/Private/SceneRendererState.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/SelectionPass.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/UpscalePass.h"
#include "Shared/ViewConstants.h"

#include <glm/matrix.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

// The scene renderer's last three stages (SceneRenderer.h; Docs/Decisions/0029-m18-contract.md decisions 1 and 6): the
// scene-resolution overlays (game Text, then M22's reserved Ui slot), the output stage's integer upscale with its
// letterbox, and the view-resolution editor overlays.
//
// At the identity mapping (the default scene resolution) the Output stage records nothing and the editor overlays draw into
// the scene's LDR target with SceneDepth and the scene's ViewConstants, exactly as before the output stage. Otherwise the
// Output stage upscales the scene image into OutputColor (and, when an editor overlay will draw, SceneDepth into
// OutputDepth), and the editor overlays draw at the view's resolution with the overlay camera (Utils::MakeOverlayCamera):
// the scene camera whose projection is followed by the clip-space transform that maps the scene's clip volume onto the
// scaled scene rectangle of the view, so with the view's whole extent as viewport every overlay lands on the view pixels the
// rectangle's viewport would give it, one-pixel lines stay one view pixel wide and labels and icons keep their pixel sizes.
// The overlay passes (DebugRenderer, TextRenderer, SelectionPass) take no viewport of their own, so the scaled scene
// rectangle does not clip them while they draw: an overlay beyond the scene camera's frustum reaches the letterbox. The
// stage's last row, "Letterbox" (UpscalePass::RecordLetterbox), then gives the letterbox bars the letterbox colour again,
// so every overlay (lines, labels and the selection composite's dilated outline alike) ends clipped to the scaled scene
// rectangle, as that rectangle's scissor would clip it (decision 6).

namespace Engine {

	namespace Utils {

		// A per-view target of the output stage from the renderer's pool, kept between command lists in its writing state or,
		// for OutputColor, in ShaderResource (BlitPass samples it and Readback copies it, as the scene's LDR targets).
		static Result<nvrhi::TextureHandle> AcquireOutputTarget(RenderTargetPool& pool, uint32_t width, uint32_t height, nvrhi::Format format,
			std::string_view name)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = format;
			desc.dimension = nvrhi::TextureDimension::Texture2D;
			desc.isShaderResource = true;
			desc.isRenderTarget = true;
			// OutputColor is the selection composite's storage image, as the scene's LDR targets are.
			desc.isUAV = format == LdrColorFormat;
			if (format == LdrColorFormat)
				desc.initialState = nvrhi::ResourceStates::ShaderResource;
			else
				desc.initialState = format == SceneDepthFormat ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			desc.debugName = std::format("SceneRenderer.{}", name);
			return pool.Acquire(desc);
		}

		// Ensures `targets` at `mapping`'s view size: OutputColor always; OutputDepth with the overlay framebuffer, the
		// overlay ViewConstants and the letterbox repaint's texel when `overlays`; the view-size selection mask and scratch
		// when `selection`; the letterbox's EntityId texel when `picking`. Targets of another size are replaced; returns
		// whether any target was replaced.
		static Result<bool> EnsureOutputTargets(GraphicsDevice& device, RenderTargetPool& pool, Detail::SceneRendererOutputTargets& targets,
			const RenderOutputMapping& mapping, bool overlays, bool selection, bool picking)
		{
			const uint32_t width = mapping.ViewWidth;
			const uint32_t height = mapping.ViewHeight;
			bool replaced = false;
			if (targets.Color == nullptr || targets.Width != width || targets.Height != height)
			{
				// The size-dependent targets are made again; the overlay constants and the letterbox texels are kept.
				Detail::SceneRendererOutputTargets created;
				created.Width = width;
				created.Height = height;
				ENGINE_TRY_ASSIGN(created.Color, AcquireOutputTarget(pool, width, height, LdrColorFormat, "OutputColor"));
				ENGINE_TRY_ASSIGN(created.ColorFramebuffer, device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(created.Color)));
				created.OverlayViewConstants = std::move(targets.OverlayViewConstants);
				created.LetterboxTexel = std::move(targets.LetterboxTexel);
				created.LetterboxEntityId = std::move(targets.LetterboxEntityId);
				targets = std::move(created);
				replaced = true;
			}
			if (overlays && targets.Depth == nullptr)
			{
				ENGINE_TRY_ASSIGN(nvrhi::TextureHandle depth, AcquireOutputTarget(pool, width, height, SceneDepthFormat, "OutputDepth"));
				ENGINE_TRY_ASSIGN(nvrhi::FramebufferHandle depthFramebuffer, device.CreateFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(depth)));
				// The editor overlays test against OutputDepth without writing it (their pipelines disable depth writes; the
				// attachment is not marked read-only, as the scene's overlay framebuffers, SceneRenderer.cpp).
				ENGINE_TRY_ASSIGN(nvrhi::FramebufferHandle overlayFramebuffer,
					device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.Color).setDepthAttachment(depth)));
				targets.Depth = std::move(depth);
				targets.DepthFramebuffer = std::move(depthFramebuffer);
				targets.OverlayFramebuffer = std::move(overlayFramebuffer);
			}
			if (overlays && targets.OverlayViewConstants == nullptr)
			{
				nvrhi::BufferDesc desc;
				desc.byteSize = sizeof(ViewConstants);
				desc.isConstantBuffer = true;
				desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				desc.keepInitialState = true;
				desc.debugName = "SceneRenderer.OverlayViewConstants";
				ENGINE_TRY_ASSIGN(targets.OverlayViewConstants, device.CreateBuffer(desc));
			}
			if (overlays && targets.LetterboxTexel == nullptr)
			{
				ENGINE_TRY_ASSIGN(targets.LetterboxTexel, AcquireOutputTarget(pool, 1, 1, LdrColorFormat, "LetterboxTexel"));
			}
			if (selection && targets.SelectionMask == nullptr)
			{
				ENGINE_TRY_ASSIGN(nvrhi::TextureHandle mask, pool.Acquire(SelectionPass::GetMaskDesc(width, height)));
				ENGINE_TRY_ASSIGN(nvrhi::TextureHandle scratch, pool.Acquire(SelectionPass::GetMaskDesc(width, height)));
				targets.SelectionMask = std::move(mask);
				targets.SelectionScratch = std::move(scratch);
			}
			if (picking && targets.LetterboxEntityId == nullptr)
			{
				ENGINE_TRY_ASSIGN(targets.LetterboxEntityId, AcquireOutputTarget(pool, 1, 1, EntityIdFormat, "LetterboxEntityId"));
			}
			return replaced;
		}

		// Whether the EditorOverlay stage draws anything: a camera, and the wireframe, the selection outline, the snapshot's
		// debug lines (none in a data view), its annotations, or the editor's grid and icons (AppendEditorOverlay).
		static bool HasEditorOverlays(const Detail::SceneRendererFrame& frame)
		{
			const RenderSnapshot& snapshot = *frame.Snapshot;
			if (!frame.HasCamera)
				return false;
			const bool editorGlyphs = HasFlag(snapshot.Flags, RenderViewFlags::EditorOverlays)
				&& (HasFlag(snapshot.Flags, RenderViewFlags::Grid) || HasFlag(snapshot.Flags, RenderViewFlags::Icons));
			return frame.Wireframe || frame.Selection || !snapshot.AnnotationDraw.GetCommands().empty()
				|| (!frame.IsDataView && (editorGlyphs || !snapshot.DebugDraw.GetCommands().empty()));
		}

		// The editor overlays' camera for a mapping other than the identity: `camera` (its viewport at the scene resolution)
		// with the view's size as viewport and its projection followed by the clip-space scale and offset that map the scene's
		// clip volume onto the scaled scene rectangle of the view: x' = x * Scale * SceneWidth / ViewWidth + t * w with
		// t = (2 * OffsetX + Scale * SceneWidth) / ViewWidth - 1, and y likewise (clip-space +y is up, so the offset counts
		// from the top). Depth and w are unchanged, so the overlays test against the upscaled scene depth.
		static CameraData MakeOverlayCamera(const CameraData& camera, const RenderOutputMapping& mapping)
		{
			const double viewWidth = mapping.ViewWidth;
			const double viewHeight = mapping.ViewHeight;
			const double scaledWidth = static_cast<double>(mapping.Scale) * mapping.SceneWidth;
			const double scaledHeight = static_cast<double>(mapping.Scale) * mapping.SceneHeight;
			glm::dmat4 toView(1.0);
			toView[0][0] = scaledWidth / viewWidth;
			toView[1][1] = scaledHeight / viewHeight;
			toView[3][0] = (2.0 * mapping.OffsetX + scaledWidth) / viewWidth - 1.0;
			toView[3][1] = 1.0 - (2.0 * mapping.OffsetY + scaledHeight) / viewHeight;
			CameraData overlay = camera;
			overlay.Projection = glm::mat4(toView * glm::dmat4(camera.Projection));
			overlay.ViewportWidth = mapping.ViewWidth;
			overlay.ViewportHeight = mapping.ViewHeight;
			return overlay;
		}

		// The ViewConstants of `camera` for the overlay passes (DebugLines and Text read the matrices, the projection kind, its
		// extents and the viewport), as SceneRenderer.cpp makes a view's, at exposure 1 and without lights.
		static ViewConstants MakeOverlayViewConstants(const CameraData& camera)
		{
			const glm::vec2 viewportSize(static_cast<float>(camera.ViewportWidth), static_cast<float>(camera.ViewportHeight));
			ViewConstants view{};
			view.View = camera.View;
			view.Projection = camera.Projection;
			view.ViewProjection = camera.Projection * camera.View;
			view.InverseProjection = glm::inverse(camera.Projection);
			view.InverseViewProjection = glm::inverse(view.ViewProjection);
			view.CameraPosition = camera.Position;
			view.Near = camera.NearClip;
			view.ViewportSize = viewportSize;
			view.InverseViewportSize = 1.0f / viewportSize;
			view.AspectRatio = viewportSize.x / viewportSize.y;
			view.Exposure = 1.0f;
			if (camera.ProjectionKind == RenderProjection::Orthographic)
			{
				view.ProjectionKind = ProjectionKindOrthographic;
				view.OrthoHalfExtents = glm::vec2(1.0f / camera.Projection[0][0], 1.0f / camera.Projection[1][1]);
				view.Far = camera.FarClip;
				view.TanHalfFovY = 0.0f;
			}
			else
			{
				view.ProjectionKind = ProjectionKindPerspective;
				view.OrthoHalfExtents = glm::vec2(0.0f);
				view.Far = 0.0f;
				view.TanHalfFovY = 1.0f / camera.Projection[1][1];
			}
			return view;
		}

		static bool IsLabel(const DebugDrawCommand& command)
		{
			return std::holds_alternative<DebugText>(command.Shape);
		}

	}

	void SceneRenderer::State::RecordSceneOverlayStage(Detail::SceneRendererFrame& frame)
	{
		// Game texts, world then screen (TextRenderer), at the scene resolution into the scene image; the data views show
		// none (ADR 0017). The debug and annotation labels are editor overlays (RecordEditorOverlayStage).
		const RenderSnapshot& snapshot = *frame.Snapshot;
		if (frame.IsDataView || snapshot.Texts.empty())
			return;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		RenderRecordingContext& recording = *frame.Recording;
		recording.BeginPass("Text", &commandList);
		RenderPassCounters counters;
		const TextRenderInputs inputs{ .Texts = std::span<const TextItem>(snapshot.Texts),
			.DebugDraw = nullptr,
			.Assets = Assets,
			.Framebuffer = Targets.OverlayFramebuffers[frame.SceneTarget],
			.ViewConstants = ViewConstantsBuffer,
			.HasCamera = frame.HasCamera,
			.CameraView = frame.HasCamera ? std::optional<glm::mat4>(frame.Camera.View) : std::nullopt };
		auto result = Pipelines->Text->RecordCounted(commandList, TextBindings, inputs, counters);
		if (result)
			Stats.TextDraws += *result;
		else
			frame.KeepFirstError(std::unexpected(std::move(result).error()));
		recording.EndPass(counters);
	}

	void SceneRenderer::State::RecordOutputStage(Detail::SceneRendererFrame& frame)
	{
		const RenderOutputMapping& mapping = frame.Output;
		OutputMapping = mapping;
		if (mapping.IsIdentity())
		{
			// The editor overlays draw into the scene image, as before the output stage; an earlier mapping's output targets
			// go back to the pool.
			if (OutputTargets.Color != nullptr)
			{
				OutputTargets = {};
				UpscaleBindings.Clear();
			}
			frame.ViewColor = Targets.Ldr[frame.SceneTarget];
			frame.ViewOverlayFramebuffer = Targets.OverlayFramebuffers[frame.SceneTarget];
			frame.ViewOverlayConstants = ViewConstantsBuffer;
			return;
		}

		frame.ViewColor = nullptr;
		frame.ViewOverlayFramebuffer = nullptr;
		frame.ViewOverlayConstants = nullptr;
		ENGINE_CORE_ASSERT(Pipelines->Upscale != nullptr, "the pipeline set has no upscale pass");
		if (Pipelines->Upscale == nullptr)
		{
			frame.KeepFirstError(MakeError(ErrorCode::InvalidState, "the scene renderer's pipelines have no upscale pass"));
			return;
		}
		const RenderSnapshot& snapshot = *frame.Snapshot;
		const bool overlays = Utils::HasEditorOverlays(frame);
		const Result<bool> ensured = Utils::EnsureOutputTargets(*Device, *TargetPool, OutputTargets, mapping, overlays, overlays && frame.Selection, frame.Picking);
		if (!ensured)
		{
			// No output image this render: targets of an earlier size go back to the pool rather than stand in for it.
			OutputTargets = {};
			UpscaleBindings.Clear();
			frame.KeepFirstError(std::unexpected(ensured.error()));
			return;
		}
		if (*ensured)
		{
			// The binding sets of the replaced targets.
			UpscaleBindings.Clear();
			SelectionBindings.Clear();
		}

		nvrhi::ICommandList& commandList = *frame.CommandList;
		if (overlays)
		{
			const ViewConstants constants = Utils::MakeOverlayViewConstants(Utils::MakeOverlayCamera(frame.Camera, mapping));
			commandList.writeBuffer(OutputTargets.OverlayViewConstants, &constants, sizeof(constants));
		}
		// A pick of a letterbox pixel reads this texel (SceneRenderer::RequestPick).
		if (frame.Picking)
			commandList.clearTextureUInt(OutputTargets.LetterboxEntityId, nvrhi::AllSubresources, 0);

		RenderRecordingContext& recording = *frame.Recording;
		recording.BeginPass("Upscale", &commandList);
		RenderPassCounters counters;
		const UpscalePassInputs inputs{ .Source = Targets.Ldr[frame.SceneTarget],
			.Color = OutputTargets.ColorFramebuffer,
			.SourceDepth = overlays ? Targets.SceneDepth.Get() : nullptr,
			.Depth = overlays ? OutputTargets.DepthFramebuffer.Get() : nullptr,
			.Mapping = mapping,
			.LetterboxColor = snapshot.Output.LetterboxColor };
		const Status upscaled = Pipelines->Upscale->Record(commandList, UpscaleBindings, inputs, counters);
		recording.EndPass(counters);
		if (!upscaled)
		{
			frame.KeepFirstError(upscaled);
			return;
		}
		frame.ViewColor = OutputTargets.Color;
		if (overlays)
		{
			frame.ViewOverlayFramebuffer = OutputTargets.OverlayFramebuffer;
			frame.ViewOverlayConstants = OutputTargets.OverlayViewConstants;
		}
	}

	void SceneRenderer::State::RecordEditorOverlayStage(Detail::SceneRendererFrame& frame)
	{
		// Every editor overlay needs a camera; at a mapping other than the identity the Output stage gave them a target only
		// when one draws (Utils::HasEditorOverlays).
		if (!frame.HasCamera || frame.ViewOverlayFramebuffer == nullptr || frame.ViewOverlayConstants == nullptr)
			return;
		const RenderSnapshot& snapshot = *frame.Snapshot;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		RenderRecordingContext& recording = *frame.Recording;
		const bool identity = frame.Output.IsIdentity();
		nvrhi::IFramebuffer* overlay = frame.ViewOverlayFramebuffer;
		nvrhi::IBuffer* viewConstants = frame.ViewOverlayConstants;

		// The snapshot the overlays are built from, with the camera they are drawn with: at the identity the frame's camera
		// (the snapshot's, at the renderer's size) when the snapshot's viewport differs; otherwise the overlay camera.
		std::optional<RenderSnapshot> adjusted;
		if (!identity)
		{
			adjusted = snapshot;
			adjusted->Camera = Utils::MakeOverlayCamera(frame.Camera, frame.Output);
		}
		else if (!frame.IsDataView
			&& (snapshot.Camera.ViewportWidth != frame.Camera.ViewportWidth || snapshot.Camera.ViewportHeight != frame.Camera.ViewportHeight))
		{
			adjusted = snapshot;
			adjusted->Camera = frame.Camera;
		}
		const RenderSnapshot& overlaySnapshot = adjusted ? *adjusted : snapshot;

		const auto drawLines = [this, &frame, &commandList, &recording, overlay, viewConstants](std::string_view name, const DebugDrawList& lines)
		{
			if (std::all_of(lines.GetCommands().begin(), lines.GetCommands().end(), Utils::IsLabel))
				return;
			recording.BeginPass(name, &commandList);
			RenderPassCounters counters;
			auto result = Pipelines->Debug->RecordCounted(commandList, DebugBindings, { .DebugDraw = &lines, .Framebuffer = overlay, .ViewConstants = viewConstants },
				counters);
			if (result)
				Stats.DebugLineVertices += *result;
			else
				frame.KeepFirstError(std::unexpected(std::move(result).error()));
			recording.EndPass(counters);
		};

		// Recorded before the selection composite and listed after it (decision 1).
		if (frame.Wireframe)
		{
			DebugDrawList edges;
			frame.KeepFirstError(AppendWireframeOverlay(overlaySnapshot, *Assets, edges));
			if (edges.GetDroppedCount() > 0 && !WireframeBudgetLogged)
			{
				WireframeBudgetLogged = true;
				ENGINE_CORE_WARN("The wireframe left out {} triangle edge(s) beyond the debug draw budget of {} commands per view (logged once per view)",
					edges.GetDroppedCount(), DebugDrawList::MaxCommands);
			}
			drawLines("Wireframe", edges);
		}
		if (frame.Selection)
		{
			const SelectionRenderInputs inputs{ .Snapshot = identity ? &snapshot : &overlaySnapshot,
				.SceneDepth = identity ? Targets.SceneDepth.Get() : OutputTargets.Depth.Get(),
				.Mask = identity ? Targets.SelectionMask.Get() : OutputTargets.SelectionMask.Get(),
				.Scratch = identity ? Targets.SelectionScratch.Get() : OutputTargets.SelectionScratch.Get(),
				.LdrColor = frame.ViewColor };
			frame.KeepFirstError(Pipelines->Selection->Record(commandList, recording, SelectionBindings, *Cache, *Assets, inputs));
		}
		// The data views show no ordinary debug primitives (a session's script lines among them), only the view's annotations.
		DebugDrawList overlays;
		if (!frame.IsDataView)
			overlays.Append(snapshot.DebugDraw);
		overlays.Append(snapshot.AnnotationDraw);
		if (!frame.IsDataView)
			frame.KeepFirstError(AppendEditorOverlay(overlaySnapshot, overlays));
		drawLines("Overlays", overlays);

		// The debug and annotation labels, with the Text pipelines, at the view's resolution.
		if (std::any_of(overlays.GetCommands().begin(), overlays.GetCommands().end(), Utils::IsLabel))
		{
			recording.BeginPass("Labels", &commandList);
			RenderPassCounters counters;
			const TextRenderInputs inputs{ .Texts = {},
				.DebugDraw = &overlays,
				.Assets = Assets,
				.Framebuffer = overlay,
				.ViewConstants = viewConstants,
				.HasCamera = true,
				.CameraView = std::optional<glm::mat4>(frame.Camera.View) };
			auto result = Pipelines->Text->RecordCounted(commandList, TextBindings, inputs, counters);
			if (result)
				Stats.TextDraws += *result;
			else
				frame.KeepFirstError(std::unexpected(std::move(result).error()));
			recording.EndPass(counters);
		}

		// Last, at a mapping other than the identity: the letterbox bars get their colour again, which clips every overlay
		// above to the scaled scene rectangle (decision 6; UpscalePass::RecordLetterbox).
		if (identity || !UpscalePass::HasLetterbox(frame.Output))
			return;
		recording.BeginPass("Letterbox", &commandList);
		RenderPassCounters counters;
		const UpscaleLetterboxInputs letterbox{ .Texel = OutputTargets.LetterboxTexel,
			.Color = OutputTargets.ColorFramebuffer,
			.Mapping = frame.Output,
			.LetterboxColor = snapshot.Output.LetterboxColor };
		frame.KeepFirstError(Pipelines->Upscale->RecordLetterbox(commandList, UpscaleBindings, letterbox, counters));
		recording.EndPass(counters);
	}

}
