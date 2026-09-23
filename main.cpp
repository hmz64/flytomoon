// =============================================================================
// FlyMe2theMoon — Raylib recreation + original-asset port (C++, single file)
// Stage 1: handcrafted remake levels (no assets needed).
// Stage 2 (this file): loads the 2011 IPA asset pack when present:
//   pack/{tex,ui,sfx,music,atlas,levels,manifest.json}  (see README, folder fly/)
// Reverse-engineered notes:
//   - roleConfig.json: maxF 28, maxP 51, mass 1, damping 0.1, maxXSpeed 5,
//     yGravity -10, pushAngleRange 50, pushDistance1 51.2/2 64.0, stableFlyRate 1.6
//   - Level SeqID = 1-based index into atlas frames[] (plist dict order).
//     Background+Farground -> FargroundsS{suite}, Texture+Foreground -> TextureS{suite},
//     DynamicObject -> DynamicObjectsLite, Item 3/4/5 -> InGameUI star/crystals.
//   - StaticObject SeqID = EdgeID (edgeData.dat), pts offset by obj pos.
//   - Moon = highest-Y DynamicObject (binary: moon lives in dynamicObjectLayer).
//   - All coords flipped y-up -> y-down on export. All 60 levels ship NoMana.
// Build desktop:
//   g++ main.cpp -o flyme2themoon -std=c++17 -O2 -lraylib -lm -lpthread -ldl -lrt -lX11
// Android arm64-v8a: see repo workflow (NativeActivity + native_app_glue).
// Controls: touch/mouse LEFT half = up-right, RIGHT half = up-left.
//   Keys: A/D/W or arrows/Space, R restart, P pause, M menu.
// =============================================================================

#include "raylib.h"
#include "raymath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Raylib 5.5 does NOT hook APK assets to fopen() on Android (InitAssetManager
// is never called by rcore), so plain FileExists/LoadTexture("fly/...") fails
// on device and the game silently falls back to placeholders. Fix: route ALL
// file loads through the APK AssetManager via callbacks (native, no Java).
#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android_native_app_glue.h>
static unsigned char* AndroidLoadFileData(const char* fileName, int* dataSize) {
    if (!fileName || !dataSize) return nullptr;
    *dataSize = 0;
    android_app* app = (android_app*)GetAndroidApp();
    if (!app || !app->activity || !app->activity->assetManager) return nullptr;
    // AAssetManager wants a relative path without leading slash.
    while (*fileName == '/') fileName++;
    AAsset* asset = AAssetManager_open(app->activity->assetManager, fileName, AASSET_MODE_BUFFER);
    if (!asset) return nullptr;
    size_t len = AAsset_getLength(asset);
    unsigned char* data = (unsigned char*)malloc(len + 1);
    if (!data) { AAsset_close(asset); return nullptr; }
    int got = AAsset_read(asset, data, len);
    AAsset_close(asset);
    if (got < 0) { free(data); return nullptr; }
    data[got] = 0;
    *dataSize = got;
    return data;
}
static char* AndroidLoadFileText(const char* fileName) {
    int n = 0;
    return (char*)AndroidLoadFileData(fileName, &n);
}
static bool AndroidSaveFileData(const char* fileName, void* data, int dataSize) {
    (void)fileName; (void)data; (void)dataSize;
    return false;  // APK assets are read-only
}
#endif

// -----------------------------------------------------------------------------
// 1. ORIGINAL TUNING (roleConfig.json, Box2D/SI) + pixel derivatives
// -----------------------------------------------------------------------------
namespace Orig {
    constexpr float MAX_F = 28.0f;
    constexpr float MAX_P = 51.0f;
    constexpr float MASS = 1.0f;
    constexpr float DAMPING = 0.1f;
    constexpr float MAX_X_SPEED = 5.0f;
    constexpr float EXTRA_X_DAMP = 1.0f;
    constexpr float EXTRA_NONFLY_X_DAMP = 1.0f;
    constexpr float GRAVITY = 10.0f;
    constexpr float PUSH_ANGLE_RANGE = 50.0f;
    constexpr float PUSH_DIST1 = 51.2f;
    constexpr float PUSH_DIST2 = 64.0f;
    constexpr float STABLE_FLY_RATE = 1.6f;
    constexpr float PTM = 32.0f;
}
namespace Tune {
    constexpr float GRAVITY_PX = Orig::GRAVITY * Orig::PTM;
    constexpr float MAX_X_PX = Orig::MAX_X_SPEED * Orig::PTM;
    constexpr float MAX_Y_PX = 12.0f * Orig::PTM;
    constexpr float WORLD_W = 960.0f;
    constexpr float PLAYER_R = 20.0f;
    constexpr float MANA_MAX = 100.0f;
    constexpr float MANA_DRAIN = 32.0f;
    constexpr float MANA_REGEN = 22.0f;
}
static float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float ParTimeForLevel(int lvl) {
    static const float t[61] = {0,1.75f,10,9,8.1f,14,15,22.5f,17.5f,16,16,15,16,24,8,7.5f,
        11,15,8,5,8,8,12,15,8,15,8,10,7.5f,15,19,5,8.5f,10,9.5f,17,5.5f,10,14,6,
        16,10,15,31,12,5,64,15,9,42,5,11,10,7,29,8,13,10};
    if (lvl >= 1 && lvl <= 60) return t[lvl];
    return 15.0f;
}

// -----------------------------------------------------------------------------
// 2. Mini JSON parser (objects/arrays/strings/numbers/bool/null)
// -----------------------------------------------------------------------------
struct JNode {
    enum T { NUL, NUM, STR, BOO, ARR, OBJ } type = NUL;
    double num = 0;
    std::string str;
    bool boo = false;
    std::vector<JNode> arr;
    std::map<std::string, JNode> obj;
    const JNode& operator[](const char* k) const {
        static JNode empty;
        auto it = obj.find(k);
        return it == obj.end() ? empty : it->second;
    }
    const JNode& at(size_t i) const {
        static JNode empty;
        return i < arr.size() ? arr[i] : empty;
    }
    double numOr(double d) const { return type == NUM ? num : d; }
    int intOr(int d) const { return type == NUM ? (int)num : d; }
    std::string strOr(const char* d) const { return type == STR ? str : std::string(d); }
};
struct JParser {
    const char* p;
    explicit JParser(const char* s) : p(s) {}
    void ws() { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }
    JNode parse() { ws(); JNode n = value(); ws(); return n; }
    JNode value() {
        ws();
        if (*p == '{') return object();
        if (*p == '[') return array();
        if (*p == '"') { JNode n; n.type = JNode::STR; n.str = string(); return n; }
        if (*p == 't' && !strncmp(p, "true", 4)) { p += 4; JNode n; n.type = JNode::BOO; n.boo = true; return n; }
        if (*p == 'f' && !strncmp(p, "false", 5)) { p += 5; JNode n; n.type = JNode::BOO; n.boo = false; return n; }
        if (*p == 'n' && !strncmp(p, "null", 4)) { p += 4; return JNode(); }
        JNode n; n.type = JNode::NUM; n.num = number(); return n;
    }
    JNode object() {
        JNode n; n.type = JNode::OBJ; p++;
        ws();
        if (*p == '}') { p++; return n; }
        while (true) {
            ws();
            std::string k = string();
            ws();
            if (*p == ':') p++;
            n.obj[k] = value();
            ws();
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; break; }
            break;
        }
        return n;
    }
    JNode array() {
        JNode n; n.type = JNode::ARR; p++;
        ws();
        if (*p == ']') { p++; return n; }
        while (true) {
            n.arr.push_back(value());
            ws();
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; break; }
            break;
        }
        return n;
    }
    std::string string() {
        std::string s;
        if (*p == '"') p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                char e = *p;
                if (e == 'n') s += '\n';
                else if (e == 't') s += '\t';
                else s += e;
                p++;
            } else s += *p++;
        }
        if (*p == '"') p++;
        return s;
    }
    double number() {
        char* end = nullptr;
        double v = strtod(p, &end);
        if (end) p = end;
        return v;
    }
};

// -----------------------------------------------------------------------------
// 3. TEXTURE SWAP POINTS (placeholder fallback when pack is absent)
// -----------------------------------------------------------------------------
static void DrawKianaPlaceholder(Vector2 pos, float tiltDeg, bool thrusting) {
    Color body = Color{ 255, 150, 200, 255 };
    Color cloak = Color{ 90, 60, 160, 255 };
    DrawCircleV(pos, Tune::PLAYER_R + 4, cloak);
    DrawCircleV(pos, Tune::PLAYER_R, body);
    Vector2 nose = { pos.x + sinf(tiltDeg * DEG2RAD) * 22.0f,
                     pos.y - cosf(tiltDeg * DEG2RAD) * 22.0f };
    DrawCircleV(nose, 6, WHITE);
    DrawCircleV({ pos.x - 7, pos.y - 6 }, 3, BLACK);
    DrawCircleV({ pos.x + 7, pos.y - 6 }, 3, BLACK);
    if (thrusting) {
        Vector2 flame = { pos.x - sinf(tiltDeg * DEG2RAD) * 30.0f,
                          pos.y + cosf(tiltDeg * DEG2RAD) * 30.0f };
        DrawCircleV(flame, 8, ORANGE);
        DrawCircleV(flame, 4, YELLOW);
    }
}
// TODO(texture): these now prefer real atlas sprites; placeholders remain as fallback.
static void DrawPlatformPlaceholder(Rectangle r, bool moving, bool oneWay) {
    Color c = moving ? GREEN : (oneWay ? SKYBLUE : DARKGRAY);
    DrawRectangleRec(r, c);
    DrawRectangleLinesEx(r, 3, BLACK);
    if (oneWay) DrawLine((int)r.x, (int)r.y, (int)(r.x + r.width), (int)r.y, WHITE);
}
static void DrawStarPlaceholder(Vector2 pos, float t, bool taken) {
    if (taken) return;
    float s = 12.0f + sinf(t * 4.0f) * 2.0f;
    DrawCircleV(pos, s + 4, GOLD);
    DrawCircleV(pos, s, YELLOW);
    DrawCircleV(pos, 4, WHITE);
}
static void DrawMoonPlaceholder(Vector2 pos, float r, float t) {
    DrawCircleV(pos, r + 8 + sinf(t * 2.0f) * 3.0f, Color{ 200, 200, 255, 90 });
    DrawCircleV(pos, r, Color{ 240, 240, 255, 255 });
    DrawCircleLines((int)pos.x, (int)pos.y, r, DARKBLUE);
    DrawCircleV({ pos.x - r * 0.25f, pos.y - r * 0.2f }, r * 0.16f, LIGHTGRAY);
    DrawCircleV({ pos.x + r * 0.2f, pos.y + r * 0.25f }, r * 0.12f, LIGHTGRAY);
}
static void DrawSpikePlaceholder(Rectangle r) {
    DrawRectangleRec(r, RED);
    DrawRectangleLinesEx(r, 2, MAROON);
    DrawText("!", (int)(r.x + r.width / 2 - 4), (int)(r.y + 2), 16, WHITE);
}

// -----------------------------------------------------------------------------
// 4. ASSET PACK (atlases + frames)
// -----------------------------------------------------------------------------
struct AtlasFrame {
    std::string name;
    float x = 0, y = 0, w = 0, h = 0;
    float sw = 0, sh = 0;
    bool rot = false;
};
struct Atlas {
    Texture2D tex = { 0 };
    bool ok = false;
    std::vector<AtlasFrame> frames;
    std::map<std::string, int> byName;  // name -> seq (1-based)
};
struct AssetPack {
    std::string base;                   // resolved prefix, e.g. "fly/"
    bool ready = false;
    std::map<std::string, Atlas> atlases;
    std::map<std::string, std::vector<int> > kiana;  // state -> seqs in CharActions
};
static AssetPack GAssets;

static std::string JoinPath(const std::string& a, const std::string& b) { return a + b; }

// Existence check that works on Android APK assets too (FileExists alone
// uses raw fopen, which cannot see inside the APK).
static bool AssetExists(const char* path) {
    if (FileExists(path)) return true;
    char* t = LoadFileText(path);  // via Android callback when on device
    if (t) { UnloadFileText(t); return true; }
    return false;
}

static bool LoadAtlas(AssetPack& pk, const std::string& atlasName, const std::string& imgFile) {
    std::string jpath = JoinPath(pk.base, std::string("atlas/") + atlasName + ".json");
    if (!AssetExists(jpath.c_str())) return false;
    char* txt = LoadFileText(jpath.c_str());
    if (!txt) return false;
    JNode root = JParser(txt).parse();
    UnloadFileText(txt);
    std::string ipath = JoinPath(pk.base, std::string("tex/") + imgFile);
    if (!AssetExists(ipath.c_str())) {
        ipath = JoinPath(pk.base, std::string("ui/") + imgFile);
        if (!AssetExists(ipath.c_str())) return false;
    }
    Texture2D t = LoadTexture(ipath.c_str());
    if (t.id == 0) return false;
    Atlas a;
    a.tex = t; a.ok = true;
    const JNode& fr = root["frames"];
    for (size_t i = 0; i < fr.arr.size(); i++) {
        const JNode& f = fr.arr[i];
        AtlasFrame af;
        af.name = f["name"].strOr("");
        af.x = (float)f["x"].numOr(0); af.y = (float)f["y"].numOr(0);
        af.w = (float)f["w"].numOr(0); af.h = (float)f["h"].numOr(0);
        af.sw = (float)f["sw"].numOr(af.w); af.sh = (float)f["sh"].numOr(af.h);
        af.rot = f["rot"].type == JNode::BOO && f["rot"].boo;
        a.byName[af.name] = (int)i + 1;
        a.frames.push_back(af);
    }
    pk.atlases[atlasName] = a;
    return true;
}

static const AtlasFrame* AtlasFrameAt(const std::string& atlas, int idx0) {
    auto it = GAssets.atlases.find(atlas);
    if (it == GAssets.atlases.end() || !it->second.ok) return nullptr;
    if (idx0 < 0 || idx0 >= (int)it->second.frames.size()) return nullptr;
    return &it->second.frames[(size_t)idx0];
}
static const Atlas* AtlasGet(const std::string& atlas) {
    auto it = GAssets.atlases.find(atlas);
    return it == GAssets.atlases.end() ? nullptr : &it->second;
}
// 0-based frame index by sprite name, -1 if missing.
static int AtlasNameIdx(const std::string& atlas, const std::string& name) {
    const Atlas* a = AtlasGet(atlas);
    if (!a) return -1;
    auto it = a->byName.find(name);
    if (it == a->byName.end()) return -1;
    return it->second - 1;
}
// Stretched blit (for bars) from an atlas frame.
static void DrawAtlasStretched(const std::string& atlas, int idx0, Rectangle dst) {
    const Atlas* a = AtlasGet(atlas);
    const AtlasFrame* f = AtlasFrameAt(atlas, idx0);
    if (!a || !a->ok || !f || f->w <= 0 || f->h <= 0) return;
    Rectangle src = { f->x, f->y, f->w, f->h };
    if (f->rot) src = { f->x, f->y, f->h, f->w };
    DrawTexturePro(a->tex, src, dst, { 0, 0 }, 0, WHITE);
}

// Draws atlas sprite centered at `center` (y-down px). idx0 = 0-based frame
// index (exporter resolves original SeqID by NAME: `{Prefix}_{seq:03d}.png`).
static void DrawAtlasSprite(const std::string& atlas, int idx0, Vector2 center,
                            float rotDeg, bool flip, Color tint) {
    const Atlas* a = AtlasGet(atlas);
    const AtlasFrame* f = AtlasFrameAt(atlas, idx0);
    if (!a || !a->ok || !f || f->w <= 0 || f->h <= 0) return;
    Rectangle src = { f->x, f->y, f->w, f->h };
    Rectangle dst = { center.x, center.y, f->w, f->h };
    float rot = rotDeg;
    if (f->rot) {  // stored rotated 90 deg in atlas: swap src dims, un-rotate
        src = { f->x, f->y, f->h, f->w };
        rot = rotDeg - 90.0f;
    }
    if (flip) src.width = -src.width;
    Vector2 origin = { dst.width / 2.0f, dst.height / 2.0f };
    DrawTexturePro(a->tex, src, dst, origin, rot, tint);
}

static bool InitAssetPack() {
    const char* cands[] = { "fly/", "assets/fly/", "./assets/fly/", "/sdcard/flypack/", nullptr };
    for (int i = 0; cands[i]; i++) {
        std::string probe = std::string(cands[i]) + "manifest.json";
        if (AssetExists(probe.c_str())) { GAssets.base = cands[i]; break; }
    }
    if (GAssets.base.empty()) return false;
    // gameplay atlases (SD)
    LoadAtlas(GAssets, "TextureS1", "TextureS1.png");
    LoadAtlas(GAssets, "TextureS2", "TextureS2.png");
    LoadAtlas(GAssets, "TextureS3", "TextureS3.png");
    LoadAtlas(GAssets, "TextureS4", "TextureS4.png");
    LoadAtlas(GAssets, "FargroundsS1", "FargroundsS1.png");
    LoadAtlas(GAssets, "FargroundsS2", "FargroundsS2.png");
    LoadAtlas(GAssets, "FargroundsS3", "FargroundsS3.png");
    LoadAtlas(GAssets, "FargroundsS4", "FargroundsS4.png");
    LoadAtlas(GAssets, "CharActions", "CharActions.png");
    LoadAtlas(GAssets, "DynamicObjectsLite", "DynamicObjectsLite.png");
    LoadAtlas(GAssets, "InGameUI", "InGameUI.png");
    // Kiana animation sets (frame names seen in CharActions.plist)
    const char* states[] = { "charIdle", "charUp", "charDown", "charLeft",
                             "charRight", "charFree", "charStand", "charDeath", nullptr };
    const Atlas* ch = AtlasGet("CharActions");
    if (ch) {
        for (int s = 0; states[s]; s++) {
            std::vector<int> idxs;
            for (size_t k = 0; k < ch->frames.size(); k++) {
                if (ch->frames[k].name.find(states[s]) == 0) idxs.push_back((int)k);
            }
            if (!idxs.empty()) GAssets.kiana[states[s]] = idxs;
        }
    }
    GAssets.ready = AtlasGet("TextureS1") != nullptr || AtlasGet("TextureS3") != nullptr;
    return GAssets.ready;
}

static int KianaSeq(const char* state, float t) {
    auto it = GAssets.kiana.find(state);
    if (it == GAssets.kiana.end() || it->second.empty()) return -1;
    size_t idx = (size_t)(t * 8.0f) % it->second.size();
    return it->second[idx];
}

static void DrawKiana(Vector2 pos, float tiltDeg, bool thrusting, bool dead, Vector2 vel, float t) {
    const char* st = "charIdle";
    if (dead) st = "charDeath";
    else if (vel.x < -80) st = "charLeft";
    else if (vel.x > 80) st = "charRight";
    else if (vel.y < -60) st = "charUp";
    else if (vel.y > 140) st = "charDown";
    else if (!thrusting && fabsf(vel.y) < 40 && fabsf(vel.x) < 40) st = "charIdle";
    else st = "charFree";
    int idx = KianaSeq(st, t);
    if (idx < 0) { DrawKianaPlaceholder(pos, tiltDeg, thrusting); return; }
    DrawAtlasSprite("CharActions", idx, pos, tiltDeg * 0.4f, false, WHITE);
}

// -----------------------------------------------------------------------------
// 5. GAME DATA (remake structs + imported original levels)
// -----------------------------------------------------------------------------
enum class GameState { MENU, PLAYING, PAUSED, DEAD, WIN };

struct Platform {
    Rectangle rect = { 0, 0, 0, 0 };
    bool oneWay = false;
    bool moving = false;
    Vector2 moveAxis = { 1, 0 };
    float moveRange = 0, moveSpeed = 0, phase = 0;
    Vector2 basePos = { 0, 0 };
};
struct Star { Vector2 pos = { 0, 0 }; bool taken = false; };
struct Spike { Rectangle rect = { 0, 0, 0, 0 }; };
struct Goal { Vector2 pos = { 0, 0 }; float radius = 46; };
struct Particle {
    Vector2 pos = { 0, 0 }, vel = { 0, 0 };
    float life = 0, maxLife = 1;
    Color color = WHITE;
    float size = 4;
};
struct Level {
    int number = 1;
    std::string name;
    float width = Tune::WORLD_W;
    float height = 3200;
    Vector2 spawn = { 0, 0 };
    bool manaEnabled = false;
    std::vector<Platform> platforms;
    std::vector<Star> stars;
    std::vector<Spike> spikes;
    Goal moon;
};

// Imported original level (y-down px, exactly as exported).
struct Placed { int seq = 0; int f = -1; float x = 0, y = 0, r = 0; bool flip = false; };
struct DynObj { int seq = 0; int f = -1; float x = 0, y = 0, r = 0; };
struct ItemObj { int seq = 0; int f = -1; float x = 0, y = 0; };
struct ImportLevel {
    bool ok = false;
    int id = 0, suite = 3;
    float w = 960, h = 1440, par = 15;
    Vector2 born = { 0, 0 };
    std::vector<Placed> bac, far, tex, fore;
    std::vector<DynObj> dyn;
    std::vector<ItemObj> items;
    std::vector<std::vector<Vector2> > edges;
    int moonDyn = -1;
};
static std::string SuiteTexAtlas(int suite) {
    char b[32];
    snprintf(b, sizeof b, "TextureS%d", suite < 1 ? 1 : (suite > 4 ? 4 : suite));
    return std::string(b);
}
static std::string SuiteFarAtlas(int suite) {
    char b[32];
    snprintf(b, sizeof b, "FargroundsS%d", suite < 1 ? 1 : (suite > 4 ? 4 : suite));
    return std::string(b);
}
static bool LoadImportLevel(int n, ImportLevel& out) {
    out = ImportLevel();
    if (!GAssets.ready) return false;
    char b[128];
    snprintf(b, sizeof b, "%slevels/level_%03d.json", GAssets.base.c_str(), n);
    if (!AssetExists(b)) return false;
    char* txt = LoadFileText(b);
    if (!txt) return false;
    JNode root = JParser(txt).parse();
    UnloadFileText(txt);
    out.ok = true;
    out.id = n;
    out.w = (float)root["w"].numOr(960);
    out.h = (float)root["h"].numOr(1440);
    out.suite = root["suite"].intOr(3);
    out.par = (float)root["par"].numOr(15.0);
    out.born = { (float)root["born"].at(0).numOr(480), (float)root["born"].at(1).numOr(1200) };
    const char* keys[] = { "bac", "far", "tex", "fore", nullptr };
    std::vector<Placed>* dsts[] = { &out.bac, &out.far, &out.tex, &out.fore };
    for (int k = 0; keys[k]; k++) {
        const JNode& a = root[keys[k]];
        for (size_t i = 0; i < a.arr.size(); i++) {
            const JNode& o = a.arr[i];
            Placed p;
            p.seq = o["s"].intOr(0);
            p.f = o["f"].intOr(-1);
            p.x = (float)o["x"].numOr(0); p.y = (float)o["y"].numOr(0);
            p.r = (float)o["r"].numOr(0);
            p.flip = o["flip"].type == JNode::BOO && o["flip"].boo;
            dsts[k]->push_back(p);
        }
    }
    const JNode& dy = root["dyn"];
    for (size_t i = 0; i < dy.arr.size(); i++) {
        const JNode& o = dy.arr[i];
        DynObj d;
        d.seq = o["s"].intOr(0);
        d.f = o["f"].intOr(-1);
        d.x = (float)o["x"].numOr(0); d.y = (float)o["y"].numOr(0);
        d.r = (float)o["r"].numOr(0);
        out.dyn.push_back(d);
    }
    out.moonDyn = root["moonDyn"].intOr(-1);
    const JNode& it = root["items"];
    for (size_t i = 0; i < it.arr.size(); i++) {
        const JNode& o = it.arr[i];
        ItemObj io;
        io.seq = o["s"].intOr(0);
        io.f = o["f"].intOr(-1);
        io.x = (float)o["x"].numOr(0); io.y = (float)o["y"].numOr(0);
        out.items.push_back(io);
    }
    const JNode& ed = root["edges"];
    for (size_t i = 0; i < ed.arr.size(); i++) {
        const JNode& e = ed.arr[i];
        const JNode& pts = e["pts"];
        std::vector<Vector2> poly;
        for (size_t k = 0; k < pts.arr.size(); k++)
            poly.push_back({ (float)pts.arr[k].at(0).numOr(0), (float)pts.arr[k].at(1).numOr(0) });
        if (poly.size() >= 2) out.edges.push_back(poly);
    }
    return true;
}

struct Player {
    Vector2 pos = { 0, 0 }, vel = { 0, 0 };
    float mana = Tune::MANA_MAX;
    bool outOfMana = false;
    bool dead = false;
    float deadTimer = 0;
    int stars = 0;
    float time = 0;
    float tilt = 0;
    Vector2 lastSafe = { 0, 0 };
};
static std::vector<Vector2> g_bgStars;

// Remake level builders (fallback when pack absent).
static void AddWall(Level& lv, float x, float y, float w, float h) {
    Platform p; p.rect = { x, y, w, h }; p.oneWay = false;
    lv.platforms.push_back(p);
}
static void AddPlat(Level& lv, float x, float y, float w, bool oneWay = true,
                    bool moving = false, float range = 0, float speed = 0, float phase = 0) {
    Platform p; p.rect = { x, y, w, 22 }; p.oneWay = oneWay;
    p.moving = moving; p.moveAxis = { 1, 0 };
    p.moveRange = range; p.moveSpeed = speed; p.phase = phase;
    p.basePos = { x, y };
    lv.platforms.push_back(p);
}
static Level BuildLevel(int n) {
    Level lv;
    lv.number = n;
    lv.width = Tune::WORLD_W;
    if (n == 1) {
        lv.name = "Lv.1 Moonlit Beginning";
        lv.height = 3000; lv.manaEnabled = false;
        lv.spawn = { 480, 2780 };
        lv.moon = { { 480, 220 }, 52 };
        AddWall(lv, -30, 0, 60, lv.height);
        AddWall(lv, lv.width - 30, 0, 60, lv.height);
        AddWall(lv, 0, lv.height - 40, lv.width, 60);
        AddPlat(lv, 380, 2620, 200, false);
        AddPlat(lv, 180, 2400, 170);
        AddPlat(lv, 600, 2200, 170);
        AddPlat(lv, 380, 1980, 180);
        AddPlat(lv, 150, 1740, 160);
        AddPlat(lv, 620, 1520, 160);
        AddPlat(lv, 380, 1280, 190, false);
        AddPlat(lv, 200, 1020, 150);
        AddPlat(lv, 610, 800, 150);
        AddPlat(lv, 380, 560, 200, false);
        lv.stars = { {{280,2320}}, {{700,2120}}, {{480,1900}}, {{250,940}}, {{700,720}} };
        lv.spikes = { {{430, 1200, 100, 22}} };
    } else if (n == 2) {
        lv.name = "Lv.2 Witch's Ascent";
        lv.height = 3600; lv.manaEnabled = true;
        lv.spawn = { 480, 3380 };
        lv.moon = { { 480, 220 }, 50 };
        AddWall(lv, -30, 0, 60, lv.height);
        AddWall(lv, lv.width - 30, 0, 60, lv.height);
        AddWall(lv, 0, lv.height - 40, lv.width, 60);
        AddPlat(lv, 380, 3200, 200, false);
        AddPlat(lv, 150, 2980, 150, true, true, 130, 1.2f, 0.0f);
        AddPlat(lv, 620, 2760, 150, true, true, 130, 1.4f, 1.5f);
        AddPlat(lv, 380, 2520, 170);
        AddPlat(lv, 180, 2280, 140);
        AddPlat(lv, 640, 2060, 140, true, true, 110, 1.8f, 3.0f);
        AddPlat(lv, 390, 1820, 180, false);
        AddPlat(lv, 150, 1560, 140);
        AddPlat(lv, 650, 1320, 140);
        AddPlat(lv, 390, 1060, 170, false);
        AddPlat(lv, 200, 800, 150);
        AddPlat(lv, 610, 560, 150);
        lv.stars = { {{480,2440}}, {{250,2200}}, {{710,1980}}, {{480,1480}}, {{250,720}}, {{710,480}} };
        lv.spikes = { {{200,1740,120,22}}, {{640,980,120,22}}, {{400,480,160,22}} };
    } else if (n == 3) {
        lv.name = "Lv.3 Eclipse Trial";
        lv.height = 4000; lv.manaEnabled = true;
        lv.spawn = { 200, 3780 };
        lv.moon = { { 760, 220 }, 50 };
        AddWall(lv, -30, 0, 60, lv.height);
        AddWall(lv, lv.width - 30, 0, 60, lv.height);
        AddWall(lv, 0, lv.height - 40, lv.width, 60);
        AddWall(lv, 300, 2400, 40, 900);
        AddPlat(lv, 100, 3600, 180, false);
        AddPlat(lv, 550, 3400, 160);
        AddPlat(lv, 150, 3180, 150);
        AddPlat(lv, 600, 2960, 150, true, true, 100, 2.0f, 0);
        AddPlat(lv, 100, 2700, 150);
        AddPlat(lv, 550, 2200, 150);
        AddPlat(lv, 650, 1950, 150, false);
        AddPlat(lv, 150, 1700, 150);
        AddPlat(lv, 400, 1450, 160, false);
        AddPlat(lv, 650, 1180, 150);
        AddPlat(lv, 350, 920, 150);
        AddPlat(lv, 600, 660, 160, false);
        lv.stars = { {{630,3320}}, {{220,3100}}, {{660,2120}}, {{220,1620}}, {{480,1370}}, {{680,580}} };
        lv.spikes = { {{120,2620,140,22}}, {{560,1370,140,22}}, {{150,840,140,22}} };
    } else {
        lv.name = "Survival: Endless Night";
        lv.height = 6000; lv.manaEnabled = true;
        lv.spawn = { 480, 5800 };
        lv.moon = { { 480, 200 }, 55 };
        AddWall(lv, -30, 0, 60, lv.height);
        AddWall(lv, lv.width - 30, 0, 60, lv.height);
        AddWall(lv, 0, lv.height - 40, lv.width, 60);
        unsigned seed = 12345;
        for (float y = 5500; y > 600; y -= 260) {
            seed = seed * 1664525u + 1013904223u;
            float fr = (float)(seed >> 8) / 16777216.0f;
            float x = 100 + fr * 600;
            seed = seed * 1664525u + 1013904223u;
            float w = 130 + (float)(seed >> 8) / 16777216.0f * 90;
            Star s; s.pos = { x + w / 2, y - 60 }; s.taken = false;
            lv.stars.push_back(s);
            AddPlat(lv, x, y, w, true);
        }
    }
    return lv;
}

// -----------------------------------------------------------------------------
// 6. PHYSICS (roleConfig-faithful push-point model)
// -----------------------------------------------------------------------------
struct PushResult { Vector2 accel = { 0, 0 }; bool active = false; float angleDeg = 90; float mag = 0; };

static PushResult PushFromPoint(Vector2 rolePos, Vector2 touchWorld) {
    PushResult r;
    Vector2 d = { rolePos.x - touchWorld.x, rolePos.y - touchWorld.y };
    float dist = sqrtf(d.x * d.x + d.y * d.y);
    if (dist < 1e-3f) return r;
    float ang = atan2f(-d.y, d.x) * RAD2DEG;
    float lo = 90.0f - Orig::PUSH_ANGLE_RANGE;
    float hi = 90.0f + Orig::PUSH_ANGLE_RANGE;
    float clamped = ClampF(ang, lo, hi);
    float t = ClampF((dist - Orig::PUSH_DIST1) / (Orig::PUSH_DIST2 - Orig::PUSH_DIST1), 0.0f, 1.0f);
    float force = Orig::MAX_F + (Orig::MAX_P - Orig::MAX_F) * t;
    float a = force / Orig::MASS * Orig::PTM;
    float rad = clamped * DEG2RAD;
    r.accel = { cosf(rad) * a, -sinf(rad) * a };
    r.active = true; r.angleDeg = clamped; r.mag = a;
    return r;
}

struct AudioBank {
    Sound pickup = { 0 }, death = { 0 }, win = { 0 }, click = { 0 };
    bool hasPickup = false, hasDeath = false, hasWin = false, hasClick = false;
    Music menu = { 0 }, stage = { 0 };
    bool hasMenu = false, hasStage = false;
    int stageSuite = -1;
};
static AudioBank GAudio;

static void StepPlayer(Player& p, bool manaEnabled, const std::vector<Vector2>& pushPoints,
                       float dt, std::vector<Particle>& particles) {
    Vector2 acc = { 0, Tune::GRAVITY_PX };
    bool thrusting = false;
    float tiltTarget = 0;
    for (size_t i = 0; i < pushPoints.size(); i++) {
        if (p.outOfMana && manaEnabled) continue;
        PushResult pr = PushFromPoint(p.pos, pushPoints[i]);
        if (!pr.active) continue;
        float assist = 1.0f;
        if (fabsf(p.vel.y) < 70.0f) assist = 1.0f + (Orig::STABLE_FLY_RATE - 1.0f) * 0.5f;
        acc.x += pr.accel.x * assist;
        acc.y += pr.accel.y * assist;
        thrusting = true;
        tiltTarget = ClampF((90.0f - pr.angleDeg) * 1.4f, -38.0f, 38.0f);
        if ((int)(p.time * 120) % 2 == 0) {
            Vector2 dir = { -pr.accel.x, -pr.accel.y };
            float l = sqrtf(dir.x * dir.x + dir.y * dir.y) + 1e-4f;
            dir.x /= l; dir.y /= l;
            Particle pt;
            pt.pos = { p.pos.x + dir.x * 10, p.pos.y + 14 };
            pt.vel = { dir.x * 160 + (float)GetRandomValue(-30, 30), dir.y * 160 };
            pt.life = 0; pt.maxLife = 0.45f;
            pt.color = ORANGE; pt.size = 5;
            particles.push_back(pt);
        }
    }
    p.vel.x += acc.x * dt;
    p.vel.y += acc.y * dt;
    float damp = Orig::DAMPING + (thrusting ? Orig::EXTRA_X_DAMP * 0.35f : Orig::EXTRA_NONFLY_X_DAMP * 0.55f);
    p.vel.x *= 1.0f / (1.0f + damp * dt);
    p.vel.y *= 1.0f / (1.0f + Orig::DAMPING * 0.45f * dt);
    p.vel.x = ClampF(p.vel.x, -Tune::MAX_X_PX * 2.2f, Tune::MAX_X_PX * 2.2f);
    p.vel.y = ClampF(p.vel.y, -Tune::MAX_Y_PX * 1.6f, Tune::MAX_Y_PX);
    p.pos.x += p.vel.x * dt;
    p.pos.y += p.vel.y * dt;
    if (manaEnabled) {
        if (thrusting) {
            p.mana -= Tune::MANA_DRAIN * dt;
            if (p.mana <= 0) { p.mana = 0; p.outOfMana = true; }
        } else {
            p.mana += Tune::MANA_REGEN * dt;
            if (p.mana > Tune::MANA_MAX) p.mana = Tune::MANA_MAX;
            if (p.outOfMana && p.mana > 30.0f) p.outOfMana = false;
        }
    } else p.mana = Tune::MANA_MAX;
    float velTilt = ClampF(p.vel.x * 0.09f, -30.0f, 30.0f);
    p.tilt += ((thrusting ? tiltTarget * 0.6f + velTilt * 0.4f : velTilt) - p.tilt)
              * ClampF(10.0f * dt, 0.0f, 1.0f);
    p.time += dt;
}

static bool ResolveCircleRect(Vector2& pos, Vector2& vel, float r, Rectangle rc,
                              bool oneWay, bool& landed) {
    float cx = ClampF(pos.x, rc.x, rc.x + rc.width);
    float cy = ClampF(pos.y, rc.y, rc.y + rc.height);
    float dx = pos.x - cx, dy = pos.y - cy;
    float d2 = dx * dx + dy * dy;
    if (d2 > r * r) return false;
    if (oneWay) {
        if (vel.y < 0) return false;
        float feet = pos.y + r;
        if (feet < rc.y || feet > rc.y + 26) return false;
        pos.y = rc.y - r; vel.y = 0; landed = true;
        return true;
    }
    float d = sqrtf(d2);
    if (d < 1e-4f) { pos.y = rc.y - r; vel.y = fminf(vel.y, 0) * -0.15f; landed = true; return true; }
    Vector2 n = { dx / d, dy / d };
    pos.x += n.x * (r - d); pos.y += n.y * (r - d);
    float vn = vel.x * n.x + vel.y * n.y;
    if (vn < 0) { vel.x -= 1.15f * vn * n.x; vel.y -= 1.15f * vn * n.y; }
    if (n.y < -0.6f) landed = true;
    return true;
}

// Circle vs polyline (imported Box2D edges). Restitution 0.15 like remake.
static bool ResolveCirclePoly(Vector2& pos, Vector2& vel, float r,
                              const std::vector<Vector2>& pts, bool& landed) {
    bool hit = false;
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        Vector2 a = pts[i], b = pts[i + 1];
        Vector2 ab = { b.x - a.x, b.y - a.y };
        float len2 = ab.x * ab.x + ab.y * ab.y;
        if (len2 < 1e-6f) continue;
        float t = ((pos.x - a.x) * ab.x + (pos.y - a.y) * ab.y) / len2;
        t = ClampF(t, 0.0f, 1.0f);
        Vector2 c = { a.x + ab.x * t, a.y + ab.y * t };
        Vector2 d = { pos.x - c.x, pos.y - c.y };
        float dist = sqrtf(d.x * d.x + d.y * d.y);
        if (dist >= r || dist < 1e-4f) continue;
        Vector2 n = { d.x / dist, d.y / dist };
        pos.x += n.x * (r - dist); pos.y += n.y * (r - dist);
        float vn = vel.x * n.x + vel.y * n.y;
        if (vn < 0) { vel.x -= 1.15f * vn * n.x; vel.y -= 1.15f * vn * n.y; }
        if (n.y < -0.6f) landed = true;
        hit = true;
    }
    return hit;
}

// Item visuals come pre-resolved from the exporter (Item_00N.png in
// DynamicObjectsLite); no runtime guessing.

// -----------------------------------------------------------------------------
// 7. APP
// -----------------------------------------------------------------------------
int main() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(540, 960, "FlyMe2theMoon — Raylib Port (miHoYo 2011 homage)");
    SetTargetFPS(60);
    InitAudioDevice();
#ifdef __ANDROID__
    SetLoadFileDataCallback(AndroidLoadFileData);
    SetLoadFileTextCallback(AndroidLoadFileText);
#endif

    bool packReady = InitAssetPack();
    // SFX mapping: win/death confirmed from Level_*.lua sound.play(); pickup/click best-effort.
    if (packReady) {
        std::string sb = GAssets.base;
        struct Map { Sound* s; bool* ok; const char* f; };
        Sound pickup = { 0 }, death = { 0 }, win = { 0 }, click = { 0 };
        GAudio.pickup = pickup; GAudio.death = death; GAudio.win = win; GAudio.click = click;
        Map maps[] = {
            { &GAudio.pickup, &GAudio.hasPickup, "sfx/90-1.wav" },
            { &GAudio.death, &GAudio.hasDeath, "sfx/51-2.wav" },
            { &GAudio.win, &GAudio.hasWin, "sfx/54-1.wav" },
            { &GAudio.click, &GAudio.hasClick, "sfx/40-1.wav" },
        };
        for (size_t i = 0; i < sizeof maps / sizeof maps[0]; i++) {
            std::string p = sb + maps[i].f;
            if (AssetExists(p.c_str())) {
                *maps[i].s = LoadSound(p.c_str());
                *maps[i].ok = true;
            }
        }
        std::string mp = sb + "music/MoonTrip.ogg";
        if (AssetExists(mp.c_str())) { GAudio.menu = LoadMusicStream(mp.c_str()); GAudio.hasMenu = true; }
    }

    GameState state = GameState::MENU;
    bool useImport = false;       // true = original ported level, false = remake
    int importNo = 1;
    int remakeNo = 1;             // 1..3, 4 = survival
    Level level = BuildLevel(1);
    ImportLevel ilev;
    Player player = Player();
    player.pos = level.spawn; player.lastSafe = level.spawn;
    std::vector<Particle> particles;
    std::vector<char> itemTaken;
    Camera2D cam = { 0 };
    cam.rotation = 0;
    g_bgStars.reserve(160);
    for (int i = 0; i < 160; i++)
        g_bgStars.push_back({ (float)GetRandomValue(0, 960), (float)GetRandomValue(0, 6000) });

    const float FIXED_DT = 1.0f / 120.0f;
    float accumulator = 0, animT = 0, messageT = 0;
    int score = 0, bestScore = 0;
    std::string message, levelTitle = level.name;
    float levelW = level.width, levelH = level.height;
    bool manaOn = level.manaEnabled;
    int menuPage = 0;             // 0 = levels 1-30, 1 = 31-60
    int menuTab = 0;              // 0 = original 60, 1 = remake
    if (packReady && GAudio.hasMenu) PlayMusicStream(GAudio.menu);

    Vector2 moonPos = level.moon.pos;
    float moonR = level.moon.radius;

    while (!WindowShouldClose()) {
        float frameDt = GetFrameTime();
        if (frameDt > 0.1f) frameDt = 0.1f;
        animT += frameDt;
        if (GAudio.hasMenu && state == GameState::MENU) UpdateMusicStream(GAudio.menu);
        if (GAudio.hasStage && state == GameState::PLAYING) UpdateMusicStream(GAudio.stage);

        int sw = GetScreenWidth(), sh = GetScreenHeight();
        float zoom = (float)sw / Tune::WORLD_W;
        if (zoom < 0.05f) zoom = 0.05f;
        float visH = (float)sh / zoom;
        float visW = (float)sw / zoom;

        auto startImport = [&](int n) {
            if (LoadImportLevel(n, ilev)) {
                useImport = true; importNo = n;
                player = Player();
                player.pos = ilev.born; player.lastSafe = ilev.born;
                itemTaken.assign(ilev.items.size(), 0);
                levelTitle = "Original Lv." + std::to_string(n);
                levelW = ilev.w; levelH = ilev.h;
                manaOn = false;  // this IPA build ships NoMana on all 60 levels
                if (ilev.moonDyn >= 0 && ilev.moonDyn < (int)ilev.dyn.size()) {
                    moonPos = { ilev.dyn[(size_t)ilev.moonDyn].x, ilev.dyn[(size_t)ilev.moonDyn].y };
                    moonR = 48;
                }
                particles.clear();
                state = GameState::PLAYING;
                if (GAudio.hasMenu) StopMusicStream(GAudio.menu);
                if (packReady) {
                    const char* tracks[] = { "music/SpiralofAdventure.ogg", "music/SkyTunnel.ogg",
                                             "music/Hectic.ogg", "music/LifeforSpeed.ogg", nullptr };
                    const char* tr = tracks[(ilev.suite - 1) & 3];
                    std::string p = GAssets.base + tr;
                    if (GAudio.hasStage) { StopMusicStream(GAudio.stage); UnloadMusicStream(GAudio.stage); GAudio.hasStage = false; }
                    if (AssetExists(p.c_str())) {
                        GAudio.stage = LoadMusicStream(p.c_str());
                        GAudio.hasStage = true;
                        PlayMusicStream(GAudio.stage);
                    }
                }
            }
        };
        auto startRemake = [&](int n) {
            useImport = false; remakeNo = n;
            level = BuildLevel(n);
            player = Player();
            player.pos = level.spawn; player.lastSafe = level.spawn;
            levelTitle = level.name;
            levelW = level.width; levelH = level.height;
            manaOn = level.manaEnabled;
            moonPos = level.moon.pos; moonR = level.moon.radius;
            particles.clear();
            state = GameState::PLAYING;
            if (GAudio.hasMenu) StopMusicStream(GAudio.menu);
            if (GAudio.hasStage) { StopMusicStream(GAudio.stage); GAudio.hasStage = false; }
        };

        std::vector<Vector2> pushPoints;
        if (state == GameState::PLAYING) {
            cam.offset = { (float)sw / 2.0f, (float)sh / 2.0f };
            cam.target = player.pos;
            cam.zoom = zoom;
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON))
                pushPoints.push_back(GetScreenToWorld2D(GetMousePosition(), cam));
            for (int i = 0; i < GetTouchPointCount(); i++)
                pushPoints.push_back(GetScreenToWorld2D(GetTouchPosition(i), cam));
            if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))
                pushPoints.push_back({ player.pos.x - 130, player.pos.y + 130 });
            if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT))
                pushPoints.push_back({ player.pos.x + 130, player.pos.y + 130 });
            if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) || IsKeyDown(KEY_SPACE))
                pushPoints.push_back({ player.pos.x, player.pos.y + 150 });
        }

        if (state == GameState::PLAYING) {
            accumulator += frameDt;
            int steps = 0;
            while (accumulator >= FIXED_DT && steps < 12) {
                StepPlayer(player, manaOn, pushPoints, FIXED_DT, particles);
                bool landed = false;
                if (useImport) {
                    for (size_t i = 0; i < ilev.edges.size(); i++)
                        ResolveCirclePoly(player.pos, player.vel, Tune::PLAYER_R, ilev.edges[i], landed);
                    if (player.pos.x < Tune::PLAYER_R) { player.pos.x = Tune::PLAYER_R; player.vel.x = fabsf(player.vel.x) * 0.4f; }
                    if (player.pos.x > levelW - Tune::PLAYER_R) { player.pos.x = levelW - Tune::PLAYER_R; player.vel.x = -fabsf(player.vel.x) * 0.4f; }
                    for (size_t i = 0; i < ilev.items.size(); i++) {
                        if (itemTaken[i]) continue;
                        Vector2 ip = { ilev.items[i].x, ilev.items[i].y };
                        if (CheckCollisionCircles(player.pos, Tune::PLAYER_R + 4, ip, 18)) {
                            itemTaken[i] = 1; player.stars++;
                            if (GAudio.hasPickup) PlaySound(GAudio.pickup);
                            for (int k = 0; k < 10; k++) {
                                Particle pt;
                                pt.pos = ip;
                                pt.vel = { (float)GetRandomValue(-120, 120), (float)GetRandomValue(-160, 20) };
                                pt.life = 0; pt.maxLife = 0.6f; pt.color = GOLD; pt.size = 4;
                                particles.push_back(pt);
                            }
                        }
                    }
                    if (player.pos.y > levelH + 80 || player.pos.y < -400) {
                        state = GameState::DEAD; player.deadTimer = 0;
                        message = "Kiana fell! Tap / R to retry"; messageT = 4;
                        if (GAudio.hasDeath) PlaySound(GAudio.death);
                    }
                    if (CheckCollisionCircles(player.pos, Tune::PLAYER_R, moonPos, moonR)) {
                        state = GameState::WIN;
                        score = 40000 + player.stars * 5000 + (int)fmaxf(0, (ilev.par * 4.0f - player.time)) * 800;
                        if (score > bestScore) bestScore = score;
                        message = "Stage Clear!"; messageT = 6;
                        if (GAudio.hasWin) PlaySound(GAudio.win);
                    }
                } else {
                    for (size_t i = 0; i < level.platforms.size(); i++)
                        ResolveCircleRect(player.pos, player.vel, Tune::PLAYER_R, level.platforms[i].rect, level.platforms[i].oneWay, landed);
                    if (landed) player.lastSafe = player.pos;
                    if (player.pos.x < 40 + Tune::PLAYER_R) { player.pos.x = 40 + Tune::PLAYER_R; player.vel.x = fabsf(player.vel.x) * 0.4f; }
                    if (player.pos.x > level.width - 40 - Tune::PLAYER_R) { player.pos.x = level.width - 40 - Tune::PLAYER_R; player.vel.x = -fabsf(player.vel.x) * 0.4f; }
                    for (size_t i = 0; i < level.stars.size(); i++) {
                        if (!level.stars[i].taken && CheckCollisionCircles(player.pos, Tune::PLAYER_R + 4, level.stars[i].pos, 16)) {
                            level.stars[i].taken = true; player.stars++;
                            if (GAudio.hasPickup) PlaySound(GAudio.pickup);
                        }
                    }
                    for (size_t i = 0; i < level.spikes.size(); i++) {
                        Rectangle inner = { level.spikes[i].rect.x + 4, level.spikes[i].rect.y + 6,
                                            level.spikes[i].rect.width - 8, level.spikes[i].rect.height - 8 };
                        if (CheckCollisionCircleRec(player.pos, Tune::PLAYER_R - 3, inner)) {
                            state = GameState::DEAD; player.deadTimer = 0;
                            message = "Kiana crashed! Tap / R to retry"; messageT = 4;
                            if (GAudio.hasDeath) PlaySound(GAudio.death);
                            break;
                        }
                    }
                    if (player.pos.y > level.height + 80) {
                        state = GameState::DEAD; player.deadTimer = 0;
                        message = "Kiana fell! Tap / R to retry"; messageT = 4;
                        if (GAudio.hasDeath) PlaySound(GAudio.death);
                    }
                    if (CheckCollisionCircles(player.pos, Tune::PLAYER_R, level.moon.pos, level.moon.radius)) {
                        state = GameState::WIN;
                        float par = ParTimeForLevel(remakeNo <= 3 ? remakeNo : 20);
                        score = 40000 + player.stars * 5000 + (int)fmaxf(0, (par * 4.0f - player.time)) * 800;
                        if (score > bestScore) bestScore = score;
                        message = "Stage Clear!"; messageT = 6;
                        if (GAudio.hasWin) PlaySound(GAudio.win);
                    }
                }
                accumulator -= FIXED_DT; steps++;
            }
            if (accumulator > 0.25f) accumulator = 0;
        }

        for (size_t i = 0; i < particles.size();) {
            particles[i].life += frameDt;
            particles[i].pos.x += particles[i].vel.x * frameDt;
            particles[i].pos.y += particles[i].vel.y * frameDt;
            particles[i].vel.y += 300 * frameDt;
            if (particles[i].life >= particles[i].maxLife) particles.erase(particles.begin() + i);
            else i++;
        }

        if (IsKeyPressed(KEY_R) && state != GameState::MENU) {
            if (useImport) startImport(importNo);
            else startRemake(remakeNo);
        }
        if (IsKeyPressed(KEY_M)) {
            state = GameState::MENU;
            if (GAudio.hasStage) { StopMusicStream(GAudio.stage); GAudio.hasStage = false; }
            if (GAudio.hasMenu) PlayMusicStream(GAudio.menu);
        }
        if (IsKeyPressed(KEY_P) || IsKeyPressed(KEY_ESCAPE))
            state = (state == GameState::PLAYING) ? GameState::PAUSED : (state == GameState::PAUSED ? GameState::PLAYING : state);
        // tappable pause button (top-right); mouse edge + touch edge
        {
            static int prevTouchN = 0;
            int touchN = GetTouchPointCount();
            Rectangle pb = { (float)sw - 56, 84, 44, 44 };
            bool tapPb = false;
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), pb)) tapPb = true;
            if (prevTouchN == 0 && touchN > 0 && CheckCollisionPointRec(GetTouchPosition(0), pb)) tapPb = true;
            prevTouchN = touchN;
            if (tapPb && (state == GameState::PLAYING || state == GameState::PAUSED)) {
                state = (state == GameState::PLAYING) ? GameState::PAUSED : GameState::PLAYING;
                if (GAudio.hasClick) PlaySound(GAudio.click);
            }
        }
        if (messageT > 0) messageT -= frameDt;

        if (state == GameState::DEAD || state == GameState::WIN) {
            player.deadTimer += frameDt;
            bool tap = IsMouseButtonPressed(MOUSE_LEFT_BUTTON) || GetTouchPointCount() > 0
                       || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER);
            if (tap && player.deadTimer > 0.6f) {
                if (state == GameState::WIN && useImport && importNo < 60) startImport(importNo + 1);
                else if (state == GameState::WIN && !useImport) startRemake(remakeNo % 4 + 1);
                else if (useImport) startImport(importNo);
                else startRemake(remakeNo);
            }
        }

        // camera: follow X+Y, clamped to level bounds (imported levels vary in width)
        Vector2 desired = player.pos;
        desired.y -= 60;
        static Vector2 camSm = { 480, 2800 };
        if (state == GameState::MENU) camSm = { 480, 1500 };
        else {
            camSm.x += (desired.x - camSm.x) * ClampF(6 * frameDt, 0, 1);
            camSm.y += (desired.y - camSm.y) * ClampF(5 * frameDt, 0, 1);
        }
        float halfH = visH / 2.0f, halfW = visW / 2.0f;
        float maxCamY = levelH - halfH + 40, minCamY = halfH - 120;
        if (maxCamY < minCamY) maxCamY = minCamY;
        camSm.y = ClampF(camSm.y, minCamY, maxCamY);
        if (levelW <= visW) camSm.x = levelW / 2.0f;
        else camSm.x = ClampF(camSm.x, halfW - 40, levelW - halfW + 40);
        cam.target = (state == GameState::MENU) ? Vector2{ 480, 1500 } : camSm;
        cam.offset = { (float)sw / 2.0f, (float)sh / 2.0f };
        cam.zoom = zoom;

        BeginDrawing();
        ClearBackground(Color{ 12, 10, 35, 255 });

        if (state == GameState::MENU) {
            const char* title = "FlyMe2theMoon";
            int tw = MeasureText(title, 56);
            DrawText(title, sw / 2 - tw / 2, (int)(sh * 0.06f), 56, RAYWHITE);
            char subb[160];
            if (packReady)
                snprintf(subb, sizeof subb, "ORIGINAL 60 + remake | atlases %d | %s",
                         (int)GAssets.atlases.size(), GAssets.base.c_str());
            else snprintf(subb, sizeof subb, "REMAKE ONLY - fly/ pack NOT found");
            DrawText(subb, sw / 2 - MeasureText(subb, 16) / 2, (int)(sh * 0.06f) + 66, 16,
                     packReady ? LIGHTGRAY : ORANGE);
            int y0 = (int)(sh * 0.06f) + 100;
            if (packReady) {
                const char* tabs[] = { "ORIGINAL 60", "REMAKE" };
                for (int i = 0; i < 2; i++) {
                    Rectangle rb = { (float)sw / 2 - 220 + (float)i * 220, (float)y0, 210, 36 };
                    bool hov = CheckCollisionPointRec(GetMousePosition(), rb);
                    DrawRectangleRec(rb, (menuTab == i) ? Color{ 70, 60, 140, 255 } : Color{ 25, 25, 55, 255 });
                    DrawText(tabs[i], (int)(rb.x + 105 - MeasureText(tabs[i], 18) / 2), (int)rb.y + 9, 18, hov ? YELLOW : RAYWHITE);
                    if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        menuTab = i;
                        if (GAudio.hasClick) PlaySound(GAudio.click);
                    }
                }
                y0 += 48;
            } else menuTab = 1;
            if (menuTab == 0 && packReady) {
                int cols = 6, per = 30;
                int start = menuPage * per;
                for (int i = 0; i < per; i++) {
                    int n = start + i + 1;
                    if (n > 60) break;
                    int cx = i % cols, cy = i / cols;
                    Rectangle rb = { (float)sw / 2 - 300 + (float)cx * 102, (float)y0 + (float)cy * 46, 94, 38 };
                    bool hov = CheckCollisionPointRec(GetMousePosition(), rb);
                    char b[16]; snprintf(b, sizeof b, "%d", n);
                    DrawRectangleRec(rb, hov ? Color{ 60, 50, 120, 255 } : Color{ 22, 22, 50, 255 });
                    DrawText(b, (int)(rb.x + 47 - MeasureText(b, 20) / 2), (int)rb.y + 9, 20, hov ? YELLOW : RAYWHITE);
                    if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        if (GAudio.hasClick) PlaySound(GAudio.click);
                        startImport(n);
                    }
                }
                const char* pg = menuPage == 0 ? "31-60 >>" : "<< 1-30";
                Rectangle pb = { (float)sw / 2 - 80, (float)y0 + 5 * 46 + 6, 160, 32 };
                bool hov = CheckCollisionPointRec(GetMousePosition(), pb);
                DrawRectangleRec(pb, Color{ 25, 25, 55, 255 });
                DrawText(pg, (int)(pb.x + 80 - MeasureText(pg, 16) / 2), (int)pb.y + 8, 16, hov ? YELLOW : LIGHTGRAY);
                if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) menuPage = 1 - menuPage;
            } else {
                const char* opts[] = { "R1 Moonlit Beginning", "R2 Witch's Ascent",
                                       "R3 Eclipse Trial", "R4 Survival Endless" };
                for (int i = 0; i < 4; i++) {
                    int y = y0 + i * 52;
                    Rectangle rb = { (float)sw / 2 - 300, (float)y - 8, 600, 40 };
                    bool hov = CheckCollisionPointRec(GetMousePosition(), rb);
                    DrawRectangleRec(rb, hov ? Color{ 40, 40, 80, 255 } : Color{ 22, 22, 50, 255 });
                    DrawText(opts[i], sw / 2 - 280, y, 20, hov ? YELLOW : RAYWHITE);
                    if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        if (GAudio.hasClick) PlaySound(GAudio.click);
                        startRemake(i + 1);
                    }
                }
            }
            const char* help = "LEFT half = up-right  |  RIGHT half = up-left  |  A D W / touch / mouse";
            DrawText(help, sw / 2 - MeasureText(help, 15) / 2, sh - 52, 15, GRAY);
            if (bestScore > 0) {
                char b[64]; snprintf(b, sizeof b, "BEST %d", bestScore);
                DrawText(b, sw / 2 - MeasureText(b, 22) / 2, sh - 30, 22, GOLD);
            }
            if (!packReady) {
                const char* w = "fly/ asset pack not found — remake only";
                DrawText(w, sw / 2 - MeasureText(w, 15) / 2, sh - 74, 15, ORANGE);
            }
        } else {
            BeginMode2D(cam);
            for (size_t i = 0; i < g_bgStars.size(); i++) {
                Vector2 s = g_bgStars[i];
                if (fabsf(s.x - cam.target.x) > sw / zoom || fabsf(s.y - cam.target.y) > sh / zoom) continue;
                DrawCircleV(s, 2, Color{ 255, 255, 255, 120 });
            }
            if (useImport) {
                std::string texA = SuiteTexAtlas(ilev.suite), farA = SuiteFarAtlas(ilev.suite);
                DrawRectangle(-60, -120, (int)levelW + 120, (int)levelH + 240, Color{ 24, 20, 60, 255 });
                for (size_t i = 0; i < ilev.bac.size(); i++)
                    DrawAtlasSprite(farA, ilev.bac[i].f, { ilev.bac[i].x, ilev.bac[i].y }, ilev.bac[i].r, ilev.bac[i].flip, WHITE);
                for (size_t i = 0; i < ilev.far.size(); i++)
                    DrawAtlasSprite(farA, ilev.far[i].f, { ilev.far[i].x, ilev.far[i].y }, ilev.far[i].r, ilev.far[i].flip, WHITE);
                for (size_t i = 0; i < ilev.tex.size(); i++)
                    DrawAtlasSprite(texA, ilev.tex[i].f, { ilev.tex[i].x, ilev.tex[i].y }, ilev.tex[i].r, ilev.tex[i].flip, WHITE);
                for (size_t i = 0; i < ilev.items.size(); i++) {
                    if (itemTaken[i]) continue;
                    DrawAtlasSprite("DynamicObjectsLite", ilev.items[i].f,
                                    { ilev.items[i].x, ilev.items[i].y }, 0, false, WHITE);
                }
                for (size_t i = 0; i < ilev.dyn.size(); i++) {
                    if ((int)i == ilev.moonDyn) continue;
                    DrawAtlasSprite("DynamicObjectsLite", ilev.dyn[i].f,
                                    { ilev.dyn[i].x, ilev.dyn[i].y }, ilev.dyn[i].r, false, WHITE);
                }
                // moon gate (dyn sprite + glow)
                if (ilev.moonDyn >= 0 && ilev.moonDyn < (int)ilev.dyn.size()
                    && ilev.dyn[(size_t)ilev.moonDyn].f >= 0) {
                    const DynObj& m = ilev.dyn[(size_t)ilev.moonDyn];
                    DrawCircleV(moonPos, moonR + 10 + sinf(animT * 2.0f) * 4.0f, Color{ 200, 200, 255, 80 });
                    DrawAtlasSprite("DynamicObjectsLite", m.f, moonPos, 0, false, WHITE);
                } else DrawMoonPlaceholder(moonPos, moonR, animT);
                for (size_t i = 0; i < ilev.fore.size(); i++)
                    DrawAtlasSprite(texA, ilev.fore[i].f, { ilev.fore[i].x, ilev.fore[i].y }, ilev.fore[i].r, ilev.fore[i].flip, WHITE);
            } else {
                DrawRectangle(-40, 0, (int)level.width + 80, (int)level.height, Color{ 24, 20, 60, 255 });
                for (size_t i = 0; i < level.platforms.size(); i++)
                    DrawPlatformPlaceholder(level.platforms[i].rect, level.platforms[i].moving, level.platforms[i].oneWay);
                for (size_t i = 0; i < level.spikes.size(); i++) DrawSpikePlaceholder(level.spikes[i].rect);
                for (size_t i = 0; i < level.stars.size(); i++)
                    DrawStarPlaceholder(level.stars[i].pos, animT, level.stars[i].taken);
                DrawMoonPlaceholder(level.moon.pos, level.moon.radius, animT);
            }
            for (size_t i = 0; i < particles.size(); i++) {
                float a = 1.0f - particles[i].life / particles[i].maxLife;
                DrawCircleV(particles[i].pos, particles[i].size * a + 1, Fade(particles[i].color, a));
            }
            if (state != GameState::DEAD) {
                DrawKiana(player.pos, player.tilt, !pushPoints.empty() && !player.outOfMana,
                          false, player.vel, animT);
                // jet flame from the original effects atlas (charEffects/ball)
                if (!pushPoints.empty() && !player.outOfMana && packReady) {
                    int ball = AtlasNameIdx("CharActions", "charEffects/ball/001.png");
                    if (ball >= 0) {
                        float pulse = 1.0f + sinf(animT * 28.0f) * 0.18f;
                        Vector2 fp = { player.pos.x - player.vel.x * 0.02f,
                                       player.pos.y + 30.0f * pulse };
                        DrawAtlasSprite("CharActions", ball, fp, 0, false,
                                        Color{ 255, 220, 150, 230 });
                    }
                }
            }
            EndMode2D();

            DrawRectangle(0, 0, sw / 2, sh, Color{ 255, 255, 255, 8 });
            DrawRectangle(sw / 2, 0, sw - sw / 2, sh, Color{ 150, 200, 255, 8 });
            DrawLine(sw / 2, 0, sw / 2, sh, Color{ 255, 255, 255, 40 });
            DrawText("LEFT: up-right", 16, sh - 60, 16, Color{ 255, 255, 255, 90 });
            const char* rr = "RIGHT: up-left";
            DrawText(rr, sw - MeasureText(rr, 16) - 16, sh - 60, 16, Color{ 255, 255, 255, 90 });

            DrawRectangle(0, 0, sw, 76, Color{ 0, 0, 0, 140 });
            int totalItems = useImport ? (int)ilev.items.size() : (int)level.stars.size();
            char hud[160];
            snprintf(hud, sizeof hud, "%s  |  %.1fs  |  Stars %d/%d", levelTitle.c_str(),
                     player.time, player.stars, totalItems);
            DrawText(hud, 14, 12, 20, RAYWHITE);
            int preview = 40000 + player.stars * 5000;
            Color sc = preview >= 70000 ? MAGENTA : (preview >= 60000 ? ORANGE : RAYWHITE);
            char scb[64]; snprintf(scb, sizeof scb, "Score ~%d  Best %d", preview, bestScore);
            int scx = 14;
            if (packReady) {
                int star = AtlasNameIdx("InGameUI", "starUp.png");
                if (star >= 0) {
                    DrawAtlasStretched("InGameUI", star, { (float)scx, 37, 22, 22 });
                    scx += 26;
                }
            }
            DrawText(scb, scx, 40, 18, sc);
            if (manaOn) {
                int barBg = packReady ? AtlasNameIdx("InGameUI", "manaBar.png") : -1;
                int barFill = packReady ? AtlasNameIdx("InGameUI", "manaFill1.png") : -1;
                if (barBg >= 0 && barFill >= 0) {
                    DrawAtlasStretched("InGameUI", barBg, { (float)sw - 264, 14, 250, 22 });
                    Color mc = player.outOfMana ? RED : WHITE;
                    float fw = 250.0f * player.mana / Tune::MANA_MAX;
                    if (fw > 1) {
                        const AtlasFrame* ff = AtlasFrameAt("InGameUI", barFill);
                        Rectangle src = { ff->x, ff->y, ff->w * (fw / 250.0f), ff->h };
                        Rectangle dst = { (float)sw - 264, 14, fw, 22 };
                        DrawTexturePro(AtlasGet("InGameUI")->tex, src, dst, { 0, 0 }, 0, mc);
                    }
                    if (player.outOfMana) DrawText("OUT OF MANA!", sw - 264, 40, 16, RED);
                } else {
                    DrawRectangle(sw - 264, 14, 250, 22, DARKGRAY);
                    Color mc = player.outOfMana ? RED : (player.mana < 30 ? ORANGE : SKYBLUE);
                    DrawRectangle(sw - 264, 14, (int)(250 * player.mana / Tune::MANA_MAX), 22, mc);
                    DrawRectangleLines(sw - 264, 14, 250, 22, WHITE);
                    DrawText(player.outOfMana ? "OUT OF MANA!" : "MANA", sw - 264, 40, 16,
                             player.outOfMana ? RED : LIGHTGRAY);
                }
            }
            DrawText(TextFormat("FPS %d %dx%d", GetFPS(), sw, sh), 14, 58, 14, GRAY);
            // pause button (original sprite when pack present)
            {
                Rectangle pb = { (float)sw - 56, 84, 44, 44 };
                int pbi = packReady ? AtlasNameIdx("InGameUI", "pauseBtnUp.png") : -1;
                if (pbi >= 0) DrawAtlasStretched("InGameUI", pbi, pb);
                else {
                    DrawRectangleRec(pb, Color{ 0, 0, 0, 120 });
                    DrawText("II", (int)pb.x + 14, (int)pb.y + 10, 20, WHITE);
                }
            }

            if (state == GameState::PAUSED) {
                DrawRectangle(0, 0, sw, sh, Color{ 0, 0, 0, 160 });
                const char* t = "PAUSED — P resume, R restart, M menu";
                DrawText(t, sw / 2 - MeasureText(t, 22) / 2, sh / 2, 22, RAYWHITE);
            }
            if ((state == GameState::DEAD || state == GameState::WIN) && messageT > 0) {
                int fs = (state == GameState::WIN) ? 44 : 26;
                const char* t = (state == GameState::WIN) ? "MOON REACHED!" : message.c_str();
                int w = MeasureText(t, fs);
                DrawRectangle(sw / 2 - w / 2 - 20, sh / 3 - 16, w + 40, fs + 60, Color{ 0, 0, 0, 170 });
                DrawText(t, sw / 2 - w / 2, sh / 3, fs, (state == GameState::WIN) ? GOLD : RED);
                if (state == GameState::WIN) {
                    char sb[96]; snprintf(sb, sizeof sb, "Score %d  %.1fs  Stars %d", score, player.time, player.stars);
                    Color cc = score >= 70000 ? MAGENTA : (score >= 60000 ? ORANGE : RAYWHITE);
                    DrawText(sb, sw / 2 - MeasureText(sb, 22) / 2, sh / 3 + 54, 22, cc);
                    const char* nx = "Tap / Space next  •  R replay  •  M menu";
                    DrawText(nx, sw / 2 - MeasureText(nx, 16) / 2, sh / 3 + 84, 16, LIGHTGRAY);
                } else {
                    const char* nx = "Tap / R retry  •  M menu";
                    DrawText(nx, sw / 2 - MeasureText(nx, 16) / 2, sh / 3 + 44, 16, LIGHTGRAY);
                }
            }
        }
        EndDrawing();
    }

    if (GAudio.hasMenu) UnloadMusicStream(GAudio.menu);
    if (GAudio.hasStage) UnloadMusicStream(GAudio.stage);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}

// =============================================================================
// ANDROID (arm64-v8a): Gradle + NativeActivity + native_app_glue (see repo).
// Pack layout in APK: app/src/main/assets/fly/{tex,ui,sfx,music,atlas,levels}.
// =============================================================================
