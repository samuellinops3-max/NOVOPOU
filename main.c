// Pipa Combate - feito a partir do Cobra Arena (PSL1GHT)
// Controles: analogico = mover, X = puxao, R1 = enrolar, L1 = soltar
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

#define WORLD  3200.0f
#define NKITE  8
#define PI     3.14159265f

typedef struct {
    float x, y, z;
    float vx, vy, vz;
    float ang;
    int alive, respawn, score, cuts, inv;
    u32 col;
    float line_len;
    int pulling;
} Kite;

static const u32 palette[8] = {
    0x00FF4D4D, 0x0055D6FF, 0x0066FF66, 0x00FFD23F,
    0x00C77DFF, 0x00FF8FAB, 0x0000E5C0, 0x00FF9F43
};

/* ---------- video - IGUAL AO TEU COBRA ---------- */
static gcmContextData *ctx;
static u32 W, H;
static u32 *fb[2];
static u32 fboff[2];
static u32 *back;
static int cur = 0, first_flip = 1;
static volatile int running = 1;

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
    W = res.width; H = res.height;
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

/* ---------- desenho - IGUAL AO TEU COBRA + linha ---------- */
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

static void line_draw(int x0, int y0, int x1, int y1, u32 c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        if (x0 >= 0 && x0 < (int)W && y0 >= 0 && y0 < (int)H) back[y0 * W + x0] = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

typedef struct { char c; const char *p; } Glyph;
static const Glyph glyphs[] = {
    {'0', "###" "#.#" "#.#" "#.#" "###"}, {'1', ".#." "##." ".#." ".#." "###"},
    {'2', "###" "..#" "###" "#.." "###"}, {'3', "###" "..#" "###" "..#" "###"},
    {'4', "#.#" "#.#" "###" "..#" "..#"}, {'5', "###" "#.." "###" "..#" "###"},
    {'6', "###" "#.." "###" "#.#" "###"}, {'7', "###" "..#" "..#" "..#" "..#"},
    {'8', "###" "#.#" "###" "#.#" "###"}, {'9', "###" "#.#" "###" "..#" "###"},
    {'A', ".#." "#.#" "###" "#.#" "#.#"}, {'B', "##." "#.#" "##." "#.#" "##."},
    {'C', "###" "#.." "#.." "#.." "###"}, {'E', "###" "#.." "###" "#.." "###"},
    {'G', "###" "#.." "#.#" "#.#" "###"}, {'M', "#.#" "###" "###" "#.#" "#.#"},
    {'O', "###" "#.#" "#.#" "#.#" "###"}, {'P', "###" "#.#" "###" "#.." "#.."},
    {'R', "##." "#.#" "##." "#.#" "#.#"}, {'S', "###" "#.." "###" "..#" "###"},
    {'T', "###" ".#." ".#." ".#." ".#."}, {'U', "#.#" "#.#" "#.#" "#.#" "###"},
    {'V', "#.#" "#.#" "#.#" "#.#" ".#."}, {'X', "#.#" "#.#" ".#." "#.#" "#.#"},
    {'I', "###" ".#." ".#." ".#." "###"}, {'L', "#.." "#.." "#.." "#.." "###"},
    {'N', "#.#" "###" "###" "#.#" "#.#"}, {'D', "##." "#.#" "#.#" "#.#" "##."},
    {'F', "###" "#.." "###" "#.." "#.."}, {'H', "#.#" "#.#" "###" "#.#" "#.#"},
    {'K', "#.#" "#.#" "##." "#.#" "#.#"}, {'W', "#.#" "#.#" "###" "###" "#.#"},
    {':', "..." ".#." "..." ".#." "..."}, {'/', "..#" "..#" ".#." ".#." "#.."},
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

/* ---------- jogo PIPA ---------- */
static Kite kites[NKITE];
static float camx = WORLD / 2, camy = WORLD / 2;
static float wind_x = 1.5f, wind_y = 0.5f;
static int frame = 0, dead_t = 0, x_prev = 0, game_over = 0;
static float pad_ax = 0, pad_ay = 0, pad_mag = 0;
static int pad_x = 0, pad_r1 = 0, pad_l1 = 0;

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static void spawn_kite(int i)
{
    Kite *k = &kites[i];
    k->x = 400 + frand() * (WORLD - 800);
    k->y = 400 + frand() * (WORLD - 800);
    k->z = 500 + frand() * 300;
    k->vx = k->vy = k->vz = 0;
    k->ang = frand() * 2 * PI;
    k->alive = 1; k->respawn = 0; k->inv = 120;
    k->line_len = 900; k->pulling = 0;
    k->col = palette[i % 8];
}

static void cut_kite(int victim, int by)
{
    Kite *k = &kites[victim];
    if (k->inv > 0) return;
    k->alive = 0; k->respawn = 180;
    if (by >= 0) { kites[by].cuts++; kites[by].score += 100; }
    if (victim == 0) dead_t = 0;
}

static void init_game(void)
{
    for (int i = 0; i < NKITE; i++) { kites[i].score = 0; kites[i].cuts = 0; spawn_kite(i); }
    game_over = 0; dead_t = 0;
}

static void read_pad(void)
{
    padInfo info; padData d;
    pad_ax = pad_ay = pad_mag = 0; pad_x = pad_r1 = pad_l1 = 0;
    ioPadGetInfo(&info);
    if (info.status[0]) {
        ioPadGetData(0, &d);
        float ax = ((int)d.ANA_L_H - 128) / 128.0f, ay = ((int)d.ANA_L_V - 128) / 128.0f;
        if (fabsf(ax) < 0.20f) ax = 0;
        if (fabsf(ay) < 0.20f) ay = 0;
        if (d.BTN_LEFT) ax = -1;
        if (d.BTN_RIGHT) ax = 1;
        if (d.BTN_UP) ay = -1;
        if (d.BTN_DOWN) ay = 1;
        pad_ax = ax; pad_ay = ay;
        pad_mag = sqrtf(ax * ax + ay * ay);
        pad_x = d.BTN_CROSS;
        pad_r1 = d.BTN_R1;
        pad_l1 = d.BTN_L1;
    }
}

static void update_kite(int i)
{
    Kite *k = &kites[i];
    if (!k->alive) {
        if (--k->respawn <= 0) spawn_kite(i);
        return;
    }
    if (k->inv > 0) k->inv--;
    wind_x += (frand() - 0.5f) * 0.02f;
    wind_y += (frand() - 0.5f) * 0.02f;
    if (wind_x > 3) wind_x = 3; if (wind_x < -3) wind_x = -3;
    if (wind_y > 3) wind_y = 3; if (wind_y < -3) wind_y = -3;

    float tx = 0, ty = 0; int boost = 0;
    if (i == 0) {
        tx = pad_ax; ty = pad_ay; boost = pad_x;
        if (pad_r1) k->line_len -= 8;
        if (pad_l1) k->line_len += 8;
    } else {
        float best = 1e9f; int bi = -1;
        for (int o = 0; o < NKITE; o++) {
            if (o == i || !kites[o].alive) continue;
            float dx = kites[o].x - k->x, dy = kites[o].y - k->y;
            float d = dx * dx + dy * dy;
            if (d < best) { best = d; bi = o; }
        }
        if (bi >= 0) {
            float dx = kites[bi].x - k->x, dy = kites[bi].y - k->y;
            float d = sqrtf(dx * dx + dy * dy) + 1;
            if ((k->cuts + frame / 600) % 3 != 0) { tx = dx / d; ty = dy / d; }
            else { tx = -dx / d; ty = -dy / d; }
            boost = (rand() % 100 < 20) ? 1 : 0;
        }
    }
    if (k->line_len < 400) k->line_len = 400;
    if (k->line_len > 2000) k->line_len = 2000;

    float power = boost ? 0.9f : 0.45f;
    k->vx += tx * power + wind_x * 0.08f;
    k->vy += ty * power + wind_y * 0.08f;
    if (i == 0 && pad_r1) k->vz += 0.5f;
    if (i == 0 && pad_l1) k->vz -= 0.3f;
    k->vz += (600 - k->z) * 0.002f;
    k->vx *= 0.96f; k->vy *= 0.96f; k->vz *= 0.95f;
    k->x += k->vx; k->y += k->vy; k->z += k->vz;
    if (k->x < 200) k->x = 200; if (k->x > WORLD - 200) k->x = WORLD - 200;
    if (k->y < 200) k->y = 200; if (k->y > WORLD - 200) k->y = WORLD - 200;
    if (k->z < 200) k->z = 200; if (k->z > 1100) k->z = 1100;
    if (k->vx != 0 || k->vy != 0) k->ang = atan2f(k->vy, k->vx);
    k->pulling = boost;

    if (k->inv > 0) return;
    for (int o = 0; o < NKITE; o++) {
        if (o == i || !kites[o].alive) continue;
        float dx = kites[o].x - k->x, dy = kites[o].y - k->y;
        float dz = kites[o].z - k->z;
        float d2 = dx * dx + dy * dy + dz * dz * 0.5f;
        if (d2 < 180 * 180) {
            int my_power = k->pulling ? 70 : 30;
            int other_power = kites[o].pulling ? 70 : 30;
            if (rand() % 100 < my_power && rand() % 100 > other_power) { cut_kite(o, i); return; }
            else if (rand() % 100 < other_power && rand() % 100 > my_power) { cut_kite(i, o); return; }
        }
    }
}

static void update_game(void)
{
    if (game_over) {
        dead_t++;
        if (dead_t > 60 && pad_x && !x_prev) init_game();
        x_prev = pad_x; return;
    }
    for (int i = 0; i < NKITE; i++) update_kite(i);
    if (kites[0].alive) { camx = kites[0].x; camy = kites[0].y; }
    else {
        dead_t++;
        if (dead_t > 90 && pad_x && !x_prev) { spawn_kite(0); dead_t = 0; }
    }
    x_prev = pad_x;
    if (kites[0].cuts >= 5) game_over = 1;
    for (int i = 1; i < NKITE; i++) if (kites[i].cuts >= 5) game_over = 1;
}

static void draw_kite_shape(int cx, int cy, int r, u32 col, float ang)
{
    int x1 = cx + (int)(cosf(ang) * r), y1 = cy + (int)(sinf(ang) * r);
    int x3 = cx + (int)(cosf(ang + PI) * r * 0.7f), y3 = cy + (int)(sinf(ang + PI) * r * 0.7f);
    for (int t = 0; t <= r; t++) {
        int a1 = cx + (int)((x1 - cx) * t / r), b1 = cy + (int)((y1 - cy) * t / r);
        int a2 = cx + (int)((x3 - cx) * t / r), b2 = cy + (int)((y3 - cy) * t / r);
        line_draw(a1, b1, a2, b2, col);
    }
    line_draw(x3, y3, x3 + (int)(cosf(ang + PI) * r), y3 + (int)(sinf(ang + PI) * r), 0x00FFFFFF);
    circle(cx, cy, r / 3, 0x00FFFFFF);
    circle(cx, cy, r / 5, col);
}

static void render(void)
{
    frame++;
    float base = (float)W / 1280.0f;
    float hw = W / 2.0f, hh = H / 2.0f;
    for (u32 yy = 0; yy < H; yy++) {
        u32 c = 0x0087CEEB - (yy * 0x30 / H);
        u32 *p = back + yy * W;
        for (u32 xx = 0; xx < W; xx++) p[xx] = c;
    }
    for (int i = 0; i < 12; i++) {
        int cx = (int)((i * 997.0f - camx * 0.2f)) % (int)W;
        if (cx < 0) cx += W;
        int cy = 40 + i * 55;
        circle(cx, cy, 28, 0x00FFFFFF);
        circle(cx + 30, cy + 8, 22, 0x00FFFFFF);
        circle(cx - 30, cy + 10, 20, 0x00FFFFFF);
    }
    int ground_y = (int)H - 120;
    fill_rect(0, ground_y, W, 120, 0x003A7D44);
    for (int i = 0; i < 20; i++) {
        int bx = (int)((i * 523.0f - camx * 0.5f)) % (int)W;
        if (bx < 0) bx += W;
        fill_rect(bx, ground_y - 40 - (i % 3) * 20, 50, 40 + (i % 3) * 20, 0x00808080);
        fill_rect(bx + 8, ground_y - 30 - (i % 3) * 20, 12, 12, 0x00FFD23F);
    }
    draw_text("VENTO", 24, 90, 3, 0x00FFFFFF);
    int wx = 24 + 80, wy = 115;
    line_draw(wx, wy, wx + (int)(wind_x * 18), wy + (int)(wind_y * 18), 0x00FF0000);
    circle(wx + (int)(wind_x * 18), wy + (int)(wind_y * 18), 5, 0x00FF0000);

    for (int i = 0; i < NKITE; i++) {
        Kite *k = &kites[i];
        if (!k->alive) continue;
        float sc = base * (0.7f + k->z / 1000.0f);
        int sx = (int)((k->x - camx) * sc + hw);
        int sy = (int)((k->y - camy) * sc + hh - k->z * sc * 0.45f);
        int ground_x = (int)((k->x - camx) * sc + hw);
        int ground_y2 = (int)((k->y - camy) * sc + hh + 100);
        circle(ground_x, ground_y2, (int)(18 * sc), 0x00000000);
        int hand_x = ground_x, hand_y = ground_y2;
        if (i == 0) { hand_x = (int)W / 2; hand_y = (int)H - 40; }
        line_draw(hand_x, hand_y, sx, sy, k->pulling ? 0x00FF0000 : 0x00FFFFFF);
        if (k->inv > 0 && (frame / 6) % 2 == 0) continue;
        int r = (int)(38 * sc);
        draw_kite_shape(sx, sy, r, k->col, k->ang);
        if (i == 0) draw_text("VOCE", sx - 20, sy + r + 8, 2, 0x00FFFFFF);
    }

    char buf[64];
    draw_text("PIPA COMBATE", 24, 20, 4, 0x00FFFFFF);
    sprintf(buf, "CORTES: %d / 5", kites[0].cuts);
    draw_text(buf, 24, 48, 3, 0x00FFD23F);
    sprintf(buf, "SCORE: %d", kites[0].score);
    draw_text(buf, 24, 68, 3, 0x00FFFFFF);
    draw_text("ANALOGICO: MOVER  X: PUXAO  R1: ENROLAR  L1: SOLTAR", 24, (int)H - 30, 3, 0x00000000);
    draw_text("RANKING", (int)W - 220, 20, 3, 0x00FFFFFF);
    for (int i = 0; i < NKITE; i++) {
        char b2[32];
        sprintf(b2, "%d - %d", i == 0 ? 1 : i + 1, kites[i].cuts);
        draw_text(b2, (int)W - 220, 45 + i * 18, 2, i == 0 ? 0x00FFD23F : 0x00FFFFFF);
    }
    if (!kites[0].alive && !game_over) {
        draw_text("CORTARAM TUA PIPA!", (int)W / 2 - 180, (int)H / 2 - 20, 4, 0x00FF0000);
        if (dead_t > 90) draw_text("APERTE X PRA VOLTAR", (int)W / 2 - 160, (int)H / 2 + 20, 3, 0x00FFFFFF);
    }
    if (game_over) {
        int bw = text_width("VOCE VENCEU!", 6) + 60, bh = 120;
        fill_rect((int)W / 2 - bw / 2, (int)H / 2 - bh / 2, bw, bh, 0x00000000);
        if (kites[0].cuts >= 5) draw_text("VOCE VENCEU!", (int)W / 2 - text_width("VOCE VENCEU!", 6) / 2, (int)H / 2 - 30, 6, 0x00FFD23F);
        else draw_text("FIM DE JOGO", (int)W / 2 - text_width("FIM DE JOGO", 6) / 2, (int)H / 2 - 30, 6, 0x00FF4D4D);
        draw_text("APERTE X PRA JOGAR DE NOVO", (int)W / 2 - text_width("APERTE X PRA JOGAR DE NOVO", 3) / 2, (int)H / 2 + 30, 3, 0x00FFFFFF);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    init_screen();
    ioPadInit(7);
    sysUtilRegisterCallback(0, sysutil_cb, NULL);
    srand(20261112);
    init_game();
    while (running) {
        sysUtilCheckCallback();
        read_pad();
        update_game();
        render();
        present();
    }
    return 0;
}