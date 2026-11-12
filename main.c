#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sysmodule/sysmodule.h>
#include <rsx/rsx.h>
#include <io/pad.h>

#define MAX_BUFFERS 2

typedef struct {
    int height;
    int width;
    uint32_t *ptr;
    uint32_t offset;
} rsxBuffer;

static void setResolution(gcmContextData *context, rsxBuffer *buffers) {
    videoState state;
    videoResolution res;
    videoGetState(0, 0, &state);
    videoGetResolution(state.displayMode.resolution, &res);

    videoConfiguration vconfig;
    memset(&vconfig, 0, sizeof(videoConfiguration));
    vconfig.resolution = state.displayMode.resolution;
    vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.pitch = res.width * sizeof(uint32_t);

    videoConfigure(0, &vconfig, NULL, 0);
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    for (int i = 0; i < MAX_BUFFERS; i++) {
        buffers[i].width = res.width;
        buffers[i].height = res.height;
        buffers[i].ptr = (uint32_t *)rsxMemalign(64, res.width * res.height * sizeof(uint32_t));
        rsxAddressToOffset(buffers[i].ptr, &buffers[i].offset);
        gcmSetDisplayBuffer(i, buffers[i].offset, res.width * sizeof(uint32_t), res.width, res.height);
    }
}

// Funcao para pintar um pixel com seguranca
static inline void putPixel(rsxBuffer *buf, int x, int y, uint32_t color) {
    if (x >= 0 && x < buf->width && y >= 0 && y < buf->height) {
        buf->ptr[y * buf->width + x] = color;
    }
}

// Funcao para desenhar a linha da pipa (Algoritmo de Bresenham)
static void drawLine(rsxBuffer *buf, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        putPixel(buf, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Funcao para desenhar a pipa (losango dividido em 4 cores)
static void drawKite(rsxBuffer *buf, int cx, int cy, int size) {
    for (int y = -size; y <= size; y++) {
        int halfWidth = (size - abs(y)) * 0.75; // Deixa no formato alongado de pipa
        for (int x = -halfWidth; x <= halfWidth; x++) {
            uint32_t color;
            if (x < 0 && y < 0) color = 0x00FF0000;      // Vermelho (cima esquerda)
            else if (x >= 0 && y < 0) color = 0x00FFD700; // Amarelo (cima direita)
            else if (x < 0 && y >= 0) color = 0x0000CC00; // Verde (baixo esquerda)
            else color = 0x000044FF;                      // Azul escuro (baixo direita)
            
            putPixel(buf, cx + x, cy + y, color);
        }
    }
    // Armacao preta no meio (varetas)
    drawLine(buf, cx, cy - size, cx, cy + size, 0x00111111);
    drawLine(buf, cx - (size * 0.75), cy, cx + (size * 0.75), cy, 0x00111111);
}

int main(int argc, const char *argv[]) {
    void *host_addr = memalign(1024 * 1024, 1024 * 1024);
    gcmContextData *context = rsxInit(0x10000, 1024 * 1024, host_addr);

    rsxBuffer buffers[MAX_BUFFERS];
    setResolution(context, buffers);

    ioPadInit(7);
    padInfo padinfo;
    padData paddata;

    int currentBuffer = 0;
    
    // Posicao inicial da pipa no centro da tela
    float pipaX = buffers[0].width / 2.0f;
    float pipaY = buffers[0].height / 2.0f;
    int tamanhoPipa = 45;
    float balanco = 0.0f;

    uint32_t skyBlue = 0x0087CEEB; // Azul ceu
    uint32_t whiteLine = 0x00FFFFFF; // Linha branca com cerol

    while (1) {
        ioPadGetInfo(&padinfo);
        for (int i = 0; i < MAX_BUFFERS; i++) {
            if (padinfo.status[i]) {
                ioPadGetData(i, &paddata);
                
                // Sair do jogo apertando START
                if (paddata.BTN_START) goto end;

                // Movimento com o D-Pad (Setinhas) ou Analogico Esquerdo
                if (paddata.BTN_LEFT || paddata.ANA_L_H < 100)  pipaX -= 6.0f;
                if (paddata.BTN_RIGHT || paddata.ANA_L_H > 155) pipaX += 6.0f;
                if (paddata.BTN_UP || paddata.ANA_L_V < 100)    pipaY -= 6.0f;
                if (paddata.BTN_DOWN || paddata.ANA_L_V > 155)  pipaY += 6.0f;

                // Botao X da um "puxao" rapido na linha (subindo)
                if (paddata.BTN_CROSS) {
                    pipaY -= 10.0f;
                }
                // Botao Quadrado "desbica" para a esquerda e Bola para a direita
                if (paddata.BTN_SQUARE) pipaX -= 10.0f;
                if (paddata.BTN_CIRCLE) pipaX += 10.0f;
            }
        }

        // Efeito de vento balancando a pipa sozinha
        balanco += 0.1f;
        float ventoX = sinf(balanco) * 2.0f;
        float ventoY = cosf(balanco * 1.5f) * 1.5f;

        // Mantem a pipa dentro dos limites da tela
        if (pipaX < 50) pipaX = 50;
        if (pipaX > buffers[currentBuffer].width - 50) pipaX = buffers[currentBuffer].width - 50;
        if (pipaY < 50) pipaY = 50;
        if (pipaY > buffers[currentBuffer].height - 80) pipaY = buffers[currentBuffer].height - 80;

        // 1. Pinta o fundo de azul ceu
        int totalPixels = buffers[currentBuffer].width * buffers[currentBuffer].height;
        for (int p = 0; p < totalPixels; p++) {
            buffers[currentBuffer].ptr[p] = skyBlue;
        }

        int finalX = (int)(pipaX + ventoX);
        int finalY = (int)(pipaY + ventoY);

        // 2. Desenha a linha saindo da base da tela ate o centro da pipa
        drawLine(&buffers[currentBuffer], buffers[currentBuffer].width / 2, buffers[currentBuffer].height, finalX, finalY, whiteLine);

        // 3. Desenha a pipa por cima da linha
        drawKite(&buffers[currentBuffer], finalX, finalY, tamanhoPipa);

        gcmSetFlip(context, currentBuffer);
        rsxFlushBuffer(context);
        gcmSetWaitFlip(context);

        currentBuffer = !currentBuffer;
    }

end:
    return 0;
}
