// Pou 3D - PS3 (PSL1GHT)
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

static gcmContextData *ctx;
static u32 W, H;
static u32 *fb[2];
static u32 fboff[2];
static u32 *back;
static int cur = 0, first_flip = 1;
static volatile int running = 1;

// Estado do Pou
static int pou_hunger = 80;   // 0 a 100
static int pou_health = 90;   // 0 a 100
static int pou_fun = 85;      // 0 a 100
static int pou_coins = 150;
static int game_timer = 0;
static char status_msg[64] = "Ola! Cuida do teu Pou!";

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

// Fonte de texto minima 3x5
typedef struct { char c; const char *p; } Glyph;
static const Glyph glyphs[] = {
    {'0', "###" "#.#" "#.#" "#.#" "###"}, {'1', ".#." "##." ".#." ".#." "###"},
    {'2', "###" "..#" "###" "#.." "###"}, {'3', "###" "..#" "###" "..#" "###"},
    {'4', "#.#" "#.#" "###" "..#" "..#"}, {'5', "###" "#.." "###" "..#" "###"},
    {'6', "###" "#.." "###" "#.#" "###"}, {'7', "###" "..#" "..#" "..#" "..#"},
    {'8', "###" "#.#" "###" "#.#" "###"}, {'9', "###" "#.#" "###" "..#" "###"},
    {'A', ".#." "#.#" "###" "#.#" "#.#"}, {'C', "###" "#.." "#.." "#.." "###"},
    {'D', "##." "#.#" "#.#" "#.#" "##."}, {'E', "###" "#.." "###" "#.." "###"},
    {'F', "###" "#.." "###" "#.." "#.."}, {'G', "###" "#.." "#.#" "#.#" "###"},
    {'H', "#.#" "#.#" "###" "#.#" "#.#"}, {'I', "###" ".#." ".#." ".#." "###"},
    {'K', "#.#" "#.#" "##." "#.#" "#.#"}, {'L', "#.." "#.." "#.." "#.." "###"},
    {'M', "#.#" "###" "###" "#.#" "#.#"}, {'O', "###" "#.#" "#.#" "#.#" "###"},
    {'P', "###" "#.#" "###" "#.." "#.."}, {'R', "##." "#.#" "##." "#.#" "#.#"},
    {'S', "###" "#.." "###" "..#" "###"}, {'T', "###" ".#." ".#." ".#." ".#."},
    {'U', "#.#" "#.#" "#.#" "#.#" "###"}, {'V', "#.#" "#.#" "#.#" "#.#" ".#."},
    {'X', "#.#" "#.#" ".#." "#.#" "#.#"}, {'!', ".#." ".#." ".#." "...*.*"},
};

static const char *find_glyph(char c)
{
    for (unsigned i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
        if (glyphs[i].c == c) return glyphs[i].p;
    return NULL;
}

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

static void read_pad(void)
{
    padInfo info;
    padData d;
    ioPadGetInfo(&info);
    if (info.status[0]) {
        ioPadGetData(0, &d);
        
        // Botão X: Alimentar o Pou (+ Fome, + Moedas)
        if (d.BTN_CROSS) {
            if (pou_hunger < 100) { pou_hunger += 5; if (pou_hunger > 100) pou_hunger = 100; }
            pou_coins += 2;
            strcpy(status_msg, "Nham nham! Pou comeu bem!");
        }
        // Botão Círculo: Brincar com o Pou (+ Diversão)
        if (d.BTN_CIRCLE) {
            if (pou_fun < 100) { pou_fun += 8; if (pou_fun > 100) pou_fun = 100; }
            strcpy(status_msg, "Yupi! Pou esta super feliz!");
        }
        // Botão Quadrado: Dar remédio / Cuidar da saúde
        if (d.BTN_SQUARE) {
            if (pou_health < 100) { pou_health += 10; if (pou_health > 100) pou_health = 100; }
            strcpy(status_msg, "Pou tomou remedio e esta saudavel!");
        }
    }
}

static void update_game(void)
{
    game_timer++;
    // Com o tempo, as necessidades descem devagar
    if (game_timer % 300 == 0) {
        if (pou_hunger > 0) pou_hunger--;
        if (pou_fun > 0) pou_fun--;
        if (pou_health > 0 && pou_hunger < 20) pou_health--;
    }
}

static void render(void)
{
    u32 n = W * H;
    // Fundo acolhedor do quarto do Pou (Azul claro/pastel)
    for (u32 i = 0; i < n; i++) back[i] = 0x00E0F7FA;

    // Chão da sala
    fill_rect(0, (int)H * 3 / 4, W, H / 4, 0x00B2EBF2);

    // Desenhar o Pou 3D no centro do ecrã
    int cx = W / 2;
    int cy = H / 2 + 30;
    int pou_r = 110; // Tamanho do corpo

    // Sombra do Pou no chão
    circle(cx, cy + pou_r + 15, pou_r * 8 / 10, 0x00B0BEC5);

    // Corpo triangular/arredondado característico do Pou (Cor Bege clássica 0x00D7CCC8)
    for (int r = -pou_r; r <= pou_r; r++) {
        int width = (int)sqrtf((float)(pou_r * pou_r - r * r));
        // Alarga em baixo para dar a forma de batata/gota
        if (r > 20) width = width * (pou_r - (r - 20) / 4) / pou_r;
        fill_rect(cx - width, cy + r, width * 2, 1, 0x00D7CCC8);
    }
    // Contorno suave
    circle(cx, cy, pou_r, 0x00A1887F);

    // Olhos 3D expressivos do Pou
    int eye_off = 35;
    int eye_y = cy - 15;
    // Olho esquerdo
    circle(cx - eye_off, eye_y, 22, 0x00FFFFFF);
    circle(cx - eye_off + 4, eye_y, 10, 0x00212121);
    // Olho direito
    circle(cx + eye_off, eye_y, 22, 0x00FFFFFF);
    circle(cx + eye_off - 4, eye_y, 10, 0x00212121);

    // Boca sorridente
    fill_rect(cx - 15, cy + 35, 30, 8, 0x005D4037);

    // --- HUD / Barras de Status ---
    // Barra de Fome (Vermelho/Laranja)
    draw_text("FOME", 50, 40, 2, 0x0037474F);
    fill_rect(150, 42, 200, 20, 0x00CFD8DC);
    fill_rect(150, 42, pou_hunger * 2, 20, 0x0066BB6A);

    // Barra de Saúde (Verde)
    draw_text("SAUDE", 50, 80, 2, 0x0037474F);
    fill_rect(150, 82, 200, 20, 0x00CFD8DC);
    fill_rect(150, 82, pou_health * 2, 20, 0x0042A5F5);

    // Barra de Diversão (Amarelo)
    draw_text("DIVERSAO", 50, 120, 2, 0x0037474F);
    fill_rect(150, 122, 200, 20, 0x00CFD8DC);
    fill_rect(150, 122, pou_fun * 2, 20, 0x00FFCA28);

    // Moedas
    char coin_buf[32];
    sprintf(coin_buf, "MOEDAS: %d", pou_coins);
    draw_text(coin_buf, W - 300, 40, 3, 0x00F57F17);

    // Mensagem de Status e Controles no Rodapé
    draw_text(status_msg, W / 2 - (strlen(status_msg) * 6), H - 120, 2, 0x00455A64);
    draw_text("X: ALIMENTAR   |   O: BRINCAR   |   []: REMEDIO", W / 2 - 310, H - 60, 2, 0x000288D1);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    init_screen();
    ioPadInit(7);
    sysUtilRegisterCallback(0, sysutil_cb, NULL);

    while (running) {
        sysUtilCheckCallback();
        read_pad();
        update_game();
        render();
        present();
    }
    return 0;
}
