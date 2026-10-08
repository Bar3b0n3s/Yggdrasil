#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/SoundEffectImporter.h"

#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <array>

namespace Engine {

	Result<Json> SoundEffectToJson(const SoundEffectDescription& /*description*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "writing .sfx documents is not implemented yet (M12 stream B)");
	}

	Result<std::string> SoundEffectToText(const SoundEffectDescription& /*description*/, const TypeRegistry& /*registry*/, JsonStyle /*style*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "writing .sfx documents is not implemented yet (M12 stream B)");
	}

	Result<SoundEffectDescription> SoundEffectFromJson(const Json& /*document*/, const TypeRegistry& /*registry*/, SoundEffectLoadReport& /*report*/,
		bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "reading .sfx documents is not implemented yet (M12 stream B)");
	}

	Result<SoundEffectDescription> SoundEffectFromText(std::string_view /*text*/, const TypeRegistry& /*registry*/, SoundEffectLoadReport& /*report*/,
		bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "reading .sfx documents is not implemented yet (M12 stream B)");
	}

	std::span<const std::string_view> SoundEffectImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".sfx" };
		return Extensions;
	}

	Result<ImportResult> SoundEffectImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "importing sound effects is not implemented yet (M12 stream B)");
	}

	void SoundEffectImporter::RegisterTypes(TypeRegistry& registry)
	{
		// The ranges of Audio/SoundSynth.h; the rules across fields (notes or a positive Duration, LowPass 0 or at least
		// 20 Hz, the total length) are ValidateSoundEffect's.
		registry.Enum<SoundWave>("SoundWave", "The oscillator of a sound effect layer.")
			.Entry(SoundWave::Sine, "Sine", "A pure sine tone.")
			.Entry(SoundWave::Square, "Square", "A square wave with an adjustable duty cycle: hollow, chiptune-like.")
			.Entry(SoundWave::Triangle, "Triangle", "A triangle wave: soft, flute-like.")
			.Entry(SoundWave::Saw, "Saw", "A sawtooth wave: bright and buzzy.")
			.Entry(SoundWave::Noise, "Noise", "Seeded noise, resampled at the frequency: hiss, hits and explosions.");

		registry.Struct<SoundEnvelope>("SoundEnvelope", "The ADSR volume envelope every note of a layer plays with.")
			.Field("Attack", &SoundEnvelope::Attack, "Seconds from silence to full level at the note's start.", { .Min = 0.0, .Max = 10.0, .Unit = "s" })
			.Field("Decay", &SoundEnvelope::Decay, "Seconds from full level down to the sustain level.", { .Min = 0.0, .Max = 10.0, .Unit = "s" })
			.Field("Sustain", &SoundEnvelope::Sustain, "The level held until the note ends, from 0 to 1.", { .Min = 0.0, .Max = 1.0 })
			.Field("Release", &SoundEnvelope::Release, "Seconds from the sustain level to silence after the note ends.",
				{ .Min = 0.0, .Max = 10.0, .Unit = "s" });

		registry.Struct<SoundLayer>("SoundLayer", "One oscillator of a sound effect: a note sequence or a frequency sweep.")
			.Field("Wave", &SoundLayer::Wave, "The oscillator's waveform.")
			.Field("DutyCycle", &SoundLayer::DutyCycle, "Square waves only: the fraction of each period spent high.", { .Min = 0.01, .Max = 0.99 })
			.Field("Notes", &SoundLayer::Notes, "Notes played back to back, such as \"C5:0.06\" (name, octave, seconds) or \"R:0.1\" (a rest).")
			.Field("Duration", &SoundLayer::Duration, "Without notes: the sweep's length in seconds.", { .Min = 0.0, .Max = 10.0, .Unit = "s" })
			.Field("StartFrequency", &SoundLayer::StartFrequency, "Without notes: the sweep's first frequency.", { .Min = 20.0, .Max = 20000.0, .Unit = "Hz" })
			.Field("EndFrequency", &SoundLayer::EndFrequency, "Without notes: the sweep's last frequency (an exponential glide).",
				{ .Min = 20.0, .Max = 20000.0, .Unit = "Hz" })
			.Field("Volume", &SoundLayer::Volume, "The layer's linear volume, from 0 to 1.", { .Min = 0.0, .Max = 1.0 })
			.Field("Envelope", &SoundLayer::Envelope, "The volume envelope of each note.")
			.Field("LowPass", &SoundLayer::LowPass, "The cutoff of a one-pole low-pass filter in Hz; 0 leaves the layer unfiltered.",
				{ .Min = 0.0, .Max = 20000.0, .Unit = "Hz" })
			.Field("Delay", &SoundLayer::Delay, "Seconds from the sound's start until the layer starts.", { .Min = 0.0, .Max = 10.0, .Unit = "s" });

		registry.Struct<SoundEffectDescription>("SoundEffect", "A procedural sound effect (.sfx): layers of oscillators synthesized into a clip.")
			.Field("Seed", &SoundEffectDescription::Seed, "The seed of the noise layers' generators.")
			.Field("Volume", &SoundEffectDescription::Volume, "The sound's linear volume, from 0 to 1.", { .Min = 0.0, .Max = 1.0 })
			.Field("Layers", &SoundEffectDescription::Layers, "The layers, summed.");
	}

}
