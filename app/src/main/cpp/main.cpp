// =============================================================================
// FlyMe2theMoon — Raylib recreation (C++, single file)
// Reverse-engineered from miHoYo 2011 iOS IPA (Fly2theMoon.app)
//   - roleConfig.json  : maxF 28, maxP 51, mass 1, damping 0.1, maxXSpeed 5,
//                        yGravity -10, pushAngleRange 50, bounds 90/120,
//                        stableFlyRate 1.6, pushDistance1 51.2, pushDistance2 64
//   - Map              : 960 x 1440 px, BornPoint, Moon gate at top, Box2D edges
//                        stored in edgeData.dat (polyline soup, PTM=32)
//   - Control          : applyPush:pushPoint: — thrust dir = rolePos - touchPos,
//                        angle clamped to upward cone, magnitude by touch dist.
//                        LEFT-half touch  => up + right, RIGHT-half => up + left.
//   - Systems          : mana/fuel (drain while pushing, regen idle, outOfMana
//                        lockout), stars/items, moon goal, score 60k/70k colors.
//
// Build (desktop, GCC):
//   g++ main.cpp -o flyme2themoon -lraylib -lm -lpthread -ldl -lrt -lX11
//   # Debian/Ubuntu: sudo apt install build-essential libraylib-dev
//   # Or build raylib from source: https://github.com/raysan5/raylib
//
// Build (Android arm64-v8a, NDK r25+):
//   See ANDROID_BUILD_NOTES at bottom of this file.
//   TL;DR: ndk-build APP_ABI=arm64-v8a / or raylib android toolchain + Gradle,
//   NativeActivity, minSdk 21 (64-bit required), portrait orientation.
//
// Controls:
//   Touch / Mouse LEFT-half  = thrust up-right | RIGHT-half = thrust up-left
//   Keyboard: A/Left = left-side thrust, D/Right = right-side, W/Up/Space = up
//   R = restart, P/Esc = pause, 1/2/3/4 = levels, M = menu
// =============================================================================

#include "raylib.h"
#include "raymath.h"

#include <cmath>
#include <vector>
#include <string>
#include <cstdio>

// -----------------------------------------------------------------------------
// 1. ORIGINAL TUNING (verbatim from IPA roleConfig.json, Box2D / SI units)
// -----------------------------------------------------------------------------
namespace Orig {
    constexpr float MAX_F = 28.0f;          // min push force  (N, mass=1)
    constexpr float MAX_P = 51.0f;          // max push force  (N)
    constexpr float MASS = 1.0f;            // kg
    constexpr float DAMPING = 0.1f;         // Box2D linearDamping
    constexpr float MAX_X_SPEED = 5.0f;     // m/s (horizontal cap)
    constexpr float EXTRA_X_DAMP = 1.0f;    // extra horizontal damping (flying)
    constexpr float EXTRA_NONFLY_X_DAMP = 1.0f; // extra horizontal damping (glide)
    constexpr float GRAVITY = 10.0f;        // m/s^2 (Box2D yGravity -10)
    constexpr float PUSH_ANGLE_RANGE = 50.0f;   // deg half-cone around straight-up
    constexpr float PUSH_DIST1 = 51.2f;     // px deadzone start (touch dist)
    constexpr float PUSH_DIST2 = 64.0f;     // px full-power dist
    constexpr float STABLE_FLY_RATE = 1.6f; // hover-assist factor
    constexpr float PTM = 32.0f;            // pixels-to-meter (Cocos2D default)
}

// Pixel-space derived tuning (SI * PTM). Physics is integrated in pixels but
// ratios are identical to the original Box2D sim.
namespace Tune {
    constexpr float GRAVITY_PX   = Orig::GRAVITY * Orig::PTM;   // 320 px/s^2
    constexpr float THRUST_MIN   = Orig::MAX_F * Orig::PTM;     // 896 px/s^2
    constexpr float THRUST_MAX   = Orig::MAX_P * Orig::PTM;     // 1632 px/s^2
    constexpr float MAX_X_PX     = Orig::MAX_X_SPEED * Orig::PTM; // 160 px/s
    constexpr float MAX_Y_PX     = 12.0f * Orig::PTM;           // 384 px/s (soft cap, gameplay)
    constexpr float WORLD_W      = 960.0f;  // matches original Map Width
    constexpr float PLAYER_R     = 20.0f;
    constexpr float MANA_MAX     = 100.0f;
    constexpr float MANA_DRAIN   = 32.0f;   // /s while thrusting
    constexpr float MANA_REGEN   = 22.0f;   // /s idle (manaRecoverNotPushingPerS)
}

// Original score thresholds (NamuWiki): <=60000 orange, >70000 magenta.
namespace ScoreStyle {
    constexpr int ORANGE_CAP = 60000;
    constexpr int MAGENTA_THRESHOLD = 70000;
}

// Par times straight from IPA levelScore.json "svrs" (used for score bonus).
static float ParTimeForLevel(int lvl) {
    static const float t[61] = {0,1.75f,10,9,8.1f,14,15,22.5f,17.5f,16,16,15,16,24,8,7.5f,
        11,15,8,5,8,8,12,15,7,11,8,15,8,10,7.5f,15,19,5,8.5f,10,9.5f,17,5.5f,10,14,6,
        16,10,15,31,12,5,64,15,9,42,5,11,10,7,29,8,13,10};
    if (lvl >= 1 && lvl <= 60) return t[lvl];
    return 15.0f;
}

// =============================================================================
// 2. TEXTURE SWAP POINTS — replace these with LoadTexture()/DrawTexturePro()
//    later. Everything else stays identical.
// =============================================================================
static void DrawKianaPlaceholder(Vector2 pos, float tiltDeg, bool thrusting, bool dead) {
    // TODO(texture): Texture2D kiana = LoadTexture("assets/kiana.png");
    //   DrawTexturePro(kiana, src, dst(pos, tilt), origin, tiltDeg, WHITE);
    Color body = dead ? RED : Color{ 255, 150, 200, 255 };   // witch pink
    Color cloak = Color{ 90, 60, 160, 255 };                  // Kaslana violet
    DrawCircleV(pos, Tune::PLAYER_R + 4, cloak);              // cloak / hit ring
    DrawCircleV(pos, Tune::PLAYER_R, body);                  // body
    // broom / jet direction indicator
    Vector2 nose = { pos.x + sinf(tiltDeg * DEG2RAD) * 22.0f,
                     pos.y - cosf(tiltDeg * DEG2RAD) * 22.0f };
    DrawCircleV(nose, 6, WHITE);
    // eyes (facing up)
    DrawCircleV({ pos.x - 7, pos.y - 6 }, 3, BLACK);
    DrawCircleV({ pos.x + 7, pos.y - 6 }, 3, BLACK);
    if (thrusting) {
        Vector2 flame = { pos.x - sinf(tiltDeg * DEG2RAD) * 30.0f,
                          pos.y + cosf(tiltDeg * DEG2RAD) * 30.0f };
        DrawCircleV(flame, 8, ORANGE);
        DrawCircleV(flame, 4, YELLOW);
    }
}
static void DrawPlatformPlaceholder(Rectangle r, bool moving, bool oneWay) {
    // TODO(texture): DrawTexturePro(platformTex, ...) tiled over r
    Color c = moving ? GREEN : (oneWay ? SKYBLUE : DARKGRAY);
    DrawRectangleRec(r, c);
    DrawRectangleLinesEx(r, 3, BLACK);
    if (oneWay) DrawLine((int)r.x, (int)r.y, (int)(r.x + r.width), (int)r.y, WHITE);
}
static void DrawStarPlaceholder(Vector2 pos, float t, bool taken) {
    // TODO(texture): DrawTexturePro(starTex, ...) with spin
    if (taken) return;
    float s = 12.0f + sinf(t * 4.0f) * 2.0f;
    DrawCircleV(pos, s + 4, GOLD);
    DrawCircleV(pos, s, YELLOW);
    DrawCircleV(pos, 4, WHITE);
}
static void DrawMoonPlaceholder(Vector2 pos, float r, float t) {
    // TODO(texture): moonGateTex + glow shader
    DrawCircleV(pos, r + 8 + sinf(t * 2.0f) * 3.0f, Color{ 200, 200, 255, 90 });
    DrawCircleV(pos, r, Color{ 240, 240, 255, 255 });
    DrawCircleLines((int)pos.x, (int)pos.y, r, DARKBLUE);
    DrawCircleV({ pos.x - r * 0.25f, pos.y - r * 0.2f }, r * 0.16f, LIGHTGRAY);
    DrawCircleV({ pos.x + r * 0.2f, pos.y + r * 0.25f }, r * 0.12f, LIGHTGRAY);
}
static void DrawSpikePlaceholder(Rectangle r) {
    // TODO(texture): spikeTex
    DrawRectangleRec(r, RED);
    DrawRectangleLinesEx(r, 2, MAROON);
    DrawText("!", (int)(r.x + r.width / 2 - 4), (int)(r.y + 2), 16, WHITE);
}

// =============================================================================
// 3. DATA
// =============================================================================
enum class GameState { MENU, PLAYING, PAUSED, DEAD, WIN };

struct Platform {
    Rectangle rect{};
    bool oneWay = false;
    bool moving = false;
    Vector2 moveAxis{ 1, 0 };
    float moveRange = 0, moveSpeed = 0, phase = 0;
    Vector2 basePos{};
};

struct Star  { Vector2 pos{}; bool taken = false; };
struct Spike { Rectangle rect{}; };
struct Goal  { Vector2 pos{}; float radius = 46; };

struct Particle {
    Vector2 pos{}, vel{};
    float life = 0, maxLife = 1;
    Color color = WHITE;
    float size = 4;
};

struct Level {
    int number = 1;
    std::string name;
    float width = Tune::WORLD_W;
    float height = 3200;
    Vector2 spawn{};
    bool manaEnabled = true;   // Level_001 has <NoMana/> (tutorial unlimited)
    std::vector<Platform> platforms;
    std::vector<Star> stars;
    std::vector<Spike> spikes;
    Goal moon{};
};

struct Player {
    Vector2 pos{}, vel{};
    float mana = Tune::MANA_MAX;
    bool outOfMana = false;
    bool dead = false;
    float deadTimer = 0;
    int stars = 0;
    float time = 0;
    float tilt = 0;            // visual lean (deg)
    Vector2 lastSafe{};
};

// Parallax starfield background (replaces Farground/Background layers)
static std::vector<Vector2> g_bgStars;

// -----------------------------------------------------------------------------
// Level builders — homage to original Level_001..003 layout:
// floor + side walls (edgeData.dat shaft), staircase platforms, stars, moon.
// Coordinates: y-DOWN pixels, spawn near bottom, moon near top.
// -----------------------------------------------------------------------------
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

    if (n == 1) {           // ---- Level 1: tutorial, NoMana (original <NoMana/>) ----
        lv.name = "Lv.1 Moonlit Beginning";
        lv.height = 3000; lv.manaEnabled = false;
        lv.spawn = { 480, 2780 };
        lv.moon = { { 480, 220 }, 52 };
        AddWall(lv, -30, 0, 60, lv.height);            // left shaft wall
        AddWall(lv, lv.width - 30, 0, 60, lv.height);  // right shaft wall
        AddWall(lv, 0, lv.height - 40, lv.width, 60);  // floor
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
        lv.spikes = { {{{430, 1200, 100, 22}}} };
    } else if (n == 2) {    // ---- Level 2: mana on, moving platforms ----
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
        lv.spikes = { {{{200,1740,120,22}}}, {{{640,980,120,22}}}, {{{400,480,160,22}}} };
    } else if (n == 3) {    // ---- Level 3: puzzle, narrow + spikes ----
        lv.name = "Lv.3 Eclipse Trial";
        lv.height = 4000; lv.manaEnabled = true;
        lv.spawn = { 200, 3780 };
        lv.moon = { { 760, 220 }, 50 };
        AddWall(lv, -30, 0, 60, lv.height);
        AddWall(lv, lv.width - 30, 0, 60, lv.height);
        AddWall(lv, 0, lv.height - 40, lv.width, 60);
        AddWall(lv, 300, 2400, 40, 900);   // middle divider (puzzle route)
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
        lv.spikes = { {{{120,2620,140,22}}}, {{{560,1370,140,22}}}, {{{150,840,140,22}}}};
    } else {                // ---- Survival / endless template (procedural rows) ----
        lv.name = "Survival: Endless Night";
        lv.height = 6000; lv.manaEnabled = true;
        lv.spawn = { 480, 5800 };
        lv.moon = { { 480, 200 }, 55 };
        AddWall(lv, -30, 0, 60, lv.height);
        AddWall(lv, lv.width - 30, 0, 60, lv.height);
        AddWall(lv, 0, lv.height - 40, lv.width, 60);
        unsigned seed = 12345;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
        for (float y = 5500; y > 600; y -= 260) {
            float x = 100 + rnd() * 600;
            float w = 130 + rnd() * 90;
            bool mv = rnd() > 0.72f;
            AddPlat(lv, x, y, w, true, mv, 90 + rnd() * 80, 0.8f + rnd(), rnd() * 6);
            if (rnd() > 0.55f) lv.stars.push_back({ { x + w / 2, y - 60 } });
            if (y < 4800 && rnd() > 0.8f) lv.spikes.push_back({ { x, y - 130, w, 20 } });
        }
    }
    return lv;
}

// =============================================================================
// 4. PHYSICS — faithful to roleConfig + applyPush:pushPoint:
//    thrustDir = normalize(rolePos - touchPos), angle clamped to up-cone,
//    magnitude from touch distance [PUSH_DIST1, PUSH_DIST2] -> [MAX_F, MAX_P].
// =============================================================================
static float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct PushResult { Vector2 accel{0,0}; bool active = false; float angleDeg = 90; float mag = 0; };

// Single push-point -> acceleration (px/s^2). touchWorld must be in world px.
static PushResult PushFromPoint(Vector2 rolePos, Vector2 touchWorld) {
    PushResult r;
    Vector2 d = { rolePos.x - touchWorld.x, rolePos.y - touchWorld.y };
    // y-DOWN screen: up is -y, so flip for math angle (y-up), 90 deg = straight up
    float dist = sqrtf(d.x * d.x + d.y * d.y);
    if (dist < 1e-3f) return r;
    float ang = atan2f(-d.y, d.x) * RAD2DEG;   // -180..180, 90 = up
    // Clamp to upward cone (pushAngleRange). Touches above the role still push up.
    float lo = 90.0f - Orig::PUSH_ANGLE_RANGE;
    float hi = 90.0f + Orig::PUSH_ANGLE_RANGE;
    float clamped = ClampF(ang, lo, hi);
    // Note: touches above the role would yield a downward angle; the clamp
    // snaps them to the nearest upward diagonal — the original never dives.
    float t = ClampF((dist - Orig::PUSH_DIST1) / (Orig::PUSH_DIST2 - Orig::PUSH_DIST1), 0.0f, 1.0f);
    float force = Orig::MAX_F + (Orig::MAX_P - Orig::MAX_F) * t;  // N
    float a = force / Orig::MASS * Orig::PTM;                     // px/s^2
    float rad = clamped * DEG2RAD;
    r.accel = { cosf(rad) * a, -sinf(rad) * a };  // back to y-down
    r.active = true; r.angleDeg = clamped; r.mag = a;
    return r;
}

static void StepPlayer(Player& p, const Level& lv, const std::vector<Vector2>& pushPoints, float dt,
                       std::vector<Particle>& particles) {
    // --- accumulate thrust from all active push points (multi-touch sum) ---
    Vector2 acc = { 0, Tune::GRAVITY_PX };
    bool thrusting = false;
    float tiltTarget = 0;
    for (Vector2 tp : pushPoints) {
        if (p.outOfMana && lv.manaEnabled) continue;
        PushResult pr = PushFromPoint(p.pos, tp);
        if (!pr.active) continue;
        // stableFlyRate: hover assist when nearly hovering (reverse-engineered)
        float assist = 1.0f;
        if (fabsf(p.vel.y) < 70.0f) assist = 1.0f + (Orig::STABLE_FLY_RATE - 1.0f) * 0.5f;
        acc.x += pr.accel.x * assist;
        acc.y += pr.accel.y * assist;
        thrusting = true;
        tiltTarget = ClampF((90.0f - pr.angleDeg) * 1.4f, -38.0f, 38.0f);
        // exhaust particles opposite thrust
        if ((int)(p.time * 120) % 2 == 0) {
            Vector2 dir = { -pr.accel.x, -pr.accel.y };
            float l = sqrtf(dir.x*dir.x + dir.y*dir.y) + 1e-4f;
            dir.x /= l; dir.y /= l;
            particles.push_back({ { p.pos.x + dir.x*10, p.pos.y + 14 },
                { dir.x*160 + (float)GetRandomValue(-30,30), dir.y*160 },
                0, 0.45f, thrusting ? ORANGE : GRAY, 5 });
        }
    }

    // --- integrate (semi-implicit Euler, fixed dt) ---
    p.vel.x += acc.x * dt;
    p.vel.y += acc.y * dt;

    // --- damping: Box2D linearDamping + extra X damping (fly vs glide) ---
    float damp = Orig::DAMPING + (thrusting ? Orig::EXTRA_X_DAMP * 0.35f : Orig::EXTRA_NONFLY_X_DAMP * 0.55f);
    float keep = 1.0f / (1.0f + damp * dt);
    p.vel.x *= keep;
    p.vel.y *= 1.0f / (1.0f + Orig::DAMPING * 0.45f * dt); // lighter vertical drag

    // --- caps (maxXSpeed verbatim, soft vertical cap for playability) ---
    p.vel.x = ClampF(p.vel.x, -Tune::MAX_X_PX * 2.2f, Tune::MAX_X_PX * 2.2f);
    p.vel.y = ClampF(p.vel.y, -Tune::MAX_Y_PX * 1.6f, Tune::MAX_Y_PX);

    p.pos.x += p.vel.x * dt;
    p.pos.y += p.vel.y * dt;

    // --- mana / fuel ---
    if (lv.manaEnabled) {
        if (thrusting) {
            p.mana -= Tune::MANA_DRAIN * dt;
            if (p.mana <= 0) { p.mana = 0; p.outOfMana = true; }
        } else {
            p.mana += Tune::MANA_REGEN * dt;   // manaRecoverNotPushingPerS
            if (p.mana > Tune::MANA_MAX) p.mana = Tune::MANA_MAX;
            if (p.outOfMana && p.mana > 30.0f) p.outOfMana = false;
        }
    } else p.mana = Tune::MANA_MAX;

    // --- tilt visual follows horizontal velocity + thrust lean ---
    float velTilt = ClampF(p.vel.x * 0.09f, -30.0f, 30.0f);
    p.tilt += ((thrusting ? tiltTarget * 0.6f + velTilt * 0.4f : velTilt) - p.tilt)
              * ClampF(10.0f * dt, 0.0f, 1.0f);

    p.time += dt;
}

// Circle vs rect resolve. Returns true if collided. `landed` set when feet hit.
static bool ResolveCircleRect(Vector2& pos, Vector2& vel, float r, Rectangle rc,
                              bool oneWay, bool& landed) {
    float cx = ClampF(pos.x, rc.x, rc.x + rc.width);
    float cy = ClampF(pos.y, rc.y, rc.y + rc.height);
    float dx = pos.x - cx, dy = pos.y - cy;
    float d2 = dx * dx + dy * dy;
    if (d2 > r * r) return false;
    if (oneWay) {
        // pass through from below/side; only land when falling onto top surface
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
    if (vn < 0) { vel.x -= (1.0f + 0.15f) * vn * n.x; vel.y -= (1.0f + 0.15f) * vn * n.y; }
    if (n.y < -0.6f) landed = true;
    return true;
}

// =============================================================================
// 5. APP
// =============================================================================
int main() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    // Portrait-first like the 2011 iPhone original; desktop letterboxes fine.
    InitWindow(540, 960, "FlyMe2theMoon — Raylib Recreation (miHoYo 2011 homage)");
    SetTargetFPS(60);

    GameState state = GameState::MENU;
    int levelIndex = 1;                 // 1..3, 4 = survival
    Level level = BuildLevel(1);
    Player player{};
    player.pos = level.spawn; player.lastSafe = level.spawn;
    std::vector<Particle> particles;
    Camera2D cam{}; cam.rotation = 0;

    // background starfield in world space
    g_bgStars.reserve(160);
    for (int i = 0; i < 160; i++)
        g_bgStars.push_back({ (float)GetRandomValue(0, 960), (float)GetRandomValue(0, 6000) });

    const float FIXED_DT = 1.0f / 120.0f;
    float accumulator = 0;
    float animT = 0;
    int score = 0, bestScore = 0;
    std::string message;
    float messageT = 0;

    while (!WindowShouldClose()) {
        float frameDt = GetFrameTime();
        if (frameDt > 0.1f) frameDt = 0.1f;
        animT += frameDt;

        int sw = GetScreenWidth(), sh = GetScreenHeight();

        // ---- responsive camera: width-locked (handles 16:9 & 20:9 phones) ----
        // Visible world height adapts to aspect: tall 20:9 phones see more.
        float zoom = (float)sw / Tune::WORLD_W;
        if (zoom < 0.05f) zoom = 0.05f;
        float visH = (float)sh / zoom;

        // ---- moving platforms tick (render-rate is fine) ----
        if (state == GameState::PLAYING) {
            for (auto& pl : level.platforms) if (pl.moving)
                pl.rect.x = pl.basePos.x + sinf(animT * pl.moveSpeed + pl.phase) * pl.moveRange;
        }

        // ---- gather push points (touch + mouse + keys) in WORLD coords ----
        std::vector<Vector2> pushPoints;
        if (state == GameState::PLAYING) {
            cam.offset = { (float)sw / 2.0f, (float)sh / 2.0f };
            cam.target = player.pos;
            cam.zoom = zoom;
            // mouse / touch
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON))
                pushPoints.push_back(GetScreenToWorld2D(GetMousePosition(), cam));
            for (int i = 0; i < GetTouchPointCount(); i++)
                pushPoints.push_back(GetScreenToWorld2D(GetTouchPoint(i), cam));
            // keyboard synthesizes virtual push-points (desktop testing)
            if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))
                pushPoints.push_back({ player.pos.x - 130, player.pos.y + 130 });
            if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT))
                pushPoints.push_back({ player.pos.x + 130, player.pos.y + 130 });
            if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) || IsKeyDown(KEY_SPACE))
                pushPoints.push_back({ player.pos.x, player.pos.y + 150 });
        }

        // ---- fixed-step physics ----
        if (state == GameState::PLAYING) {
            accumulator += frameDt;
            int steps = 0;
            while (accumulator >= FIXED_DT && steps < 12) {
                StepPlayer(player, level, pushPoints, FIXED_DT, particles);
                // collisions
                bool landed = false;
                for (auto& pl : level.platforms)
                    ResolveCircleRect(player.pos, player.vel, Tune::PLAYER_R, pl.rect, pl.oneWay, landed);
                if (landed) player.lastSafe = player.pos;
                // world shaft clamp (side walls are also physical)
                if (player.pos.x < 40 + Tune::PLAYER_R) { player.pos.x = 40 + Tune::PLAYER_R; player.vel.x = fabsf(player.vel.x) * 0.4f; }
                if (player.pos.x > level.width - 40 - Tune::PLAYER_R) { player.pos.x = level.width - 40 - Tune::PLAYER_R; player.vel.x = -fabsf(player.vel.x) * 0.4f; }
                // stars
                for (auto& s : level.stars) {
                    if (!s.taken && CheckCollisionCircles(player.pos, Tune::PLAYER_R + 4, s.pos, 16)) {
                        s.taken = true; player.stars++;
                        for (int k = 0; k < 10; k++)
                            particles.push_back({ s.pos, { (float)GetRandomValue(-120,120), (float)GetRandomValue(-160,20) }, 0, 0.6f, GOLD, 4 });
                    }
                }
                // spikes
                for (auto& sp : level.spikes) {
                    Rectangle inner = { sp.rect.x + 4, sp.rect.y + 6, sp.rect.width - 8, sp.rect.height - 8 };
                    if (CheckCollisionCircleRec(player.pos, Tune::PLAYER_R - 3, inner)) {
                        state = GameState::DEAD; player.deadTimer = 0;
                        message = "Kiana crashed! Tap / R to retry"; messageT = 4;
                        break;
                    }
                }
                // fell out / ceiling moon
                if (player.pos.y > level.height + 80) { state = GameState::DEAD; player.deadTimer = 0; message = "Kiana fell! Tap / R to retry"; messageT = 4; }
                if (CheckCollisionCircles(player.pos, Tune::PLAYER_R, level.moon.pos, level.moon.radius)) {
                    state = GameState::WIN;
                    float par = ParTimeForLevel(levelIndex <= 3 ? levelIndex : 20);
                    score = 40000 + player.stars * 5000 + (int)fmaxf(0, (par * 4.0f - player.time)) * 800;
                    if (score > bestScore) bestScore = score;
                    message = "Stage Clear!"; messageT = 6;
                }
                accumulator -= FIXED_DT; steps++;
            }
            if (accumulator > 0.25f) accumulator = 0;
        }

        // ---- particles tick ----
        for (size_t i = 0; i < particles.size();) {
            particles[i].life += frameDt;
            particles[i].pos.x += particles[i].vel.x * frameDt;
            particles[i].pos.y += particles[i].vel.y * frameDt;
            particles[i].vel.y += 300 * frameDt;
            if (particles[i].life >= particles[i].maxLife) particles.erase(particles.begin() + i);
            else i++;
        }

        // ---- global keys ----
        if (IsKeyPressed(KEY_R) && (state == GameState::DEAD || state == GameState::WIN || state == GameState::PLAYING)) {
            level = BuildLevel(levelIndex); player = Player{};
            player.pos = level.spawn; player.lastSafe = level.spawn;
            particles.clear(); state = GameState::PLAYING;
        }
        if (IsKeyPressed(KEY_M)) state = GameState::MENU;
        if (IsKeyPressed(KEY_P) || IsKeyPressed(KEY_ESCAPE))
            state = (state == GameState::PLAYING) ? GameState::PAUSED : (state == GameState::PAUSED ? GameState::PLAYING : state);
        if (IsKeyPressed(KEY_ONE))   { levelIndex = 1; level = BuildLevel(1); player = Player{}; player.pos = level.spawn; player.lastSafe = level.spawn; particles.clear(); state = GameState::PLAYING; }
        if (IsKeyPressed(KEY_TWO))   { levelIndex = 2; level = BuildLevel(2); player = Player{}; player.pos = level.spawn; player.lastSafe = level.spawn; particles.clear(); state = GameState::PLAYING; }
        if (IsKeyPressed(KEY_THREE)) { levelIndex = 3; level = BuildLevel(3); player = Player{}; player.pos = level.spawn; player.lastSafe = level.spawn; particles.clear(); state = GameState::PLAYING; }
        if (IsKeyPressed(KEY_FOUR))  { levelIndex = 4; level = BuildLevel(4); player = Player{}; player.pos = level.spawn; player.lastSafe = level.spawn; particles.clear(); state = GameState::PLAYING; }
        if (messageT > 0) messageT -= frameDt;

        // dead/win tap-to-continue
        if ((state == GameState::DEAD || state == GameState::WIN)) {
            player.deadTimer += frameDt;
            bool tap = IsMouseButtonPressed(MOUSE_LEFT_BUTTON) || GetTouchPointCount() > 0 || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER);
            if (tap && player.deadTimer > 0.6f) {
                if (state == GameState::WIN) {  // advance like original stage flow
                    levelIndex = (levelIndex % 4) + 1;
                    level = BuildLevel(levelIndex);
                } else level = BuildLevel(levelIndex);
                player = Player{}; player.pos = level.spawn; player.lastSafe = level.spawn;
                particles.clear(); state = GameState::PLAYING;
            }
        }

        // ---- camera follow (smooth, clamped to shaft) ----
        Vector2 desired = player.pos;
        desired.y -= 60; // look slightly ahead upward
        static Vector2 camSm = { 480, 2800 };
        if (state == GameState::MENU) camSm = { 480, 1500 };
        else {
            camSm.x += (desired.x - camSm.x) * ClampF(6 * frameDt, 0, 1);
            camSm.y += (desired.y - camSm.y) * ClampF(5 * frameDt, 0, 1);
        }
        camSm.x = Tune::WORLD_W / 2.0f; // shaft levels are width-locked
        float halfH = visH / 2.0f;
        float maxCamY = level.height - halfH + 40;
        float minCamY = halfH - 120;
        if (maxCamY < minCamY) maxCamY = minCamY;
        camSm.y = ClampF(camSm.y, minCamY, maxCamY);
        cam.target = (state == GameState::PLAYING || state == GameState::PAUSED ||
                      state == GameState::DEAD || state == GameState::WIN) ? camSm : Vector2{480, 1500};
        cam.offset = { (float)sw / 2.0f, (float)sh / 2.0f };
        cam.zoom = zoom;

        // ================= DRAW =================
        BeginDrawing();
        ClearBackground(Color{ 12, 10, 35, 255 }); // night sky (original moonlit tone)

        if (state == GameState::MENU) {
            // title menu (screen space)
            const char* title = "FlyMe2theMoon";
            int tw = MeasureText(title, 56);
            DrawText(title, sw / 2 - tw / 2, (int)(sh * 0.16f), 56, RAYWHITE);
            const char* sub = "miHoYo 2011 homage  •  Raylib  •  Kiana Kaslana";
            int sw2 = MeasureText(sub, 18);
            DrawText(sub, sw / 2 - sw2 / 2, (int)(sh * 0.16f) + 70, 18, LIGHTGRAY);
            const char* opts[] = { "1 — Lv.1  Moonlit Beginning  (no fuel, tutorial)",
                                   "2 — Lv.2  Witch's Ascent     (fuel + moving)",
                                   "3 — Lv.3  Eclipse Trial      (puzzle)",
                                   "4 — Survival: Endless Night  (procedural)" };
            for (int i = 0; i < 4; i++) {
                int y = (int)(sh * 0.36f) + i * 52;
                bool hov = CheckCollisionPointRec(GetMousePosition(), { (float)sw / 2 - 300, (float)y - 8, 600, 40 });
                DrawRectangle(sw / 2 - 300, y - 8, 600, 40, hov ? Color{40,40,80,255} : Color{22,22,50,255});
                DrawText(opts[i], sw / 2 - 280, y, 20, hov ? YELLOW : RAYWHITE);
                if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    levelIndex = i + 1; level = BuildLevel(levelIndex);
                    player = Player{}; player.pos = level.spawn; player.lastSafe = level.spawn;
                    particles.clear(); state = GameState::PLAYING;
                    camSm = player.pos;
                }
            }
            const char* help = "TOUCH left half = fly up-right  |  right half = fly up-left\n"
                               "Mouse / multi-touch supported  •  Keys: A D W / arrows / Space\n"
                               "Collect stars, avoid red spikes, reach the MOON gate on top.";
            DrawText(help, sw / 2 - MeasureText("TOUCH left half = fly up-right  |  right half = fly up-left", 18) / 2,
                     (int)(sh * 0.36f) + 4 * 52 + 20, 18, GRAY);
            if (bestScore > 0) {
                char b[64]; snprintf(b, sizeof b, "BEST SCORE: %d", bestScore);
                DrawText(b, sw / 2 - MeasureText(b, 22) / 2, (int)(sh * 0.36f) + 4 * 52 + 90, 22, GOLD);
            }
        } else {
            BeginMode2D(cam);
            // parallax bg stars
            for (Vector2 s : g_bgStars) {
                if (fabsf(s.x - cam.target.x) > sw / zoom) continue;
                if (fabsf(s.y - cam.target.y) > sh / zoom) continue;
                DrawCircleV(s, 2, Color{ 255, 255, 255, 120 });
            }
            // gradient shaft backdrop
            DrawRectangle(-40, 0, (int)level.width + 80, (int)level.height, Color{ 24, 20, 60, 255 });
            DrawRectangle(-40, 0, (int)level.width + 80, (int)level.height / 3, Color{ 35, 30, 90, 255 });
            // platforms / spikes / stars / moon
            for (auto& pl : level.platforms) DrawPlatformPlaceholder(pl.rect, pl.moving, pl.oneWay);
            for (auto& sp : level.spikes) DrawSpikePlaceholder(sp.rect);
            for (auto& s : level.stars) DrawStarPlaceholder(s.pos, animT, s.taken);
            DrawMoonPlaceholder(level.moon.pos, level.moon.radius, animT);
            // spawn marker
            DrawCircleLines((int)level.spawn.x, (int)level.spawn.y, 26, Color{ 255,255,255,120 });
            // particles
            for (auto& pt : particles) {
                float a = 1.0f - pt.life / pt.maxLife;
                DrawCircleV(pt.pos, pt.size * a + 1, Fade(pt.color, a));
            }
            // Kiana (skip while dead)
            if (state != GameState::DEAD)
                DrawKianaPlaceholder(player.pos, player.tilt,
                    !pushPoints.empty() && !player.outOfMana, false);
            EndMode2D();

            // ---- touch-zone overlay (faint, helps learn dual-side control) ----
            DrawRectangle(0, 0, sw / 2, sh, Color{ 255, 255, 255, 8 });
            DrawRectangle(sw / 2, 0, sw - sw / 2, sh, Color{ 150, 200, 255, 8 });
            DrawLine(sw / 2, 0, sw / 2, sh, Color{ 255, 255, 255, 40 });
            DrawText("LEFT: push UP-RIGHT", 16, sh - 60, 16, Color{255,255,255,90});
            const char* rr = "RIGHT: push UP-LEFT";
            DrawText(rr, sw - MeasureText(rr, 16) - 16, sh - 60, 16, Color{255,255,255,90});

            // ---- HUD ----
            DrawRectangle(0, 0, sw, 76, Color{ 0, 0, 0, 140 });
            char hud[128];
            snprintf(hud, sizeof hud, "%s  |  Time %.1fs  |  Stars %d/%d", level.name.c_str(),
                     player.time, player.stars, (int)level.stars.size());
            DrawText(hud, 14, 12, 20, RAYWHITE);
            // score preview with original color rule
            int preview = 40000 + player.stars * 5000;
            Color sc = preview >= ScoreStyle::MAGENTA_THRESHOLD ? MAGENTA
                     : preview >= ScoreStyle::ORANGE_CAP ? ORANGE : RAYWHITE;
            char scb[64]; snprintf(scb, sizeof scb, "Score ~%d  Best %d", preview, bestScore);
            DrawText(scb, 14, 40, 18, sc);
            // mana bar (hidden on NoMana tutorial like original)
            if (level.manaEnabled) {
                DrawRectangle(sw - 264, 14, 250, 22, DARKGRAY);
                Color mc = player.outOfMana ? RED : (player.mana < 30 ? ORANGE : SKYBLUE);
                DrawRectangle(sw - 264, 14, (int)(250 * player.mana / Tune::MANA_MAX), 22, mc);
                DrawRectangleLines(sw - 264, 14, 250, 22, WHITE);
                DrawText(player.outOfMana ? "OUT OF MANA!" : "MANA", sw - 264, 40, 16,
                         player.outOfMana ? RED : LIGHTGRAY);
            } else DrawText("TUTORIAL: unlimited fly", sw - 300, 22, 18, GREEN);
            DrawText(TextFormat("FPS %d  %dx%d  zoom %.2f", GetFPS(), sw, sh, zoom), 14, 58, 14, GRAY);

            if (state == GameState::PAUSED) {
                DrawRectangle(0, 0, sw, sh, Color{ 0, 0, 0, 160 });
                const char* t = "PAUSED — P to resume, R restart, M menu";
                DrawText(t, sw / 2 - MeasureText(t, 22) / 2, sh / 2, 22, RAYWHITE);
            }
            if ((state == GameState::DEAD || state == GameState::WIN) && messageT > 0) {
                int fs = (state == GameState::WIN) ? 44 : 26;
                const char* t = (state == GameState::WIN) ? "MOON REACHED!" : message.c_str();
                int w = MeasureText(t, fs);
                DrawRectangle(sw/2 - w/2 - 20, sh/3 - 16, w + 40, fs + 60, Color{0,0,0,170});
                DrawText(t, sw / 2 - w / 2, sh / 3, fs, (state == GameState::WIN) ? GOLD : RED);
                if (state == GameState::WIN) {
                    char sb[96]; snprintf(sb, sizeof sb, "Score %d  Time %.1fs  Stars %d", score, player.time, player.stars);
                    Color cc = score >= ScoreStyle::MAGENTA_THRESHOLD ? MAGENTA
                             : score >= ScoreStyle::ORANGE_CAP ? ORANGE : RAYWHITE;
                    DrawText(sb, sw/2 - MeasureText(sb, 22)/2, sh/3 + 54, 22, cc);
                    const char* nx = "Tap / Space for next stage  •  R replay  •  M menu";
                    DrawText(nx, sw/2 - MeasureText(nx, 16)/2, sh/3 + 84, 16, LIGHTGRAY);
                } else {
                    const char* nx = "Tap / R to retry  •  M menu";
                    DrawText(nx, sw/2 - MeasureText(nx, 16)/2, sh/3 + 44, 16, LIGHTGRAY);
                }
            }
        }
        EndDrawing();
    }

    CloseWindow();
    return 0;
}

// =============================================================================
// ANDROID_BUILD_NOTES (arm64-v8a, 64-bit, NDK r25+ / Raylib 4.5+5.x)
// -----------------------------------------------------------------------------
// Option A — NativeActivity + ndk-build (no Gradle, fastest for single file):
//   1. Install Android NDK r25+ and export NDK=$ANDROID_NDK_ROOT
//      (needs LLVM clang arm64 toolchain, API 21+ — 64-bit requires minSdk>=21).
//   2. Build raylib for Android arm64 once:
//        cd raylib/src
//        make PLATFORM=PLATFORM_ANDROID ANDROID_NDK=$NDK ANDROID_ARCH=arm64 \
//             ANDROID_API_VERSION=21 -j$(nproc)
//      -> produces libraylib.a (arm64-v8a).
//   3. Project layout:
//        proj/jni/main.cpp            (this file)
//        proj/jni/Android.mk          (LOCAL_STATIC_LIBRARIES += raylib, APP_ABI := arm64-v8a)
//        proj/jni/Application.mk      (APP_ABI := arm64-v8a, APP_PLATFORM := android-21,
//                                      APP_STL := c++_static, NDK_TOOLCHAIN_VERSION := clang)
//        proj/AndroidManifest.xml     (NativeActivity, portrait, arm64 only)
//   4. ndk-build NDK_PROJECT_PATH=proj APP_BUILD_SCRIPT=proj/jni/Android.mk
//      -> libs/arm64-v8a/libflyme2themoon.so  (64-bit only; drop armeabi-v7a/
//         x86 to satisfy Play 64-bit requirement and halve APK size).
//   5. Package + sign (apksigner) with NativeActivity glue; raylib handles
//      lifecycle, touch (GetTouchPoint*), back button, pause on focus loss.
//
// Option B — Gradle (recommended for Play upload / App Bundle):
//   - Use raylib android example template (android/gradle), set in build.gradle:
//       ndk { abiFilters 'arm64-v8a' }  minSdk 21  targetSdk 34
//       externalNativeBuild { cmake { cppFlags "-std=c++17" } }
//   - Place this main.cpp under app/src/main/cpp/, CMakeLists links raylib.
//   - ./gradlew bundleRelease -> .aab contains only arm64-v8a 64-bit .so.
//
// Runtime notes:
//   - Orientation portrait (original iPhone portrait); sensorPortrait covers
//     16:9 and 20:9 — width-locked camera in code adapts visible height.
//   - Audio: add .ogg under assets + InitAudioDevice(); placeholders are silent.
//   - Textures: put .png under assets, load via LoadTexture() in swap points.
//   - Permissions: no special permissions needed (no internet unless ads).
// =============================================================================
