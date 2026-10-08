#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/SoundEffectImporter.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/AssetPipeline/Private/ImportErrors.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace Engine {

	namespace Utils {

		// The oldest readable .sfx version (there has been no other yet).
		static constexpr uint32_t MinimumSoundEffectVersion = 1;
		// The JSON pointer of the only layer of a one-layer description (ValidateSoundLayer).
		static constexpr std::string_view SingleLayerPointer = "/Layers/0";
		// The smallest LowPass cutoff other than 0 (SoundSynth.h).
		static constexpr float MinLowPassFrequency = 20.0f;
		// What the Generate hooks draw notes from: valid names, octaves 0 to 8 and short durations, so a generated layer
		// stays far below the length limit.
		static constexpr std::array<std::string_view, 12> GeneratedNoteNames = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
		static constexpr std::array<std::string_view, 4> GeneratedNoteDurations = { "0.05", "0.1", "0.125", "0.25" };

		// The registered "SoundEffect" struct. A registry without it is a programmer error (asserted); the error return
		// keeps builds without asserts safe.
		[[nodiscard]] static Result<const StructInfo*> FindSoundEffectType(const TypeRegistry& registry)
		{
			ENGINE_CORE_ASSERT(registry.IsFrozen(), "Sound effects need a frozen type registry");
			const StructInfo* type = registry.FindStruct<SoundEffectDescription>();
			ENGINE_CORE_ASSERT(type != nullptr, "Sound effects need a registry on which SoundEffectImporter::RegisterTypes ran");
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no SoundEffect struct (SoundEffectImporter::RegisterTypes was not called)");
			return type;
		}

		// Adds the issues of a ValidateSoundEffect result to `context` below its current pointer, each issue's pointer with
		// `prefix` (the part that locates the validated object in the description) removed.
		static void AddSoundEffectIssues(const Status& status, std::string_view prefix, ValidationContext& context)
		{
			if (status.has_value())
				return;
			const Error& error = status.error();
			if (error.GetIssues().empty())
			{
				context.Error({}, error.GetMessageText());
				return;
			}
			for (const ErrorIssue& issue : error.GetIssues())
			{
				std::string_view relative = issue.JsonPointer;
				const bool below = relative.starts_with(prefix) && (relative.size() == prefix.size() || relative[prefix.size()] == '/');
				relative = below ? relative.substr(prefix.size()) : std::string_view();
				ValidationIssue added;
				added.Severity = DiagnosticSeverity::Error;
				added.JsonPointer = context.GetPointer() + std::string(relative);
				added.Message = issue.Message;
				added.Hint = issue.Hint;
				added.Suggestions = issue.Suggestions;
				context.AddIssue(std::move(added));
			}
		}

		// The "SoundLayer" Validate hook: ValidateSoundEffect's rules for one layer (its notes, Duration without notes,
		// LowPass, its length). It runs inside each layer as it is read, so a document reports a bad layer even when another
		// field of the sound is invalid too.
		static void ValidateSoundLayer(const SoundLayer& layer, ValidationContext& context)
		{
			SoundEffectDescription single;
			single.Layers = { layer };
			AddSoundEffectIssues(ValidateSoundEffect(single), SingleLayerPointer, context);
		}

		// The "SoundEffect" Validate hook: ValidateSoundEffect on the whole description. The registry's default object has
		// no layer (SoundEffectDescription's default member initializers), so the registry suite's defaults round-trip
		// passes; a document always gets the "at least one layer" rule from SoundEffectFromJson, which runs
		// ValidateSoundEffect after the read.
		static void ValidateSoundEffectDescription(const SoundEffectDescription& description, ValidationContext& context)
		{
			if (description.Layers.empty())
				return;
			AddSoundEffectIssues(ValidateSoundEffect(description), {}, context);
		}

		// A valid note: a name, an octave and a duration from the tables above, or a rest.
		[[nodiscard]] static std::string DrawNote(Random& random)
		{
			const std::string_view duration = GeneratedNoteDurations[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(GeneratedNoteDurations.size()) - 1))];
			if (random.NextBool(0.125))
				return std::format("R:{}", duration);
			const std::string_view name = GeneratedNoteNames[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(GeneratedNoteNames.size()) - 1))];
			const int64_t octave = random.RangeInt(0, 8);
			return std::format("{}{}:{}", name, octave, duration);
		}

		// The "SoundLayer" Generate hook: random notes become valid notes, a sweep gets a positive length, a LowPass below
		// 20 Hz is raised to 20 Hz, and a layer longer than the limit (random Delay, Duration and Release of up to 10 s
		// each) is shortened.
		static void GenerateSoundLayer(SoundLayer& layer, Random& random)
		{
			if (layer.Notes.size() > MaxSoundNotesPerLayer)
				layer.Notes.resize(MaxSoundNotesPerLayer);
			for (std::string& note : layer.Notes)
				note = DrawNote(random);
			if (layer.Notes.empty() && !(layer.Duration > 0.0f))
				layer.Duration = 0.1f;
			if (layer.LowPass > 0.0f && layer.LowPass < MinLowPassFrequency)
				layer.LowPass = MinLowPassFrequency;

			SoundEffectDescription single;
			single.Layers = { layer };
			const uint64_t frames = ComputeSoundEffectFrameCount(single);
			if (frames == 0 || frames > MaxSoundEffectFrames)
			{
				layer.Delay = 0.0f;
				layer.Envelope.Release = std::min(layer.Envelope.Release, 1.0f);
				if (layer.Notes.empty())
					layer.Duration = std::clamp(layer.Duration, 0.1f, 8.0f);
			}
		}

		// The "SoundEffect" Generate hook: at most MaxSoundLayers layers (each already repaired by its own hook).
		static void GenerateSoundEffect(SoundEffectDescription& description, Random& /*random*/)
		{
			if (description.Layers.size() > MaxSoundLayers)
				description.Layers.resize(MaxSoundLayers);
		}

		[[nodiscard]] static Result<SoundEffectDescription> ReadSoundEffectDocument(const Json& document, const TypeRegistry& registry,
			SoundEffectLoadReport& report, bool strictUnknowns)
		{
			ENGINE_TRY_ASSIGN(const StructInfo* type, FindSoundEffectType(registry));
			const JsonReader root(document);
			ENGINE_TRY_ASSIGN(report.FileVersion, root.ReadFormatHeader(SoundEffectFormatName, MinimumSoundEffectVersion, SoundEffectFormatVersion));

			// Everything below the header is the SoundEffect object; the header check above guarantees an object.
			Json fields = document;
			fields.erase("Format");
			fields.erase("Version");

			SoundEffectDescription description;
			ReadContext context;
			context.Strict = strictUnknowns;
			context.Diagnostics = &report.Diagnostics;
			ENGINE_TRY(type->FromJson(&description, JsonReader(fields), context));
			ENGINE_TRY(ValidateSoundEffect(description));
			return description;
		}

	}

	Result<Json> SoundEffectToJson(const SoundEffectDescription& description, const TypeRegistry& registry)
	{
		ENGINE_TRY_ASSIGN(const StructInfo* type, Utils::FindSoundEffectType(registry));
		ENGINE_TRY_ASSIGN(Json fields, type->ToJson(&description));

		// The header first, then the fields in registry order (§6).
		Json document = Json::object();
		document["Format"] = std::string(SoundEffectFormatName);
		document["Version"] = SoundEffectFormatVersion;
		for (auto field = fields.begin(); field != fields.end(); ++field)
			document[field.key()] = std::move(*field);
		return document;
	}

	Result<std::string> SoundEffectToText(const SoundEffectDescription& description, const TypeRegistry& registry, JsonStyle style)
	{
		ENGINE_TRY_ASSIGN(const Json document, SoundEffectToJson(description, registry));
		return JsonWriter::Write(document, style);
	}

	Result<SoundEffectDescription> SoundEffectFromJson(const Json& document, const TypeRegistry& registry, SoundEffectLoadReport& report,
		bool strictUnknowns)
	{
		report = SoundEffectLoadReport{};
		return Utils::ReadSoundEffectDocument(document, registry, report, strictUnknowns);
	}

	Result<SoundEffectDescription> SoundEffectFromText(std::string_view text, const TypeRegistry& registry, SoundEffectLoadReport& report,
		bool strictUnknowns)
	{
		report = SoundEffectLoadReport{};
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		return SoundEffectFromJson(document, registry, report, strictUnknowns);
	}

	std::span<const std::string_view> SoundEffectImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".sfx" };
		return Extensions;
	}

	Result<ImportResult> SoundEffectImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();
		const std::string importing = std::format("while importing sound effect '{}'", sourcePath);

		// Strict: an unknown member is an error at import, so a misspelled key never silently keeps its default.
		SoundEffectLoadReport report;
		Result<SoundEffectDescription> description = SoundEffectFromText(AsStringView(context.GetSourceBytes()), context.GetRegistry(), report, true);
		if (!description.has_value())
			return std::unexpected(Utils::MakeImportFailed(description.error(), sourcePath, importing));
		Result<std::vector<int16_t>> samples = SynthesizeSoundEffect(*description);
		if (!samples.has_value())
			return std::unexpected(Utils::MakeImportFailed(samples.error(), sourcePath, importing));

		AudioClipData clip;
		clip.Encoding = AudioClipEncoding::Pcm16;
		clip.SampleRate = SoundSynthSampleRate;
		clip.ChannelCount = 1;
		clip.FrameCount = samples->size();
		clip.Stream = false;
		BinaryWriter writer;
		for (const int16_t sample : *samples)
			writer.WriteI16(sample);
		clip.Bytes = writer.TakeBuffer();
		ENGINE_TRY(WithContext(ValidateAudioClipData(clip), importing));

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::AudioClip, .SubAssetKey = {}, .Cooked = CookAudioClip(clip, Version) });
		return result;
	}

	void SoundEffectImporter::RegisterTypes(TypeRegistry& registry)
	{
		// The ranges of Audio/SoundSynth.h; the rules across fields (notes or a positive Duration, LowPass 0 or at least
		// 20 Hz, the length of a layer and the number of layers) are ValidateSoundEffect's, run as Validate hooks: per layer,
		// and on the whole sound.
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
			.Field("Delay", &SoundLayer::Delay, "Seconds from the sound's start until the layer starts.", { .Min = 0.0, .Max = 10.0, .Unit = "s" })
			.Validate(&Utils::ValidateSoundLayer)
			.Generate(&Utils::GenerateSoundLayer);

		registry.Struct<SoundEffectDescription>("SoundEffect", "A procedural sound effect (.sfx): layers of oscillators synthesized into a clip.")
			.Field("Seed", &SoundEffectDescription::Seed, "The seed of the noise layers' generators.")
			.Field("Volume", &SoundEffectDescription::Volume, "The sound's linear volume, from 0 to 1.", { .Min = 0.0, .Max = 1.0 })
			.Field("Layers", &SoundEffectDescription::Layers, "The layers, summed.")
			.Validate(&Utils::ValidateSoundEffectDescription)
			.Generate(&Utils::GenerateSoundEffect);
	}

}
