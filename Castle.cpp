// =============================================================================
//  Libraries : GLFW, GLAD (v1, gl 3.3 core), GLM
//  Build (Linux/macOS) : g++ Castle.cpp glad.c -o castle -lglfw -ldl -lGL
//  Build (Visual Studio): add glad.c to the project, link glfw3.lib + opengl32.lib
//
//  No image files are needed: every texture (stone, roof tiles, door, window,
//  flag, grass, water, planks, leaves) is generated procedurally in code, so
//  this single .cpp file runs on its own.
//
//  Efficiency notes (score item 1):
//   * ONE vertex buffer + ONE index buffer (EBO) for the whole scene.
//   * Cylinders / cones share ring vertices through indices.
//   * Hidden faces (box bottoms, tower bottoms) are never generated.
//   * One draw call per texture (9 draw calls total).
//
//  Controls : SPACE pause/resume   LEFT/RIGHT rotation speed
//             UP/DOWN zoom         ESC quit
// =============================================================================

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>
#include <functional>

using glm::vec2;
using glm::vec3;
using glm::mat3;
using glm::mat4;

// ----------------------------------------------------------------------------
//  Window settings
// ----------------------------------------------------------------------------
static int   gWidth  = 1024;
static int   gHeight = 768;
static bool  gPaused = false;
static float gSpeed  = 0.6f;    // radians per second
static float gCamDist = 19.0f;

// ----------------------------------------------------------------------------
//  Shaders
// ----------------------------------------------------------------------------
static const char* kVertSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

out vec3 vPos;
out vec3 vNormal;
out vec2 vUV;

void main()
{
    vec4 world = uModel * vec4(aPos, 1.0);
    vPos    = world.xyz;
    vNormal = mat3(uModel) * aNormal;      // model = rotation only
    vUV     = aUV;
    gl_Position = uProj * uView * world;
}
)";

static const char* kFragSrc = R"(
#version 330 core
in vec3 vPos;
in vec3 vNormal;
in vec2 vUV;

uniform sampler2D uTex;
uniform vec3  uLightDir;     // direction the light travels
uniform vec3  uViewPos;
uniform float uSpec;

out vec4 FragColor;

void main()
{
    vec4 texel = texture(uTex, vUV);
    if (texel.a < 0.5) discard;              // arched door/window, flag notch

    vec3 N = normalize(vNormal);
    vec3 L = normalize(-uLightDir);
    vec3 V = normalize(uViewPos - vPos);
    vec3 H = normalize(L + V);

    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 48.0) * uSpec;
    float sky  = 0.5 + 0.5 * N.y;            // simple hemisphere ambient

    vec3 ambient = mix(vec3(0.28, 0.26, 0.24), vec3(0.42, 0.48, 0.60), sky);
    vec3 color   = texel.rgb * (ambient + diff * vec3(1.0, 0.94, 0.82)) + vec3(spec);

    // gentle distance fog toward the sky colour
    float d   = length(uViewPos - vPos);
    float fog = clamp((d - 22.0) / 40.0, 0.0, 0.6);
    color = mix(color, vec3(0.62, 0.78, 0.93), fog);

    FragColor = vec4(color, 1.0);
}
)";

// ----------------------------------------------------------------------------
//  Texture ids
// ----------------------------------------------------------------------------
enum TexId { T_STONE, T_ROOF, T_DOOR, T_GRASS, T_FLAG, T_WINDOW, T_WATER, T_PLANK, T_LEAF, T_COUNT };

// ----------------------------------------------------------------------------
//  Procedural texture helpers
// ----------------------------------------------------------------------------
static const int TS = 128;   // texture size (pixels)

struct RGBA { unsigned char r, g, b, a; };

static unsigned char clamp8(float v) { return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
static RGBA mk(float r, float g, float b, float a = 255.0f) { return { clamp8(r), clamp8(g), clamp8(b), clamp8(a) }; }

static float hash2(int x, int y)
{
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return (h & 0xFFFFu) / 65535.0f;
}

// value noise that tiles with the given period (so textures repeat seamlessly)
static float noise2(float x, float y, int period)
{
    int   xi = (int)std::floor(x), yi = (int)std::floor(y);
    float fx = x - xi, fy = y - yi;
    auto W = [&](int v) { return ((v % period) + period) % period; };
    float a = hash2(W(xi),     W(yi));
    float b = hash2(W(xi + 1), W(yi));
    float c = hash2(W(xi),     W(yi + 1));
    float d = hash2(W(xi + 1), W(yi + 1));
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    return glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fy);
}

// Generator receives (x, y) with y = 0 at the BOTTOM of the image (v = 0).
static GLuint makeTexture(const std::function<RGBA(int, int)>& gen)
{
    std::vector<unsigned char> data(TS * TS * 4);
    for (int y = 0; y < TS; ++y)
        for (int x = 0; x < TS; ++x)
        {
            RGBA c = gen(x, y);
            unsigned char* p = &data[(y * TS + x) * 4];
            p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
        }

    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TS, TS, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return id;
}

// inside test for an arch: rectangle below cy, half-ellipse above
static bool insideArch(int x, int y, float cx, float cy, float ax, float ay)
{
    if (x < cx - ax || x > cx + ax) return false;
    if (y <= cy) return true;
    float ex = (x - cx) / ax, ey = (y - cy) / ay;
    return ex * ex + ey * ey <= 1.0f;
}

static RGBA genStone(int x, int y)
{
    const int RH = 16, BW = 32;
    int row = y / RH, by = y % RH;
    int xo  = (x + ((row & 1) ? BW / 2 : 0)) % TS;
    int bx  = xo % BW, col = xo / BW;
    float n = noise2(x / 4.0f, y / 4.0f, TS / 4) * 0.6f + hash2(x, y) * 0.4f;
    float base = 138.0f + hash2(col, row) * 48.0f + (n - 0.5f) * 38.0f;
    if (by < 2 || bx < 2) base = 88.0f + (n - 0.5f) * 18.0f;      // mortar
    else if (by == 2)     base += 14.0f;                          // highlight edge
    return mk(base, base * 0.97f, base * 0.90f);
}

static RGBA genRoof(int x, int y)
{
    int row = y / 16, by = y % 16;
    int xo  = (x + ((row & 1) ? 8 : 0)) % TS;
    int bx  = xo % 16;
    float id = hash2(xo / 16, row);
    float shade = 0.62f + 0.38f * (by / 15.0f);
    if (bx < 1 || by < 1) shade *= 0.55f;
    float n = hash2(x, y) * 14.0f;
    return mk((150 + id * 55) * shade + n, (46 + id * 22) * shade + n * 0.4f, (38 + id * 14) * shade);
}

static RGBA genDoor(int x, int y)
{
    if (!insideArch(x, y, 64, 80, 64, 48)) return mk(0, 0, 0, 0);                 // outside arch
    bool inner = insideArch(x, y, 64, 80, 54, 40) && x >= 10 && x < 118;          // door leaf
    if (!inner) { float b = 110 + hash2(x / 3, y / 3) * 50; return mk(b, b * 0.97f, b * 0.9f); }  // stone frame
    float grain = noise2(x / 1.5f, y / 20.0f, 64) * 0.5f + 0.5f * hash2(x, y / 8);
    float base  = 85 + grain * 55;
    if (x % 32 == 0 || x % 32 == 31) base *= 0.45f;                               // plank seams
    RGBA c = mk(base * 1.1f, base * 0.72f, base * 0.40f);
    bool band = (y >= 24 && y <= 31) || (y >= 70 && y <= 77);                    // iron bands
    if (band) c = mk(48, 48, 54);
    if (band && x % 32 == 16) c = mk(150, 150, 160);                              // rivets
    float dx1 = x - 52.0f, dx2 = x - 76.0f, dy = y - 46.0f;
    if (dx1 * dx1 + dy * dy < 16 || dx2 * dx2 + dy * dy < 16) c = mk(214, 172, 60); // brass knobs
    return c;
}

static RGBA genWindow(int x, int y)
{
    if (!insideArch(x, y, 64, 72, 64, 56)) return mk(0, 0, 0, 0);
    bool glass = insideArch(x, y, 64, 72, 52, 44) && y >= 12;
    if (!glass) { float b = 150 + hash2(x / 2, y / 2) * 35; return mk(b, b * 0.96f, b * 0.88f); }
    if ((x >= 60 && x <= 68) || (y >= 50 && y <= 58)) return mk(70, 48, 28);     // mullions
    float g = 40 + y * 0.45f;
    if ((x + y) % 44 < 7) g += 38;                                                // reflection streaks
    return mk(g * 0.55f, g * 0.8f, g * 1.4f);
}

static RGBA genFlag(int x, int y)
{
    if (x > 100 && std::abs(y - 64) < (x - 100) * 2) return mk(0, 0, 0, 0);       // swallow-tail notch
    float cx = x - 48.0f, cy = y - 64.0f;
    float d = std::sqrt(cx * cx + cy * cy);
    if (d < 20 && d > 12) return mk(235, 190, 50);                                // gold ring
    if (d <= 12) return mk(240, 240, 240);
    if (y < 8 || y > 120) return mk(235, 190, 50);                                // gold border
    return mk(190 + hash2(x / 2, y / 2) * 12, 28, 38);
}

static RGBA genGrass(int x, int y)
{
    float n = noise2(x / 8.0f, y / 8.0f, 16);
    float b = hash2(x, y);
    return mk(44 + n * 40 + b * 22, 112 + n * 55 + b * 30, 36 + n * 22);
}

static RGBA genWater(int x, int y)
{
    const float TAU = 6.2831853f;
    float w = std::sin(TAU * 3.0f * x / TS + 2.0f * std::sin(TAU * 2.0f * y / TS))
            + std::sin(TAU * 4.0f * y / TS + 1.3f * std::sin(TAU * 3.0f * x / TS));
    float k = 0.5f + 0.25f * w;
    return mk(22 + k * 35, 85 + k * 60, 150 + k * 65);
}

static RGBA genPlank(int x, int y)
{
    int row = y / 16;
    float grain = noise2(x / 14.0f, y / 2.0f, 16) ;
    float base  = 100 + hash2(row, 3) * 40 + grain * 30;
    if (y % 16 == 0) base *= 0.4f;                                                // gaps
    RGBA c = mk(base * 1.12f, base * 0.75f, base * 0.42f);
    if (y % 16 == 8 && (x % 64 == 8 || x % 64 == 56)) c = mk(60, 60, 64);         // nails
    return c;
}

static RGBA genLeaf(int x, int y)
{
    float n = noise2(x / 3.0f, y / 3.0f, 42);
    return mk(18 + n * 30, 78 + n * 52 + hash2(x, y) * 14, 30 + n * 22);
}

// ----------------------------------------------------------------------------
//  Geometry storage : ONE vertex array, one index list per texture
// ----------------------------------------------------------------------------
struct Vtx { float p[3]; float n[3]; float uv[2]; };

static std::vector<Vtx>      gVerts;
static std::vector<unsigned> gIdx[T_COUNT];

static const float PI = 3.14159265358979f;

static unsigned vtx(vec3 p, vec3 n, vec2 uv)
{
    gVerts.push_back({ { p.x, p.y, p.z }, { n.x, n.y, n.z }, { uv.x, uv.y } });
    return (unsigned)gVerts.size() - 1;
}
static void tri(int t, unsigned a, unsigned b, unsigned c)
{
    gIdx[t].push_back(a); gIdx[t].push_back(b); gIdx[t].push_back(c);
}
// p0..p3 counter-clockwise seen from the front; w,h = UV repeat counts
static void quad(int t, vec3 p0, vec3 p1, vec3 p2, vec3 p3, vec3 n, float w, float h)
{
    unsigned a = vtx(p0, n, { 0, 0 }), b = vtx(p1, n, { w, 0 });
    unsigned c = vtx(p2, n, { w, h }), d = vtx(p3, n, { 0, h });
    tri(t, a, b, c); tri(t, a, c, d);
}

// Box centred at c with size s. 'tile' = world units per texture repeat.
static void box(int t, vec3 c, vec3 s, float tile, float yaw = 0.0f, bool bottom = false)
{
    mat4 M = glm::rotate(glm::translate(mat4(1.0f), c), yaw, vec3(0, 1, 0));
    mat3 R(M);
    vec3 h = s * 0.5f;
    auto P = [&](float x, float y, float z) { return vec3(M * glm::vec4(x * h.x, y * h.y, z * h.z, 1.0f)); };
    auto N = [&](float x, float y, float z) { return glm::normalize(R * vec3(x, y, z)); };

    quad(t, P(-1,-1, 1), P( 1,-1, 1), P( 1, 1, 1), P(-1, 1, 1), N(0, 0, 1), s.x / tile, s.y / tile);
    quad(t, P( 1,-1,-1), P(-1,-1,-1), P(-1, 1,-1), P( 1, 1,-1), N(0, 0,-1), s.x / tile, s.y / tile);
    quad(t, P( 1,-1, 1), P( 1,-1,-1), P( 1, 1,-1), P( 1, 1, 1), N(1, 0, 0), s.z / tile, s.y / tile);
    quad(t, P(-1,-1,-1), P(-1,-1, 1), P(-1, 1, 1), P(-1, 1,-1), N(-1,0, 0), s.z / tile, s.y / tile);
    quad(t, P(-1, 1, 1), P( 1, 1, 1), P( 1, 1,-1), P(-1, 1,-1), N(0, 1, 0), s.x / tile, s.z / tile);
    if (bottom)
        quad(t, P(-1,-1,-1), P( 1,-1,-1), P( 1,-1, 1), P(-1,-1, 1), N(0,-1, 0), s.x / tile, s.z / tile);
}

// flat circle facing up (world-planar UVs)
static void disc(int t, vec3 c, float r, int segs, float scale)
{
    unsigned ctr = vtx(c, { 0, 1, 0 }, { c.x / scale, c.z / scale });
    unsigned first = (unsigned)gVerts.size();
    for (int i = 0; i <= segs; ++i)
    {
        float a = 2 * PI * i / segs, cs = std::cos(a), sn = std::sin(a);
        vtx(c + vec3(r * cs, 0, r * sn), { 0, 1, 0 }, { (c.x + r * cs) / scale, (c.z + r * sn) / scale });
    }
    for (int i = 0; i < segs; ++i) tri(t, ctr, first + i + 1, first + i);
}

// open-bottom cylinder with shared ring vertices; optional top cap
static void cylinder(int t, vec3 base, float r, float h, int segs, float uTile, float vTile, bool capTop)
{
    unsigned first = (unsigned)gVerts.size();
    for (int i = 0; i <= segs; ++i)
    {
        float a = 2 * PI * i / segs, cs = std::cos(a), sn = std::sin(a);
        float u = uTile * i / segs;
        vtx(base + vec3(r * cs, 0, r * sn), { cs, 0, sn }, { u, 0 });
        vtx(base + vec3(r * cs, h, r * sn), { cs, 0, sn }, { u, vTile });
    }
    for (int i = 0; i < segs; ++i)
    {
        unsigned b0 = first + 2 * i, t0 = b0 + 1, b1 = b0 + 2, t1 = b0 + 3;
        tri(t, b0, t0, b1);
        tri(t, b1, t0, t1);
    }
    if (capTop) disc(t, base + vec3(0, h, 0), r, segs, 1.5f);
}

// cone / pyramid. flat = faceted normals (use for the 4-sided pyramid roof)
static void cone(int t, vec3 base, float r, float h, int segs, float startAng,
                 float uTile, float vTile, bool flat)
{
    auto normalAt = [&](float a) { return glm::normalize(vec3(std::cos(a) * h, r, std::sin(a) * h)); };
    vec3 apex = base + vec3(0, h, 0);

    if (flat)
    {
        for (int i = 0; i < segs; ++i)
        {
            float a0 = startAng + 2 * PI * i / segs, a1 = startAng + 2 * PI * (i + 1) / segs;
            vec3 n = normalAt(0.5f * (a0 + a1));
            unsigned v0 = vtx(base + vec3(r * std::cos(a0), 0, r * std::sin(a0)), n, { 0, 0 });
            unsigned v1 = vtx(base + vec3(r * std::cos(a1), 0, r * std::sin(a1)), n, { uTile, 0 });
            unsigned va = vtx(apex, n, { uTile * 0.5f, vTile });
            tri(t, v0, va, v1);
        }
    }
    else
    {
        unsigned ring = (unsigned)gVerts.size();
        for (int i = 0; i <= segs; ++i)
        {
            float a = startAng + 2 * PI * i / segs;
            vtx(base + vec3(r * std::cos(a), 0, r * std::sin(a)), normalAt(a), { uTile * i / segs, 0 });
        }
        unsigned tip = (unsigned)gVerts.size();
        for (int i = 0; i < segs; ++i)
        {
            float am = startAng + 2 * PI * (i + 0.5f) / segs;
            vtx(apex, normalAt(am), { uTile * (i + 0.5f) / segs, vTile });
        }
        for (int i = 0; i < segs; ++i) tri(t, ring + i, tip + i, ring + i + 1);
    }
}

// upright rectangle: bottom-centre at c, facing +Z rotated by yaw
static void panel(int t, vec3 c, float yaw, float w, float h, bool doubleSided = false)
{
    mat4 M = glm::rotate(glm::translate(mat4(1.0f), c), yaw, vec3(0, 1, 0));
    mat3 R(M);
    auto P = [&](float x, float y) { return vec3(M * glm::vec4(x, y, 0, 1)); };
    vec3 n = R * vec3(0, 0, 1);
    quad(t, P(-w / 2, 0), P(w / 2, 0), P(w / 2, h), P(-w / 2, h), n, 1, 1);
    if (doubleSided)
        quad(t, P(w / 2, 0), P(-w / 2, 0), P(-w / 2, h), P(w / 2, h), -n, 1, 1);
}

// row of battlement blocks along a line
static void merlons(int t, vec3 a, vec3 b, int n, vec3 size)
{
    for (int i = 0; i < n; ++i)
    {
        float f = (n == 1) ? 0.5f : (float)i / (n - 1);
        box(t, glm::mix(a, b, f) + vec3(0, size.y * 0.5f, 0), size, 1.5f);
    }
}

// pole + double-sided banner
static void flagPole(vec3 base, float height)
{
    cylinder(T_PLANK, base, 0.035f, height, 6, 1, 4, true);
    panel(T_FLAG, base + vec3(0.0f, height - 0.45f, 0.0f) + vec3(0.30f, 0, 0), 0.0f, 0.60f, 0.40f, true);
}

// ----------------------------------------------------------------------------
//  Build the whole castle scene
// ----------------------------------------------------------------------------
static void buildScene()
{
    const float STONE = 1.5f;   // world units per stone texture repeat

    // ---- water (moat / lake), drawn un-rotated --------------------------------
    {
        float y = -0.42f, s = 60.0f;
        quad(T_WATER, { -s, y, s }, { s, y, s }, { s, y, -s }, { -s, y, -s }, { 0, 1, 0 }, s / 2.5f, s / 2.5f);
    }

    // ---- island ---------------------------------------------------------------
    cylinder(T_STONE, { 0, -0.7f, 0 }, 7.3f, 0.7f, 48, 14.0f, 0.5f, true);   // rocky rim
    disc(T_GRASS, { 0, 0.01f, 0 }, 6.9f, 48, 2.0f);                           // grass

    // ---- walls ----------------------------------------------------------------
    const float WH = 2.5f;      // wall height
    box(T_STONE, { 0, WH / 2, -4.0f }, { 8.6f, WH, 0.6f }, STONE);                // back
    box(T_STONE, { 0, WH / 2,  4.0f }, { 8.6f, WH, 0.6f }, STONE);                // front
    box(T_STONE, { -4.0f, WH / 2, 0 }, { 0.6f, WH, 8.6f }, STONE);                // left
    box(T_STONE, {  4.0f, WH / 2, 0 }, { 0.6f, WH, 8.6f }, STONE);                // right

    vec3 mlX(0.5f, 0.5f, 0.22f), mlZ(0.22f, 0.5f, 0.5f);
    merlons(T_STONE, { -3.5f, WH, -4.2f }, { 3.5f, WH, -4.2f }, 9, mlX);          // back
    merlons(T_STONE, { -3.5f, WH,  4.2f }, { -1.9f, WH,  4.2f }, 3, mlX);         // front-left
    merlons(T_STONE, {  1.9f, WH,  4.2f }, {  3.5f, WH,  4.2f }, 3, mlX);         // front-right
    merlons(T_STONE, { -4.2f, WH, -3.5f }, { -4.2f, WH, 3.5f }, 9, mlZ);          // left
    merlons(T_STONE, {  4.2f, WH, -3.5f }, {  4.2f, WH, 3.5f }, 9, mlZ);          // right

    // ---- four corner towers with conical roofs + flags ------------------------
    const float TH = 3.6f;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2)
        {
            vec3 c(4.0f * sx, 0.0f, 4.0f * sz);
            cylinder(T_STONE, c, 0.9f, TH, 20, 4.0f, TH / STONE, false);
            cylinder(T_STONE, c + vec3(0, TH, 0), 1.05f, 0.25f, 20, 4.5f, 0.25f / STONE, true);  // corbel
            cone(T_ROOF, c + vec3(0, TH + 0.25f, 0), 1.3f, 2.0f, 20, 0.0f, 5.0f, 2.0f, false);
            flagPole(c + vec3(0, TH + 2.25f, 0), 0.9f);

            vec3 d = glm::normalize(vec3((float)sx, 0, (float)sz));              // outward diagonal
            float yaw = std::atan2(d.x, d.z);
            panel(T_WINDOW, c + d * 0.93f + vec3(0, 0.9f, 0), yaw, 0.42f, 0.8f);
            panel(T_WINDOW, c + d * 0.93f + vec3(0, 2.3f, 0), yaw, 0.42f, 0.8f);
        }

    // ---- central keep -----------------------------------------------------------
    box(T_STONE, { 0, 1.75f, 0 }, { 3.4f, 3.5f, 3.4f }, STONE);                  // lower block
    box(T_STONE, { 0, 3.575f, 0 }, { 3.8f, 0.15f, 3.8f }, STONE);                // ledge
    box(T_STONE, { 0, 4.5f, 0 }, { 2.4f, 2.0f, 2.4f }, STONE);                   // upper block
    merlons(T_STONE, { -1.6f, 3.65f,  1.75f }, { 1.6f, 3.65f,  1.75f }, 5, vec3(0.42f, 0.42f, 0.2f));
    merlons(T_STONE, { -1.6f, 3.65f, -1.75f }, { 1.6f, 3.65f, -1.75f }, 5, vec3(0.42f, 0.42f, 0.2f));
    merlons(T_STONE, { -1.75f, 3.65f, -1.6f }, { -1.75f, 3.65f, 1.6f }, 5, vec3(0.2f, 0.42f, 0.42f));
    merlons(T_STONE, {  1.75f, 3.65f, -1.6f }, {  1.75f, 3.65f, 1.6f }, 5, vec3(0.2f, 0.42f, 0.42f));
    cone(T_ROOF, { 0, 5.5f, 0 }, 2.0f, 2.4f, 4, PI / 4, 2.0f, 2.0f, true);       // pyramid roof
    flagPole({ 0, 7.9f, 0 }, 1.4f);

    for (int k = 0; k < 4; ++k)                                                    // keep windows
    {
        float ang = k * PI / 2;
        vec3 d(std::sin(ang), 0, std::cos(ang)), p(std::cos(ang), 0, -std::sin(ang));
        panel(T_WINDOW, d * 1.22f + vec3(0, 4.0f, 0), ang, 0.5f, 0.95f);
        panel(T_WINDOW, d * 1.72f + p *  0.8f + vec3(0, 1.7f, 0), ang, 0.5f, 0.95f);
        panel(T_WINDOW, d * 1.72f + p * -0.8f + vec3(0, 1.7f, 0), ang, 0.5f, 0.95f);
    }

    // ---- gatehouse, door, path, drawbridge --------------------------------------
    box(T_STONE, { 0, 1.9f, 4.4f }, { 2.4f, 3.8f, 1.4f }, STONE);
    merlons(T_STONE, { -1.0f, 3.8f, 5.0f }, { 1.0f, 3.8f, 5.0f }, 3, mlX);
    merlons(T_STONE, { -1.1f, 3.8f, 4.3f }, { -1.1f, 3.8f, 3.9f }, 2, mlZ);
    merlons(T_STONE, {  1.1f, 3.8f, 4.3f }, {  1.1f, 3.8f, 3.9f }, 2, mlZ);
    panel(T_DOOR, { 0, 0, 5.12f }, 0.0f, 1.4f, 2.2f);
    panel(T_WINDOW, { 0, 2.45f, 5.12f }, 0.0f, 0.5f, 0.95f);

    box(T_STONE, { 0, 0.02f, 6.0f }, { 1.6f, 0.04f, 1.8f }, STONE);               // stone path
    box(T_PLANK, { 0, -0.05f, 8.9f }, { 1.6f, 0.2f, 4.0f }, 1.5f);                // bridge deck
    box(T_PLANK, { -0.76f, 0.3f, 8.9f }, { 0.08f, 0.5f, 4.0f }, 1.5f);            // rails
    box(T_PLANK, {  0.76f, 0.3f, 8.9f }, { 0.08f, 0.5f, 4.0f }, 1.5f);

    // ---- pine trees ---------------------------------------------------------------
    const vec3 trees[] = { { 5.6f, 0, 0.0f }, { -5.6f, 0, 0.0f }, { 0.0f, 0, -5.6f }, { 4.4f, 0, -5.0f }, { -4.4f, 0, -5.0f } };
    for (const vec3& tp : trees)
    {
        cylinder(T_PLANK, tp, 0.16f, 0.9f, 8, 1.0f, 1.0f, false);
        cone(T_LEAF, tp + vec3(0, 0.6f, 0), 0.95f, 1.6f, 10, 0.0f, 3.0f, 2.0f, false);
        cone(T_LEAF, tp + vec3(0, 1.5f, 0), 0.72f, 1.4f, 10, 0.0f, 3.0f, 2.0f, false);
        cone(T_LEAF, tp + vec3(0, 2.3f, 0), 0.50f, 1.2f, 10, 0.0f, 3.0f, 2.0f, false);
    }
}

// ----------------------------------------------------------------------------
//  GL helpers
// ----------------------------------------------------------------------------
static GLuint compileShader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log); std::fprintf(stderr, "Shader error:\n%s\n", log); }
    return s;
}

static GLuint makeProgram()
{
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragSrc);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p, 1024, nullptr, log); std::fprintf(stderr, "Link error:\n%s\n", log); }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

static void onResize(GLFWwindow*, int w, int h)
{
    gWidth = w > 1 ? w : 1; gHeight = h > 1 ? h : 1;
    glViewport(0, 0, gWidth, gHeight);
}

static void onKey(GLFWwindow* win, int key, int, int action, int)
{
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(win, GLFW_TRUE);
    if (key == GLFW_KEY_SPACE)  gPaused = !gPaused;
}

// ----------------------------------------------------------------------------
//  main
// ----------------------------------------------------------------------------
int main()
{
    if (!glfwInit()) { std::fprintf(stderr, "Failed to init GLFW\n"); return -1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* win = glfwCreateWindow(gWidth, gHeight, "CSS171 PE1 - Rotating 3D Castle", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "Failed to create window\n"); glfwTerminate(); return -1; }

    // centre the window on the monitor (screen layout: centered)
    if (const GLFWvidmode* vm = glfwGetVideoMode(glfwGetPrimaryMonitor()))
        glfwSetWindowPos(win, (vm->width - gWidth) / 2, (vm->height - gHeight) / 2);

    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetFramebufferSizeCallback(win, onResize);
    glfwSetKeyCallback(win, onKey);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) { std::fprintf(stderr, "Failed to load GLAD\n"); return -1; }
    glfwGetFramebufferSize(win, &gWidth, &gHeight);
    glViewport(0, 0, gWidth, gHeight);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_MULTISAMPLE);

    // textures
    GLuint tex[T_COUNT];
    tex[T_STONE]  = makeTexture(genStone);
    tex[T_ROOF]   = makeTexture(genRoof);
    tex[T_DOOR]   = makeTexture(genDoor);
    tex[T_GRASS]  = makeTexture(genGrass);
    tex[T_FLAG]   = makeTexture(genFlag);
    tex[T_WINDOW] = makeTexture(genWindow);
    tex[T_WATER]  = makeTexture(genWater);
    tex[T_PLANK]  = makeTexture(genPlank);
    tex[T_LEAF]   = makeTexture(genLeaf);

    // geometry -> single VBO + single EBO
    buildScene();
    std::vector<unsigned> indices;
    unsigned first[T_COUNT], count[T_COUNT];
    for (int t = 0; t < T_COUNT; ++t)
    {
        first[t] = (unsigned)indices.size();
        count[t] = (unsigned)gIdx[t].size();
        indices.insert(indices.end(), gIdx[t].begin(), gIdx[t].end());
    }
    std::printf("Vertices: %zu   Triangles: %zu\n", gVerts.size(), indices.size() / 3);

    GLuint vao, vbo, ebo;
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo); glGenBuffers(1, &ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, gVerts.size() * sizeof(Vtx), gVerts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned), indices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, p));  glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, n));  glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, uv)); glEnableVertexAttribArray(2);

    GLuint prog = makeProgram();
    glUseProgram(prog);
    GLint uModel = glGetUniformLocation(prog, "uModel"), uView = glGetUniformLocation(prog, "uView");
    GLint uProj  = glGetUniformLocation(prog, "uProj"),  uLight = glGetUniformLocation(prog, "uLightDir");
    GLint uEye   = glGetUniformLocation(prog, "uViewPos"), uSpec = glGetUniformLocation(prog, "uSpec");
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glUniform3f(uLight, -0.5f, -0.8f, -0.4f);

    const float specular[T_COUNT] = { 0.0f, 0.12f, 0.0f, 0.0f, 0.0f, 0.7f, 0.8f, 0.0f, 0.0f };

    float angle = 0.0f, last = (float)glfwGetTime();
    while (!glfwWindowShouldClose(win))
    {
        float now = (float)glfwGetTime(), dt = now - last; last = now;
        if (glfwGetKey(win, GLFW_KEY_RIGHT) == GLFW_PRESS) gSpeed += 1.0f * dt;
        if (glfwGetKey(win, GLFW_KEY_LEFT)  == GLFW_PRESS) gSpeed -= 1.0f * dt;
        if (glfwGetKey(win, GLFW_KEY_UP)    == GLFW_PRESS) gCamDist = glm::max(8.0f,  gCamDist - 8.0f * dt);
        if (glfwGetKey(win, GLFW_KEY_DOWN)  == GLFW_PRESS) gCamDist = glm::min(40.0f, gCamDist + 8.0f * dt);
        if (!gPaused) angle += gSpeed * dt;

        glClearColor(0.62f, 0.78f, 0.93f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        vec3 eye(0.0f, 0.42f * gCamDist, gCamDist);       // camera on the Z axis => castle stays centred
        mat4 view = glm::lookAt(eye, vec3(0.0f, 3.6f, 0.0f), vec3(0, 1, 0));
        mat4 proj = glm::perspective(glm::radians(45.0f), (float)gWidth / (float)gHeight, 0.5f, 120.0f);
        glUniformMatrix4fv(uView, 1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(uProj, 1, GL_FALSE, glm::value_ptr(proj));
        glUniform3fv(uEye, 1, glm::value_ptr(eye));

        mat4 spin = glm::rotate(mat4(1.0f), angle, vec3(0, 1, 0));
        glBindVertexArray(vao);
        glActiveTexture(GL_TEXTURE0);
        for (int t = 0; t < T_COUNT; ++t)
        {
            glUniformMatrix4fv(uModel, 1, GL_FALSE, glm::value_ptr(t == T_WATER ? mat4(1.0f) : spin));
            glUniform1f(uSpec, specular[t]);
            glBindTexture(GL_TEXTURE_2D, tex[t]);
            glDrawElements(GL_TRIANGLES, (GLsizei)count[t], GL_UNSIGNED_INT, (void*)(first[t] * sizeof(unsigned)));
        }

        glfwSwapBuffers(win);
        glfwPollEvents();
    }

    glDeleteBuffers(1, &vbo); glDeleteBuffers(1, &ebo); glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    glfwTerminate();
    return 0;
}
