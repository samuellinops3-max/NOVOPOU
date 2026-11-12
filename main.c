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
        if (e2 >= ady) { err += ady; x0 += sx; }
        if (e2 <= adx) { err += adx; y0 += sy; }
    }
}

// Triangulo preenchido (cor chapada) por scanline
static void fill_tri(float ax, float ay, float bx, float by, float cx, float cy, u32 c)
{
    float t;
    if (ay > by) { t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; }
    if (ay > cy) { t = ax; ax = cx; cx = t; t = ay; ay = cy; cy = t; }
    if (by > cy) { t = bx; bx = cx; cx = t; t = by; by = cy; cy = t; }

    float total = cy - ay;
    if (total <= 0.0f) return;

    int y0 = (int)ceilf(ay), y2 = (int)floorf(cy);
    if (y0 < 0) y0 = 0;
    if (y2 > (int)H - 1) y2 = H - 1;

    for (int y = y0; y <= y2; y++) {
        float fy = (float)y;
        float xa = ax + (cx - ax) * (fy - ay) / total;
        float xb;
        if (fy < by) {
            float d = by - ay;
            xb = d > 0.0f ? ax + (bx - ax) * (fy - ay) / d : bx;
        } else {
            float d = cy - by;
            xb = d > 0.0f ? bx + (cx - bx) * (fy - by) / d : cx;
        }
        xa = clampf(xa, -1.0f, (float)W);
        xb = clampf(xb, -1.0f, (float)W);
        int xl = (int)xa, xr = (int)xb;
        if (xl > xr) { int tt = xl; xl = xr; xr = tt; }
        if (xl < 0) xl = 0;
        if (xr > (int)W - 1) xr = W - 1;
        u32 *p = back + y * W;
        for (int x = xl; x <= xr; x++) p[x] = c;
    }
}

// ---------- fonte simples 3x5 ----------
typedef struct { char c; const char *p; } Glyph;
static const Glyph glyphs[] = {
    {'0', "###" "#.#" "#.#" "#.#" "###"}, {'1', ".#." "##." ".#." ".#." "###"},
    {'2', "###" "..#" "###" "#.." "###"}, {'3', "###" "..#" "###" "..#" "###"},
    {'4', "#.#" "#.#" "###" "..#" "..#"}, {'5', "###" "#.." "###" "..#" "###"},
    {'6', "###" "#.." "###" "#.#" "###"}, {'7', "###" "..#" "..#" "..#" "..#"},
    {'8', "###" "#.#" "###" "#.#" "###"}, {'9', "###" "#.#" "###" "..#" "###"},
    {'A', ".#." "#.#" "###" "#.#" "#.#"}, {'B', "##." "#.#" "##." "#.#" "##."},
    {'C', "###" "#.." "#.." "#.." "###"}, {'D', "##." "#.#" "#.#" "#.#" "##."},
    {'E', "###" "#.." "###" "#.." "###"}, {'G', "###" "#.." "#.#" "#.#" "###"},
    {'H', "#.#" "#.#" "###" "#.#" "#.#"}, {'I', "###" ".#." ".#." ".#." "###"},
    {'L', "#.." "#.." "#.." "#.." "###"}, {'M', "#.#" "###" "###" "#.#" "#.#"},
    {'N', "#.#" "###" "###" "###" "#.#"}, {'O', "###" "#.#" "#.#" "#.#" "###"},
    {'P', "###" "#.#" "###" "#.." "#.."}, {'R', "##." "#.#" "##." "#.#" "#.#"},
    {'S', "###" "#.." "###" "..#" "###"}, {'T', "###" ".#." ".#." ".#." ".#."},
    {'U', "#.#" "#.#" "#.#" "#.#" "###"}, {'V', "#.#" "#.#" "#.#" "#.#" ".#."},
    {'X', "#.#" "#.#" ".#." "#.#" "#.#"},
};

static const char *find_glyph(char c)
{
    for (unsigned i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
        if (glyphs[i].c == c) return glyphs[i].p;
    return NULL;
}

static int text_width(const char *s, int sc) { return (int)strlen(s) * 4 * sc - sc; }

static void draw_text(const char *s, int x, int y, int sc, u32 c)
{
    for (; *s; s++) {
        const char *g = find_glyph(*s);
        if (g)
            for (int r = 0; r < 5; r++)
                for (int k = 0; k < 3; k++)
                    if (g[r * 3 + k] == '#') fill_rect(x + k * sc, y + r * sc, sc, sc, c);
        x += 4 * sc;
    }
}

// ---------- camera e projecao ----------
static int project(V3 p, int *sx, int *sy, float *depth)
{
    V3 v = v_sub(p, cam_pos);
    float z = v_dot(v, cam_f);
    if (z < 0.5f) return 0;
    float x = v_dot(v, cam_r), y = v_dot(v, cam_u);
    *sx = (int)(W * 0.5f + x * focal / z);
    *sy = (int)(H * 0.5f - y * focal / z);
    *depth = z;
    return 1;
}

static void draw_seg3(V3 a, V3 b, u32 c)
{
    int x0, y0, x1, y1;
    float d0, d1;
    if (!project(a, &x0, &y0, &d0) || !project(b, &x1, &y1, &d1)) return;
    line_draw(x0, y0, x1, y1, c);
}

// ---------- pipas ----------
// Ponto 3D da pipa a partir do angulo e do tamanho da linha
static V3 kite_point(const Kite *k)
{
    float cp = cosf(k->pitch);
    return v3(k->hx + k->len * cp * cosf(k->yaw),
              k->len * sinf(k->pitch),
              k->hz + k->len * cp * sinf(k->yaw));
}

static void spawn_kite(int i)
{
    Kite *k = &kt[i];
    k->hx = (frand() * 2 - 1) * FIELD * 0.6f;
    k->hz = (frand() * 2 - 1) * FIELD * 0.6f;
    k->yaw = frand() * 2 * PI - PI;
    k->pitch = 0.9f;
    k->len = (i == 0) ? 30.0f : 20.0f + frand() * 20.0f;
    k->tgt_yaw = k->yaw;
    k->tgt_pitch = k->pitch;
    k->tgt_len = k->len;
    k->tmr = 60;
    k->p = kite_point(k);
    k->prev = k->p;
    k->vy = 0;
    k->alive = 1;
    k->falling = 0;
    k->respawn = 0;
    if (i != 0) {
        k->col = palette[rand() % 8];
        k->col2 = palette[rand() % 8];
    }
}

static void player_ai(Kite *k)
{
    k->yaw   += pad_lx * 0.035f;
    k->pitch -= pad_ly * 0.025f;       // analogico pra cima sobe a pipa
    if (pad_reel_out) k->len += 0.6f;
    if (pad_reel_in)  k->len -= 0.6f;
    k->hx += pad_rx * 0.9f;
    k->hz += pad_ry * 0.9f;
    k->len = clampf(k->len, 8.0f, 70.0f);
    k->pitch = clampf(k->pitch, 0.1f, 1.45f);
    k->hx = clampf(k->hx, -FIELD, FIELD);
    k->hz = clampf(k->hz, -FIELD, FIELD);
}

static void bot_ai(int i)
{
    Kite *k = &kt[i];
    if (--k->tmr <= 0) {
        k->tgt_yaw = frand() * 2 * PI - PI;
        k->tgt_pitch = 0.6f + frand() * 0.7f;
        k->tgt_len = 20.0f + frand() * 40.0f;
        k->tmr = 120 + rand() % 180;
    }
    float dy = k->tgt_yaw - k->yaw;
    while (dy > PI) dy -= 2 * PI;
    while (dy < -PI) dy += 2 * PI;
    k->yaw   += clampf(dy, -0.03f, 0.03f);
    k->pitch += clampf(k->tgt_pitch - k->pitch, -0.01f, 0.01f);
    k->len   += clampf(k->tgt_len - k->len, -0.2f, 0.2f);

    // Ancora dos bots anda devagar pelo campo
    k->hx += cosf(k->yaw + i) * 0.12f - k->hx * 0.001f;
    k->hz += sinf(k->yaw + i) * 0.12f - k->hz * 0.001f;
    k->hx = clampf(k->hx, -FIELD, FIELD);
    k->hz = clampf(k->hz, -FIELD, FIELD);
}

static void kite_step(int i)
{
    Kite *k = &kt[i];
    if (!k->alive) {
        if (!k->is_player && --k->respawn <= 0) spawn_kite(i);
        return;
    }

    if (k->falling) {
        // Pipa caindo (cortada ou sem altura)
        k->vy -= 0.02f;
        k->p.y += k->vy;
        if (k->p.y <= 0.0f) {
            k->alive = 0;
            k->falling = 0;
            k->respawn = 150;
            if (k->is_player) dead_t = 0;
        }
        return;
    }

    k->prev = k->p;
    // Vento leve: a pipa balanca um pouco sozinha
    k->yaw += sinf(frame * 0.02f + i) * 0.002f;

    if (k->is_player) player_ai(k);
    else bot_ai(i);

    k->p = kite_point(k);
    if (k->p.y < 1.5f) {       // sem altura: cai
        k->falling = 1;
        k->vy = -0.1f;
    }
}

// Distancia minima ao quadrado entre dois segmentos (amostragem simples)
static float seg_dist2(V3 a, V3 b, V3 c, V3 d)
{
    float best = 1e9f;
    for (int i = 0; i <= 6; i++) {
        float t = i / 6.0f;
        V3 p = v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
        for (int j = 0; j <= 6; j++) {
            float s = j / 6.0f;
            V3 q = v3(c.x + (d.x - c.x) * s, c.y + (d.y - c.y) * s, c.z + (d.z - c.z) * s);
            V3 e = v_sub(p, q);
            float d2 = v_dot(e, e);
            if (d2 < best) best = d2;
        }
    }
    return best;
}

// Corte: se duas linhas se encostam, a pipa mais rapida corta a outra
static void cut_check(void)
{
    for (int i = 0; i < NKITE; i++) {
        Kite *a = &kt[i];
        if (!a->alive || a->falling) continue;
        for (int j = i + 1; j < NKITE; j++) {
            Kite *b = &kt[j];
            if (!b->alive || b->falling) continue;
            V3 ga = v3(a->hx, 0, a->hz), gb = v3(b->hx, 0, b->hz);
            if (seg_dist2(a->p, ga, b->p, gb) < CUT_D2) {
                float sa = v_len(v_sub(a->p, a->prev));
                float sb = v_len(v_sub(b->p, b->prev));
                Kite *loser = sa >= sb ? b : a;
                Kite *winner = (loser == a) ? b : a;
                loser->falling = 1;
                loser->vy = 0;
                if (winner->is_player) winner->score += 100;
                if (loser == a) break;
            }
        }
    }
}

static void camera_update(void)
{
    Kite *k = &kt[0];
    V3 target = k->alive ? k->p : v3(k->hx, 0, k->hz);
    float dx = cosf(k->yaw), dz = sinf(k->yaw);
    // Camera atras da ancora, olhando pra pipa
    V3 want = v3(k->hx - dx * 18.0f, 10.0f, k->hz - dz * 18.0f);
    cam_pos.x += (want.x - cam_pos.x) * 0.08f;
    cam_pos.y += (want.y - cam_pos.y) * 0.08f;
    cam_pos.z += (want.z - cam_pos.z) * 0.08f;

    V3 f = v_norm(v_sub(target, cam_pos));
    V3 r = v_norm(v_cross(f, v3(0, 1, 0)));
    V3 u = v_cross(r, f);
    cam_f = f;
    cam_r = r;
    cam_u = u;
}

// ---------- controle ----------
static void read_pad(void)
{
    padInfo info;
    padData d;
    pad_lx = pad_ly = pad_rx = pad_ry = 0;
    pad_reel_out = pad_reel_in = pad_go = 0;

    ioPadGetInfo(&info);
    if (!info.status[0]) { have_pad = 0; return; }

    memset(&d, 0, sizeof(d));
    ioPadGetData(0, &d);
    if (d.len > 0) {
        last_pad = d;
        have_pad = 1;
    }
    if (!have_pad) return;
    d = last_pad;

    float lx = ((int)d.ANA_L_H - 128) / 128.0f;
    float ly = ((int)d.ANA_L_V - 128) / 128.0f;
    float rx = ((int)d.ANA_R_H - 128) / 128.0f;
    float ry = ((int)d.ANA_R_V - 128) / 128.0f;
    if (fabsf(lx) < 0.20f) lx = 0;
    if (fabsf(ly) < 0.20f) ly = 0;
    if (fabsf(rx) < 0.20f) rx = 0;
    if (fabsf(ry) < 0.20f) ry = 0;

    if (d.BTN_LEFT)  lx = -1;
    if (d.BTN_RIGHT) lx = 1;
    if (d.BTN_UP)    ly = -1;
    if (d.BTN_DOWN)  ly = 1;

    pad_lx = lx;
    pad_ly = ly;
    pad_rx = rx;
    pad_ry = ry;
    pad_reel_out = d.BTN_CROSS;
    pad_reel_in  = d.BTN_CIRCLE;
    pad_go       = d.BTN_CROSS;
}

// ---------- jogo ----------
static void init_game(void)
{
    memset(kt, 0, sizeof(kt));
    for (int i = 0; i < NKITE; i++) kt[i].is_player = (i == 0);
    kt[0].col = 0x00FFFFFF;
    kt[0].col2 = 0x00FF4D4D;
    for (int i = 0; i < NKITE; i++) spawn_kite(i);
    cam_pos = v3(0, 10, -20);
    camera_update();
    dead_t = 0;
}

static void update_game(void)
{
    read_pad();

    // Jogador morto: aperta X pra voltar
    if (!kt[0].alive) {
        dead_t++;
        if (dead_t > 60 && pad_go && !x_prev) {
            spawn_kite(0);
            kt[0].score = 0;
            dead_t = 0;
        }
    }

    for (int i = 0; i < NKITE; i++) kite_step(i);
    cut_check();
    camera_update();
    x_prev = pad_go;
}

// ---------- render ----------
static void kite_draw(const Kite *k)
{
    V3 g = v3(k->hx, 0, k->hz);
    V3 out = v_sub(k->p, g);
    float L = v_len(out);
    if (L < 0.01f) return;
    out = v_scale(out, 1.0f / L);

    V3 side = v_norm(v_cross(v3(0, 1, 0), out));
    V3 upk  = v_norm(v_cross(side, out));

    // Losango da pipa: topo, esquerda, base, direita
    V3 pts[4] = {
        v_add(k->p, v_scale(upk, 2.0f)),
        v_add(k->p, v_scale(side, -2.4f)),
        v_add(k->p, v_scale(upk, -1.6f)),
        v_add(k->p, v_scale(side, 2.4f)),
    };
    float sx[4], sy[4];
    for (int i = 0; i < 4; i++) {
        int ix, iy;
        float d;
        if (!project(pts[i], &ix, &iy, &d)) return;
        sx[i] = (float)ix;
        sy[i] = (float)iy;
    }

    u32 c1 = k->falling ? dark_col(k->col)  : k->col;
    u32 c2 = k->falling ? dark_col(k->col2) : k->col2;
    fill_tri(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], c1);
    fill_tri(sx[0], sy[0], sx[3], sy[3], sx[2], sy[2], c2);
}

static void render(void)
{
    frame++;
    float base = (float)W / 1280.0f;
    focal = H * 0.9f;

    // Ceu e chao (horizonte aproximado pela direcao da camera)
    int hy = (int)(H * 0.5f);
    V3 dh = v3(cam_f.x, 0, cam_f.z);
    float dl = v_len(dh);
    if (dl > 0.001f) {
        dh = v_scale(dh, 1.0f / dl);
        float cdu = v_dot(dh, cam_u), cdf = v_dot(dh, cam_f);
        if (cdf > 0.01f) hy = (int)(H * 0.5f - focal * cdu / cdf);
    }
    if (hy < 0) hy = 0;
    if (hy > (int)H) hy = H;
    for (int y = 0; y < hy; y++)
        fill_rect(0, y, W, 1, lerp_col(0x00102A4A, 0x006C9AC8, (float)y / (float)(hy ? hy : 1)));
    for (int y = hy; y < (int)H; y++)
        fill_rect(0, y, W, 1, lerp_col(0x00305A2A, 0x00142412, (float)(y - hy) / (float)((int)H - hy ? (int)H - hy : 1)));

    // Grade do campo
    for (int i = -GRID_N; i <= GRID_N; i++) {
        float x = i * GRID_STEP;
        draw_seg3(v3(x, 0, -GRID_N * GRID_STEP), v3(x, 0, GRID_N * GRID_STEP), 0x00356B3A);
        draw_seg3(v3(-GRID_N * GRID_STEP, 0, x), v3(GRID_N * GRID_STEP, 0, x), 0x00356B3A);
    }

    // Sombras no chao
    for (int i = 0; i < NKITE; i++) {
        const Kite *k = &kt[i];
        if (!k->alive) continue;
        int sx, sy;
        float d;
        if (project(v3(k->p.x, 0, k->p.z), &sx, &sy, &d)) {
            int r = (int)(focal * 1.6f / d);
            if (r < 2) r = 2;
            if (r > 40) r = 40;
            circle(sx, sy, r, 0x00142012);
        }
    }

    // Linhas (do ponto da pipa ate a ancora no chao)
    for (int i = 0; i < NKITE; i++) {
        const Kite *