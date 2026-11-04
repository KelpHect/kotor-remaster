#include "game/AudioSystem.h"

#include <algorithm>
#include <cctype>
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
    mixer_.setSampleRate(48000);
    mixer_.setListener({0.0f, 0.0f, 0.0f}, 0.0f);
    indexLooseAudio();
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
    mixer_.setReverb(areaAudio_.eaxPreset);
    mixer_.setBusVolume(audio::Bus::Music, musicVolume_);
    ambientVolume_ = areaAudio_.ambientDayVolume > 0
        ? static_cast<float>(areaAudio_.ambientDayVolume) / 100.0f
        : 0.65f;
    mixer_.setBusVolume(audio::Bus::Ambient, ambientVolume_);
    mixer_.stopMusic();
    mixer_.stopBus(audio::Bus::Ambient);
    mixer_.stopBus(audio::Bus::Effects);
    mixer_.stopBus(audio::Bus::Voice);

    const std::int32_t musicId = areaAudio_.musicDayId >= 0 ? areaAudio_.musicDayId
        : (areaAudio_.musicNightId >= 0 ? areaAudio_.musicNightId : areaAudio_.musicBattleId);
    if (musicId > 0) {
        playMusic(musicId);
    }
    const std::int32_t ambientId = areaAudio_.ambientDayId >= 0 ? areaAudio_.ambientDayId
        : areaAudio_.ambientNightId;
    if (ambientId >= 0) {
        playAmbient(ambientId);
    }
    return true;
}

void AudioSystem::setListener(kotor::Vec3 position, float yaw)
{
    mixer_.setListener(position, yaw);
}

bool AudioSystem::playMusic(std::int32_t musicId)
{
    const std::string resref = tableResource("ambientmusic", musicId);
    if (resref.empty()) {
        return false;
    }
    std::string error;
    const std::shared_ptr<const audio::SoundData> data = loadSound(resref, error);
    if (!data) {
        return false;
    }
    mixer_.playMusic(data, musicVolume_, true);
    return true;
}

bool AudioSystem::playAmbient(std::int32_t ambientId)
{
    const std::string resref = tableResource("ambientsound", ambientId);
    if (resref.empty()) {
        return false;
    }
    std::string error;
    const std::shared_ptr<const audio::SoundData> data = loadSound(resref, error);
    if (!data) {
        return false;
    }
    mixer_.play(data, audio::Bus::Ambient, ambientVolume_, true, false);
    return true;
}

void AudioSystem::stopMusic()
{
    mixer_.stopMusic();
}

void AudioSystem::stopAmbient()
{
    mixer_.stopBus(audio::Bus::Ambient);
}

std::shared_ptr<const audio::SoundData> AudioSystem::loadSound(const std::string& resref,
                                                               std::string& error)
{
    const std::string key = lower(resref);
    if (const auto cached = sounds_.find(key); cached != sounds_.end()) {
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
    return shared;
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

bool AudioSystem::playSoundObject(std::uint32_t entity)
{
    for (const kotor::SoundEmitter& emitter : soundEmitters_) {
        if (emitter.entity != entity || emitter.sounds.empty() || !emitter.active) {
            continue;
        }
        std::string error;
        const std::shared_ptr<const audio::SoundData> data =
            loadSound(emitter.sounds.front(), error);
        if (!data) {
            return false;
        }
        kotor::Vec3 at = emitter.position;
        at.z += emitter.elevation;
        const int voice = mixer_.play(data, audio::Bus::Effects, emitter.volume,
                                      emitter.looping, emitter.positional, at,
                                      emitter.minDistance, emitter.maxDistance);
        objectVoices_[entity] = voice;
        return true;
    }
    return false;
}

bool AudioSystem::stopSoundObject(std::uint32_t entity)
{
    const auto it = objectVoices_.find(entity);
    if (it == objectVoices_.end()) {
        return false;
    }
    mixer_.stopVoice(it->second);
    objectVoices_.erase(it);
    return true;
}

bool AudioSystem::setSoundObjectVolume(std::uint32_t entity, float volume)
{
    const auto it = objectVoices_.find(entity);
    if (it == objectVoices_.end()) {
        return false;
    }
    mixer_.setVoiceVolume(it->second, volume);
    return true;
}

bool AudioSystem::setSoundObjectPosition(std::uint32_t entity, kotor::Vec3 position)
{
    const auto it = objectVoices_.find(entity);
    if (it == objectVoices_.end()) {
        return false;
    }
    mixer_.setVoicePosition(it->second, position);
    return true;
}

bool AudioSystem::playVoice(const std::string& resref)
{
    std::string error;
    const std::shared_ptr<const audio::SoundData> data = loadSound(resref, error);
    if (!data) {
        return false;
    }
    mixer_.play(data, audio::Bus::Voice, 1.0f, false, false);
    return true;
}

bool AudioSystem::hasSound(const std::string& resref) const
{
    const std::string key = lower(resref);
    return sounds_.find(key) != sounds_.end() || looseFiles_.find(key) != looseFiles_.end();
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
