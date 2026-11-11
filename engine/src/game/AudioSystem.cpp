#include "game/AudioSystem.h"

#include "game/Ambience.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>

namespace ke {

namespace {

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::optional<kotor::TwoDa> loadTable(kotor::Install& install, const char* name)
{
    auto data = install.find(name, kotor::resType(kotor::ResType::TwoDa));
    if (!data.has_value()) {
        return std::nullopt;
    }
    try {
        return kotor::TwoDa::parse(std::move(*data), name);
    } catch (const kotor::FormatError&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return {};
    }
    return bytes;
}

std::string rowResource(const std::optional<kotor::TwoDa>& table, std::int32_t id)
{
    if (!table.has_value() || id < 0) {
        return {};
    }
    const std::string row = std::to_string(id);
    if (table->rowIndex(row) < 0) {
        return {};
    }
    return table->cell(row, "resource");
}

} // namespace

AudioSystem::AudioSystem(kotor::Install& install)
    : install_(install)
{
    musicTable_ = loadTable(install_, "ambientmusic");
    ambientTable_ = loadTable(install_, "ambientsound");
    eaxTable_ = loadTable(install_, "soundeax");
    footstepTable_ = loadTable(install_, "footstepsounds");
    surfaceTable_ = loadTable(install_, "surfacemat");
    loadPriorityGroups();
    if (const std::optional<kotor::TwoDa> excited = loadTable(install_, "excitedduration"); excited.has_value()) {
        for (int row = 0; row < 3; ++row) {
            const std::string key = std::to_string(row);
            if (excited->rowIndex(key) >= 0) {
                excitedMs_[row] = std::atoi(excited->cell(key, "duration").c_str());
            }
        }
    }
    mixer_.setSampleRate(48000);
    mixer_.setListener({0.0f, 0.0f, 0.0f}, 0.0f);
    indexLooseAudio();
    ambience_ = std::make_unique<AreaAmbience>(*this);
    readUserVolumes();
}

AudioSystem::~AudioSystem() = default;

// swkotor.ini [Sound Options] holds the retail volume settings in percent (shipped
// defaults: Music 85, Voiceover 85, Sound Effects 85, Movie 100). The ini has no
// separate ambient key, so the ambient loop follows Sound Effects Volume.
void AudioSystem::readUserVolumes()
{
    UserVolumes volumes;
    std::ifstream file(install_.root() / "swkotor.ini");
    std::string line;
    bool inSection = false;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty() && line.front() == '[') {
            inSection = lower(line) == "[sound options]";
            continue;
        }
        const std::size_t eq = line.find('=');
        if (!inSection || eq == std::string::npos) {
            continue;
        }
        const std::string key = lower(line.substr(0, eq));
        float value = 0.0f;
        try {
            value = std::stof(line.substr(eq + 1)) / 100.0f;
        } catch (const std::exception&) {
            continue;
        }
        value = std::clamp(value, 0.0f, 1.0f);
        if (key == "music volume") {
            volumes.music = value;
        } else if (key == "voiceover volume") {
            volumes.voice = value;
        } else if (key == "sound effects volume") {
            volumes.effects = value;
        } else if (key == "movie volume") {
            volumes.movie = value;
        }
    }
    setUserVolumes(volumes);
}

void AudioSystem::setUserVolumes(const UserVolumes& volumes)
{
    userVolumes_ = volumes;
    musicVolume_ = volumes.music;
    ambientVolume_ = volumes.effects;
    applyMusicBus();
    mixer_.setBusVolume(audio::Bus::Ambient, volumes.effects);
    mixer_.setBusVolume(audio::Bus::Effects, volumes.effects);
    mixer_.setBusVolume(audio::Bus::Voice, volumes.voice);
    mixer_.setBusVolume(audio::Bus::Movie, volumes.movie);
}

void AudioSystem::applyMusicBus()
{
    mixer_.setBusVolume(audio::Bus::Music, musicVolume_ * musicScriptVolume_);
}

void AudioSystem::update(float dt)
{
    ambience_->update(dt);
}

void AudioSystem::setTimeOfDay(int dayState, int hour)
{
    ambience_->setTimeOfDay(dayState, hour);
}

void AudioSystem::setCombatState(bool inCombat)
{
    ambience_->setCombat(inCombat);
}

void AudioSystem::playConversationTrack(const std::string& resref)
{
    ambience_->playConversationTrack(resref);
}

void AudioSystem::stopConversationTrack()
{
    ambience_->stopConversationTrack();
}

bool AudioSystem::playMenuMusic(bool alternate)
{
    // Retail 005f9af0 starts the main-menu stream by name (alternate chooses the second
    // theme) at volume 127, looping. The ambientmusic.2da row 1 name has no file in the
    // install, so the table is not used for the menu.
    return ambience_->playUiMusic(alternate ? "mus_theme_rep" : "mus_theme_cult", 1.0f);
}

bool AudioSystem::playLoadScreenMusic()
{
    return ambience_->playUiMusic("mus_loadscreen", 1.0f);
}

void AudioSystem::stopUiMusic()
{
    ambience_->stopUiMusic();
}

// A movie pauses every sound and resumes it where it stopped. Retail ACT_733_PlayMovie 00540ed0,
// WaitForMovie 005f6f20 and PlayLoadModuleMovies 00602650 push sound mode 3 (DAT_0074c5f8) and pop it
// when the movie ends; 005d8560 answers mode 3 with 005d82c0, which pauses every streaming source and
// every sample whatever its priority group (modes 2 and 4 spare groups 1, 2, 4 and 11), and the pop
// runs 005d83f0, which resumes them. The music is therefore stopped mid-track and continues, not
// muted while it keeps playing.
void AudioSystem::setMovieMode(bool on)
{
    movieMode_ = on;
    for (const audio::Bus bus : {audio::Bus::Music, audio::Bus::Ambient, audio::Bus::Effects,
                                 audio::Bus::Voice}) {
        mixer_.setBusPaused(bus, on);
    }
}

void AudioSystem::indexLooseAudio()
{
    const std::filesystem::path root = install_.root();
    for (const char* directory : {"streammusic", "streamsounds", "streamwaves"}) {
        std::error_code error;
        const std::filesystem::path base = root / directory;
        std::filesystem::recursive_directory_iterator iterator(
            base, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        while (!error && iterator != end) {
            if (iterator->is_regular_file(error)) {
                const std::string key = lower(iterator->path().stem().string());
                if (!key.empty() && looseFiles_.find(key) == looseFiles_.end()) {
                    looseFiles_.emplace(key, iterator->path());
                }
            }
            iterator.increment(error);
        }
    }
}

std::string AudioSystem::tableResource(const char* table, std::int32_t id) const
{
    if (std::string(table) == "ambientmusic") {
        return rowResource(musicTable_, id);
    }
    if (std::string(table) == "ambientsound") {
        return rowResource(ambientTable_, id);
    }
    return {};
}

bool AudioSystem::loadScene(const kotor::Scene& scene, std::string& error)
{
    error.clear();
    areaAudio_ = scene.areaAudio;
    soundEmitters_ = scene.soundEmitters;
    entitySoundSets_.clear();
    entityPositions_.clear();
    for (std::size_t index = 0; index < scene.entities.size(); ++index) {
        const kotor::Entity& entity = scene.entities[index];
        entityPositions_[static_cast<std::uint32_t>(index)] = entity.position;
        if (!entity.soundSet.empty()) {
            entitySoundSets_[static_cast<std::uint32_t>(index)] = entity.soundSet;
        }
    }
    // Stand-in until the first room lookup: the GIT EnvAudio label. Retail does not
    // read that field (it starts at environment 0 and follows ARE Rooms/EnvAudio).
    mixer_.setReverb(areaAudio_.eaxPreset);
    currentRoom_.clear();
    roomEnvironment_ = -1;
    musicScriptVolume_ = 1.0f;
    applyMusicBus();
    mixer_.stopMusic();
    mixer_.stopBus(audio::Bus::Ambient);
    mixer_.stopBus(audio::Bus::Effects);
    mixer_.stopBus(audio::Bus::Voice);
    mixer_.stopBus(audio::Bus::Movie);
    ambience_->startArea();
    return true;
}

std::string AudioSystem::eaxLabel(int row) const
{
    if (!eaxTable_.has_value() || row < 0) {
        return {};
    }
    const std::string key = std::to_string(row);
    return eaxTable_->rowIndex(key) >= 0 ? eaxTable_->cell(key, "label") : std::string{};
}

void AudioSystem::setListenerRoom(const std::string& roomName)
{
    if (roomName.empty() || roomName == currentRoom_) {
        return;
    }
    const std::string wanted = lower(roomName);
    for (const kotor::AreaRoomAudio& room : areaAudio_.rooms) {
        if (lower(room.room) == wanted) {
            currentRoom_ = roomName;
            ambience_->setListenerRoomScale(room.ambientScale);
            if (room.envAudio >= 0 && room.envAudio != roomEnvironment_) {
                roomEnvironment_ = room.envAudio;
                mixer_.setReverbEnvironment(room.envAudio, true);
            }
            return;
        }
    }
}

void AudioSystem::setListener(kotor::Vec3 position, float yaw)
{
    listener_ = position;
    mixer_.setListener(position, yaw);
    ambience_->setListener(position);
}

bool AudioSystem::playMusic(std::int32_t musicId)
{
    return ambience_->forceMusicRow(musicId);
}

bool AudioSystem::playMusicResource(const std::string& resref, float volume)
{
    return ambience_->playUiMusic(resref, volume);
}

bool AudioSystem::playAmbient(std::int32_t ambientId)
{
    return ambience_->forceAmbientRow(ambientId);
}

void AudioSystem::stopMusic()
{
    ambience_->stopAllMusic();
}

void AudioSystem::stopAmbient()
{
    ambience_->setAmbientEnabled(false);
}

std::shared_ptr<const audio::SoundData> AudioSystem::loadSound(const std::string& resref,
                                                               std::string& error)
{
    const std::string key = lower(resref);
    if (const auto cached = sounds_.find(key); cached != sounds_.end()) {
        soundLastUse_[key] = ++soundUseClock_;
        return cached->second;
    }
    std::vector<std::uint8_t> bytes;
    if (const auto loose = looseFiles_.find(key); loose != looseFiles_.end()) {
        bytes = readFile(loose->second);
    }
    if (bytes.empty()) {
        if (auto embedded = install_.find(resref, 4); embedded.has_value()) {
            bytes = std::move(*embedded);
        }
    }
    if (bytes.empty()) {
        error = key + ": audio resource not found";
        return nullptr;
    }
    audio::SoundData decoded;
    if (!audio::decodeAudio(bytes, key, decoded, error)) {
        return nullptr;
    }
    if (!audio::resample(decoded, 48000, 2)) {
        error = key + ": audio resample failed";
        return nullptr;
    }
    auto shared = std::make_shared<audio::SoundData>(std::move(decoded));
    sounds_.emplace(key, shared);
    soundLastUse_[key] = ++soundUseClock_;
    soundsEverLoaded_.insert(key);
    soundCacheBytes_ += shared->samples.size() * sizeof(float);
    trimSoundCache(key);
    return shared;
}

void AudioSystem::trimSoundCache(const std::string& keep)
{
    // An engine choice, not a retail value: room for a module's music and ambience. A sound
    // evicted here decodes again from the install the next time it is asked for.
    constexpr std::size_t kSoundCacheBudget = 96u * 1024u * 1024u;
    while (soundCacheBytes_ > kSoundCacheBudget) {
        auto oldest = sounds_.end();
        std::uint64_t oldestUse = UINT64_MAX;
        for (auto it = sounds_.begin(); it != sounds_.end(); ++it) {
            if (it->first != keep && it->second.use_count() == 1 && soundLastUse_[it->first] < oldestUse) {
                oldestUse = soundLastUse_[it->first];
                oldest = it;
            }
        }
        if (oldest == sounds_.end()) {
            return; // everything left is playing
        }
        soundCacheBytes_ -= oldest->second->samples.size() * sizeof(float);
        soundLastUse_.erase(oldest->first);
        sounds_.erase(oldest);
    }
}

bool AudioSystem::playEffect(const std::string& resref, const kotor::Vec3* position,
                             float volume, bool loop)
{
    std::string error;
    const std::shared_ptr<const audio::SoundData> data = loadSound(resref, error);
    if (!data) {
        return false;
    }
    const bool positional = position != nullptr;
    const kotor::Vec3 at = position != nullptr ? *position : kotor::Vec3{};
    mixer_.play(data, audio::Bus::Effects, volume, loop, positional, at, 1.0f, 20.0f);
    return true;
}

int AudioSystem::playSfx(const std::string& resref, const kotor::Vec3* position, float volume,
                         float minDistance, float maxDistance, bool loop)
{
    std::string error;
    const std::shared_ptr<const audio::SoundData> data = loadSound(resref, error);
    if (!data) {
        return -1;
    }
    const bool positional = position != nullptr;
    const kotor::Vec3 at = position != nullptr ? *position : kotor::Vec3{};
    return mixer_.play(data, audio::Bus::Effects, volume, loop, positional, at, minDistance,
                       maxDistance);
}

int AudioSystem::playGuiSound(int row)
{
    if (!guiSoundTable_.has_value()) {
        guiSoundTable_ = loadTable(install_, "guisounds");
    }
    if (!guiSoundTable_.has_value() || row < 0) {
        return -1;
    }
    const std::string key = std::to_string(row);
    if (guiSoundTable_->rowIndex(key) < 0) {
        return -1;
    }
    const std::string resref = guiSoundTable_->cell(key, "soundresref");
    if (resref.empty()) {
        return -1;
    }
    // Retail 00409f00 gives every guisounds source priority group 11 (GUI: volume 127, MaxPlaying 2,
    // Interrupt 1); the executable never reads soundgain.2da. The UI is not positional.
    GroupSound request;
    request.group = kGroupGui;
    return playGroupSound(resref, request);
}

bool AudioSystem::playSoundObject(std::uint32_t entity)
{
    return ambience_->playSoundObject(entity);
}

void AudioSystem::setCreatureSoundSet(std::uint32_t entity, const std::string& resref)
{
    if (resref.empty()) {
        entitySoundSets_.erase(entity);
    } else {
        entitySoundSets_[entity] = resref;
    }
}

bool AudioSystem::playCreatureSound(std::uint32_t entity, int slot)
{
    return playSoundSetSlot(entity, slot, SoundSetVoice{});
}

bool AudioSystem::playSoundSetSlot(std::uint32_t entity, int slot, const SoundSetVoice& who)
{
    if (tlk_ == nullptr) {
        return false;
    }
    const auto setIt = entitySoundSets_.find(entity);
    if (setIt == entitySoundSets_.end() || setIt->second.empty() || slot < 0 || slot >= 28) {
        return false;
    }
    // 00611470: a creature that is dead (or being cleaned up) only plays slot type 0x10, the death cry.
    if (who.dead && slot != 15) {
        return false;
    }
    const std::string key = lower(setIt->second);
    auto cached = soundSetCache_.find(key);
    if (cached == soundSetCache_.end()) {
        auto data = install_.find(setIt->second, 2060); // SSF
        if (!data.has_value()) {
            return false;
        }
        try {
            cached = soundSetCache_.emplace(
                key, kotor::Ssf::parse(std::move(*data), key + ".ssf")).first;
        } catch (const kotor::FormatError&) {
            return false;
        }
    }
    const std::int32_t strref = cached->second.strref(static_cast<std::size_t>(slot));
    if (strref < 0) {
        return false;
    }
    const std::string sound = tlk_->sound(static_cast<std::uint32_t>(strref));
    kotor::Vec3 position{};
    if (const auto pos = entityPositions_.find(entity); pos != entityPositions_.end()) {
        position = pos->second;
    }
    if (sound.empty()) {
        // 00611470 starts the streaming source whatever the resource name is: with an empty one the Play call
        // (005dca30) is still made and then fails, which the state trace sees as a start with no name.
        static const std::string kNoName;
        audio::Mixer::PlayInfo attempt;
        attempt.name = &kNoName;
        attempt.bus = audio::Bus::Voice;
        attempt.positional = true;
        attempt.position = position;
        attempt.minDistance = std::min(priorityGroup(kGroupSmallVoice).maxVolumeDist, priorityGroup(kGroupSmallVoice).minVolumeDist);
        attempt.maxDistance = std::max(priorityGroup(kGroupSmallVoice).maxVolumeDist, priorityGroup(kGroupSmallVoice).minVolumeDist);
        attempt.group = who.isPlayer ? kGroupPlayerChat : (who.sizeCategory < 4 ? kGroupSmallVoice : kGroupLargeVoice);
        attempt.volume127 = 127;
        attempt.stream = 1;
        mixer_.reportAttempt(attempt);
        return false;
    }
    // 00611470: the controlled creature speaks as Player_Chat (8); every other creature as a
    // medium-and-small (17, SizeCategory < 4) or large (16) creature vocalization. A creature
    // has one voice: a new sound-set sound replaces the one still playing (+0xcc is deleted).
    if (const auto previous = creatureVoices_.find(entity); previous != creatureVoices_.end()) {
        mixer_.stopVoice(previous->second);
        creatureVoices_.erase(previous);
    }
    GroupSound request;
    request.group = who.isPlayer ? kGroupPlayerChat
                                 : (who.sizeCategory < 4 ? kGroupSmallVoice : kGroupLargeVoice);
    request.position = &position;
    request.stream = true;
    const int voice = playGroupSound(sound, request);
    if (voice <= 0) {
        return false;
    }
    creatureVoices_[entity] = voice;
    return true;
}

bool AudioSystem::playFootstep(int footstepType, int surface, kotor::Vec3 position, float)
{
    FootstepRequest request;
    request.footstepType = footstepType;
    request.surface = surface;
    request.position = position;
    return playCreatureFootstep(request) > 0;
}

// Retail CSWCCreature::PlayFootstep 0061a2d0, run by the snd_Footstep clip event (0061ab40 / 0061a970).
int AudioSystem::playCreatureFootstep(const FootstepRequest& request, std::string* resource)
{
    if (!footstepTable_.has_value() || !surfaceTable_.has_value()) {
        return 0;
    }
    // Stealth (creature+0x194 == 1) forces row 0; a blank footsteptype leaves the lookup empty.
    const int row = request.stealth ? 0 : request.footstepType;
    if (row < 0) {
        return 0;
    }
    const std::string rowKey = std::to_string(row);
    if (footstepTable_->rowIndex(rowKey) < 0) {
        return 0;
    }
    // GetCExoStringEntry(row, "Rolling"): a rolling or floating droid (rows 8 and 9) has no step
    // sound, the looped rolling sound plays instead (006105b0 / 006107c0).
    const auto filled = [](const std::string& text) { return !text.empty() && text != "****"; };
    if (filled(footstepTable_->cell(rowKey, "rolling"))) {
        return 0;
    }
    // The column is surfacemat.2da "Name" + rand() % 3 (005c0c21 reads "Name"): a blank name (Swamp,
    // Obscuring, ...) is silent, NotDefined and Dirt are dirt, Mud and Puddles are puddles.
    const int surface = request.surface < 0 ? 0 : request.surface;
    const std::string surfaceKey = std::to_string(surface);
    if (surfaceTable_->rowIndex(surfaceKey) < 0) {
        return 0;
    }
    const std::string name = lower(surfaceTable_->cell(surfaceKey, "name"));
    if (!filled(name)) {
        return 0;
    }
    const std::string sound =
        footstepTable_->cell(rowKey, name + std::to_string(random() % 3u));
    if (!filled(sound)) {
        return 0;
    }
    float pitchOffset = 0.0f;
    if (const std::string cell = footstepTable_->cell(rowKey, "pitchoffset"); filled(cell)) {
        pitchOffset = static_cast<float>(std::atof(cell.c_str()));
    }
    // Priority group: the controlled creature 19, else SizeCategory 1..3 is 20, else 18 (0061a450).
    const int group = request.isPlayer ? kGroupPlayerFootstep
        : (request.sizeCategory >= 1 && request.sizeCategory <= 3 ? kGroupSmallFootstep
                                                                    : kGroupLargeFootstep);
    // A step further from the listener than MaxVolumeDist + 2 m is not started at all.
    const PriorityGroup& info = priorityGroup(group);
    const float dx = request.position.x - listener_.x;
    const float dy = request.position.y - listener_.y;
    const float dz = request.position.z - listener_.z;
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > info.maxVolumeDist + 2.0f) {
        return 0;
    }
    GroupSound play;
    play.group = group;
    play.position = &request.position;
    // The PitchOffset cell goes into the pitch-variance slot of Play3DOneShotSound: 0 keeps the
    // group's PlaybackVariance, anything else replaces it (clamped 0..1 by 005dba40).
    play.pitchVariance = pitchOffset != 0.0f ? pitchOffset : -1.0f;
    const int voice = playGroupSound(sound, play);
    if (resource != nullptr) {
        *resource = sound;
    }
    return voice > 0 ? voice : 0;
}

// ---- PriorityGroups.2da -------------------------------------------------------------------

// Retail 005d7bd0: every row is read cell by cell; a blank cell keeps the 005d6c10 default.
void AudioSystem::loadPriorityGroups()
{
    groups_.clear();
    const std::optional<kotor::TwoDa> table = loadTable(install_, "prioritygroups");
    if (!table.has_value()) {
        groups_.resize(27);
        return;
    }
    const auto filled = [](const std::string& text) { return !text.empty() && text != "****"; };
    for (std::size_t row = 0; row < table->rowCount(); ++row) {
        PriorityGroup group;
        const auto number = [&](const char* column, double& out) {
            const int index = table->columnIndex(column);
            if (index < 0) {
                return false;
            }
            const std::string cell = table->cell(row, static_cast<std::size_t>(index));
            if (!filled(cell)) {
                return false;
            }
            out = std::atof(cell.c_str());
            return true;
        };
        double value = 0.0;
        if (number("priority", value)) {
            group.priority = static_cast<int>(value);
        }
        if (number("volume", value)) {
            group.volume = std::clamp(static_cast<int>(value), 0, 127);
        }
        if (number("maxplaying", value)) {
            group.maxPlaying = std::clamp(static_cast<int>(value), 0, 255);
        }
        if (number("interrupt", value)) {
            group.interrupt = static_cast<int>(value) != 0;
        }
        if (number("fadetime", value)) {
            group.fadeTimeMs = static_cast<int>(value);
        }
        double maxDist = 0.0;
        double minDist = 0.0;
        if (number("maxvolumedist", maxDist) && number("minvolumedist", minDist)) {
            group.maxVolumeDist = static_cast<float>(maxDist);
            group.minVolumeDist = static_cast<float>(minDist);
        }
        if (number("playbackvariance", value)) {
            group.playbackVariance = std::clamp(static_cast<float>(value), 0.0f, 1.0f);
        }
        groups_.push_back(group);
    }
}

const AudioSystem::PriorityGroup& AudioSystem::priorityGroup(int group) const
{
    static const PriorityGroup fallback{};
    if (group < 0 || static_cast<std::size_t>(group) >= groups_.size()) {
        return fallback;
    }
    return groups_[static_cast<std::size_t>(group)];
}

audio::Bus AudioSystem::groupBus(int group)
{
    switch (group) {
    case kGroupPlayerChat:
    case kGroupNonPlayerChat:
    case kGroupLargeVoice:
    case kGroupSmallVoice:
    case kGroupBark:
        return audio::Bus::Voice; // 005d6a90 / 005d6ad0: the dialog volume
    case kGroupStinger:
    case kGroupMusic:
        return audio::Bus::Music; // the music volume
    default:
        return audio::Bus::Effects; // the sound-effects volume
    }
}

float AudioSystem::groupLevel(int group, int sourceVolume127) const
{
    const int source = std::clamp(sourceVolume127, 0, 127);
    // 005db800 truncates group * source / 127 to an integer before the 1/127 scale; the streaming
    // setter 005dc930 multiplies the two 1/127 fractions instead.
    return static_cast<float>(priorityGroup(group).volume * source / 127) / 127.0f;
}

std::uint32_t AudioSystem::random()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_ >> 1;
}

// The MaxPlaying cap of 005db4d0: when the group is full an Interrupt group stops its oldest
// sound (005d7fe0), any other group drops the new sound. Returns -1 when the new sound is dropped.
int AudioSystem::enforceGroupCap(int group)
{
    std::erase_if(groupVoices_, [&](const GroupVoice& entry) {
        return !mixer_.voiceActive(entry.voice);
    });
    const PriorityGroup& info = priorityGroup(group);
    for (;;) {
        int playing = 0;
        const GroupVoice* oldest = nullptr;
        for (const GroupVoice& entry : groupVoices_) {
            if (entry.group != group) {
                continue;
            }
            ++playing;
            if (oldest == nullptr || entry.serial < oldest->serial) {
                oldest = &entry;
            }
        }
        if (playing < info.maxPlaying) {
            return 0;
        }
        if (!info.interrupt || oldest == nullptr) {
            return -1;
        }
        const int voice = oldest->voice;
        mixer_.stopVoice(voice);
        std::erase_if(groupVoices_, [&](const GroupVoice& entry) { return entry.voice == voice; });
    }
}

float AudioSystem::voiceLevel(int group, int sourceVolume127, bool stream, bool positional) const
{
    if (stream) {
        // 005dc930: a stream multiplies the two 1/127 fractions, linearly. A positional stream adds its
        // falloff in the mixer (Mixer::setVoiceStream3D); the pow(x, 0.6) Update3DStream 005dc560 hands to Miles
        // is undone by Miles, so the rendered amplitude is linear (r6_steam_a / b / c).
        return (static_cast<float>(priorityGroup(group).volume) / 127.0f)
            * (static_cast<float>(std::clamp(sourceVolume127, 0, 127)) / 127.0f);
    }
    float level = groupLevel(group, sourceVolume127);
    if (positional && level > 0.0f) {
        // 005db800: a 3D sample's volume is pow(level * setting, 0.6). The bus carries the setting
        // linearly, so the voice takes pow(level, 0.6) * setting^-0.4 and the product comes out equal.
        const float setting = mixer_.busVolume(groupBus(group));
        level = std::pow(level, 0.6f) * (setting > 1.0e-4f ? std::pow(setting, -0.4f) : 1.0f);
    }
    return level;
}

int AudioSystem::playGroupSound(const std::string& resref, const GroupSound& request)
{
    if (resref.empty()) {
        return -1;
    }
    std::string error;
    const std::shared_ptr<const audio::SoundData> data = loadSound(resref, error);
    if (!data) {
        return -1;
    }
    if (enforceGroupCap(request.group) < 0) {
        return -1;
    }
    const PriorityGroup& info = priorityGroup(request.group);
    const audio::Bus bus = groupBus(request.group);
    const bool positional = request.position != nullptr;
    const float level = voiceLevel(request.group, request.sourceVolume127, request.stream, positional);
    kotor::Vec3 at{};
    if (positional) {
        at = *request.position;
        at.z += request.zOffset;
    }
    float minDistance = request.minDistance;
    float maxDistance = request.maxDistance;
    if (request.useGroupDistances) {
        // The table names MaxVolumeDist (full level) and MinVolumeDist (inaudible) are the near and
        // far limits; the constructor defaults 20 and 10 come the other way round, so order them.
        minDistance = std::min(info.maxVolumeDist, info.minVolumeDist);
        maxDistance = std::max(info.maxVolumeDist, info.minVolumeDist);
    }
    // Rate: a FixedVariance in 0.5..2.0 is a constant factor (005db980, it also clears the random
    // variance); otherwise PlaybackVariance moves the rate by a random amount, half the time up
    // by up to the fraction and otherwise down by up to half of it (005db3d0).
    float pitch = 1.0f;
    if (request.fixedVariance >= 0.5f && request.fixedVariance <= 2.0f
        && request.fixedVariance != 1.0f) {
        pitch = request.fixedVariance;
    } else {
        const float variance = request.pitchVariance < 0.0f
            ? info.playbackVariance : std::clamp(request.pitchVariance, 0.0f, 1.0f);
        if (variance > 0.0f) {
            const bool up = (random() & 1u) != 0u;
            const float fraction = static_cast<float>(random() % 1000u) / 1000.0f;
            pitch = up ? 1.0f + fraction * variance : 1.0f - fraction * variance * 0.5f;
        }
    }
    mixer_.tagNextPlay({request.group, std::clamp(request.sourceVolume127, 0, 127), request.stream ? 1 : 0});
    const int voice = mixer_.play(data, bus, level, request.loop, positional, at, minDistance,
                                  maxDistance, 0.0f, pitch);
    if (voice > 0) {
        groupVoices_.push_back({voice, request.group, ++groupSerial_});
        if (positional && request.stream) {
            mixer_.setVoiceStream3D(voice, true);
        }
    }
    return voice;
}

void AudioSystem::updateObstruction()
{
    if (!obstructionProbe_) {
        return;
    }
    const kotor::Vec3 listener = mixer_.listenerPosition();
    const auto moved = [](kotor::Vec3 a, kotor::Vec3 b) {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz >= 0.25f;
    };
    std::map<int, ObstructionSample> kept;
    for (const audio::Mixer::StreamProbe& probe : mixer_.stream3DProbes()) {
        ObstructionSample sample{probe.position, listener};
        const auto old = obstructionCache_.find(probe.voice);
        if (old == obstructionCache_.end() || moved(old->second.source, probe.position)
            || moved(old->second.listener, listener)) {
            mixer_.setVoiceObstruction(probe.voice, obstructionProbe_(probe.position, listener));
        } else {
            sample = old->second;
        }
        kept[probe.voice] = sample;
    }
    obstructionCache_ = std::move(kept);
}

bool AudioSystem::stopSoundObject(std::uint32_t entity)
{
    return ambience_->stopSoundObject(entity);
}

bool AudioSystem::setSoundObjectVolume(std::uint32_t entity, float volume)
{
    return ambience_->setSoundObjectVolume127(
        entity, static_cast<int>(std::lround(std::clamp(volume, 0.0f, 1.0f) * 127.0f)));
}

bool AudioSystem::setSoundObjectPosition(std::uint32_t entity, kotor::Vec3 position)
{
    return ambience_->setSoundObjectPosition(entity, position);
}

bool AudioSystem::playVoice(const std::string& resref)
{
    // Retail 0062e1d0 streams every dialogue line as priority group 9 (Non_Player_Chat, volume 90).
    GroupSound request;
    request.group = kGroupNonPlayerChat;
    request.stream = true;
    return playGroupSound(resref, request) > 0;
}

bool AudioSystem::hasSound(const std::string& resref) const
{
    const std::string key = lower(resref);
    return soundsEverLoaded_.count(key) != 0 || looseFiles_.find(key) != looseFiles_.end();
}

bool AudioSystem::decodeProbe(const std::string& resref, audio::SoundData& out,
                              std::string& error) const
{
    const std::string key = lower(resref);
    std::vector<std::uint8_t> bytes;
    if (const auto loose = looseFiles_.find(key); loose != looseFiles_.end()) {
        bytes = readFile(loose->second);
    }
    if (bytes.empty()) {
        auto embedded = install_.find(resref, 4);
        if (embedded.has_value()) {
            bytes = std::move(*embedded);
        }
    }
    if (bytes.empty()) {
        error = key + ": audio resource not found";
        return false;
    }
    return audio::decodeAudio(bytes, key, out, error);
}

const audio::SoundData* AudioSystem::sound(const std::string& resref) const
{
    const auto it = sounds_.find(lower(resref));
    return it == sounds_.end() ? nullptr : it->second.get();
}

} // namespace ke
