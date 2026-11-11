// Does the audio sound right? The gate behind --family-test audioquality.
//
// Nobody can listen inside a test, so this gate compares what the engine produces with what an
// independent decoder (FFmpeg through audio/AudioReference.h) produces for the same shipped
// bytes, and measures the mix for the signatures of noise: broken decodes (low correlation with
// the reference), steps and clipping in the final buffer, and a flat spectrum where the source
// is tonal. A pass proves the signal matches the reference within the stated thresholds; it
// does not replace listening.
//
// Set KE_AUDIO_DIR to a folder to also write the decodes and the session captures as WAV files.
#include "audio/Audio.h"
#include "audio/AudioReference.h"
#include "core/Log.h"
#include "game/Ambience.h"
#include "game/AudioSystem.h"
#include "game/GameSession.h"
#include "game/ScriptHost.h"
#include "kotor/Formats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ke {

namespace {

namespace fs = std::filesystem;

std::string lowerText(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string num(double value, int digits = 4)
{
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(digits);
    stream << value;
    return stream.str();
}

std::vector<std::uint8_t> readBytes(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Every resource name the area audio can ask for: ambientsound.2da, ambientmusic.2da (tracks
// and stingers) and the sounds of every UTS template in every module.
std::set<std::string> referencedNames(GameSession& game, std::map<std::string, std::string>& origin)
{
    std::set<std::string> names;
    const auto add = [&](const std::string& raw, const std::string& why) {
        const std::string name = lowerText(raw);
        if (!name.empty() && name != "****") {
            names.insert(name);
            origin.emplace(name, why);
        }
    };
    kotor::Install& install = game.install();
    if (const auto table = install.table("ambientsound")) {
        const int column = table->columnIndex("resource");
        for (std::size_t row = 0; column >= 0 && row < table->rowCount(); ++row) {
            add(table->cell(row, static_cast<std::size_t>(column)), "ambientsound.2da");
        }
    }
    if (const auto table = install.table("ambientmusic")) {
        for (const char* column : {"resource", "stinger1", "stinger2", "stinger3"}) {
            const int index = table->columnIndex(column);
            if (index < 0) {
                continue;
            }
            for (std::size_t row = 0; row < table->rowCount(); ++row) {
                add(table->cell(row, static_cast<std::size_t>(index)), "ambientmusic.2da");
            }
        }
    }
    // Templates live in the modules' archives: bind each on a private Install.
    kotor::Install scratch(game.installPath(), false);
    std::set<std::string> stems;
    for (const auto& entry : fs::directory_iterator(fs::path(game.installPath()) / "modules")) {
        if (entry.path().extension() == ".rim") {
            std::string stem = entry.path().stem().string();
            if (stem.size() > 2 && stem.compare(stem.size() - 2, 2, "_s") == 0) {
                stem.resize(stem.size() - 2);
            }
            stems.insert(stem);
        }
    }
    std::set<std::string> templates;
    for (const std::string& stem : stems) {
        try {
            scratch.bindModule(stem);
        } catch (const std::exception&) {
            continue;
        }
        for (const std::string& name : scratch.resourceNames(2035)) {
            if (!templates.insert(lowerText(name)).second) {
                continue;
            }
            auto data = scratch.find(name, 2035);
            if (!data.has_value()) {
                continue;
            }
            try {
                const kotor::Gff gff = kotor::Gff::parse(std::move(*data), name + ".uts");
                if (const kotor::GffField* sounds = gff.field(gff.rootIndex(), "Sounds")) {
                    for (const std::uint32_t item : gff.list(*sounds)) {
                        if (const kotor::GffField* sound = gff.field(item, "Sound")) {
                            add(gff.text(*sound), "uts");
                        }
                    }
                }
            } catch (const kotor::FormatError&) {
            }
        }
    }
    return names;
}

struct ResourceFiles {
    std::map<std::string, fs::path> loose;

    explicit ResourceFiles(const std::string& installPath)
    {
        // Same order as AudioSystem::indexLooseAudio.
        for (const char* directory : {"streammusic", "streamsounds", "streamwaves"}) {
            std::error_code error;
            for (fs::recursive_directory_iterator it(fs::path(installPath) / directory, error), end;
                 !error && it != end; it.increment(error)) {
                if (it->is_regular_file(error)) {
                    loose.emplace(lowerText(it->path().stem().string()), it->path());
                }
            }
        }
    }

    std::vector<std::uint8_t> bytes(kotor::Install& install, const std::string& name) const
    {
        if (const auto it = loose.find(name); it != loose.end()) {
            return readBytes(it->second);
        }
        if (auto data = install.find(name, 4); data.has_value()) {
            return std::move(*data);
        }
        return {};
    }
};

// What the mixer does to a decoded sound: linear resample to 48 kHz stereo (mono is copied to
// both channels). Written out here from the Mixer's rule so the expectation is independent of
// audio::resample.
std::vector<float> expectedStereo(const audio::SoundData& reference, double& frames48)
{
    const std::size_t inFrames = reference.frameCount();
    const double ratio = 48000.0 / static_cast<double>(reference.sampleRate);
    const std::size_t outFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(static_cast<double>(inFrames) * ratio)));
    frames48 = static_cast<double>(outFrames);
    std::vector<float> out(outFrames * 2);
    const std::size_t channels = static_cast<std::size_t>(reference.channels);
    for (std::size_t frame = 0; frame < outFrames; ++frame) {
        const double source = static_cast<double>(frame) / ratio;
        const std::size_t lo = std::min(static_cast<std::size_t>(std::floor(source)), inFrames - 1);
        const std::size_t hi = std::min(lo + 1, inFrames - 1);
        const float t = static_cast<float>(source - std::floor(source));
        for (std::size_t c = 0; c < 2; ++c) {
            const std::size_t sc = std::min(c, channels - 1);
            const float a = reference.samples[lo * channels + sc];
            const float b = reference.samples[hi * channels + sc];
            out[frame * 2 + c] = a + (b - a) * t;
        }
    }
    return out;
}

std::vector<float> channelOf(const std::vector<float>& stereo, std::size_t channel)
{
    std::vector<float> out(stereo.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = stereo[i * 2 + channel];
    }
    return out;
}

} // namespace

int runAudioQualityFamilyTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("audioquality: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const auto environment = [](const char* name) {
        std::string text;
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
            text = value;
            std::free(value);
        }
        return text;
    };
    const std::string dumpDir = environment("KE_AUDIO_DIR");
    // Proof run: with KE_AUDIO_LEGACY_IMA=1 the IMA ADPCM decoder is put back to its pre-fix
    // behaviour and this gate is expected to FAIL.
    const bool legacyRun = environment("KE_AUDIO_LEGACY_IMA") == "1";
    audio::setLegacyImaDecodeForTest(legacyRun);
    if (legacyRun) {
        logInfo("audioquality: LEGACY IMA decoder forced on (this run is expected to fail)");
    }
    if (!dumpDir.empty()) {
        std::error_code ignored;
        fs::create_directories(dumpDir, ignored);
    }
    check(audio::referenceAvailable(), "reference_decoder_available");
    if (!audio::referenceAvailable()) {
        return 31;
    }

    // ---- A. every referenced resource decodes to what FFmpeg decodes -----------------------
    std::map<std::string, std::string> origin;
    const std::set<std::string> names = referencedNames(game, origin);
    ResourceFiles files(game.installPath());
    kotor::Install& install = game.install();
    struct Bucket {
        int count = 0;
        int bad = 0;
        double minCorr = 2.0;
        std::string worst;
        std::vector<std::string> badNames;
    };
    std::map<std::string, Bucket> buckets;
    int missing = 0;
    int decoded = 0;
    int badTotal = 0;
    std::vector<std::string> badList;
    // Frames the engine and FFmpeg may differ by: MP3 encoder delay and padding.
    constexpr int kMaxLag = 2600;
    // The whole install, not only what areas reference: every sound the BIFs and the loose stream
    // folders hold, and every 20th voice-over file (13,799 of them share one layout).
    std::vector<std::string> work(names.begin(), names.end());
    const std::size_t referencedCount = work.size();
    {
        std::set<std::string> seen(names.begin(), names.end());
        for (const std::string& raw : install.resourceNames(4)) {
            const std::string name = lowerText(raw);
            if (seen.insert(name).second) {
                work.push_back(name);
            }
        }
        int voiceIndex = 0;
        for (const auto& [name, path] : files.loose) {
            const bool voice = path.generic_string().find("streamwaves") != std::string::npos;
            if ((!voice || (voiceIndex++ % 20) == 0) && seen.insert(name).second) {
                work.push_back(name);
            }
        }
    }
    for (const std::string& name : work) {
        const std::vector<std::uint8_t> bytes = files.bytes(install, name);
        if (bytes.empty()) {
            ++missing;
            continue;
        }
        audio::SoundData reference;
        audio::AudioLayout layout;
        std::string error;
        if (!audio::referenceDecode(bytes, reference, error, &layout)) {
            logInfo(concat("audioquality: reference failed for ", name, ": ", error));
            ++buckets["reference-failed"].count;
            ++buckets["reference-failed"].bad;
            ++badTotal;
            badList.push_back(name + "(reference)");
            continue;
        }
        audio::SoundData engine;
        std::string engineError;
        Bucket& bucket = buckets[layout.description + "/" + std::to_string(reference.channels) + "ch"];
        ++bucket.count;
        ++decoded;
        bool good = game.audio().decodeProbe(name, engine, engineError);
        double corr = 0.0;
        if (good) {
            good = engine.sampleRate == reference.sampleRate && engine.channels == reference.channels;
            const std::vector<float> a = audio::mono(reference);
            const std::vector<float> b = audio::mono(engine);
            const audio::Correlation c = audio::correlate(a, b, layout.mp3 ? kMaxLag : 8, 480000);
            corr = c.value;
            const double frameRatio = a.empty() ? 0.0 : static_cast<double>(b.size()) / static_cast<double>(a.size());
            good = good && corr >= (layout.mp3 ? 0.99 : 0.999) && frameRatio > 0.97 && frameRatio < 1.03;
            if (!dumpDir.empty() && (name == "al_en_cantinabed" || name == "al_el_compbed_01"
                                      || name == "mus_area_acad" || name == "al_en_rmmtarlrg")) {
                audio::writeWav16(dumpDir + "/decode_engine_" + name + ".wav", engine.samples, engine.channels, engine.sampleRate);
                audio::writeWav16(dumpDir + "/decode_reference_" + name + ".wav", reference.samples, reference.channels, reference.sampleRate);
            }
        }
        bucket.minCorr = std::min(bucket.minCorr, corr);
        if (!good) {
            ++bucket.bad;
            ++badTotal;
            if (bucket.worst.empty() || corr < bucket.minCorr + 1.0e-12) {
                bucket.worst = name;
            }
            if (bucket.badNames.size() < 6) {
                bucket.badNames.push_back(name + "(" + num(corr, 3) + ")");
            }
            badList.push_back(name);
        }
    }
    logInfo(concat("audioquality: referenced=", referencedCount, " swept=", work.size(), " decoded=", decoded, " missing=", missing,
                   " mismatching=", badTotal));
    for (const auto& [label, bucket] : buckets) {
        std::string listing;
        for (const std::string& item : bucket.badNames) {
            listing += " " + item;
        }
        logInfo(concat("audioquality: class ", label, " count=", bucket.count, " bad=", bucket.bad,
                       " minCorr=", num(bucket.minCorr, 4), listing));
    }
    check(referencedCount >= 600 && decoded >= 2500, "inventory_covers_the_referenced_resources_and_the_install_sweep");
    check(badTotal == 0, "every_resource_decodes_like_the_reference");

    // ---- B. a looped ambient voice through the mixer matches the expected signal ----------------
    // One resource of each shipped format class; gain and pan are applied to the reference.
    const char* probes[] = {"al_en_cantinabed", "al_el_compbed_01", "al_en_rmmtarlrg", "as_an_dantext_01"};
    for (const char* probe : probes) {
        const std::string name = probe;
        audio::SoundData reference;
        const std::vector<std::uint8_t> bytes = files.bytes(install, name);
        std::string error;
        if (bytes.empty() || !audio::referenceDecode(bytes, reference, error)) {
            check(false, "probe_" + name + "_reference");
            continue;
        }
        audio::SoundData engine;
        std::string engineError;
        if (!game.audio().decodeProbe(name, engine, engineError) || !audio::resample(engine, 48000, 2)) {
            check(false, "probe_" + name + "_engine_decode");
            continue;
        }
        double frames48 = 0.0;
        const std::vector<float> expected = expectedStereo(reference, frames48);
        auto data = std::make_shared<audio::SoundData>(engine);
        audio::Mixer mixer;
        constexpr float kVolume = 0.5f;
        constexpr float kMaster = 1.0f; // Mixer::Impl::master (retail has none after the buses)
        const float panGain = 1.0f;     // a centred 2D voice plays at the full level in both channels
        mixer.play(data, audio::Bus::Ambient, kVolume, true, false);
        const std::size_t total = static_cast<std::size_t>(frames48) + 9600; // runs past the loop point
        std::vector<float> mixed;
        mixed.reserve(total * 2);
        std::vector<float> block;
        const int blockSizes[] = {1024, 701, 480, 1600};
        for (std::size_t block_i = 0; mixed.size() < total * 2; ++block_i) {
            const int frames = blockSizes[block_i % 4];
            block.assign(static_cast<std::size_t>(frames) * 2, 0.0f);
            mixer.mix(block.data(), frames);
            mixed.insert(mixed.end(), block.begin(), block.end());
        }
        mixed.resize(total * 2);
        // The expected signal, looped, with the same gain staging.
        std::vector<float> looped(total * 2);
        const std::size_t period = static_cast<std::size_t>(frames48);
        for (std::size_t frame = 0; frame < total; ++frame) {
            for (std::size_t c = 0; c < 2; ++c) {
                looped[frame * 2 + c] = expected[(frame % period) * 2 + c] * kVolume * panGain * kMaster;
            }
        }
        const audio::Correlation left = audio::correlate(channelOf(looped, 0), channelOf(mixed, 0), 0, 480000);
        const audio::Correlation right = audio::correlate(channelOf(looped, 1), channelOf(mixed, 1), 0, 480000);
        const audio::StepStats mixedSteps = audio::stepStats(mixed, 0.0f);
        const audio::StepStats expectedSteps = audio::stepStats(looped, 0.0f);
        double mixedEnergy = 0.0;
        double expectedEnergy = 0.0;
        for (std::size_t i = 0; i < mixed.size(); ++i) {
            mixedEnergy += static_cast<double>(mixed[i]) * mixed[i];
            expectedEnergy += static_cast<double>(looped[i]) * looped[i];
        }
        const double gainRatio = expectedEnergy > 0.0 ? std::sqrt(mixedEnergy / expectedEnergy) : 0.0;
        const std::vector<float> mixedMono = channelOf(mixed, 0);
        const std::vector<float> expectedMono = channelOf(looped, 0);
        const double flatMixed = audio::spectralFlatness(mixedMono, 48000, 100.0, 8000.0);
        const double flatExpected = audio::spectralFlatness(expectedMono, 48000, 100.0, 8000.0);
        logInfo(concat("audioquality: mixer ", name, " corrL=", num(left.value, 5), " corrR=", num(right.value, 5),
                       " gainRatio=", num(gainRatio, 4), " maxStep=", num(mixedSteps.maxStep, 4), "/",
                       num(expectedSteps.maxStep, 4), " flatness=", num(flatMixed, 4), "/", num(flatExpected, 4)));
        check(left.value >= 0.999 && right.value >= 0.999, "mixer_" + name + "_output_correlates_with_the_reference");
        check(std::fabs(gainRatio - 1.0) < 0.02, "mixer_" + name + "_gain_staging_is_exact");
        check(mixedSteps.maxStep <= expectedSteps.maxStep * 1.05f + 1.0e-4f, "mixer_" + name + "_no_extra_discontinuities_across_the_loop");
        check(mixedSteps.nonFinite == 0, "mixer_" + name + "_no_nan");
        check(std::fabs(flatMixed - flatExpected) <= 0.02 + 0.1 * flatExpected, "mixer_" + name + "_spectral_flatness_matches_the_reference");
        if (!dumpDir.empty()) {
            audio::writeWav16(dumpDir + "/mixer_" + name + "_engine.wav", mixed, 2, 48000);
            audio::writeWav16(dumpDir + "/mixer_" + name + "_expected.wav", looped, 2, 48000);
        }
    }

    // ---- C. real sessions: the buses carry the source signal and the final mix is clean ----------
    // Each module is entered through the game's own loader, ticked for a few seconds (the music and
    // ambient fades finish), then captured for 12 s. The music and ambient buses are read back and
    // compared with the looping source at the voice's own cursor, gain and the equal-power pan; the
    // final mix is checked for NaN, clipping and steps.
    const auto readWav16 = [](const std::string& path) {
        std::vector<float> samples;
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return samples;
        }
        const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (raw.size() <= 44) {
            return samples;
        }
        const std::size_t count = (raw.size() - 44) / 2;
        samples.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::int16_t value = 0;
            std::memcpy(&value, raw.data() + 44 + i * 2, 2);
            samples[i] = static_cast<float>(value) / 32768.0f;
        }
        return samples;
    };
    const std::string captureDir = !dumpDir.empty()
        ? dumpDir
        : (fs::temp_directory_path() / "ke_audioquality").string();
    {
        std::error_code ignored;
        fs::create_directories(captureDir, ignored);
    }
    constexpr int kWarmTicks = 90;     // 3 s: the one-second fades are done
    constexpr int kCaptureSeconds = 12;
    // Each module must hold one area track for the whole capture. tar_m02aa no longer can: its opening
    // conversation now starts on entry and its node scripts change the music, as in retail, so the
    // Taris session is the next street.
    for (const std::string module : {"end_m01aa", "tar_m03aa", "danm13"}) {
        if (!game.enterMiniGameModule(module)) {
            logInfo(concat("audioquality: could not enter ", module));
            check(false, "session_" + module + "_enters");
            continue;
        }
        // A module's entry movie mutes the world buses until it ends; the session under test is the world.
        game.stopMovie();
        AudioSystem& audioSystem = game.audio();
        audio::Mixer& mixer = audioSystem.mixer();
        std::vector<float> block(1600 * 2);
        for (int tick = 0; tick < kWarmTicks; ++tick) {
            game.update(1.0f / 30.0f, InputState{});
            mixer.mix(block.data(), 1600);
        }
        AreaAmbience& amb = audioSystem.ambience();
        // The world's own events may still be changing the music (end_m01aa: the opening duel starts the battle
        // track and its stinger, then the area track returns). The bus is compared against one voice, so run on
        // until the music bus has held one voice, the same one, for a second; the checks themselves are unchanged.
        {
            constexpr int kSettleTicks = 30;
            constexpr int kSettleLimit = 30 * 60;
            int stable = 0;
            int lastVoice = 0;
            for (int tick = 0; tick < kSettleLimit && stable < kSettleTicks; ++tick) {
                game.update(1.0f / 30.0f, InputState{});
                mixer.mix(block.data(), 1600);
                const int voice = amb.musicVoice();
                stable = (mixer.activeVoices(audio::Bus::Music) <= 1 && voice == lastVoice) ? stable + 1 : 0;
                lastVoice = voice;
            }
        }
        struct Tracked {
            int voice = 0;
            std::string resource;
            double cursor0 = 0.0;
            float volume = 0.0f;
        };
        const Tracked tracked[2] = {
            {amb.musicVoice(), amb.musicResource(), mixer.voiceCursor(amb.musicVoice()), mixer.voiceVolume(amb.musicVoice())},
            {amb.ambientVoice(), amb.ambientResource(), mixer.voiceCursor(amb.ambientVoice()), mixer.voiceVolume(amb.ambientVoice())},
        };
        const std::string prefix = captureDir + "/session_" + module;
        mixer.startCapture(prefix);
        std::vector<float> mix;
        for (int tick = 0; tick < kCaptureSeconds * 30; ++tick) {
            game.update(1.0f / 30.0f, InputState{});
            mixer.mix(block.data(), 1600);
            mix.insert(mix.end(), block.begin(), block.end());
        }
        mixer.stopCapture();
        logInfo(concat("audioquality: ", module, " movieMode=", audioSystem.movieMode() ? 1 : 0,
                       " voices music=", mixer.activeVoices(audio::Bus::Music), " ambient=",
                       mixer.activeVoices(audio::Bus::Ambient), " effects=", mixer.activeVoices(audio::Bus::Effects),
                       " music=", tracked[0].resource, " ambient=", tracked[1].resource));
        const audio::StepStats stats = audio::stepStats(mix, 0.5f);
        double energy = 0.0;
        for (const float v : mix) {
            energy += static_cast<double>(v) * v;
        }
        const double rms = std::sqrt(energy / static_cast<double>(std::max<std::size_t>(1, mix.size())));
        const double flat = audio::spectralFlatness(channelOf(mix, 0), 48000, 100.0, 8000.0);
        logInfo(concat("audioquality: session ", module, " samples=", stats.samples, " rms=", num(rms, 4),
                       " maxStep=", num(stats.maxStep, 4), " stepsOver0.5=", stats.over, " clipped=", stats.clipped,
                       " nonFinite=", stats.nonFinite, " flatness=", num(flat, 4)));
        check(stats.nonFinite == 0, "session_" + module + "_no_nan");
        check(stats.clipped * 1000 <= stats.samples, "session_" + module + "_no_clipping");
        check(stats.over <= 4, "session_" + module + "_no_discontinuities");
        check(rms > 0.0005, "session_" + module + "_is_not_silent");

        const char* const busFiles[2] = {"music", "ambient"};
        for (int which = 0; which < 2; ++which) {
            const Tracked& t = tracked[which];
            if (t.voice == 0 || t.resource.empty() || audioSystem.sound(t.resource) == nullptr) {
                continue;
            }
            const audio::SoundData& source = *audioSystem.sound(t.resource);
            const std::vector<float> bus = readWav16(prefix + "_" + busFiles[which] + ".wav");
            const std::size_t frames = bus.size() / 2;
            const std::size_t sourceFrames = source.frameCount();
            if (frames == 0 || sourceFrames == 0) {
                check(false, std::string("session_") + module + "_" + busFiles[which] + "_bus_captured");
                continue;
            }
            // Expected: the source at the voice's cursor, the voice volume, the bus volume and the centre pan.
            const double start = t.cursor0 * 48000.0;
            std::vector<float> expected(frames * 2);
            const float gain = t.volume
                * mixer.busVolume(which == 0 ? audio::Bus::Music : audio::Bus::Ambient);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const std::size_t index = static_cast<std::size_t>(std::fmod(start + static_cast<double>(frame),
                                                                             static_cast<double>(sourceFrames)));
                for (std::size_t c = 0; c < 2; ++c) {
                    const std::size_t channel = std::min<std::size_t>(c, static_cast<std::size_t>(source.channels) - 1);
                    expected[frame * 2 + c] = source.samples[index * static_cast<std::size_t>(source.channels) + channel] * gain;
                }
            }
            const double corr = audio::correlate(channelOf(expected, 0), channelOf(bus, 0), 0, 480000).value;
            double eb = 0.0;
            double ee = 0.0;
            for (std::size_t i = 0; i < bus.size(); ++i) {
                eb += static_cast<double>(bus[i]) * bus[i];
                ee += static_cast<double>(expected[i]) * expected[i];
            }
            const double gainRatio = ee > 0.0 ? std::sqrt(eb / ee) : 0.0;
            const double flatBus = audio::spectralFlatness(channelOf(bus, 0), 48000, 100.0, 8000.0);
            const double flatSource = audio::spectralFlatness(channelOf(expected, 0), 48000, 100.0, 8000.0);
            const float stepBus = audio::stepStats(bus, 1.0f).maxStep;
            const float stepSource = audio::stepStats(expected, 1.0f).maxStep;
            logInfo(concat("audioquality: ", module, " ", busFiles[which], " bus ", t.resource, " vol=", num(t.volume, 3),
                           " corr=", num(corr, 5), " gainRatio=", num(gainRatio, 4), " flatness=", num(flatBus, 4), "/",
                           num(flatSource, 4), " maxStep=", num(stepBus, 4), "/", num(stepSource, 4)));
            const std::string label = std::string("session_") + module + "_" + busFiles[which] + "_bus_";
            check(corr >= 0.999, label + "correlates_with_the_source");
            check(std::fabs(gainRatio - 1.0) < 0.03, label + "gain_matches_voice_volume");
            check(std::fabs(flatBus - flatSource) <= 0.02 + 0.1 * flatSource, label + "spectral_flatness_matches_the_source");
            check(stepBus <= stepSource * 1.05f + 1.0e-3f, label + "no_extra_discontinuities");
        }
    }

    // ---- D. effects and bus: the reverb stays stable on real input, and the mixer survives a game thread ----
    {
        const std::string name = "al_en_rmmtarlrg";
        audio::SoundData decodedProbe;
        std::string error;
        if (!game.audio().decodeProbe(name, decodedProbe, error) || !audio::resample(decodedProbe, 48000, 2)) {
            check(false, "reverb_probe_decodes");
        } else {
            auto data = std::make_shared<audio::SoundData>(std::move(decodedProbe));
            const int environments = static_cast<int>(audio::reverbLabels().size());
            const auto run = [&](bool reverb, std::vector<float>& out, std::size_t& denormals) {
                audio::Mixer mixer;
                mixer.setListener({0.0f, 0.0f, 0.0f}, 0.0f);
                const int voice = mixer.play(data, audio::Bus::Effects, 0.8f, true, true, {1.0f, 0.0f, 0.0f}, 1.0f, 10.0f);
                std::vector<float> block(1024 * 2);
                const auto pump = [&](int frames) {
                    for (int done = 0; done < frames; done += 1024) {
                        mixer.mix(block.data(), 1024);
                        out.insert(out.end(), block.begin(), block.end());
                    }
                };
                if (reverb) {
                    // Every shipped EAX room in turn, each change a crossfade, every half second.
                    for (int env = 0; env < environments; ++env) {
                        mixer.setReverbEnvironment(env, true);
                        pump(24000);
                    }
                } else {
                    pump(24000 * environments);
                }
                // The source stops: the tail must die away, not ring or grow.
                mixer.stopVoice(voice);
                pump(48000 * 20);
                denormals = mixer.reverbDenormalCount();
            };
            std::vector<float> wet;
            std::vector<float> dry;
            std::size_t denormals = 0;
            std::size_t denormalsDry = 0;
            run(true, wet, denormals);
            run(false, dry, denormalsDry);
            const audio::StepStats wetStats = audio::stepStats(wet, 0.5f);
            const std::size_t tail = 48000 * 2; // last second, interleaved stereo
            double tailEnergy = 0.0;
            double wetEnergy = 0.0;
            double dryEnergy = 0.0;
            for (std::size_t i = wet.size() - tail; i < wet.size(); ++i) {
                tailEnergy += static_cast<double>(wet[i]) * wet[i];
            }
            const std::size_t playing = 24000u * 2u * static_cast<std::size_t>(environments);
            for (std::size_t i = 0; i < playing && i < wet.size() && i < dry.size(); ++i) {
                wetEnergy += static_cast<double>(wet[i]) * wet[i];
                dryEnergy += static_cast<double>(dry[i]) * dry[i];
            }
            const double tailRms = std::sqrt(tailEnergy / static_cast<double>(tail));
            const double gain = dryEnergy > 0.0 ? std::sqrt(wetEnergy / dryEnergy) : 0.0;
            logInfo(concat("audioquality: reverb ", environments, " rooms nonFinite=", wetStats.nonFinite, " clipped=",
                           wetStats.clipped, " stepsOver0.5=", wetStats.over, " maxStep=", num(wetStats.maxStep, 4),
                           " wet/dry rms=", num(gain, 3), " tailRms=", num(tailRms, 7), " denormals=", denormals));
            check(wetStats.nonFinite == 0, "reverb_all_rooms_no_nan");
            check(wetStats.clipped == 0, "reverb_all_rooms_no_clipping");
            check(wetStats.over == 0, "reverb_all_rooms_no_discontinuities_across_crossfades");
            check(gain > 0.8 && gain < 2.5, "reverb_all_rooms_level_stays_near_the_dry_signal");
            check(tailRms < 1.0e-4, "reverb_tail_decays_to_silence");
            check(denormals == 0, "reverb_no_denormals_after_the_tail");
            if (!dumpDir.empty()) {
                audio::writeWav16(dumpDir + "/reverb_all_rooms_" + name + ".wav", wet, 2, 48000);
            }
        }
    }
    {
        // The device thread mixes while the game thread starts, retunes and stops voices and changes rooms.
        const std::string name = "al_el_compbed_01";
        audio::SoundData decodedProbe;
        std::string error;
        if (!game.audio().decodeProbe(name, decodedProbe, error) || !audio::resample(decodedProbe, 48000, 2)) {
            check(false, "thread_probe_decodes");
        } else {
            auto data = std::make_shared<audio::SoundData>(std::move(decodedProbe));
            audio::Mixer mixer;
            std::atomic<bool> stop{false};
            std::thread gameThread([&] {
                int iteration = 0;
                while (!stop.load()) {
                    const int a = mixer.play(data, audio::Bus::Effects, 0.02f, true, true, {2.0f, 1.0f, 0.0f});
                    const int b = mixer.play(data, audio::Bus::Ambient, 0.02f, true, false);
                    mixer.setVoiceVolume(a, 0.01f + 0.0002f * static_cast<float>(iteration % 50));
                    mixer.setVoicePosition(a, {static_cast<float>(iteration % 7), 0.0f, 0.0f});
                    mixer.setReverbEnvironment(iteration % 20, (iteration & 1) != 0);
                    mixer.fadeVoice(b, 0.0f, 0.01f, true);
                    mixer.stopVoice(a);
                    ++iteration;
                    std::this_thread::sleep_for(std::chrono::microseconds(300));
                }
            });
            std::vector<float> block(480 * 2);
            std::vector<float> all;
            for (int i = 0; i < 2000; ++i) {
                mixer.mix(block.data(), 480);
                all.insert(all.end(), block.begin(), block.end());
            }
            stop.store(true);
            gameThread.join();
            const audio::StepStats stats = audio::stepStats(all, 1.0f);
            logInfo(concat("audioquality: threads 20 s mixed while a second thread churned voices: nonFinite=", stats.nonFinite,
                           " clipped=", stats.clipped, " maxStep=", num(stats.maxStep, 4)));
            check(stats.nonFinite == 0, "threads_mix_has_no_nan");
            check(stats.clipped == 0, "threads_mix_has_no_clipping");
        }
    }

    // ---- E. the metric itself: the pre-fix IMA decode must be rejected ---------------------------
    if (!legacyRun) {
        const std::string name = "al_en_cantinabed";
        const std::vector<std::uint8_t> bytes = files.bytes(install, name);
        audio::SoundData reference;
        std::string error;
        if (!bytes.empty() && audio::referenceDecode(bytes, reference, error)) {
            audio::setLegacyImaDecodeForTest(true);
            audio::SoundData engine;
            const bool decodedLegacy = game.audio().decodeProbe(name, engine, error);
            audio::setLegacyImaDecodeForTest(false);
            double corr = 0.0;
            double ratio = 0.0;
            if (decodedLegacy) {
                const std::vector<float> a = audio::mono(reference);
                const std::vector<float> b = audio::mono(engine);
                corr = audio::correlate(a, b, 8, 480000).value;
                ratio = static_cast<double>(b.size()) / static_cast<double>(std::max<std::size_t>(1, a.size()));
                if (!dumpDir.empty()) {
                    audio::writeWav16(dumpDir + "/decode_legacy_" + name + ".wav", engine.samples, engine.channels, engine.sampleRate);
                }
            }
            logInfo(concat("audioquality: legacy ima decode ", name, " corr=", num(corr, 4), " lengthRatio=", num(ratio, 4)));
            check(!decodedLegacy || corr < 0.5 || ratio < 0.97, "gate_rejects_the_prefix_ima_decode");
        } else {
            check(false, "legacy_probe_reference");
        }
    }

    logInfo(ok ? "audioquality family: PASS" : "audioquality family: FAIL");
    return ok ? 0 : 32;
}

} // namespace ke
