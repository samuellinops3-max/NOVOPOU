// Pipa Combate 3D - versao inicial (PSL1GHT)
// Render 3D por software, leve: grade, linhas e pipas em triangulos planos.
// Controles:
//   Analogico esquerdo : direciona a pipa (esquerda/direita e cima/baixo)
//   Analogico direito  : move o ponto de ancoragem no chao
//   X (cross)          : solta linha (aumenta o tamanho)  / volta depois de cair
//   O (circle)         : puxa linha (diminui o tamanho)
//   Cortar: passe a linha da sua pipa na linha de outra mais rapido que ela
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>
#include <rsx/rsx.h>
#include <io/pad.h>

#define CB_SIZE   (0x100000)
#define HOST_SIZE (32 * 1024 * 1024)

#define NKITE     8        // 1 jogador + 7 bots
#define FIELD     150.0f   // area de jogo: -FIELD a +FIELD
#define GRID_STEP 15.0f
#define GRID_N    10
#define PI        3.14159265f
#define CUT_D2    (1.5f * 1.5f)

typedef struct { float x, y, z; } V3;

typedef struct {
    float hx, hz;              // ponto no chao onde a linha esta presa
    float yaw, pitch, len;     // angulos e tamanho da linha
    float tgt_yaw, tgt_pitch, tgt_len;
    int tmr;
    V3 p, prev;                // posicao da pipa
    float vy;
    int alive, falling, respawn, score, is_player;
    u32 col, col2;
} Kite;

static const u32 palette[8] = {
    0x00FF4D4D, 0x0055D6FF, 0x0066FF66, 0x00FFD23F,
    0x00C77DFF, 0x00FF8FAB, 0x0000E5C0, 0x00FF9F43
};

static Kite kt[NKITE];
static gcmContextData *ctx;
static u32 W, H;
static u32 *fb[2];
static u32 fboff[2];
static u32 *back;
static int cur = 0, first_flip = 1;
static volatile int running = 1;
static int frame = 0;
static int dead_t = 0;
static float focal;
static V3 cam_pos, cam_r, cam_u, cam_f;

static float pad_lx, pad_ly, pad_rx, pad_ry;
static int pad_reel_out, pad_reel_in, pad_go, x_prev;
static padData last_pad;
static int have_pad = 0;

// ---------- utilitarios de vetor ----------
static V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static V3 v_add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 v_sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 v_scale(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float v_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 v_cross(V3 a, V3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static float v_len(V3 a) { return sqrtf(v_dot(a, a)); }
static V3 v_norm(V3 a)
{
    float l = v_len(a);
    if (l < 1e-6f) return v3(0, 0, 0);
    return v_scale(a, 1.0f / l);
}

static float clampf(float v, float a, float b)
{
    return v < a ? a : (v > b ? b : v);
}

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static u32 lerp_col(u32 a, u32 b, float t)
{
    t = clampf(t, 0.0f, 1.0f);
    u32 r = (u32)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
    u32 g = (u32)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
    u32 bl = (u32)((a & 255) * (1 - t) + (b & 255) * t);
    return (r << 16) | (g << 8) | bl;
}

static u32 dark_col(u32 c) { return (c >> 1) & 0x007F7F7F; }

// ---------- video ----------
static void sysutil_cb(u64 status, u64 param, void *usrdata)
{
    (void)param; (void)usrdata;
    if (status == SYSUTIL_EXIT_GAME) running = 0;
}

static void init_screen(void)
{
    void *host = memalign(1024 * 1024, HOST_SIZE);
    rsxInit(&ctx, CB_SIZE, HOST_SIZE, host);

    videoState st;
    videoGetState(VIDEO_PRIMARY, 0, &st);
    u32 resid = st.displayMode.resolution;
    if (videoGetResolutionAvailability(VIDEO_PRIMARY, VIDEO_RESOLUTION_720, VIDEO_ASPECT_AUTO, 0))
        resid = VIDEO_RESOLUTION_720;

    videoResolution res;
    videoGetResolution(resid, &res);
    W = res.width;
    H = res.height;

    videoConfiguration vc;
    memset(&vc, 0, sizeof(vc));
    vc.resolution = resid;
    vc.format = VIDEO_BUFFER_FORMAT_XRGB;
    vc.aspect = VIDEO_ASPECT_AUTO;
    vc.pitch = W * 4;
    videoConfigure(VIDEO_PRIMARY, &vc, NULL, 0);
    videoGetState(VIDEO_PRIMARY, 0, &st);

    gcmSetFlipMode(GCM_FLIP_VSYNC);

    for (int i = 0; i < 2; i++) {
        fb[i] = (u32 *)rsxMemalign(64, W * H * 4);
        gcmAddressToOffset(fb[i], &fboff[i]);
        gcmSetDisplayBuffer(i, fboff[i], W * 4, W, H);
    }
    back = (u32 *)memalign(64, W * H * 4);
    memset(back, 0, W * H * 4);
    gcmResetFlipStatus();
}

static void wait_flip(void)
{
    while (gcmGetFlipStatus() != 0) usleep(200);
    gcmResetFlipStatus();
}

static void present(void)
{
    if (!first_flip) wait_flip();
    else gcmResetFlipStatus();
    memcpy(fb[cur], back, W * H * 4);
    gcmSetFlip(ctx, cur);
    rsxFlushBuffer(ctx);
    gcmSetWaitFlip(ctx);
    cur ^= 1;
    first_flip = 0;
}

// ---------- desenho 2D basico ----------
static void fill_rect(int x, int y, int w, int h, u32 c)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > (int)W) x1 = W;
    if (y1 > (int)H) y1 = H;
    for (int yy = y0; yy < y1; yy++) {
        u32 *p = back + yy * W;
        for (int xx = x0; xx < x1; xx++) p[xx] = c;
    }
}

static void circle(int cx, int cy, int r, u32 c)
{
    if (r < 1) r = 1;
    if (cx + r < 0 || cx - r >= (int)W || cy + r < 0 || cy - r >= (int)H) return;
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= (int)H) continue;
        int hw = (int)sqrtf((float)(r * r - dy * dy));
        int x0 = cx - hw, x1 = cx + hw;
        if (x0 < 0) x0 = 0;
        if (x1 >= (int)W) x1 = W - 1;
        u32 *p = back + y * W;
        for (int x = x0; x <= x1; x++) p[x] = c;
    }
}

// Linha com recorte na tela (Liang-Barsky) + Bresenham
static void line_draw(int x0, int y0, int x1, int y1, u32 c)
{
    float fx0 = x0, fy0 = y0, fx1 = x1, fy1 = y1;
    float dx = fx1 - fx0, dy = fy1 - fy0;
    float t0 = 0.0f, t1 = 1.0f;
    float p[4] = {-dx, dx, -dy, dy};
    float q[4] = {fx0, (W - 1) - fx0, fy0, (H - 1) - fy0};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) return;
        } else {
            float r = q[i] / p[i];
            if (p[i] < 0.0f) {
                if (r > t1) return;
                if (r > t0) t0 = r;
            } else {
                if (r < t0) return;
                if (r < t1) t1 = r;
            }
        }
    }
    x0 = (int)(fx0 + t0 * dx); y0 = (int)(fy0 + t0 * dy);
    x1 = (int)(fx0 + t1 * dx); y1 = (int)(fy0 + t1 * dy);

    int adx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int ady = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = adx + ady;
    for (;;) {
        if (x0 >= 0 && x0 < (int)W && y0 >= 0 && y0 < (int)H) back[y0 * W + x0] = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= ady) { err