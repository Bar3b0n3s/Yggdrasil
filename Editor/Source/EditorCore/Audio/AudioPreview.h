#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>

// The asset browser's audio preview (Architecture §10.2: "Edit mode is silent except asset-browser previews"; §12.2
// ContentBrowserPanel): the EditorCore-level API that M10's panel calls to audition an AudioClip asset (an imported WAV,
// FLAC, MP3 or Ogg Vorbis file, or a .sfx sound effect). EditorContext owns one when its engine context has an AudioEngine
// (EditorContext::GetAudioPreview). Frozen by the M12 contract (Docs/Decisions/0015-m12-decisions.md).

namespace Engine {

	class AssetManager;
	class AudioEngine;

	// The stealing priority of a preview voice (AudioVoiceDescription::Priority): above every voice AudioSystem starts
	// (AudioSystemMusicPriority), so a busy play session never cuts off what the user is auditioning.
	inline constexpr int32_t AudioPreviewPriority = 2;

	// One preview at a time: Play replaces the current one. The preview plays the clip once, non-spatial, at full volume in
	// the Ui group, with AudioPreviewPriority, whatever the play state (edit mode, a play session, a lockstep session whose
	// ticks then advance it, §10.1). Main thread only; not copyable or movable.
	class AudioPreview
	{
	public:
		// `audio` and `assets` are documented back-references that outlive the preview (the engine context's AudioEngine and
		// the editor's EditorAssetManager).
		AudioPreview(AudioEngine& audio, AssetManager& assets);
		// Stops the preview and unregisters its clip.
		~AudioPreview();

		AudioPreview(const AudioPreview&) = delete;
		AudioPreview& operator=(const AudioPreview&) = delete;

		// Stops the current preview, loads `clip` (AssetManager::Load; never the silent placeholder: a missing or broken clip
		// is an error the panel shows), registers it with the engine and plays it. Errors: InvalidArgument for a null handle;
		// the asset manager's errors (NotFound, ImportFailed, ...); Validation naming the type for an asset that is not an
		// AudioClip; the engine's errors.
		[[nodiscard]] Status Play(AssetHandle clip);

		// Stops the current preview; nothing when none plays.
		void Stop();

		// True from Play until the clip ends or Stop.
		[[nodiscard]] bool IsPlaying() const;
		// The clip being previewed; the null handle when none plays.
		[[nodiscard]] AssetHandle GetClip() const;

		// Once per frame (EditorContext::Update): forgets a preview whose voice ended and unregisters its clip.
		void Update();
	private:
		// The back-references, the voice, the clip handle and its engine registration (AudioPreview.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
