// Movies and scripted cutscenes: the gate behind --family-test cinema.
//
// Every assertion is a measurement on the shipped install: the Bink movies are decoded
// frame by frame, the movie queue/skip/pause rules are driven through the script VM and
// the session, and the dialogue/cutscene rules run on shipped DLGs. Nothing here has been
// listened to or looked at: the picture checks are statistics, and the captures that were
// viewed by eye are listed in the lane report, not asserted.
#include "game/ScriptHost.h"

#include "audio/Audio.h"
#include "core/Log.h"
#include "game/AudioSystem.h"
#include "game/GameSession.h"
#include "kotor/BinkDecoder.h"
#include "kotor/Media.h"
#include "script/ActionRuntime.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace ke {

namespace {

using Args = std::vector<std::vector<script::Value>>;

std::vector<script::Value> num(std::int64_t v) { return {script::Value::makeInt(v)}; }
std::vector<script::Value> text(std::string v) { return {script::Value::makeString(std::move(v))}; }

std::string lowerName(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

struct FrameStats {
    int colours = 0;
    float meanLuma = 0.0f;
    bool black = false;
};

FrameStats frameStats(const kotor::BinkVideoFrame& frame)
{
    FrameStats stats;
    std::set<std::uint32_t> seen;
    const std::size_t pixels = frame.rgba.size() / 4;
    const std::size_t step = std::max<std::size_t>(1, pixels / 6000);
    double luma = 0.0;
    std::size_t counted = 0;
    int peak = 0;
    for (std::size_t i = 0; i < pixels; i += step) {
        const std::uint8_t* p = &frame.rgba[i * 4];
        seen.insert((static_cast<std::uint32_t>(p[0] >> 3) << 10)
                    | (static_cast<std::uint32_t>(p[1] >> 3) << 5)
                    | static_cast<std::uint32_t>(p[2] >> 3));
        luma += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
        peak = std::max({peak, static_cast<int>(p[0]), static_cast<int>(p[1]),
                         static_cast<int>(p[2])});
        ++counted;
    }
    stats.colours = static_cast<int>(seen.size());
    stats.meanLuma = counted > 0 ? static_cast<float>(luma / static_cast<double>(counted)) : 0.0f;
    stats.black = peak <= 6;
    return stats;
}

// Advances the session in 1/30 s steps until `seconds` have passed.
void tick(GameSession& game, float seconds)
{
    for (float t = 0.0f; t < seconds; t += 1.0f / 30.0f) {
        game.update(1.0f / 30.0f, InputState{});
    }
}

} // namespace

int runCinemaFamilyTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("cinema: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const std::filesystem::path movies = std::filesystem::path(game.installPath()) / "movies";
    check(kotor::BinkPlayer::available(), "decoder_available");

    // ---- A. every shipped movie decodes, frame by frame --------------------------------
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(movies)) {
        if (entry.is_regular_file() && lowerName(entry.path().extension().string()) == ".bik") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    check(files.size() == 61, "movie_count_61");
    int decodedMovies = 0;
    int silentMovies = 0;
    std::string silent;
    std::size_t totalFrames = 0;
    std::set<std::string> aspects;
    for (const std::filesystem::path& path : files) {
        const std::string name = path.stem().string();
        kotor::BinkPlayer player;
        std::string error;
        if (!player.open(path.string(), error)) {
            check(false, "open_" + name + " (" + error + ")");
            continue;
        }
        const int frames = player.frameCount();
        const double fps = player.framesPerSecond();
        bool allDecoded = true;
        int blackFrames = 0;
        int firstLit = -1;
        int colourPeak = 0;
        kotor::BinkVideoFrame frame;
        for (int index = 0; index < frames; ++index) {
            if (!player.decodeFrame(index, frame, error)
                || frame.width != player.width() || frame.height != player.height()
                || frame.rgba.size() != static_cast<std::size_t>(frame.width) * frame.height * 4) {
                allDecoded = false;
                check(false, "decode_" + name + "_frame_" + std::to_string(index) + " (" + error + ")");
                break;
            }
            const FrameStats stats = frameStats(frame);
            if (stats.black) {
                ++blackFrames;
            } else if (firstLit < 0) {
                firstLit = index;
            }
            colourPeak = std::max(colourPeak, stats.colours);
        }
        totalFrames += static_cast<std::size_t>(frames);
        const double videoSeconds = static_cast<double>(frames) / fps;
        bool audioOk = true;
        std::string audioNote = "none";
        if (player.hasAudio()) {
            const kotor::BinkAudioTrack& track = player.audio();
            double energy = 0.0;
            for (const float sample : track.samples) {
                energy += static_cast<double>(sample) * sample;
            }
            const double rms = track.samples.empty()
                ? 0.0 : std::sqrt(energy / static_cast<double>(track.samples.size()));
            audioOk = (track.sampleRate == 44100 || track.sampleRate == 48000)
                && track.channels == 2
                && std::fabs(track.durationSeconds() - videoSeconds) < 0.5
                && rms > 1.0e-4;
            audioNote = concat(track.sampleRate, "Hz/", track.channels, "ch dur=",
                               track.durationSeconds(), " rms=", rms);
        } else {
            ++silentMovies;
            silent += name + " ";
        }
        const bool fpsOk = std::fabs(fps - 29.97) < 0.01;
        const bool litOk = firstLit >= 0 && blackFrames * 100 < frames * 95 && colourPeak >= 8;
        const bool sizeOk = player.width() == 640
            && (player.height() == 272 || player.height() == 360 || player.height() == 480);
        aspects.insert(concat(player.width(), "x", player.height()));
        logInfo(concat("cinema: movie ", name, " ", player.width(), "x", player.height(), " ",
                       fps, "fps frames=", frames, " dur=", videoSeconds, "s black=",
                       blackFrames, " firstLit=", firstLit, " colours<=", colourPeak,
                       " audio=", audioNote));
        if (allDecoded && audioOk && fpsOk && litOk && sizeOk) {
            ++decodedMovies;
        } else {
            check(false, concat("movie_", name, " decoded=", allDecoded ? 1 : 0, " audio=",
                                audioOk ? 1 : 0, " fps=", fpsOk ? 1 : 0, " lit=", litOk ? 1 : 0,
                                " size=", sizeOk ? 1 : 0));
        }
    }
    logInfo(concat("cinema: decoded ", decodedMovies, "/", files.size(), " movies, ",
                   totalFrames, " frames; without an audio track: ", silent));
    check(decodedMovies == static_cast<int>(files.size()), "all_movies_decode");
    // The shipped data decides which movie has no audio; legal.bik is the one.
    check(silentMovies == 1 && silent == "legal ", "only_legal_has_no_audio");
    check(aspects.size() == 3, "three_frame_sizes");

    // ---- B. letterbox / pillarbox rectangle ---------------------------------------------
    {
        const kotor::MovieRect wide = kotor::fitMovie(640, 272, 1280.0f, 720.0f);
        check(std::fabs(wide.w - 1280.0f) < 0.01f && std::fabs(wide.h - 544.0f) < 0.01f
                  && std::fabs(wide.y - 88.0f) < 0.01f && wide.x == 0.0f,
              "letterbox_640x272_in_1280x720");
        const kotor::MovieRect logo = kotor::fitMovie(640, 480, 1280.0f, 720.0f);
        check(std::fabs(logo.w - 960.0f) < 0.01f && std::fabs(logo.h - 720.0f) < 0.01f
                  && std::fabs(logo.x - 160.0f) < 0.01f && logo.y == 0.0f,
              "pillarbox_640x480_in_1280x720");
        const kotor::MovieRect tall = kotor::fitMovie(640, 360, 800.0f, 600.0f);
        check(std::fabs(tall.w / tall.h - 640.0f / 360.0f) < 0.001f, "aspect_kept_640x360");
    }

    // ---- C. queue, skip, order and the held world ---------------------------------------
    {
        game.stopMovie();
        game.setMovieHoldsWorld(false);
        // QueueMovie(movie, bSkippable) only queues; PlayMovieQueue plays in order.
        game.actionAsForTest(game.scriptWorld().pc, 769, Args{text("07_1"), num(1)});
        game.actionAsForTest(game.scriptWorld().pc, 769, Args{text("07_2"), num(0)});
        game.actionAsForTest(game.scriptWorld().pc, 769, Args{text("07_3"), num(1)});
        check(!game.moviePlaying() && game.queuedMovies() == 3, "queue_does_not_play");
        game.actionAsForTest(game.scriptWorld().pc, 770, Args{num(1)});
        check(game.moviePlaying() && game.movieName() == "07_1" && game.movieSkippable()
                  && game.queuedMovies() == 2,
              "play_queue_starts_first");
        const auto isPlaying = game.actionAsForTest(game.scriptWorld().pc, 768, Args{});
        check(isPlaying.has_value() && !isPlaying->empty() && isPlaying->front().asInt() == 1,
              "is_movie_playing_reports_it");
        // Escape on a skippable movie goes to the next one; a non-skippable movie ignores it.
        check(game.skipMovie() && game.movieName() == "07_2" && !game.movieSkippable(),
              "skip_goes_to_next");
        check(!game.skipMovie() && game.movieName() == "07_2" && game.moviePlaying(),
              "unskippable_ignores_escape");
        // The movie in the middle plays out on its own clock and hands over in order.
        float elapsed = 0.0f;
        while (game.moviePlaying() && game.movieName() == "07_2" && elapsed < 12.0f) {
            game.advanceMovie(1.0f / 30.0f);
            elapsed += 1.0f / 30.0f;
        }
        check(game.moviePlaying() && game.movieName() == "07_3" && std::fabs(elapsed - 3.2f) < 0.15f,
              "natural_end_hands_over_in_order");
        check(game.skipMovie() && !game.moviePlaying() && game.movieFrame() == nullptr,
              "last_skip_clears_frame");

        // Without separate skips Escape drops the whole queue.
        game.queueMovie("07_1", true);
        game.queueMovie("07_2", true);
        game.playMovieQueue(false);
        check(game.moviePlaying() && game.queuedMovies() == 1, "joint_queue_playing");
        check(game.skipMovie() && !game.moviePlaying() && game.queuedMovies() == 0,
              "joint_skip_ends_queue");

        // A finished movie leaves no stale picture behind.
        game.playMovie("07_2");
        for (float t = 0.0f; t < 4.0f && game.moviePlaying(); t += 1.0f / 30.0f) {
            game.advanceMovie(1.0f / 30.0f);
        }
        check(!game.moviePlaying() && game.movieFrame() == nullptr, "finished_movie_clears_frame");

        // The world holds while a movie plays (when the frontend asks for it).
        game.playMovie("07_1");
        const float before = game.elapsedSeconds();
        tick(game, 1.0f);
        check(game.elapsedSeconds() > before && game.moviePlaying(), "world_runs_when_not_held");
        game.stopMovie();
        game.setMovieHoldsWorld(true);
        game.playMovie("07_1");
        const float held = game.elapsedSeconds();
        const float movieAt = game.movieTime();
        tick(game, 1.0f);
        check(game.elapsedSeconds() == held && game.movieTime() > movieAt + 0.9f,
              "world_held_while_movie_plays");
        tick(game, 4.0f);
        check(!game.moviePlaying(), "held_movie_ends");
        const float resumed = game.elapsedSeconds();
        tick(game, 0.5f);
        check(game.elapsedSeconds() > resumed, "world_resumes_after_movie");
        game.setMovieHoldsWorld(false);
    }

    // ---- D. the retail startup sequence ---------------------------------------------------
    {
        game.stopMovie();
        check(game.playStartupMovies() && game.movieName() == "leclogo" && game.queuedMovies() == 2
                  && game.movieSkippable(),
              "startup_begins_with_leclogo");
        check(game.skipMovie() && game.movieName() == "biologo", "startup_second_is_biologo");
        check(game.skipMovie() && game.movieName() == "legal", "startup_third_is_legal");
        check(game.skipMovie() && !game.moviePlaying(), "startup_ends_after_legal");
    }

    // ---- E. movie audio stays on the movie clock -----------------------------------------
    {
        game.stopMovie();
        check(game.playMovie("01a") && game.movieAudioVoice() != 0, "audio_voice_started");
        audio::Mixer& mixer = game.audio().mixer();
        float worst = 0.0f;
        for (int step = 0; step < 90; ++step) {
            // Uneven frame times, as a real window gives.
            game.advanceMovie(step % 3 == 0 ? 0.040f : 0.016f);
            worst = std::max(worst, static_cast<float>(std::fabs(
                mixer.voiceCursor(game.movieAudioVoice()) - game.movieTime())));
        }
        check(worst <= 0.002f, "audio_cursor_follows_movie_clock");
        const int index = game.movieFrameIndex();
        check(index == static_cast<int>(std::floor(game.movieTime() * 29.97)), "frame_index_follows_clock");
        game.seekMovie(30.0f);
        std::vector<float> block(4800 * 2);
        mixer.mix(block.data(), 4800);
        check(mixer.lastRms() > 1.0e-4f && mixer.activeVoices(audio::Bus::Movie) == 1,
              "audio_audible_on_movie_bus");
        game.stopMovie();
        check(mixer.activeVoices(audio::Bus::Movie) == 0, "audio_stops_with_movie");
    }

    // ---- F. a shipped script queues and plays its movie ------------------------------------
    {
        GameSession dark(game.installPath(), "STUNT_55a");
        dark.endDialogue();
        dark.stopMovie();
        ScriptApi{dark}.runScript("k_stunt_end", dark.scriptWorld().pc, ScriptContext{});
        check(dark.moviePlaying() && dark.movieName() == "55", "k_stunt_end_plays_movie_55");
        dark.stopMovie();
    }

    // ---- G. dialogue presentation: voice, fade, gestures, auto-advance ---------------------
    {
        const Rules& rules = game.rules();
        check(rules.dialogAnimationName(10040) == "tlkforce" && rules.dialogAnimationName(10042) == "tlksad"
                  && rules.dialogAnimationName(10039) == "tlkplead" && rules.dialogAnimationName(35) == "bow",
              "dialoganimations_ids_map_to_model_clips");
        check(rules.dialogAnimationName(1201).empty() && rules.dialogAnimationLooping(10040),
              "cutscene_clip_ids_are_not_dialoganimations");

        // A voice that decodes, taken from a shipped dialogue.
        std::string voice;
        double voiceSeconds = 0.0;
        if (auto data = game.install().find("k_hbas_dialog", 2029); data.has_value()) {
            kotor::Gff gff = kotor::Gff::parse(std::move(*data), "k_hbas_dialog.dlg");
            const kotor::Dlg shipped = kotor::Dlg::parse(gff, nullptr, "k_hbas_dialog", 0, 0);
            for (const kotor::DlgEntry& entry : shipped.entries) {
                audio::SoundData probe;
                std::string error;
                if (!entry.voResRef.empty() && game.audio().decodeProbe(entry.voResRef, probe, error)
                    && probe.durationSeconds() > 0.5) {
                    voice = entry.voResRef;
                    voiceSeconds = probe.durationSeconds();
                    break;
                }
            }
        }
        check(!voice.empty(), "shipped_vo_resref_decodes");

        kotor::Dlg dlg;
        dlg.entries.resize(2);
        dlg.replies.resize(1);
        dlg.starting = {0};
        dlg.entries[0].text = "first";
        dlg.entries[0].voResRef = voice;
        dlg.entries[0].fadeType = 4;
        dlg.entries[0].fadeLength = 0.5f;
        dlg.entries[0].fadeColor[0] = 1.0f;
        dlg.entries[0].animList.push_back({"PLAYER", 10040});
        dlg.entries[0].replies = {0};
        dlg.replies[0].entries = {1};
        dlg.entries[1].text = "second";
        dlg.entries[1].voResRef = voice;
        dlg.entries[1].fadeType = 3;
        dlg.entries[1].fadeLength = 0.5f;

        audio::Mixer& mixer = game.audio().mixer();
        game.setDialogueAutoAdvance(false);
        game.setDialogueForTest(dlg, "cinema_g", "Tester");
        check(game.dialogue().entry == 0 && mixer.activeVoices(audio::Bus::Voice) == 1,
              "entry_voice_plays_on_voice_bus");
        const FadeState fade = game.fade();
        check(fade.mode == FadeState::Mode::Out && std::fabs(fade.length - 0.5f) < 1.0e-4f
                  && std::fabs(fade.color[0] - 1.0f) < 1.0e-4f,
              "fadetype_4_fades_out_to_the_node_colour");
        const std::size_t pcEntity = game.playerEntity();
        if (pcEntity != static_cast<std::size_t>(-1) && !game.animationName(pcEntity).empty()) {
            check(game.animationName(pcEntity) == "tlkforce", "animlist_plays_talk_clip_on_player");
        }

        std::vector<float> block(2400 * 2);
        const auto run = [&](float seconds) {
            const InputState idle{};
            for (float t = 0.0f; t < seconds; t += 0.05f) {
                game.update(0.05f, idle);
                mixer.mix(block.data(), 2400);
            }
        };
        run(static_cast<float>(voiceSeconds) + 1.0f);
        check(game.dialogue().entry == 0, "manual_dialogue_waits_for_a_choice");

        game.setDialogueAutoAdvance(true);
        game.setDialogueForTest(dlg, "cinema_g", "Tester");
        run(static_cast<float>(voiceSeconds) * 0.5f);
        check(game.dialogue().entry == 0, "auto_advance_waits_for_the_voice");
        run(static_cast<float>(voiceSeconds) * 0.5f + 0.6f);
        check(game.dialogue().entry == 1 && mixer.activeVoices(audio::Bus::Voice) == 1
                  && game.fade().mode != FadeState::Mode::Out,
              "auto_advance_follows_the_voice_and_next_node_plays");
        run(static_cast<float>(voiceSeconds) + 1.0f);
        check(!game.dialogue().active, "auto_advance_ends_at_the_last_node");
        game.setDialogueAutoAdvance(false);
        game.clearDialogueForTest();
    }

    logInfo(ok ? "cinema family: PASS" : "cinema family: FAIL");
    return ok ? 0 : 30;
}

} // namespace ke
