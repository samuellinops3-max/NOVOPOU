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
#define MAX_KITES 7
#define PI        3.14159265f

typedef struct {
    float x, y, z, vx, vy, vz, tilt, anchorX;
    int alive, pulling, respawn, score, tick;
    u32 c1, c2;
} Kite;

static u32 get_palette(int idx) {
    u32 p[8] = {0x00FF2A2A,0x00FFD700,0x0000E676,0x002979FF,0x00D500F9,0x00FF6D00,0x0000E5FF,0x00FF1744};
    return p[idx & 7];
}

static gcmContextData *ctx;
static u32 W, H, *fb0, *fb1, fboff0, fboff1, *back;
static int cur = 0, first_flip = 1;
static volatile int running = 1;

static void sysutil_cb(u64 status, u64 param, void *usrdata) {
    (void)param; (void)usrdata;
    if (status == SYSUTIL_EXIT_GAME) running = 0;
}

static void init_screen(void) {
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
    vc.resolution = resid; vc.format = VIDEO_BUFFER_FORMAT_XRGB;
    vc.aspect = VIDEO_ASPECT_AUTO; vc.pitch = W * 4;
    videoConfigure(VIDEO_PRIMARY, &vc, NULL, 0);
    videoGetState(VIDEO_PRIMARY, 0, &st);
    gcmSetFlipMode(GCM_FLIP_VSYNC);
    fb0 = (u32 *)rsxMemalign(64, W * H * 4);
    gcmAddressToOffset(fb0, &fboff0);
    gcmSetDisplayBuffer(0, fboff0, W * 4, W, H);
    fb1 = (u32 *)rsxMemalign(64, W * H * 4);
    gcmAddressToOffset(fb1, &fboff1);
    gcmSetDisplayBuffer(1, fboff1, W * 4, W, H);
    back = (u32 *)memalign(64, W * H * 4);
    memset(back, 0, W * H * 4);
    gcmResetFlipStatus();
}

static void present(void) {
    if (!first_flip) { while (gcmGetFlipStatus() != 0) usleep(200); }
    gcmResetFlipStatus();
    memcpy((cur == 0) ? fb0 : fb1, back, W * H * 4);
    gcmSetFlip(ctx, cur); rsxFlushBuffer(ctx); gcmSetWaitFlip(ctx);
    cur ^= 1; first_flip = 0;
}

static inline void put_px(int x, int y, u32 c) {
    if ((u32)x < W && (u32)y < H) back[y * W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, u32 c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > (int)W ? (int)W : x + w;
    int y1 = y + h > (int)H ? (int)H : y + h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) back[yy * W + xx] = c;
}

static void draw_line(int x0, int y0, int x1, int y1, u32 c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy, e2;
    while (1) {
        put_px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void circle(int cx, int cy, int r, u32 c) {
    if (r < 1) r = 1;
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= (int)H) continue;
        int hw = (int)sqrtf((float)(r * r - dy * dy));
        for (int x = cx - hw; x <= cx + hw; x++) put_px(x, y, c);
    }
}

static void project_3d(float x, float y, float z, int *sx, int *sy, float *scale) {
    if (z < 120.0f) z = 120.0f;
    *scale = 450.0f / z;
    *sx = (int)(W * 0.5f + x * (*scale));
    *sy = (int)(H * 0.5f + y * (*scale));
}
static void draw_kite_3d(const Kite *k) {
    int sx, sy; float sc;
    project_3d(k->x, k->y, k->z, &sx, &sy, &sc);
    int size = (int)(48.0f * sc);
    if (size < 6) size = 6;
    int gx = (int)(W * 0.5f + k->anchorX * (450.0f / (k->z + 100.0f)));
    int midx = (sx + gx) / 2 + (int)(sinf(k->tick * 0.08f) * 18.0f * sc);
    int midy = (sy + (int)H) / 2 + (int)(25.0f * sc);
    u32 lcol = k->pulling ? 0x00FFFF00 : 0x00F0F0F0;
    draw_line(gx, (int)H, midx, midy, lcol);
    draw_line(midx, midy, sx, sy, lcol);
    float tx = (float)sx, ty = (float)(sy + size);
    for (int t = 1; t <= 8; t++) {
        float nx = sx + sinf(k->tick * 0.15f - t * 0.6f) * (t * 4.5f * sc) - k->tilt * t * 3.0f;
        float ny = sy + size + t * (12.0f * sc);
        draw_line((int)tx, (int)ty, (int)nx, (int)ny, 0x00FFFFFF);
        fill_rect((int)nx - 2, (int)ny - 1, 5, 3, (t % 2 == 0) ? k->c1 : k->c2);
        tx = nx; ty = ny;
    }
    float cs = cosf(k->tilt), sn = sinf(k->tilt);
    for (int dy = -size; dy <= size; dy++) {
        int hw = (int)((size - abs(dy)) * 0.72f);
        for (int dx = -hw; dx <= hw; dx++) {
            int rx = sx + (int)(dx * cs - dy * sn), ry = sy + (int)(dx * sn + dy * cs);
            put_px(rx, ry, ((dx < 0 && dy < 0) || (dx >= 0 && dy >= 0)) ? k->c1 : k->c2);
        }
    }
    draw_line(sx + (int)(size * sn), sy - (int)(size * cs), sx - (int)(size * sn), sy + (int)(size * cs), 0x00151515);
    draw_line(sx - (int)(size * 0.72f * cs), sy - (int)(size * 0.72f * sn), sx + (int)(size * 0.72f * cs), sy + (int)(size * 0.72f * sn), 0x00151515);
}

static const char *find_glyph(char c) {
    switch (c) {
        case '0': return "####.##.##.####"; case '1': return ".#.##..#..#.###";
        case '2': return "###..#####..###"; case '3': return "###..####..####";
        case '4': return "#.##.####..#..#"; case '5': return "####..###..####";
        case '6': return "####..####.####"; case '7': return "###..#..#..#..#";
        case '8': return "####.#####.####"; case '9': return "####.####..####";
        case 'A': return ".#.#.######.#.#"; case 'C': return "####..#..#..###";
        case 'D': return "##.#.##.##.###."; case 'E': return "####..###..####";
        case 'I': return "###.#..#..#.###"; case 'O': return "####.##.##.####";
        case 'P': return "####.#####..#.."; case 'R': return "##.#.###.#.##.#";
        case 'S': return "####..###..####"; case 'T': return "###.#..#..#..#.";
        case 'U': return "#.##.##.##.####"; case 'V': return "#.##.##.##.#.#.";
        case 'X': return "#.##.#.#.#.##.#"; default:  return NULL;
    }
}

static void draw_text(const char *s, int x, int y, int sc, u32 c) {
    for (; *s; s++) {
        const char *g = find_glyph(*s);
        if (g) for (int r = 0; r < 5; r++) for (int k = 0; k < 3; k++)
            if (g[r * 3 + k] == '#') fill_rect(x + k * sc, y + r * sc, sc, sc, c);
        x += 4 * sc;
    }
}

static Kite kites[MAX_KITES];
static float pad_ax = 0, pad_ay = 0;
static int pad_pull = 0, pad_desb = 0, x_prev = 0, dead_t = 0;
static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static void spawn_kite(int i) {
    Kite *k = kites + i;
    k->x = (i == 0) ? 0.0f : (frand() - 0.5f) * 800.0f;
    k->y = (i == 0) ? -40.0f : -180.0f + frand() * 200.0f;
    k->z = (i == 0) ? 380.0f : 320.0f + frand() * 480.0f;
    k->vx = k->vy = k->vz = k->tilt = 0.0f;
    k->anchorX = k->x * 0.7f; k->alive = 1; k->pulling = 0; k->respawn = 0;
    k->tick = rand() % 100; k->c1 = get_palette(i * 2); k->c2 = get_palette(i * 2 + 1);
}

static void read_pad(void) {
    padInfo info; padData d;
    pad_ax = pad_ay = 0; pad_pull = pad_desb = 0;
    ioPadGetInfo(&info);
    if (*(info.status)) {
        ioPadGetData(0, &d);
        float ax = ((int)d.ANA_L_H - 128) / 128.0f, ay = ((int)d.ANA_L_V - 128) / 128.0f;
        if (fabsf(ax) < 0.15f) ax = 0;
        if (fabsf(ay) < 0.15f) ay = 0;
        if (d.BTN_LEFT) ax = -1.0f;
        if (d.BTN_RIGHT) ax = 1.0f;
        if (d.BTN_UP) ay = -1.0f;
        if (d.BTN_DOWN) ay = 1.0f;
        pad_ax = ax; pad_ay = ay;
        pad_pull = d.BTN_CROSS || d.BTN_R2;
        pad_desb = d.BTN_R1 || d.BTN_CIRCLE;
    }
}

static int check_relo(const Kite *a, const Kite *b) {
    if (fabsf(a->z - b->z) > 95.0f) return 0;
    int ax, ay, bx, by; float sa, sb;
    project_3d(a->x, a->y, a->z, &ax, &ay, &sa);
    project_3d(b->x, b->y, b->z, &bx, &by, &sb);
    return (abs(ax - bx) < (int)(45 * sa) && abs(ay - by) < (int)(55 * sa));
}

static void update_game(void) {
    for (int i = 0; i < MAX_KITES; i++) {
        Kite *k = kites + i; k->tick++;
        if (!k->alive) {
            k->y += 2.5f; k->x += sinf(k->tick * 0.1f) * 3.0f; k->tilt += 0.12f;
            if (i > 0 && --k->respawn <= 0) spawn_kite(i);
            continue;
        }
        if (i == 0) {
            k->vx += pad_ax * 0.65f; k->vy += pad_ay * 0.65f; k->pulling = pad_pull;
            if (pad_pull) { k->vy -= 0.85f; k->vz -= 1.8f; } else { k->vz += 0.6f; }
            if (pad_desb) { k->vx += sinf(k->tick * 0.35f) * 1.6f; k->tilt = sinf(k->tick * 0.35f) * 0.8f; }
        } else {
            k->vx += sinf(k->tick * 0.03f + i) * 0.25f; k->vy += cosf(k->tick * 0.04f + i * 2) * 0.22f;
            k->vz += sinf(k->tick * 0.02f + i) * 0.8f; k->pulling = ((k->tick / 70) % 4 == 0);
            if (k->pulling) k->vy -= 0.4f;
        }
        k->vx = (k->vx + sinf(k->tick * 0.05f + i) * 0.08f) * 0.90f;
        k->vy = (k->vy + cosf(k->tick * 0.06f + i) * 0.06f) * 0.90f;
        k->vz *= 0.92f; k->x += k->vx; k->y += k->vy; k->z += k->vz;
        if (!pad_desb || i > 0) k->tilt = k->vx * 0.06f;
        if (k->x < -550) k->x = -550;
        if (k->x > 550) k->x = 550;
        if (k->y < -300) k->y = -300;
        if (k->y > 220) k->y = 220;
        if (k->z < 240) k->z = 240;
        if (k->z > 850) k->z = 850;
    }
    for (int i = 0; i < MAX_KITES; i++) {
        Kite *ki = kites + i; if (!ki->alive) continue;
        for (int j = i + 1; j < MAX_KITES; j++) {
            Kite *kj = kites + j; if (!kj->alive) continue;
            if (check_relo(ki, kj)) {
                if (ki->pulling && (!kj->pulling || ki->y < kj->y)) {
                    kj->alive = 0; kj->respawn = 180; ki->score += 100;
                } else if (kj->pulling) {
                    ki->alive = 0; ki->respawn = 180; kj->score += 100;
                    if (i == 0) dead_t = 0;
                }
            }
        }
    }
    Kite *pl = kites;
    if (!pl->alive) {
        dead_t++;
        if (dead_t > 50 && pad_pull && !x_prev) { spawn_kite(0); pl->score = 0; }
    }
    x_prev = pad_pull;
}

static void render(void) {
    for (u32 y = 0; y < H; y++) {
        u32 col = ((30 + (y * 90) / H) << 16) | ((130 + (y * 95) / H) << 8) | 235;
        for (u32 x = 0; x < W; x++) back[y * W + x] = col;
    }
    circle(W - 160, 110, 48, 0x00FFF59D); circle(W - 160, 110, 36, 0x00FFEE58);
    Kite *pl = kites;
    int coff = (pl->tick / 3) % (W + 300);
    circle(coff - 150, 130, 35, 0x00FFFFFF); circle(coff - 115, 125, 42, 0x00FFFFFF); circle(coff - 80, 132, 32, 0x00FFFFFF);
    int ord[MAX_KITES];
    for (int i = 0; i < MAX_KITES; i++) ord[i] = i;
    for (int i = 0; i < MAX_KITES - 1; i++)
        for (int j = i + 1; j < MAX_KITES; j++)
            if ((kites + ord[i])->z < (kites + ord[j])->z) { int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
    for (int i = 0; i < MAX_KITES; i++) draw_kite_3d(kites + ord[i]);
    char buf[16]; sprintf(buf, "%d", pl->score / 100);
    draw_text("CORTADAS", 24, 20, 3, 0x00FFFFFF); draw_text(buf, 24, 45, 5, 0x00FFD700);
    draw_text("X PUXAR  R1 DESBICAR", 24, (int)H - 35, 3, 0x000D47A1);
    if (!pl->alive) {
        fill_rect((int)W / 2 - 150, (int)H / 2 - 65, 300, 130, 0x00111111);
        draw_text("AVOOU", (int)W / 2 - 76, (int)H / 2 - 42, 8, 0x00FF3D00);
        if (dead_t > 50) draw_text("APERTE X", (int)W / 2 - 62, (int)H / 2 + 20, 4, 0x00FFFFFF);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    init_screen(); ioPadInit(7); sysUtilRegisterCallback(0, sysutil_cb, NULL);
    srand(20261112);
    for (int i = 0; i < MAX_KITES; i++) { (kites + i)->score = 0; spawn_kite(i); }
    while (running) {
        sysUtilCheckCallback(); read_pad(); update_game(); render(); present();
    }
    return 0;
}
