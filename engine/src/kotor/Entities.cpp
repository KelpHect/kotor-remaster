#include "kotor/Formats.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

namespace ke::kotor {

namespace {

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

Vec3 rotate(const Mat3& matrix, Vec3 value)
{
    return {matrix.m[0][0] * value.x + matrix.m[0][1] * value.y + matrix.m[0][2] * value.z,
            matrix.m[1][0] * value.x + matrix.m[1][1] * value.y + matrix.m[1][2] * value.z,
            matrix.m[2][0] * value.x + matrix.m[2][1] * value.y + matrix.m[2][2] * value.z};
}

Vec3 normalized(Vec3 value)
{
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 0.0f) {
        return {0.0f, 0.0f, 0.0f};
    }
    return {value.x / length, value.y / length, value.z / length};
}

const std::vector<std::string>& scriptHookLabels()
{
    static const std::vector<std::string> labels = {
        // Creature/UTC
        "ScriptHeartbeat", "ScriptOnNotice", "ScriptSpellAt", "ScriptAttacked",
        "ScriptDamaged", "ScriptDisturbed", "ScriptEndRound", "ScriptEndDialogu",
        "ScriptDialogue", "ScriptSpawn", "ScriptRested", "ScriptDeath",
        "ScriptUserDefine", "ScriptOnBlocked",
        // Door/placeable/trigger/store
        "OnClosed", "OnDamaged", "OnDeath", "OnDisarm", "OnHeartbeat", "OnLock",
        "OnMeleeAttacked", "OnOpen", "OnSpellCastAt", "OnTrapTriggered", "OnUnlock",
        "OnUserDefined", "OnClick", "OnFailToOpen", "OnUsed", "OnInvDisturbed",
        "ScriptOnEnter", "ScriptOnExit", "ScriptUserDefine",
        "OnOpenStore", "OnOpen", "OnClose",
        "OnEntered", "OnExhausted", "OnEndDialogue", "OnDialog",
        // Area (ARE)
        "OnEnter", "OnExit", "OnUserDefined",
        // Module (IFO)
        "Mod_OnHeartbeat", "Mod_OnModLoad", "Mod_OnModStart", "Mod_OnClientEntr",
        "Mod_OnClientLeav", "Mod_OnActvtItem", "Mod_OnAcquirItem", "Mod_OnUsrDefined",
        "Mod_OnUnAqreItem", "Mod_OnPlrDeath", "Mod_OnPlrDying", "Mod_OnPlrLvlUp",
        "Mod_OnSpawnBtnDn", "Mod_OnPlrRest",
    };
    return labels;
}

std::string textField(const Gff& gff, std::uint32_t structure, const char* label)
{
    const GffField* field = gff.field(structure, label);
    return field != nullptr ? gff.text(*field) : std::string{};
}

float numberField(const Gff& gff, std::uint32_t structure, const char* label, float fallback = 0.0f)
{
    const GffField* field = gff.field(structure, label);
    return field != nullptr ? gff.number(*field) : fallback;
}

std::int32_t intField(const Gff& gff, std::uint32_t structure, const char* label, std::int32_t fallback = 0)
{
    const GffField* field = gff.field(structure, label);
    return field != nullptr ? gff.integer(*field) : fallback;
}

// Shared by UTC, UTD and UTP. A template that lacks a field keeps the default.
void applyGameplayFields(Entity& entity, const Gff& gff, std::uint32_t root)
{
    entity.locked = intField(gff, root, "Locked", entity.locked ? 1 : 0) != 0;
    entity.lockable = intField(gff, root, "Lockable") != 0;
    entity.keyRequired = intField(gff, root, "KeyRequired") != 0;
    entity.keyName = textField(gff, root, "KeyName");
    entity.openLockDC = intField(gff, root, "OpenLockDC");
    entity.closeLockDC = intField(gff, root, "CloseLockDC");
    entity.autoRemoveKey = intField(gff, root, "AutoRemoveKey") != 0;
    entity.plot = intField(gff, root, "Plot") != 0;
    entity.min1Hp = intField(gff, root, "Min1HP") != 0;
    entity.trapFlag = intField(gff, root, "TrapFlag") != 0;
    entity.trapDetectable = intField(gff, root, "TrapDetectable") != 0;
    entity.trapDisarmable = intField(gff, root, "TrapDisarmable") != 0;
    entity.trapOneShot = intField(gff, root, "TrapOneShot") != 0;
    entity.trapDetectDC = intField(gff, root, "TrapDetectDC");
    entity.trapDisarmDC = intField(gff, root, "DisarmDC");
    entity.trapType = intField(gff, root, "TrapType");
    entity.hasInventory = intField(gff, root, "HasInventory") != 0;
    entity.isStatic = intField(gff, root, "Static") != 0;
    entity.hardness = intField(gff, root, "Hardness");
    // retail 0058a1f0 (door OpenState), 00585670 (placeable Open, then AnimationState).
    entity.openState = intField(gff, root, "OpenState");
    entity.animationState = intField(gff, root, "AnimationState");
    entity.open = entity.open || entity.openState == 1 || entity.openState == 2
        || intField(gff, root, "Open") != 0;
}

// UTC sheet fields behind GetRacialType, GetGender, GetAbilityScore, the class and
// skill queries. Data only; ScriptObjects.cpp reads it.
void applyCreatureStats(Entity& entity, const Gff& utc, std::uint32_t root)
{
    CreatureStats& stats = entity.stats;
    stats.parsed = true;
    stats.race = intField(utc, root, "Race");
    stats.subRace = intField(utc, root, "SubraceIndex");
    stats.gender = intField(utc, root, "Gender");
    stats.challengeRating = numberField(utc, root, "ChallengeRating", 0.0f);
    stats.perceptionRange = numberField(utc, root, "PerceptionRange", 11.0f);
    stats.naturalAC = intField(utc, root, "NaturalAC");
    stats.goodEvil = intField(utc, root, "GoodEvil", 50);
    constexpr const char* kAbilities[6] = {"Str", "Dex", "Con", "Int", "Wis", "Cha"};
    for (std::size_t i = 0; i < 6; ++i) {
        stats.abilities[i] = intField(utc, root, kAbilities[i], 10);
    }
    stats.classes.clear();
    stats.powers.clear();
    if (const GffField* list = utc.field(root, "ClassList"); list != nullptr) {
        for (const std::uint32_t entry : utc.list(*list)) {
            stats.classes.emplace_back(intField(utc, entry, "Class"),
                                       intField(utc, entry, "ClassLevel"));
            if (const GffField* known = utc.field(entry, "KnownList0"); known != nullptr) {
                for (const std::uint32_t spell : utc.list(*known)) {
                    stats.powers.push_back(intField(utc, spell, "Spell", -1));
                }
            }
        }
    }
    stats.feats.clear();
    if (const GffField* list = utc.field(root, "FeatList"); list != nullptr) {
        for (const std::uint32_t entry : utc.list(*list)) {
            stats.feats.push_back(intField(utc, entry, "Feat", -1));
        }
    }
    stats.maxForcePoints = intField(utc, root, "ForcePoints");
    stats.forcePoints = intField(utc, root, "CurrentForce", stats.maxForcePoints);
    stats.combatDerived = false;
    stats.skills.clear();
    if (const GffField* list = utc.field(root, "SkillList"); list != nullptr) {
        for (const std::uint32_t entry : utc.list(*list)) {
            stats.skills.push_back(intField(utc, entry, "Rank"));
        }
    }
}

std::string locField(const Gff& gff, std::uint32_t structure, const char* label, const Tlk* tlk)
{
    const GffField* field = gff.field(structure, label);
    if (field == nullptr) {
        return {};
    }
    if (field->type != GffType::LocString) {
        return gff.text(*field);
    }
    const std::uint32_t strref = gff.locStringStrref(*field);
    if (tlk != nullptr && strref != UINT32_MAX) {
        const std::string resolved = tlk->text(strref);
        if (!resolved.empty()) {
            return resolved;
        }
    }
    return gff.locString(*field);
}

std::shared_ptr<Mdl> loadModel(Install& install, const std::string& modelName, std::string& error)
{
    if (modelName.empty()) {
        error = "empty model name";
        return nullptr;
    }
    const std::string cacheKey = lower(modelName);
    if (const auto cached = install.modelCache().find(cacheKey); cached != install.modelCache().end()) {
        error = cached->second.error;
        return cached->second.model;
    }
    const auto remember = [&](std::shared_ptr<Mdl> result) {
        install.modelCache().insert_or_assign(cacheKey, Install::CachedModel{result, error});
        return result;
    };
    auto mdl = install.find(modelName, resType(ResType::Mdl));
    auto mdx = install.find(modelName, resType(ResType::Mdx));
    if (!mdl.has_value() || !mdx.has_value()) {
        error = "missing MDL/MDX for " + modelName;
        return remember(nullptr);
    }
    try {
        auto model = std::make_shared<Mdl>(Mdl::parse(std::move(*mdl), std::move(*mdx),
                                                      lower(modelName) + ".mdl"));
        std::set<std::string> seen;
        seen.insert(lower(modelName));
        std::string supermodel = model->supermodel();
        for (int depth = 0; depth < 8; ++depth) {
            const std::string key = lower(supermodel);
            if (key.empty() || key == "null" || !seen.insert(key).second) {
                break;
            }
            std::shared_ptr<const Mdl>& parent = install.supermodelCache()[key];
            if (parent == nullptr) {
                auto parentMdl = install.find(supermodel, resType(ResType::Mdl));
                auto parentMdx = install.find(supermodel, resType(ResType::Mdx));
                if (!parentMdl.has_value() || !parentMdx.has_value()) {
                    break;
                }
                try {
                    parent = std::make_shared<const Mdl>(
                        Mdl::parse(std::move(*parentMdl), std::move(*parentMdx), key + ".mdl"));
                } catch (const FormatError&) {
                    break;
                }
            }
            supermodel = parent->supermodel();
            model->mergeAnimations(*parent);
        }
        return remember(std::move(model));
    } catch (const FormatError& exception) {
        error = exception.what();
        return remember(nullptr);
    }
}

void refreshEntityMeshImpl(EntityMesh& mesh);

using TriangleGroups = std::map<std::string, std::vector<MdlMeshTriangle>>;

// Groups triangles by material. A node's triangles arrive together and share one texture and
// material, and the key depends on nothing else, so a run reuses it instead of rebuilding it.
void groupByMaterial(std::vector<MdlMeshTriangle>& triangles, TriangleGroups& groups)
{
    std::string previousTexture;
    const MdlNodeMaterial* previousMaterial = nullptr;
    std::vector<MdlMeshTriangle>* group = nullptr;
    for (MdlMeshTriangle& triangle : triangles) {
        if (group == nullptr || triangle.material != previousMaterial || triangle.texture != previousTexture) {
            previousTexture = triangle.texture;
            previousMaterial = triangle.material;
            group = &groups[materialKey(triangle)];
        }
        group->push_back(std::move(triangle));
    }
}

using VertexGroups = std::map<std::string, std::vector<SceneVertex>>;

// A pose refresh only rewrites vertices: same groups and order as buildMesh, no triangle copies.
void groupVertices(const std::vector<MdlMeshTriangle>& triangles, VertexGroups& groups)
{
    for (std::size_t first = 0; first < triangles.size();) {
        const MdlMeshTriangle& lead = triangles[first];
        std::size_t end = first + 1;
        while (end < triangles.size() && triangles[end].material == lead.material
               && triangles[end].texture == lead.texture) {
            ++end;
        }
        std::vector<SceneVertex>& group = groups[materialKey(lead)];
        group.reserve(group.size() + (end - first) * 3);
        for (std::size_t index = first; index < end; ++index) {
            const MdlMeshTriangle& triangle = triangles[index];
            for (int i = 0; i < 3; ++i) {
                group.push_back(toSceneVertex(triangle.v[i], triangle));
            }
        }
        first = end;
    }
}

void flattenVertices(const VertexGroups& groups, EntityMesh& mesh)
{
    std::size_t total = 0;
    for (const auto& [name, list] : groups) {
        total += list.size();
    }
    mesh.vertices.clear();
    mesh.vertices.reserve(total);
    for (const auto& [name, list] : groups) {
        mesh.vertices.insert(mesh.vertices.end(), list.begin(), list.end());
    }
}

// The clip a mesh's model is showing, with its blend and lip track.
MdlPoseRequest bodyPoseRequest(const EntityMesh& mesh)
{
    MdlPoseRequest request;
    request.animation = mesh.animation;
    request.time = mesh.animationTime;
    request.danglyTime = mesh.danglyTime;
    if (mesh.blendRemaining > 0.0f && !mesh.previousAnimation.empty() && mesh.blendDuration > 0.0f) {
        request.previous = mesh.previousAnimation;
        request.previousTime = mesh.previousAnimationTime;
        request.blend = 1.0f - mesh.blendRemaining / mesh.blendDuration;
    }
    request.face = mesh.face.weight > 0.0f ? &mesh.face : nullptr;
    request.headTurn = mesh.headYaw != 0.0f || mesh.headPitch != 0.0f ? &mesh.headTurn : nullptr;
    return request;
}

Mat3 multiplyRotations(const Mat3& a, const Mat3& b)
{
    Mat3 out;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
    }
    return out;
}

// retail 006993b0 plays a clip on the body and the same name on the head; the head's own clips
// (its pause1 holds the blink) and the lip track (006994a0) pose it. The head hangs on the body's
// hook as the body is posed this frame. Keeping a head's clips to its head root is inferred: the
// model header names that root, and the clips otherwise also turn the head's unused torso.
// The head's pose request and the body hook it hangs on, as posedHeadTriangles poses it.
// `headFrameTurn` holds the turn the request may point at.
void headPoseRequest(const EntityMesh& mesh, MdlPoseRequest& head, Mat3& headFrameTurn,
                     std::optional<Mdl::Pose>& pose)
{
    const MdlPoseRequest body = bodyPoseRequest(mesh);
    pose = mesh.model ? mesh.model->nodePoseFor(mesh.headNode, body) : std::nullopt;
    head = body;
    head.headRootOnly = true;
    head.danglyTime = -1.0f;
    // A body with no hturn_g of its own (the player's): the head model's turns instead. It hangs
    // on the hook, so the turn is put in the hook's frame.
    if (body.headTurn != nullptr) {
        if (mesh.model && mesh.model->hasNode("hturn_g")) {
            head.headTurn = nullptr;
        } else if (pose.has_value()) {
            // hook^-1 * turn * hook; a rotation's inverse is its transpose.
            Mat3 inverse;
            for (int c = 0; c < 3; ++c) {
                for (int r = 0; r < 3; ++r) {
                    inverse.m[c][r] = pose->rotation.m[r][c];
                }
            }
            headFrameTurn = multiplyRotations(inverse, multiplyRotations(mesh.headTurn, pose->rotation));
            head.headTurn = &headFrameTurn;
        }
    }
    const float own = mesh.headModel->animationDuration(mesh.animation);
    if (mesh.animationLoop && own > 0.0f) {
        head.time = std::fmod(mesh.animationTime, own);
    }
}

std::vector<MdlMeshTriangle> posedHeadTriangles(const EntityMesh& mesh)
{
    if (!mesh.headModel) {
        return {};
    }
    MdlPoseRequest head;
    Mat3 headFrameTurn;
    std::optional<Mdl::Pose> pose;
    headPoseRequest(mesh, head, headFrameTurn, pose);
    std::vector<MdlMeshTriangle> triangles = mesh.headModel->posedTrianglesFor(head);
    if (pose.has_value()) {
        for (MdlMeshTriangle& triangle : triangles) {
            for (MdlMeshVertex& vertex : triangle.v) {
                const Vec3 rotated = rotate(pose->rotation, vertex.position);
                vertex.position = {pose->translation.x + rotated.x,
                                   pose->translation.y + rotated.y,
                                   pose->translation.z + rotated.z};
                vertex.normal = normalized(rotate(pose->rotation, vertex.normal));
            }
        }
    }
    return triangles;
}

// Retail 00697610 builds "%s%02d" from the row's tex<letter> and the armour's TextureVar (a variation
// below 1 reads as 1, 0069768c), falling back to 01 when no TGA or TPC has that name; its caller
// (006984a3 -> 0063c0a0) hands that one name to the whole body model. What retail binds per node
// (paired/retail/raw/texnames_summary.tsv): a model-type B row draws the rebuilt map on every body
// node whatever the model names (N_CommM as appearance 345 draws n_commmd01, N_TwilekM as 269
// n_twilekmb01); a racetex row draws its racetex on every node (c_khounda02, c_ithorian03 on the eyes
// too); any other whole-body row keeps the maps its model names (appearance 28 N_SithSoldier03, the
// Rodian's Hair N_Rodian01). The head is its own model and keeps its own map.
void applyBodyTexture(Install& install, const Entity& entity, std::string& texture)
{
    if (entity.texture.empty()) {
        return;
    }
    const auto shipped = [&](const std::string& name) {
        return install.find(name, 3).has_value() || install.find(name, 3007).has_value();
    };
    std::string modelType;
    std::string raceTexture;
    if (const std::shared_ptr<const TwoDa> appearances = install.table("appearance"); appearances != nullptr) {
        const std::string row = std::to_string(entity.appearance);
        if (appearances->rowIndex(row) >= 0) {
            modelType = appearances->cell(row, "modeltype");
            raceTexture = appearances->cell(row, "racetex");
        }
    }
    if (modelType != "B") {
        if (!raceTexture.empty() && raceTexture != "****" && shipped(entity.texture)) {
            texture = entity.texture;
        }
        return;
    }
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "%02d", std::max(1, static_cast<int>(entity.bodyVariant.texture)));
    std::string wanted = entity.texture + suffix;
    if (!shipped(wanted)) {
        wanted = entity.texture + "01";
    }
    if (shipped(wanted)) {
        texture = wanted;
    }
}

void buildMesh(Scene& scene, Install& install, std::uint32_t entityIndex,
               const std::shared_ptr<Mdl>& model,
               const std::shared_ptr<Mdl>& attachModel = nullptr,
               const std::string& attachNode = {},
               const std::shared_ptr<Mdl>& headModel = nullptr,
               const std::string& headNode = {})
{
    if (!model) {
        return;
    }
    std::map<std::string, std::vector<MdlMeshTriangle>> byTexture;
    for (MdlMeshTriangle& triangle : model->posedTriangles({}, 0.0f)) {
        byTexture[materialKey(triangle)].push_back(std::move(triangle));
    }
    EntityMesh mesh;
    mesh.entity = entityIndex;
    mesh.model = model;
    mesh.headModel = headModel;
    mesh.headNode = headNode;
    std::vector<MdlMeshTriangle> headTriangles = posedHeadTriangles(mesh);
    // Read before groupByMaterial moves the triangles out.
    std::set<std::string> headMaps;
    for (const MdlMeshTriangle& triangle : headTriangles) {
        headMaps.insert(normalizeResref(triangle.texture));
    }
    groupByMaterial(headTriangles, byTexture);
    for (const auto& [materialName, list] : byTexture) {
        (void)materialName;
        MdlMeshTriangle look = list.front();
        // A held item is its own model under attachModel and keeps its own maps (RETAIL texnames, tar_m02aa:
        // C_DrdWar's w_RptnBlstr_001 node RptBlstr draws w_RptnBlstr_001, not the body's C_DrdWar01).
        if (entityIndex < scene.entities.size() && !attachModel && headMaps.count(normalizeResref(look.texture)) == 0u) {
            applyBodyTexture(install, scene.entities[entityIndex], look.texture);
        }
        // A placed object's parts have their owner set: 00449cc0 builds the part tree with 00449200, which writes
        // part +0x40 = the gob on every part. Rigid and skinned parts both draw through 00473900 (the skin vtable
        // 0x740e70 holds 00473900 next to its skinning step 0048d6c0 at 0x740e74), so they take the mesh's own
        // ambient and diffuse, never the forced white of StaticPartNeedsMat 0046f410.
        TextureBatch batch = makeMaterialBatch(scene, install, look, true);
        // Skinned draws: 0048d6c0 takes the vertex-program route 0048a520 when 0045fbd0 reports vertex programs, and
        // that route reaches 0046ff70's one-pass 0x14, program 830248: lerp(texel alpha, texel x primary, environment),
        // the reflection unlit. Retail never took it in the captures: 0045fbd0 returned 0 and every skinned draw went
        // through the CPU route 0048a0d0 and 0046ff70's lit two-pass (RETAIL row 28, armour_state_end.json), so the
        // reflection is lit like any other: the Endar soldier is matte black, not bronze. See ENGINE_CHOICES item 1.
        batch.envReflectionUnlit = false;
        batch.firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
        for (const MdlMeshTriangle& triangle : list) {
            for (int i = 0; i < 3; ++i) {
                mesh.vertices.push_back(toSceneVertex(triangle.v[i], triangle));
                mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size() - 1));
            }
        }
        batch.indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - batch.firstIndex;
        if (batch.indexCount != 0) {
            mesh.batches.push_back(batch);
        }
    }
    if (!mesh.vertices.empty()) {
        if (attachModel) {
            mesh.attachModel = attachModel;
            mesh.attachNode = attachNode;
            if (!attachModel->hasNode(attachNode) && attachModel->hasNode(attachNode + "_g")) {
                mesh.attachNode = attachNode + "_g"; // A body without the hook: its hand bone.
            }
            // retail 00610f40: the hand item is scaled by the creature's appearance row.
            if (entityIndex < scene.entities.size()) {
                if (const std::shared_ptr<const TwoDa> looks = install.table("appearance")) {
                    const std::string row = std::to_string(scene.entities[entityIndex].appearance);
                    const std::string scale = looks->rowIndex(row) >= 0 ? looks->cell(row, "weaponscale") : std::string{};
                    if (!scale.empty() && scale != "****") {
                        mesh.attachScale = std::strtof(scale.c_str(), nullptr);
                    }
                }
            }
            // A powered item rests unlit (retail 00647130).
            if (model->animationIndex("off") >= 0) {
                mesh.animation = "off";
                mesh.animationLoop = false;
            }
            refreshEntityMeshImpl(mesh);
        }
        scene.entityMeshes.push_back(std::move(mesh));
    }
    // Outside the test above: a model of emitters alone has no vertices (galaxy.mdl).
    if (!attachModel) {
        const Mat3 identity{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
        addModelEmitters(scene, install, model, entityIndex, {}, identity);
    }
}

void refreshEntityMeshImpl(EntityMesh& mesh);

// Retail 006964c0 (CSWCCreatureAppearance::CreateHead) attaches the head slot item's model to the
// head model's "GoggleHook". Our head rides the body's headhook, so the item is posed through it:
// headhook, then the head's turn and clip, then GoggleHook (refreshEntityMeshImpl).
void attachHeadItem(Scene& scene, Install& install, std::uint32_t entityIndex,
                    const std::shared_ptr<Mdl>& body, const std::shared_ptr<Mdl>& head)
{
    if (!body || !head || entityIndex >= scene.entities.size()) {
        return;
    }
    const std::string name = scene.entities[entityIndex].headItemModel;
    if (name.empty()) {
        return;
    }
    if (!head->hasNode("gogglehook")) {
        return;
    }
    std::string error;
    const std::shared_ptr<Mdl> item = loadModel(install, name, error);
    if (!item) {
        if (!error.empty()) {
            scene.warnings.push_back(name + ": " + error);
        }
        return;
    }
    const std::size_t before = scene.entityMeshes.size();
    buildMesh(scene, install, entityIndex, item, body, "headhook");
    if (scene.entityMeshes.size() == before + 1u) {
        EntityMesh& mesh = scene.entityMeshes.back();
        mesh.attachScale = 1.0f; // 00610f40's weaponscale is for hand items only.
        mesh.attachHead = head;
        refreshEntityMeshImpl(mesh);
    }
}

void refreshEntityMeshImpl(EntityMesh& mesh)
{
    if (!mesh.model) {
        return;
    }
    if (mesh.attachModel) {
        MdlPoseRequest handPose;
        handPose.animation = mesh.attachAnimation;
        handPose.time = mesh.attachTime;
        handPose.previous = mesh.attachPrevious;
        handPose.previousTime = mesh.attachPreviousTime;
        handPose.blend = mesh.attachBlend;
        std::optional<Mdl::Pose> hand;
        if (mesh.attachHead) {
            // As posedHeadTriangles poses the head: a body with hturn_g turns it itself, any other
            // body has the turn put in the hook's frame on the head model.
            const bool bodyTurns = mesh.attachModel->hasNode("hturn_g");
            MdlPoseRequest bodyRequest = handPose;
            if (bodyTurns) {
                bodyRequest.headTurn = &mesh.attachHeadTurn;
            }
            const std::optional<Mdl::Pose> hook = mesh.attachModel->nodePoseFor(mesh.attachNode, bodyRequest);
            MdlPoseRequest headRequest;
            headRequest.animation = mesh.attachAnimation;
            headRequest.time = mesh.attachTime;
            headRequest.headRootOnly = true;
            headRequest.danglyTime = -1.0f;
            const float own = mesh.attachHead->animationDuration(mesh.attachAnimation);
            if (own > 0.0f) {
                headRequest.time = std::fmod(mesh.attachTime, own);
            }
            Mat3 frameTurn;
            if (!bodyTurns && hook.has_value()) {
                Mat3 inverse;
                for (int col = 0; col < 3; ++col) {
                    for (int row = 0; row < 3; ++row) {
                        inverse.m[col][row] = hook->rotation.m[row][col];
                    }
                }
                frameTurn = multiplyRotations(inverse, multiplyRotations(mesh.attachHeadTurn, hook->rotation));
                headRequest.headTurn = &frameTurn;
            }
            const std::optional<Mdl::Pose> goggle = mesh.attachHead->nodePoseFor("gogglehook", headRequest);
            if (hook.has_value() && goggle.has_value()) {
                Mdl::Pose composed;
                composed.rotation = multiplyRotations(hook->rotation, goggle->rotation);
                const Vec3 moved = rotate(hook->rotation, goggle->translation);
                composed.translation = {hook->translation.x + moved.x, hook->translation.y + moved.y,
                                        hook->translation.z + moved.z};
                hand = composed;
            }
        } else {
            hand = mesh.attachModel->nodePoseFor(mesh.attachNode, handPose);
        }
        MdlPoseRequest itemPose;
        itemPose.animation = mesh.animation;
        itemPose.time = mesh.animationTime;
        const float itemScale = mesh.attachHidden ? 0.0f : mesh.attachScale;
        // A saber's swing trail keeps world places (00481ae0): the item model goes to the world
        // by the hand, then the entity's place and bearing as the renderer draws it.
        SaberTrailFeed trailFeed;
        if (mesh.trailPlaced && hand.has_value()) {
            const float c = std::cos(mesh.trailBearing);
            const float s = std::sin(mesh.trailBearing);
            const Mat3 bearing{{{c, -s, 0.0f}, {s, c, 0.0f}, {0.0f, 0.0f, 1.0f}}};
            const Vec3 at = rotate(bearing, hand->translation);
            trailFeed.trails = &mesh.saberTrails;
            trailFeed.rotation = multiplyRotations(bearing, hand->rotation);
            trailFeed.origin = {mesh.trailPosition.x + at.x, mesh.trailPosition.y + at.y, mesh.trailPosition.z + at.z};
            trailFeed.scale = itemScale;
            trailFeed.dt = mesh.trailDt;
            trailFeed.step = mesh.trailStep;
            mesh.trailDt = 0.0f;
            mesh.trailStep = false;
            itemPose.saberTrail = &trailFeed;
        }
        // The item's casters hang in the hand's part tree, scaled by the weapon scale (00474710 walks
        // the creature's parts into attached models; each part's +0x24 scale goes on with glScalef).
        mesh.shadowParts.clear();
        if (hand.has_value() && !mesh.attachHidden) {
            for (Mdl::ShadowPart part : mesh.model->shadowPartsFor(itemPose)) {
                const Vec3 moved = rotate(hand->rotation, part.pose.translation * itemScale);
                part.pose.rotation = multiplyRotations(hand->rotation, part.pose.rotation);
                part.pose.translation = hand->translation + moved;
                part.pose.scale *= itemScale;
                mesh.shadowParts.push_back(part);
            }
        }
        std::map<std::string, std::vector<MdlMeshTriangle>> byTexture;
        for (MdlMeshTriangle& triangle : mesh.model->posedTrianglesFor(itemPose)) {
            if (hand.has_value()) {
                for (MdlMeshVertex& vertex : triangle.v) {
                    const Vec3 scaled{vertex.position.x * itemScale, vertex.position.y * itemScale,
                                      vertex.position.z * itemScale};
                    const Vec3 rotated = rotate(hand->rotation, scaled);
                    vertex.position = {hand->translation.x + rotated.x,
                                       hand->translation.y + rotated.y,
                                       hand->translation.z + rotated.z};
                    vertex.normal = normalized(rotate(hand->rotation, vertex.normal));
                }
            }
            byTexture[materialKey(triangle)].push_back(std::move(triangle));
        }
        mesh.vertices.clear();
        for (const auto& [textureName, list] : byTexture) {
            (void)textureName;
            for (const MdlMeshTriangle& triangle : list) {
                for (int i = 0; i < 3; ++i) {
                    mesh.vertices.push_back(toSceneVertex(triangle.v[i], triangle));
                }
            }
        }
        if (mesh.indices.size() != mesh.vertices.size()) {
            mesh.indices.clear();
            mesh.indices.reserve(mesh.vertices.size());
            for (std::uint32_t i = 0; i < mesh.vertices.size(); ++i) {
                mesh.indices.push_back(i);
            }
        }
        ++mesh.revision;
        return;
    }
    VertexGroups groups;
    std::vector<MdlMeshTriangle> posedTriangles;
    if (mesh.referenceHolder) {
        Vec3 at;
        Mat3 turn{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
        mesh.referenceHolder->nodeWorldAt(mesh.referenceNode, mesh.referenceClip, mesh.referenceTime, at, turn);
        posedTriangles = mesh.model->placedPosedTriangles(mesh.animation, mesh.animationTime, at + mesh.origin, turn);
    } else if (mesh.scriptLoop) {
        posedTriangles = mesh.model->scriptLoopTriangles(mesh.animation, mesh.animationTime, mesh.origin);
    } else if (mesh.roomAnimated) {
        posedTriangles = mesh.model->loopMeshTriangles(mesh.animation, mesh.animationTime, mesh.origin);
    } else {
        posedTriangles = mesh.model->posedTrianglesFor(bodyPoseRequest(mesh));
    }
    mesh.shadowParts.clear();
    if (!mesh.referenceHolder && !mesh.scriptLoop && !mesh.roomAnimated) {
        mesh.shadowParts = mesh.model->shadowPartsFor(bodyPoseRequest(mesh));
        if (mesh.headModel && mesh.headModel->hasShadowMeshes()) {
            MdlPoseRequest head;
            Mat3 headFrameTurn;
            std::optional<Mdl::Pose> hook;
            headPoseRequest(mesh, head, headFrameTurn, hook);
            for (Mdl::ShadowPart part : mesh.headModel->shadowPartsFor(head)) {
                if (hook.has_value()) {
                    part.pose.translation = hook->translation + rotate(hook->rotation, part.pose.translation);
                    part.pose.rotation = multiplyRotations(hook->rotation, part.pose.rotation);
                }
                mesh.shadowParts.push_back(part);
            }
        }
    }
    groupVertices(posedTriangles, groups);
    groupVertices(posedHeadTriangles(mesh), groups);
    flattenVertices(groups, mesh);
    if (mesh.indices.size() != mesh.vertices.size()) {
        mesh.indices.clear();
        mesh.indices.reserve(mesh.vertices.size());
        for (std::uint32_t i = 0; i < mesh.vertices.size(); ++i) {
            mesh.indices.push_back(i);
        }
    }
    if (mesh.clipPlaced && !mesh.vertices.empty()) {
        Vec3 low = mesh.vertices.front().position;
        Vec3 high = low;
        for (const SceneVertex& vertex : mesh.vertices) {
            low.x = std::min(low.x, vertex.position.x);
            low.y = std::min(low.y, vertex.position.y);
            low.z = std::min(low.z, vertex.position.z);
            high.x = std::max(high.x, vertex.position.x);
            high.y = std::max(high.y, vertex.position.y);
            high.z = std::max(high.z, vertex.position.z);
        }
        mesh.center = (low + high) * 0.5f;
    }
    ++mesh.revision;
}

// UTC ItemList (carried, with a Dropable flag) and Equip_ItemList (worn). The
// struct type of an Equip_ItemList entry is the slot bit mask, so the lowest set
// bit is the INVENTORY_SLOT_* number.
// `root` is the creature's struct: the root of a UTC, a saved creature inside a retail save's GIT. A saved
// item has no InventoryRes / EquippedRes; its Tag is the template resref.
void readCreatureItems(Entity& entity, const Gff& utc, std::uint32_t root)
{
    entity.creatureItems.clear();
    if (const GffField* list = utc.field(root, "ItemList"); list != nullptr) {
        for (const std::uint32_t item : utc.list(*list)) {
            std::string resref = textField(utc, item, "InventoryRes");
            if (resref.empty()) {
                resref = lower(textField(utc, item, "Tag"));
            }
            if (!resref.empty()) {
                entity.creatureItems.push_back({resref, intField(utc, item, "Dropable", 1) != 0, -1});
            }
        }
    }
    if (const GffField* list = utc.field(root, "Equip_ItemList"); list != nullptr) {
        for (const std::uint32_t item : utc.list(*list)) {
            std::string resref = textField(utc, item, "EquippedRes");
            if (resref.empty()) {
                resref = lower(textField(utc, item, "Tag"));
            }
            if (resref.empty()) {
                continue;
            }
            std::int32_t slot = -1;
            const std::uint32_t mask = utc.structure(item).typeId;
            for (std::int32_t bit = 0; bit < 32 && mask != 0; ++bit) {
                if ((mask & (1u << bit)) != 0) {
                    slot = bit;
                    break;
                }
            }
            entity.creatureItems.push_back({resref, intField(utc, item, "Dropable", 1) != 0, slot});
        }
    }
}

void applyEquippedWeapon(Entity& entity, Install& install, const Gff& utc,
                         const TwoDa& baseitems, std::uint32_t root)
{
    const GffField* list = utc.field(root, "Equip_ItemList");
    if (list == nullptr) {
        return;
    }
    for (const std::uint32_t item : utc.list(*list)) {
        // The entry's struct type is the slot's bit mask: 0x10 right hand, 0x20 left hand.
        const std::uint32_t mask = utc.structure(item).typeId;
        const bool right = (mask & 0x10u) != 0;
        const bool left = (mask & 0x20u) != 0;
        const bool head = (mask & 0x01u) != 0;
        if (!right && !left && !head) {
            continue;
        }
        std::int32_t baseItem = intField(utc, item, "BaseItem", -1);
        std::int32_t variation = intField(utc, item, "ModelVariation", -1);
        if (baseItem < 0 || variation < 0) {
            const std::string resref = textField(utc, item, "EquippedRes");
            if (!resref.empty()) {
                if (const std::optional<ItemTemplate> templ = loadItemTemplate(install, resref);
                    templ.has_value()) {
                    baseItem = baseItem < 0 ? templ->baseItem : baseItem;
                    variation = variation < 0 ? templ->modelVariation : variation;
                }
            }
        }
        if (baseItem < 0) {
            continue;
        }
        const std::string row = std::to_string(baseItem);
        if (baseitems.rowIndex(row) < 0) {
            continue;
        }
        if (head) {
            entity.headItemModel = itemHandModel(install, baseitems, baseItem, variation);
            continue;
        }
        const std::string weaponType = baseitems.cell(row, "weapontype");
        if (!weaponType.empty() && weaponType != "0" && weaponType != "****") {
            const std::string model = itemHandModel(install, baseitems, baseItem, variation);
            const std::int32_t wield = std::atoi(baseitems.cell(row, "weaponwield").c_str());
            if (right) {
                entity.weaponModel = model;
                entity.rightWield = wield;
            } else {
                entity.offhandModel = model;
                entity.leftWield = wield;
            }
        }
        if (weaponType == "4") {
            entity.ranged = true;
            entity.attackRange = 15.0f;
        }
    }
}

std::string creatureModel(const TwoDa& appearances, std::uint32_t appearance, std::string& texture,
                          BodyVariant body = {})
{
    const std::string row = std::to_string(appearance);
    if (appearances.rowIndex(row) < 0) {
        return {};
    }
    // Retail 00697610: the armour's letter, clamped to 1..10 (a..j); without armour, a.
    const std::string letter(1, static_cast<char>('a' + std::clamp(body.variation, 1, 10) - 1));
    std::string model = body.variation > 0 ? appearances.cell(row, "model" + letter) : std::string{};
    if (!model.empty() && model != "****") {
        texture = appearances.cell(row, "tex" + letter);
    } else {
        model = appearances.cell(row, "modela");
        texture = appearances.cell(row, "texa");
    }
    // Rows that aren't body-part models take their map from racetex: Alien_Duros_02 and _03 share
    // the model and differ only there (retail frames: green Duros, tar_m02aa).
    const std::string raceTexture = appearances.cell(row, "racetex");
    const std::string modelType = appearances.cell(row, "modeltype");
    if (texture.empty() || (modelType != "B" && !raceTexture.empty() && raceTexture != "****")) {
        texture = raceTexture;
    }
    if (model.empty()) {
        model = appearances.cell(row, "race");
    }
    return model;
}

// The Body slot's item among a creature's template items (slot bit 1, equipableslots 0x2).
BodyVariant wornBodyVariant(Install& install, const Entity& entity)
{
    for (const auto& item : entity.creatureItems) {
        if (item.slot == 1) {
            return armorBodyVariant(install, item.resref);
        }
    }
    return {};
}

std::string creatureHeadModel(Install& install, const TwoDa& appearances,
                              std::uint32_t appearance)
{
    const std::string row = std::to_string(appearance);
    if (appearances.rowIndex(row) < 0) {
        return {};
    }
    int head = 0;
    const auto parseInt = [](const std::string& text) {
        if (text.empty() || text == "****") {
            return -1;
        }
        try {
            return std::stoi(text);
        } catch (const std::exception&) {
            return -1;
        }
    };
    head = parseInt(appearances.cell(row, "normalhead"));
    if (head < 0) {
        head = parseInt(appearances.cell(row, "backuphead"));
    }
    if (head < 0) {
        return {};
    }
    const std::shared_ptr<const TwoDa> heads = install.table("heads");
    if (heads == nullptr) {
        return {};
    }
    const std::string headRow = std::to_string(head);
    if (heads->rowIndex(headRow) < 0) {
        return {};
    }
    std::string model = heads->cell(headRow, "head");
    if (model == "****") {
        model.clear();
    }
    return model;
}

void applyCreatureAudio(Entity& entity, const TwoDa& appearances,
                        const std::shared_ptr<const TwoDa>& soundSets, const Gff& utc,
                        std::uint32_t root)
{
    const std::int32_t soundSetId = intField(utc, root, "SoundSetFile", -1);
    if (soundSets != nullptr && soundSetId >= 0 && soundSetId != 65535) {
        const std::string row = std::to_string(soundSetId);
        if (soundSets->rowIndex(row) >= 0) {
            entity.soundSet = soundSets->cell(row, "resref");
        }
    }
    const std::string appearanceRow = std::to_string(entity.appearance);
    if (appearances.rowIndex(appearanceRow) >= 0) {
        const std::string footstep = appearances.cell(appearanceRow, "footsteptype");
        entity.footstepType = footstep.empty() || footstep == "****" ? -1 : std::atoi(footstep.c_str());
        const std::string size = appearances.cell(appearanceRow, "sizecategory");
        entity.sizeCategory = size.empty() || size == "****" ? 0 : std::atoi(size.c_str());
        entity.soundAppType = std::atoi(appearances.cell(appearanceRow, "soundapptype").c_str());
    }
}

void addTriggerGeometry(Scene& scene, const Gff& git)
{    std::size_t triggerOrdinal = 0;
    for (const std::uint32_t structure : [&]() {
             const GffField* field = git.field(git.rootIndex(), "TriggerList");
             return field != nullptr ? git.list(*field) : std::vector<std::uint32_t>{};
         }()) {
        std::size_t entityIndex = scene.entities.size();
        std::size_t seen = 0;
        for (std::size_t i = 0; i < scene.entities.size(); ++i) {
            if (scene.entities[i].kind != EntityKind::Trigger) {
                continue;
            }
            if (seen++ == triggerOrdinal) {
                entityIndex = i;
                break;
            }
        }
        ++triggerOrdinal;
        if (entityIndex >= scene.entities.size()) {
            continue;
        }
        const Entity& entity = scene.entities[entityIndex];
        TriggerVolume volume;
        volume.entity = static_cast<std::uint32_t>(entityIndex);
        const GffField* geometryField = git.field(structure, "Geometry");
        if (geometryField == nullptr) {
            continue;
        }
        for (const std::uint32_t point : git.list(*geometryField)) {
            volume.vertices.push_back({entity.position.x + numberField(git, point, "PointX"),
                                       entity.position.y + numberField(git, point, "PointY"),
                                       entity.position.z + numberField(git, point, "PointZ")});
        }
        if (volume.vertices.size() >= 3) {
            for (std::size_t i = 1; i + 1 < volume.vertices.size(); ++i) {
                volume.indices.push_back(0);
                volume.indices.push_back(static_cast<std::uint32_t>(i));
                volume.indices.push_back(static_cast<std::uint32_t>(i + 1));
            }
            scene.triggers.push_back(std::move(volume));
        }
    }
}

void addEncounterGeometry(Scene& scene, Install& install, const Gff& git)
{
    std::size_t encounterOrdinal = 0;
    for (const std::uint32_t structure : [&]() {
             const GffField* field = git.field(git.rootIndex(), "Encounter List");
             return field != nullptr ? git.list(*field) : std::vector<std::uint32_t>{};
         }()) {
        std::size_t entityIndex = scene.entities.size();
        std::size_t seen = 0;
        for (std::size_t i = 0; i < scene.entities.size(); ++i) {
            if (scene.entities[i].kind != EntityKind::Encounter) {
                continue;
            }
            if (seen++ == encounterOrdinal) {
                entityIndex = i;
                break;
            }
        }
        ++encounterOrdinal;
        if (entityIndex >= scene.entities.size()) {
            continue;
        }
        const Entity& entity = scene.entities[entityIndex];
        EncounterVolume volume;
        volume.entity = static_cast<std::uint32_t>(entityIndex);
        volume.templateResref = entity.templateResref;

        std::optional<Gff> ownedUte;
        const Gff* uteSource = nullptr;
        std::uint32_t uteRoot = 0;
        if (entity.templateResref.empty()) { // a saved encounter holds its own spawn list
            uteSource = &git;
            uteRoot = structure;
        } else if (auto data = install.find(entity.templateResref, 2040); data.has_value()) { // UTE
            try {
                ownedUte.emplace(Gff::parse(std::move(*data), entity.templateResref + ".ute"));
                uteSource = &*ownedUte;
                uteRoot = ownedUte->rootIndex();
            } catch (const FormatError&) {
            }
        }
        if (uteSource == nullptr) {
            continue;
        }
        try {
            const Gff& ute = *uteSource;
            EncounterTemplate& encounter = volume.templateData;
            encounter.resref = entity.templateResref;
            encounter.tag = textField(ute, uteRoot, "Tag");
            encounter.active = intField(ute, uteRoot, "Active", 1) != 0;
            encounter.difficulty = intField(ute, uteRoot, "Difficulty", 0);
            encounter.maxCreatures = intField(ute, uteRoot, "MaxCreatures", 0);
            encounter.recCreatures = intField(ute, uteRoot, "RecCreatures", 0);
            encounter.resetTime = intField(ute, uteRoot, "ResetTime", 0);
            encounter.respawns = intField(ute, uteRoot, "Respawns", 0);
            encounter.spawnOption = intField(ute, uteRoot, "SpawnOption", 0);
            encounter.difficultyIndex = intField(ute, uteRoot, "DifficultyIndex", -1);
            encounter.faction = intField(ute, uteRoot, "Faction", 0);
            encounter.playerOnly = intField(ute, uteRoot, "PlayerOnly", 0) != 0;
            encounter.reset = intField(ute, uteRoot, "Reset", 0) != 0;
            if (const GffField* list = ute.field(uteRoot, "CreatureList"); list != nullptr) {
                for (const std::uint32_t creatureStruct : ute.list(*list)) {
                    EncounterCreature creature;
                    creature.resref = textField(ute, creatureStruct, "ResRef");
                    creature.appearance = intField(ute, creatureStruct, "Appearance", 0);
                    creature.challengeRating = numberField(ute, creatureStruct, "CR", 0.0f);
                    creature.singleSpawn = intField(ute, creatureStruct, "SingleSpawn", 0) != 0;
                    if (!creature.resref.empty()) {
                        encounter.creatures.push_back(std::move(creature));
                    }
                }
            }
        } catch (const FormatError&) {
            continue;
        }

        if (const GffField* geometryField = git.field(structure, "Geometry");
            geometryField != nullptr) {
            for (const std::uint32_t point : git.list(*geometryField)) {
                volume.vertices.push_back({entity.position.x + numberField(git, point, "X"),
                                           entity.position.y + numberField(git, point, "Y"),
                                           entity.position.z + numberField(git, point, "Z")});
            }
            if (volume.vertices.size() >= 3) {
                for (std::size_t i = 1; i + 1 < volume.vertices.size(); ++i) {
                    volume.indices.push_back(0);
                    volume.indices.push_back(static_cast<std::uint32_t>(i));
                    volume.indices.push_back(static_cast<std::uint32_t>(i + 1));
                }
            }
        }
        if (const GffField* spawnField = git.field(structure, "SpawnPointList");
            spawnField != nullptr) {
            for (const std::uint32_t point : git.list(*spawnField)) {
                volume.spawnPoints.push_back({
                    {numberField(git, point, "X"), numberField(git, point, "Y"),
                     numberField(git, point, "Z")},
                    numberField(git, point, "Orientation", 0.0f)});
            }
        }
        scene.encounters.push_back(std::move(volume));
    }
}

} // namespace

void refreshEntityMesh(EntityMesh& mesh)
{
    refreshEntityMeshImpl(mesh);
}

EntityMesh buildWorldEntityMesh(Scene& scene, Install& install, const std::shared_ptr<Mdl>& model)
{
    EntityMesh mesh;
    mesh.entity = UINT32_MAX;
    if (!model) {
        return mesh;
    }
    std::map<std::string, std::vector<MdlMeshTriangle>> byTexture;
    for (MdlMeshTriangle& triangle : model->posedTrianglesLayered({}, true)) {
        byTexture[materialKey(triangle)].push_back(std::move(triangle));
    }
    for (const auto& [materialName, list] : byTexture) {
        (void)materialName;
        TextureBatch batch = makeMaterialBatch(scene, install, list.front(), true);
        batch.firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
        for (const MdlMeshTriangle& triangle : list) {
            for (int i = 0; i < 3; ++i) {
                mesh.vertices.push_back(toSceneVertex(triangle.v[i], triangle));
                mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size() - 1));
            }
        }
        batch.indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - batch.firstIndex;
        mesh.batches.push_back(batch);
    }
    return mesh;
}

void bakeWorldEntityMesh(EntityMesh& mesh, const std::shared_ptr<Mdl>& model,
                         const std::vector<MdlAnimLayer>& layers, const Mat3& rotation, Vec3 translation)
{
    if (!model || mesh.vertices.empty()) {
        return;
    }
    std::map<std::string, std::vector<MdlMeshTriangle>> byTexture;
    for (MdlMeshTriangle& triangle : model->posedTrianglesLayered(layers, true)) {
        byTexture[materialKey(triangle)].push_back(std::move(triangle));
    }
    std::size_t at = 0;
    for (const auto& [textureName, list] : byTexture) {
        (void)textureName;
        for (const MdlMeshTriangle& triangle : list) {
            for (int i = 0; i < 3 && at < mesh.vertices.size(); ++i, ++at) {
                SceneVertex& vertex = mesh.vertices[at];
                vertex.position = translation + rotate(rotation, triangle.v[i].position);
                vertex.normal = rotate(rotation, triangle.v[i].normal);
                vertex.uv = triangle.v[i].uv;
                vertex.uv1 = triangle.v[i].uv1;
                vertex.selfIllum = meshEmission(triangle);
                vertex.alpha = triangle.alpha;
            }
        }
    }
    mesh.hiddenFaces.clear();
    for (const MdlMeshTriangle& triangle : model->posedHiddenTrianglesLayered(layers)) {
        mesh.hiddenFaces.push_back({translation + rotate(rotation, triangle.v[0].position),
                                    translation + rotate(rotation, triangle.v[1].position),
                                    translation + rotate(rotation, triangle.v[2].position)});
    }
    ++mesh.revision;
}

std::shared_ptr<Mdl> loadWorldModel(Install& install, const std::string& modelName)
{
    std::string error;
    return loadModel(install, modelName, error);
}

void collectScriptHooks(const Gff& gff, std::uint32_t structure,
                        std::map<std::string, std::string>& scripts)
{
    if (structure >= gff.structureCount()) {
        return;
    }
    for (const std::string& label : scriptHookLabels()) {
        const GffField* field = gff.field(structure, label);
        if (field == nullptr) {
            continue;
        }
        const std::string resref = gff.text(*field);
        if (!resref.empty()) {
            scripts[label] = resref;
        }
    }
}

std::string itemHandModel(Install& install, const TwoDa& baseitems, std::int32_t baseItem,
                          std::int32_t modelVariation)
{
    const std::string row = std::to_string(baseItem);
    if (baseitems.rowIndex(row) < 0) {
        return {};
    }
    const std::string itemClass = baseitems.cell(row, "itemclass");
    if (modelVariation > 0 && !itemClass.empty() && itemClass != "****") {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%03d", static_cast<int>(modelVariation));
        const std::string name = itemClass + suffix;
        if (install.find(name, resType(ResType::Mdl)).has_value()) {
            return name;
        }
    }
    const std::string fallback = baseitems.cell(row, "defaultmodel");
    return fallback == "I_Null" || fallback == "****" ? std::string{} : fallback;
}

std::int32_t wieldClass(std::int32_t rightWield, std::int32_t leftWield, bool hasRight, bool hasLeft)
{
    if (!hasRight) {
        return hasLeft ? 0 : 8;
    }
    if (hasLeft) {
        return leftWield == 2 ? 4 : leftWield == 4 ? 6 : 0;
    }
    switch (rightWield) {
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case 4: return 5;
    case 5: return 7;
    case 6: return 9;
    default: return 8;
    }
}

std::optional<ItemTemplate> loadItemTemplate(Install& install, const std::string& resref)
{
    auto data = install.find(resref, 2025); // UTI
    if (!data.has_value()) {
        return std::nullopt;
    }
    try {
        Gff uti = Gff::parse(std::move(*data), resref + ".uti");
        ItemTemplate item;
        item.resref = resref;
        item.tag = textField(uti, uti.rootIndex(), "Tag");
        item.baseItem = intField(uti, uti.rootIndex(), "BaseItem", 0);
        item.cost = intField(uti, uti.rootIndex(), "Cost", 0);
        item.stackSize = std::max(1, intField(uti, uti.rootIndex(), "StackSize", 1));
        item.identified = intField(uti, uti.rootIndex(), "Identified", 1) != 0;
        item.plot = intField(uti, uti.rootIndex(), "Plot", 0) != 0;
        item.charges = intField(uti, uti.rootIndex(), "Charges", 0);
        item.addCost = intField(uti, uti.rootIndex(), "AddCost", 0);
        item.modelVariation = intField(uti, uti.rootIndex(), "ModelVariation", 0);
        if (const GffField* list = uti.field(uti.rootIndex(), "PropertiesList"); list != nullptr) {
            for (const std::uint32_t entry : uti.list(*list)) {
                ItemTemplate::Property property;
                property.name = intField(uti, entry, "PropertyName", 0);
                property.subtype = intField(uti, entry, "Subtype", 0);
                property.costTable = intField(uti, entry, "CostTable", 0);
                property.costValue = intField(uti, entry, "CostValue", 0);
                property.param1 = intField(uti, entry, "Param1", 0);
                property.param1Value = intField(uti, entry, "Param1Value", 0);
                property.chanceAppear = intField(uti, entry, "ChanceAppear", 100);
                property.usesPerDay = intField(uti, entry, "UsesPerDay", 255);
                property.useable = intField(uti, entry, "Useable", 1) != 0;
                property.upgradeType = intField(uti, entry, "UpgradeType", 255);
                item.properties.push_back(property);
            }
        }
        const std::shared_ptr<const Tlk> tlk = install.dialogTlk();
        item.name = locField(uti, uti.rootIndex(), "LocalizedName", tlk ? &*tlk : nullptr);
        return item;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

CreatureHitPoints creatureHitPointsFromGff(const Gff& gff, std::uint32_t structure)
{
    // 005afce0 reads HitPoints into the creature's base (+0xe0) and CurrentHitPoints (default: the
    // base), both as signed words. The ability modifier is (score - 10) / 2 with C division
    // (a score under 10 takes 11), stored at stats +0xee for Constitution.
    const auto word = [](std::int32_t value) { return static_cast<std::int32_t>(static_cast<std::int16_t>(value)); };
    const std::int32_t base = word(intField(gff, structure, "HitPoints", 0));
    const std::int32_t stored = word(intField(gff, structure, "CurrentHitPoints", base));
    const std::int32_t score = intField(gff, structure, "Con", 10) & 0xff;
    const std::int32_t conMod = (score + (score < 10 ? -11 : -10)) / 2;
    std::int32_t level = 0;
    if (const GffField* classes = gff.field(structure, "ClassList"); classes != nullptr) {
        for (const std::uint32_t entry : gff.list(*classes)) {
            level += intField(gff, entry, "ClassLevel", 0);
        }
    }
    level &= 0xff;
    bool feat124 = false;
    bool feat84 = false;
    bool feat95 = false;
    if (const GffField* feats = gff.field(structure, "FeatList"); feats != nullptr) {
        for (const std::uint32_t entry : gff.list(*feats)) {
            const std::int32_t feat = intField(gff, entry, "Feat", -1);
            feat124 = feat124 || feat == 0x7c;
            feat84 = feat84 || feat == 0x54;
            feat95 = feat95 || feat == 0x5f;
        }
    }
    // 004ed310 with bIncludeBonus set: Master Toughness 2 per level, else Toughness 1, and 2 more for
    // Wookiee Endurance (feat 0x5f), each added to the Constitution modifier.
    const auto maxHitPoints = [&](bool withFeats) {
        std::int32_t bonus = 0;
        if (withFeats) {
            bonus = feat124 ? 2 : (feat84 ? 1 : 0);
            bonus += feat95 ? 2 : 0;
        }
        if (level <= level * conMod + base) {
            return word(level * (conMod + bonus) + base);
        }
        return word(level * bonus + level);
    };
    std::int32_t current = 0;
    const std::int32_t share = level * conMod + base;
    if (stored < base) {
        current = level < share ? level * conMod + stored : level + (stored - base);
    } else {
        current = level < share ? level * conMod + stored : level;
    }
    // The feats are added after that, and the current value moves by what they added to the maximum.
    const std::int32_t before = maxHitPoints(false);
    const std::int32_t after = maxHitPoints(true);
    if (before != after) {
        current += after - before;
    }
    if (intField(gff, structure, "Min1HP", 0) != 0 && current < 1) {
        current = 1;
    }
    return {word(current), after};
}

std::size_t spawnCreature(Scene& scene, Install& install, const std::string& utcResref,
                          Vec3 position, float bearing)
{
    auto data = install.find(utcResref, 2027); // UTC
    if (!data.has_value()) {
        return static_cast<std::size_t>(-1);
    }
    try {
        Gff utc = Gff::parse(std::move(*data), utcResref + ".utc");
        const std::uint32_t root = utc.rootIndex();
        const std::shared_ptr<const Tlk> tlk = install.dialogTlk();
        const std::shared_ptr<const TwoDa> appearances = install.table("appearance");
        const std::shared_ptr<const TwoDa> soundSets = install.table("soundset");
        const std::shared_ptr<const TwoDa> baseitems = install.table("baseitems");

        Entity entity;
        entity.kind = EntityKind::Creature;
        entity.sourceStructure = root;
        entity.templateResref = utcResref;
        entity.position = position;
        entity.bearing = bearing;
        entity.tag = textField(utc, root, "Tag");
        if (entity.tag.empty()) {
            entity.tag = utcResref;
        }
        entity.name = locField(utc, root, "FirstName", tlk ? &*tlk : nullptr);
        entity.conversation = textField(utc, root, "Conversation");
        entity.appearance = static_cast<std::uint32_t>(
            intField(utc, root, "Appearance_Type", 0));
        entity.faction = static_cast<std::uint32_t>(intField(utc, root, "FactionID", 0));
        {
            const CreatureHitPoints hp = creatureHitPointsFromGff(utc, root);
            entity.maxHitPoints = std::max(1, hp.max);
            entity.hitPoints = std::max(1, hp.current);
        }
        // defense, attackBonus and damageMin/Max come from Rules::deriveCreatureCombat.
        // Retail 005afce0 reads the per-creature save bonuses fortbonus, willbonus and refbonus
        // (stats +0x1a0..+0x1a2); *SaveThrow are totals 005b1b90 only writes.
        entity.fortSave = intField(utc, root, "fortbonus", 0);
        entity.willSave = intField(utc, root, "willbonus", 0);
        entity.refSave = intField(utc, root, "refbonus", 0);
        applyGameplayFields(entity, utc, root);
        applyCreatureStats(entity, utc, root);
        entity.scriptDialogue = textField(utc, root, "ScriptDialogue");
        entity.scriptOnEnter = textField(utc, root, "ScriptOnNotice");
        entity.walkRate = 3.0f + numberField(utc, root, "WalkRate", 7.0f) * 0.15f;
        entity.walkRateRow = intField(utc, root, "WalkRate", 7);
        entity.hostile = false;
        readCreatureItems(entity, utc, root);
        if (baseitems != nullptr) {
            applyEquippedWeapon(entity, install, utc, *baseitems, root);
        }
        collectScriptHooks(utc, root, entity.scripts);
        if (appearances != nullptr) {
            std::string texture;
            entity.bodyVariant = wornBodyVariant(install, entity);
            entity.model = creatureModel(*appearances, entity.appearance, texture, entity.bodyVariant);
            entity.texture = texture;
            applyCreatureAudio(entity, *appearances, soundSets, utc, root);
        }
        scene.entities.push_back(std::move(entity));
        const std::size_t index = scene.entities.size() - 1;
        if (!scene.entities[index].model.empty()) {
            std::string error;
            if (const std::shared_ptr<Mdl> model =
                    loadModel(install, scene.entities[index].model, error)) {
                std::shared_ptr<Mdl> head;
                // Retail 00698219 takes the head from the appearance row and 0069b0fa hangs it on
                // HEADHOOK, whatever else the body has. `head_g` is not a head: in the 27 bodies
                // with a head row it is a 14 to 168 face proxy, untextured in all but one.
                if (appearances != nullptr) {
                    const std::string headName = creatureHeadModel(
                        install, *appearances, scene.entities[index].appearance);
                    if (!headName.empty()) {
                        std::string headError;
                        head = loadModel(install, headName, headError);
                        if (!head && !headError.empty()) {
                            scene.warnings.push_back(headName + ": " + headError);
                        }
                    }
                }
                buildMesh(scene, install, static_cast<std::uint32_t>(index), model,
                          nullptr, {}, head, head ? "headhook" : "");
                const Entity& spawned = scene.entities[index];
                if (!spawned.weaponModel.empty()) {
                    std::string weaponError;
                    if (const std::shared_ptr<Mdl> weapon =
                            loadModel(install, spawned.weaponModel, weaponError)) {
                        buildMesh(scene, install, static_cast<std::uint32_t>(index), weapon,
                                  model, spawned.weaponAttach);
                    }
                }
                if (!spawned.offhandModel.empty()) {
                    std::string weaponError;
                    if (const std::shared_ptr<Mdl> weapon =
                            loadModel(install, spawned.offhandModel, weaponError)) {
                        buildMesh(scene, install, static_cast<std::uint32_t>(index), weapon,
                                  model, spawned.offhandAttach);
                    }
                }
                attachHeadItem(scene, install, static_cast<std::uint32_t>(index), model, head);
            } else {
                scene.warnings.push_back(scene.entities[index].model + ": " + error);
            }
        }
        return index;
    } catch (const FormatError&) {
        return static_cast<std::size_t>(-1);
    }
}

BodyVariant armorBodyVariant(Install& install, const std::string& itemResref)
{
    BodyVariant variant;
    auto data = install.find(itemResref, 2025); // UTI
    if (!data.has_value()) {
        return variant;
    }
    try {
        const Gff uti = Gff::parse(std::move(*data), itemResref + ".uti");
        variant.variation = intField(uti, uti.rootIndex(), "BodyVariation", 0);
        variant.texture = intField(uti, uti.rootIndex(), "TextureVar", 0);
    } catch (const FormatError&) {
    }
    return variant;
}

std::size_t spawnPlayerAvatar(Scene& scene, Install& install, std::uint32_t appearance,
                              const std::string& name, Vec3 position, float bearing,
                              BodyVariant body)
{
    const std::shared_ptr<const TwoDa> appearances = install.table("appearance");
    if (appearances == nullptr) {
        return static_cast<std::size_t>(-1);
    }
    Entity entity;
    entity.kind = EntityKind::Creature;
    entity.templateResref = "player";
    entity.tag = "PC";
    entity.name = name.empty() ? "Player" : name;
    entity.position = position;
    entity.bearing = bearing;
    entity.appearance = appearance;
    entity.faction = 0;
    entity.hitPoints = 30;
    entity.maxHitPoints = 30;
    entity.defense = 10;
    entity.walkRate = 3.8f;
    entity.hostile = false;
    std::string texture;
    entity.bodyVariant = body;
    entity.model = creatureModel(*appearances, appearance, texture, body);
    entity.texture = texture;
    scene.entities.push_back(std::move(entity));
    const std::size_t index = scene.entities.size() - 1;
    if (scene.entities[index].model.empty()) {
        return index;
    }
    std::string error;
    const std::shared_ptr<Mdl> model = loadModel(install, scene.entities[index].model, error);
    if (!model) {
        scene.warnings.push_back(scene.entities[index].model + ": " + error);
        return index;
    }
    std::shared_ptr<Mdl> head;
    const std::string headName = creatureHeadModel(install, *appearances, appearance);
    if (!headName.empty()) {
        std::string headError;
        head = loadModel(install, headName, headError);
        if (!head && !headError.empty()) {
            scene.warnings.push_back(headName + ": " + headError);
        }
    }
    buildMesh(scene, install, static_cast<std::uint32_t>(index), model,
              nullptr, {}, head, "headhook");
    return index;
}

namespace {

// A reference node on a door or placeable shows another rigid model at that node, moving with it.
// Shipped: dor_ukn01 shows dor_ukn02; the other holders name fx_ref, which has no geometry. No
// creature model holds one. Each is its own mesh on the attachment path, at the model's own scale.
void buildReferencedMeshes(Scene& scene, Install& install, std::uint32_t entityIndex, const std::shared_ptr<Mdl>& holder)
{
    for (const Mdl::ReferenceNode& reference : holder->references()) {
        std::string error;
        const std::shared_ptr<Mdl> shown = loadModel(install, lower(reference.model), error);
        if (!shown || shown->hasSkinnedMesh()) {
            continue;
        }
        const std::size_t before = scene.entityMeshes.size();
        buildMesh(scene, install, entityIndex, shown, holder, reference.nodeName);
        if (scene.entityMeshes.size() != before) {
            scene.entityMeshes.back().attachScale = 1.0f;
            refreshEntityMesh(scene.entityMeshes.back());
        }
    }
}

} // namespace

std::size_t spawnStuntBody(Scene& scene, Install& install, const std::string& body,
                           const std::shared_ptr<Mdl>& head, const std::string& headNode,
                           const Mdl& clips)
{
    std::string error;
    // A model of its own: two participants can share a body and differ in their clips.
    const std::shared_ptr<Mdl> model = loadModel(install, body, error);
    if (!model) {
        scene.warnings.push_back(body + ": " + error);
        return static_cast<std::size_t>(-1);
    }
    model->mergeAnimations(clips);
    Entity entity;
    entity.kind = EntityKind::Placeable;
    entity.tag = clips.name();
    entity.name = clips.name();
    entity.model = body;
    scene.entities.push_back(std::move(entity));
    const std::size_t index = scene.entities.size() - 1;
    buildMesh(scene, install, static_cast<std::uint32_t>(index), model, nullptr, {}, head, headNode);
    return index;
}

std::size_t spawnModelEntity(Scene& scene, Install& install, const std::string& modelName,
                             Vec3 position, float bearing)
{
    std::string error;
    const std::shared_ptr<Mdl> model = loadModel(install, modelName, error);
    if (!model) {
        scene.warnings.push_back(modelName + ": " + error);
        return static_cast<std::size_t>(-1);
    }
    Entity entity;
    entity.kind = EntityKind::Placeable;
    entity.tag = modelName;
    entity.name = modelName;
    entity.model = modelName;
    entity.position = position;
    entity.bearing = bearing;
    scene.entities.push_back(std::move(entity));
    const std::size_t index = scene.entities.size() - 1;
    buildMesh(scene, install, static_cast<std::uint32_t>(index), model, nullptr, {}, nullptr, {});
    buildReferencedMeshes(scene, install, static_cast<std::uint32_t>(index), model);
    return index;
}

void loadEntityAssets(Scene& scene, Install& install, const Gff& git)
{
    const std::shared_ptr<const Tlk> tlk = install.dialogTlk();
    const std::shared_ptr<const TwoDa> appearances = install.table("appearance");
    const std::shared_ptr<const TwoDa> soundSets = install.table("soundset");
    const std::shared_ptr<const TwoDa> genericDoors = install.table("genericdoors");
    const std::shared_ptr<const TwoDa> placeables = install.table("placeables");
    const std::shared_ptr<const TwoDa> baseitems = install.table("baseitems");

    for (std::size_t index = 0; index < scene.entities.size(); ++index) {
        Entity& entity = scene.entities[index];
        const std::uint32_t structure = entity.sourceStructure;
        if (entity.kind == EntityKind::Creature) {
            // A retail save writes every object whole and without a TemplateResRef (UseTemplates is
            // 0): the struct itself then plays the part of the UTx template (retail 00504a70 and
            // the Load* of each object read the saved struct).
            std::optional<Gff> owned;
            const Gff* source = nullptr;
            std::uint32_t srcRoot = 0;
            if (entity.templateResref.empty()) {
                source = &git;
                srcRoot = structure;
            } else if (auto data = install.find(entity.templateResref, 2027); data.has_value()) { // UTC
                try {
                    owned.emplace(Gff::parse(std::move(*data), entity.templateResref + ".utc"));
                    source = &*owned;
                    srcRoot = owned->rootIndex();
                } catch (const FormatError&) {
                }
            }
            if (source != nullptr) {
                try {
                    const Gff& utc = *source;
                    collectScriptHooks(utc, srcRoot, entity.scripts);
                    entity.tag = textField(utc, srcRoot, "Tag");
                    entity.name = locField(utc, srcRoot, "FirstName", tlk ? &*tlk : nullptr);
                    entity.conversation = textField(utc, srcRoot, "Conversation");
                    entity.appearance = static_cast<std::uint32_t>(
                        intField(utc, srcRoot, "Appearance_Type", 0));
                    entity.faction = static_cast<std::uint32_t>(intField(utc, srcRoot, "FactionID", 0));
                    {
                        const CreatureHitPoints hp = creatureHitPointsFromGff(utc, srcRoot);
                        entity.maxHitPoints = std::max(1, hp.max);
                        entity.hitPoints = std::max(1, hp.current);
                    }
                    // defense, attackBonus and damageMin/Max: Rules::deriveCreatureCombat.
                    entity.fortSave = intField(utc, srcRoot, "fortbonus", 0);
                    entity.willSave = intField(utc, srcRoot, "willbonus", 0);
                    entity.refSave = intField(utc, srcRoot, "refbonus", 0);
                    applyGameplayFields(entity, utc, srcRoot);
                    applyCreatureStats(entity, utc, srcRoot);
                    entity.scriptDialogue = textField(utc, srcRoot, "ScriptDialogue");
                    entity.scriptOnEnter = textField(utc, srcRoot, "ScriptOnNotice");
                    entity.walkRate = 3.0f + numberField(utc, srcRoot, "WalkRate", 7.0f) * 0.15f;
                    entity.walkRateRow = intField(utc, srcRoot, "WalkRate", 7);
                    entity.hostile = false; // GameSession applies faction relations
                    readCreatureItems(entity, utc, srcRoot);
                    if (baseitems != nullptr) {
                        applyEquippedWeapon(entity, install, utc, *baseitems, srcRoot);
                    }
                    if (appearances != nullptr) {
                        std::string texture;
                        entity.bodyVariant = wornBodyVariant(install, entity);
                        entity.model = creatureModel(*appearances, entity.appearance, texture, entity.bodyVariant);
                        entity.texture = texture;
                        applyCreatureAudio(entity, *appearances, soundSets, utc, srcRoot);
                    }
                } catch (const FormatError&) {
                }
            }
        } else if (entity.kind == EntityKind::Door) {
            // A retail save writes every object whole and without a TemplateResRef (UseTemplates is
            // 0): the struct itself then plays the part of the UTx template (retail 00504a70 and
            // the Load* of each object read the saved struct).
            std::optional<Gff> owned;
            const Gff* source = nullptr;
            std::uint32_t srcRoot = 0;
            if (entity.templateResref.empty()) {
                source = &git;
                srcRoot = structure;
            } else if (auto data = install.find(entity.templateResref, 2042); data.has_value()) { // UTD
                try {
                    owned.emplace(Gff::parse(std::move(*data), entity.templateResref + ".utd"));
                    source = &*owned;
                    srcRoot = owned->rootIndex();
                } catch (const FormatError&) {
                }
            }
            if (source != nullptr) {
                try {
                    const Gff& utd = *source;
                    collectScriptHooks(utd, srcRoot, entity.scripts);
                    entity.tag = textField(utd, srcRoot, "Tag");
                    entity.name = locField(utd, srcRoot, "LocName", tlk ? &*tlk : nullptr); // the target block's name
                    entity.faction = static_cast<std::uint32_t>(intField(utd, srcRoot, "Faction", 0));
                    applyGameplayFields(entity, utd, srcRoot);
                    // A door is bashable: UTD HP / CurrentHP are its hit points.
                    entity.maxHitPoints = std::max(1, intField(utd, srcRoot, "HP", 1));
                    entity.hitPoints = std::clamp(
                        intField(utd, srcRoot, "CurrentHP", entity.maxHitPoints), 1,
                        entity.maxHitPoints);
                    const std::uint32_t genericType = static_cast<std::uint32_t>(
                        intField(utd, srcRoot, "GenericType", 0));
                    entity.appearance = genericType;
                    if (genericDoors != nullptr) {
                        entity.model = genericDoors->cell(std::to_string(genericType), "modelname");
                    }
                } catch (const FormatError&) {
                }
            }
        } else if (entity.kind == EntityKind::Placeable) {
            // A retail save writes every object whole and without a TemplateResRef (UseTemplates is
            // 0): the struct itself then plays the part of the UTx template (retail 00504a70 and
            // the Load* of each object read the saved struct).
            std::optional<Gff> owned;
            const Gff* source = nullptr;
            std::uint32_t srcRoot = 0;
            if (entity.templateResref.empty()) {
                source = &git;
                srcRoot = structure;
            } else if (auto data = install.find(entity.templateResref, 2044); data.has_value()) { // UTP
                try {
                    owned.emplace(Gff::parse(std::move(*data), entity.templateResref + ".utp"));
                    source = &*owned;
                    srcRoot = owned->rootIndex();
                } catch (const FormatError&) {
                }
            }
            if (source != nullptr) {
                try {
                    const Gff& utp = *source;
                    collectScriptHooks(utp, srcRoot, entity.scripts);
                    entity.tag = textField(utp, srcRoot, "Tag");
                    entity.name = locField(utp, srcRoot, "LocName", tlk ? &*tlk : nullptr);
                    entity.conversation = textField(utp, srcRoot, "Conversation");
                    applyGameplayFields(entity, utp, srcRoot);
                    entity.appearance = static_cast<std::uint32_t>(
                        intField(utp, srcRoot, "Appearance", 0));
                    entity.faction = static_cast<std::uint32_t>(intField(utp, srcRoot, "Faction", 0));
                    // Retail 00585670: HP is the maximum, CurrentHP the current value (both signed words,
                    // default 0), and Min1HP lifts a current value under 1 (SetCurrentHitPoints 004cc280).
                    entity.maxHitPoints = std::max<std::int32_t>(
                        1, static_cast<std::int16_t>(intField(utp, srcRoot, "HP", 0)));
                    entity.hitPoints = static_cast<std::int16_t>(intField(utp, srcRoot, "CurrentHP", 0));
                    if (entity.min1Hp && entity.hitPoints < 1) {
                        entity.hitPoints = 1;
                    }
                    entity.useable = intField(utp, srcRoot, "Useable", 0) != 0
                        || !entity.conversation.empty();
                    if (const GffField* list = utp.field(srcRoot, "ItemList"); list != nullptr) {
                        for (const std::uint32_t itemStruct : utp.list(*list)) {
                            std::string resref = textField(utp, itemStruct, "InventoryRes");
                            if (resref.empty() && entity.templateResref.empty()) {
                                resref = lower(textField(utp, itemStruct, "Tag")); // a saved item's Tag is its resref
                            }
                            if (!resref.empty()) {
                                entity.lootItems.push_back(resref);
                            }
                        }
                    }
                    entity.useable = entity.useable || !entity.lootItems.empty();
                    if (placeables != nullptr) {
                        entity.model = placeables->cell(std::to_string(entity.appearance), "modelname");
                    }
                } catch (const FormatError&) {
                }
            }
        } else if (entity.kind == EntityKind::Trigger) {
            std::optional<Gff> owned;
            const Gff* source = nullptr;
            std::uint32_t srcRoot = 0;
            if (entity.templateResref.empty()) { // a saved trigger (see the creature branch)
                source = &git;
                srcRoot = structure;
            } else if (auto data = install.find(entity.templateResref, 2032); data.has_value()) { // UTT
                try {
                    owned.emplace(Gff::parse(std::move(*data), entity.templateResref + ".utt"));
                    source = &*owned;
                    srcRoot = owned->rootIndex();
                } catch (const FormatError&) {
                }
            }
            if (source != nullptr) {
                try {
                    const Gff& utt = *source;
                    collectScriptHooks(utt, srcRoot, entity.scripts);
                    entity.tag = textField(utt, srcRoot, "Tag");
                    entity.scriptOnEnter = textField(utt, srcRoot, "ScriptOnEnter");
                    entity.scriptOnExit = textField(utt, srcRoot, "ScriptOnExit");
                    applyGameplayFields(entity, utt, srcRoot);
                    entity.triggerType = intField(utt, srcRoot, "Type", 0);
                } catch (const FormatError&) {
                }
            }
        } else if (entity.kind == EntityKind::Store) {
            std::optional<Gff> owned;
            const Gff* source = nullptr;
            std::uint32_t srcRoot = 0;
            if (entity.templateResref.empty()) { // a saved store holds its stock whole (retail 005c7180)
                source = &git;
                srcRoot = structure;
            } else if (auto data = install.find(entity.templateResref, 2051); data.has_value()) { // UTM
                try {
                    owned.emplace(Gff::parse(std::move(*data), entity.templateResref + ".utm"));
                    source = &*owned;
                    srcRoot = owned->rootIndex();
                } catch (const FormatError&) {
                }
            }
            if (source != nullptr) {
                try {
                    const Gff& utm = *source;
                    collectScriptHooks(utm, srcRoot, entity.scripts);
                    entity.tag = textField(utm, srcRoot, "Tag");
                    entity.name = locField(utm, srcRoot, "LocName", tlk ? &*tlk : nullptr);
                    entity.storeMarkUp = intField(utm, srcRoot, "MarkUp", 100);
                    entity.storeMarkDown = intField(utm, srcRoot, "MarkDown", 40);
                    entity.storeBuySell = intField(utm, srcRoot, "BuySellFlag", 3);
                    entity.useable = true;
                    if (const GffField* list = utm.field(srcRoot, "ItemList"); list != nullptr) {
                        for (const std::uint32_t itemStruct : utm.list(*list)) {
                            std::string resref = textField(utm, itemStruct, "InventoryRes");
                            if (resref.empty() && entity.templateResref.empty()) {
                                resref = lower(textField(utm, itemStruct, "Tag")); // a saved item's Tag is its resref
                            }
                            if (!resref.empty()) {
                                entity.storeItems.emplace_back(
                                    resref, intField(utm, itemStruct, "Infinite", 0) != 0);
                            }
                        }
                    }
                } catch (const FormatError&) {
                }
            }
        } else if (entity.kind == EntityKind::Encounter) {
            std::optional<Gff> owned;
            const Gff* source = nullptr;
            std::uint32_t srcRoot = 0;
            if (entity.templateResref.empty()) { // a saved encounter (see the creature branch)
                source = &git;
                srcRoot = structure;
            } else if (auto data = install.find(entity.templateResref, 2040); data.has_value()) { // UTE
                try {
                    owned.emplace(Gff::parse(std::move(*data), entity.templateResref + ".ute"));
                    source = &*owned;
                    srcRoot = owned->rootIndex();
                } catch (const FormatError&) {
                }
            }
            if (source != nullptr) {
                try {
                    const Gff& ute = *source;
                    collectScriptHooks(ute, srcRoot, entity.scripts);
                    entity.tag = textField(ute, srcRoot, "Tag");
                    entity.name = locField(ute, srcRoot, "LocalizedName", tlk ? &*tlk : nullptr);
                } catch (const FormatError&) {
                }
            }
        }
        collectScriptHooks(git, structure, entity.scripts);
        if (!entity.model.empty()
            && (entity.kind == EntityKind::Creature || entity.kind == EntityKind::Door
                || entity.kind == EntityKind::Placeable)) {
            std::string error;
            if (const std::shared_ptr<Mdl> model = loadModel(install, entity.model, error)) {
                std::shared_ptr<Mdl> head;
                if (entity.kind == EntityKind::Creature && appearances != nullptr) {
                    const std::string headName = creatureHeadModel(
                        install, *appearances, entity.appearance);
                    if (!headName.empty()) {
                        std::string headError;
                        head = loadModel(install, headName, headError);
                        if (!head && !headError.empty()) {
                            scene.warnings.push_back(headName + ": " + headError);
                        }
                    }
                }
                buildMesh(scene, install, static_cast<std::uint32_t>(index), model,
                          nullptr, {}, head, head ? "headhook" : "");
                if (entity.kind != EntityKind::Creature) {
                    buildReferencedMeshes(scene, install, static_cast<std::uint32_t>(index), model);
                }
                if (!entity.weaponModel.empty()) {
                    std::string weaponError;
                    if (const std::shared_ptr<Mdl> weapon =
                            loadModel(install, entity.weaponModel, weaponError)) {
                        buildMesh(scene, install, static_cast<std::uint32_t>(index), weapon,
                                  model, entity.weaponAttach);
                    }
                }
                if (!entity.offhandModel.empty()) {
                    std::string weaponError;
                    if (const std::shared_ptr<Mdl> weapon =
                            loadModel(install, entity.offhandModel, weaponError)) {
                        buildMesh(scene, install, static_cast<std::uint32_t>(index), weapon,
                                  model, entity.offhandAttach);
                    }
                }
                attachHeadItem(scene, install, static_cast<std::uint32_t>(index), model, head);
            } else {
                scene.warnings.push_back(entity.model + ": " + error);
            }
        }
    }
    addTriggerGeometry(scene, git);
    addEncounterGeometry(scene, install, git);
}

ObjectClip objectStateClip(const Entity& entity, const Mdl& model)
{
    const auto steady = [&](const char* clip) {
        // A pose clip has length 0 and still carries keys (dor_lhr01 closed/opened1/opened2: 4 and 7 controllers), so it
        // counts as present (doors only); retail shows the closed door in that pose, not the bind pose (retail row 27 frames).
        const bool present = entity.kind == EntityKind::Door ? model.animationIndex(clip) >= 0
                                                             : model.animationDuration(clip) > 0.0f;
        return present ? ObjectClip{clip} : ObjectClip{};
    };
    if (entity.kind == EntityKind::Door) {
        // retail 0058a1f0: the door's four states.
        if (entity.openState == 3) {
            return steady("dead");
        }
        if (entity.open) {
            const bool second = entity.openState == 2;
            ObjectClip clip = steady(second ? "opened2" : "opened1");
            if (clip.name.empty()) {
                // No resting clip: the end of the swing.
                const char* swing = second && model.animationDuration("opening2") > 0.0f ? "opening2" : "opening1";
                const float duration = model.animationDuration(swing);
                if (duration > 0.0f) {
                    clip = ObjectClip{swing, duration, false};
                }
            }
            return clip;
        }
        return steady("closed");
    }
    if (entity.kind != EntityKind::Placeable) {
        return {};
    }
    if (entity.open) {
        return steady("open"); // retail 00585670: a set Open starts in the open state.
    }
    // Inferred: the state values are not spelled out in retail's reader. The clips the placeables
    // in each state carry agree (see the worldposes survey): 1 open, 2 close, 3 dead, 4 on, 5 off.
    switch (entity.animationState) {
    case 1: return steady("open");
    case 2: return steady("close");
    case 3: return steady("dead");
    case 4: return steady("on");
    case 5: return steady("off");
    default: return {};
    }
}

const char* placeableScriptClip(std::int32_t animation, bool& loop)
{
    // ANIMATION_PLACEABLE_* of the shipped nwscript.nss; the transition clips of each are the
    // state changes in retail 0063e930, and their names rows 312 to 317 of animations.2da.
    static const char* const kClips[] = {
        "off2on", "on2off", "close2open", "open2close", "animloop01", "animloop02", "animloop03",
        "animloop04", "animloop05", "animloop06", "animloop07", "animloop08", "animloop09", "animloop10"};
    if (animation < 200 || animation > 213) {
        return nullptr;
    }
    loop = animation >= 204;
    return kClips[animation - 200];
}

void addRoomScriptMesh(Scene& scene, Install& install, const std::shared_ptr<Mdl>& model, Vec3 origin, int room)
{
    std::map<std::string, std::vector<MdlMeshTriangle>> byTexture;
    for (MdlMeshTriangle& triangle : model->scriptLoopTriangles({}, 0.0f, origin)) {
        byTexture[materialKey(triangle)].push_back(std::move(triangle));
    }
    EntityMesh mesh;
    mesh.entity = UINT32_MAX;
    mesh.model = model;
    mesh.roomAnimated = true;
    mesh.scriptLoop = true;
    mesh.origin = origin;
    mesh.room = room;
    mesh.animationLoop = true;
    for (const auto& [materialName, list] : byTexture) {
        (void)materialName;
        TextureBatch batch = makeMaterialBatch(scene, install, list.front());
        batch.room = room;
        batch.firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
        Vec3 sum{};
        for (const MdlMeshTriangle& triangle : list) {
            for (int i = 0; i < 3; ++i) {
                sum = sum + triangle.v[i].position;
                mesh.vertices.push_back(toSceneVertex(triangle.v[i], triangle));
                mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size() - 1));
            }
        }
        batch.indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - batch.firstIndex;
        if (batch.indexCount != 0) {
            batch.center = sum * (1.0f / static_cast<float>(batch.indexCount));
            mesh.batches.push_back(batch);
        }
    }
    if (!mesh.vertices.empty()) {
        scene.entityMeshes.push_back(std::move(mesh));
    }
}

bool setRoomClip(Scene& scene, const std::string& room, const std::string& clip)
{
    const std::string wanted = lower(room);
    bool found = false;
    for (EntityMesh& mesh : scene.entityMeshes) {
        if (!mesh.scriptLoop || mesh.room < 0 || static_cast<std::size_t>(mesh.room) >= scene.rooms.size()
            || lower(scene.rooms[static_cast<std::size_t>(mesh.room)].room) != wanted) {
            continue;
        }
        mesh.animation = clip;
        mesh.animationTime = 0.0f;
        mesh.animationLoop = true;
        refreshEntityMeshImpl(mesh);
        poseRoomScriptLights(scene, mesh);
        found = true;
    }
    return found;
}

void poseRoomScriptLights(Scene& scene, const EntityMesh& mesh)
{
    if (!mesh.scriptLoop || mesh.room < 0 || !mesh.model) {
        return;
    }
    Mat3 turn{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    for (Scene::WorldLight& light : scene.lights) {
        if (light.room == mesh.room && light.node != SIZE_MAX) {
            Vec3 at = light.position - mesh.origin;
            mesh.model->nodeWorldAt(light.node, mesh.animation, mesh.animationTime, at, turn);
            light.position = at + mesh.origin;
        }
    }
    for (FlareLight& flare : scene.flareLights) {
        if (flare.room == mesh.room && flare.node != SIZE_MAX && flare.entity == UINT32_MAX) {
            mesh.model->nodeWorldAt(flare.node, mesh.animation, mesh.animationTime, flare.local, turn);
        }
    }
}

void addRoomLoopMesh(Scene& scene, Install& install, const std::shared_ptr<Mdl>& model,
                     const std::string& clip, Vec3 origin, int room)
{
    std::map<std::string, std::vector<MdlMeshTriangle>> byTexture;
    for (MdlMeshTriangle& triangle : model->loopMeshTriangles(clip, 0.0f, origin)) {
        byTexture[materialKey(triangle)].push_back(std::move(triangle));
    }
    EntityMesh mesh;
    mesh.entity = UINT32_MAX;
    mesh.model = model;
    mesh.roomAnimated = true;
    mesh.origin = origin;
    mesh.room = room;
    mesh.animation = clip;
    mesh.animationLoop = true;
    for (const auto& [materialName, list] : byTexture) {
        (void)materialName;
        TextureBatch batch = makeMaterialBatch(scene, install, list.front());
        batch.room = room;
        batch.firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
        Vec3 sum{};
        for (const MdlMeshTriangle& triangle : list) {
            for (int i = 0; i < 3; ++i) {
                sum = sum + triangle.v[i].position;
                mesh.vertices.push_back(toSceneVertex(triangle.v[i], triangle));
                mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size() - 1));
            }
        }
        batch.indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - batch.firstIndex;
        if (batch.indexCount != 0) {
            batch.center = sum * (1.0f / static_cast<float>(batch.indexCount));
            mesh.batches.push_back(batch);
        }
    }
    if (!mesh.vertices.empty()) {
        scene.entityMeshes.push_back(std::move(mesh));
    }
}

void addSkinnedReferenceMesh(Scene& scene, Install& install, const std::shared_ptr<const Mdl>& model,
                             const std::shared_ptr<const Mdl>& holder, std::size_t node,
                             const std::string& holderClip, Vec3 origin, int room)
{
    EntityMesh mesh;
    mesh.entity = UINT32_MAX;
    mesh.model = std::const_pointer_cast<Mdl>(model);
    mesh.roomAnimated = true;
    mesh.origin = origin;
    mesh.room = room;
    mesh.animationLoop = true;
    mesh.referenceHolder = holder;
    mesh.referenceNode = node;
    mesh.referenceClip = holderClip;
    // Engine choice: the clip the reference's child plays was not traced in retail. The list is the
    // one a placeable without an idle uses (GameSession::initAnimations).
    for (const char* name : {"pause1", "pause", "cpause1", "default", "animloop01", "animloop1"}) {
        if (model->animationDuration(name) > 0.0f) {
            mesh.animation = name;
            break;
        }
    }
    Vec3 at;
    Mat3 turn{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    holder->nodeWorldAt(node, holderClip, 0.0f, at, turn);
    std::map<std::string, std::vector<MdlMeshTriangle>> byTexture;
    for (MdlMeshTriangle& triangle : model->placedPosedTriangles(mesh.animation, 0.0f, at + origin, turn)) {
        byTexture[materialKey(triangle)].push_back(std::move(triangle));
    }
    for (const auto& [materialName, list] : byTexture) {
        (void)materialName;
        TextureBatch batch = makeMaterialBatch(scene, install, list.front());
        batch.room = room;
        batch.firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
        Vec3 sum{};
        for (const MdlMeshTriangle& triangle : list) {
            for (int i = 0; i < 3; ++i) {
                sum = sum + triangle.v[i].position;
                mesh.vertices.push_back(toSceneVertex(triangle.v[i], triangle));
                mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size() - 1));
            }
        }
        batch.indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - batch.firstIndex;
        if (batch.indexCount != 0) {
            batch.center = sum * (1.0f / static_cast<float>(batch.indexCount));
            mesh.batches.push_back(batch);
        }
    }
    if (!mesh.vertices.empty()) {
        scene.entityMeshes.push_back(std::move(mesh));
    }
}

void rebuildEntityWeapons(Scene& scene, Install& install, std::size_t entityIndex)
{
    if (entityIndex >= scene.entities.size()) {
        return;
    }
    const std::uint32_t owner = static_cast<std::uint32_t>(entityIndex);
    std::shared_ptr<Mdl> body;
    // Mesh slots stay where they are: other systems keep indices into this list.
    std::vector<std::size_t> freeSlots;
    for (std::size_t i = 0; i < scene.entityMeshes.size(); ++i) {
        EntityMesh& mesh = scene.entityMeshes[i];
        if (mesh.entity != owner) {
            continue;
        }
        if (mesh.attachModel) {
            mesh.visible = false;
            freeSlots.push_back(i);
        } else if (!body) {
            body = mesh.model;
        }
    }
    if (!body) {
        return;
    }
    const Entity& entity = scene.entities[entityIndex];
    const std::string names[2] = {entity.weaponModel, entity.offhandModel};
    const std::string attach[2] = {entity.weaponAttach, entity.offhandAttach};
    for (int hand = 0; hand < 2; ++hand) {
        if (names[hand].empty()) {
            continue;
        }
        std::string error;
        const std::shared_ptr<Mdl> weapon = loadModel(install, names[hand], error);
        if (!weapon) {
            scene.warnings.push_back(names[hand] + ": " + error);
            continue;
        }
        const std::size_t before = scene.entityMeshes.size();
        buildMesh(scene, install, owner, weapon, body, attach[hand]);
        if (scene.entityMeshes.size() != before && !freeSlots.empty()) {
            scene.entityMeshes[freeSlots.back()] = std::move(scene.entityMeshes.back());
            scene.entityMeshes.pop_back();
            freeSlots.pop_back();
        }
    }
    // The head slot item, on the body mesh's head (006964c0).
    std::shared_ptr<Mdl> head;
    for (const EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity == owner && !mesh.attachModel && mesh.headModel) {
            head = mesh.headModel;
            break;
        }
    }
    const std::size_t before = scene.entityMeshes.size();
    attachHeadItem(scene, install, owner, body, head);
    if (scene.entityMeshes.size() != before && !freeSlots.empty()) {
        scene.entityMeshes[freeSlots.back()] = std::move(scene.entityMeshes.back());
        scene.entityMeshes.pop_back();
        freeSlots.pop_back();
    }
    ++scene.meshRevision;
}

} // namespace ke::kotor
