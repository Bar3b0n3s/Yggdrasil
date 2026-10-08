#include "EnginePCH.h"
#include "Engine/Audio/SoundSynth.h"

#include "Engine/Core/Assert.h"

#include <utility>

namespace Engine {

	Result<SoundNote> ParseSoundNote(std::string_view /*note*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "sound effect notes are not implemented yet (M12 stream B)");
	}

	Status ValidateSoundEffect(const SoundEffectDescription& /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "sound effect validation is not implemented yet (M12 stream B)");
	}

	uint64_t ComputeSoundEffectFrameCount(const SoundEffectDescription& /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	double ComputeSoundEffectDuration(const SoundEffectDescription& /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0.0;
	}

	Result<std::vector<int16_t>> SynthesizeSoundEffect(const SoundEffectDescription& /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "sound synthesis is not implemented yet (M12 stream B)");
	}

	std::string_view SoundWaveToString(SoundWave wave)
	{
		switch (wave)
		{
			case SoundWave::Sine:     return "Sine";
			case SoundWave::Square:   return "Square";
			case SoundWave::Triangle: return "Triangle";
			case SoundWave::Saw:      return "Saw";
			case SoundWave::Noise:    return "Noise";
		}

		ENGINE_CORE_ASSERT(false, "Unknown SoundWave {}", std::to_underlying(wave));
		return "Unknown";
	}

}
