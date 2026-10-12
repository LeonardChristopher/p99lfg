/* QR display for the Windows app: renders the Nostr secret key as a QR
 * code popup so phones can scan it. Uses the vendored Nayuki qrcodegen
 * (MIT) + plain GDI drawing. */
#include "win32_qr.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qrcodegen.h"

#define QR_MARGIN 4 /* modules of quiet zone */

static uint8_t g_qr[qrcodegen_BUFFER_LEN_MAX];
static uint8_t g_qr_temp[qrcodegen_BUFFER_LEN_MAX];
static int g_qr_size;
static char g_qr_payload[160];

int qr_encode_payload(const char *payload)
{
    if (!payload || !payload[0])
        return -1;
    if (!qrcodegen_encodeText(payload, g_qr_temp, g_qr,
                              qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true))
        return -1;
    g_qr_size = qrcodegen_getSize(g_qr);
    snprintf(g_qr_payload, sizeof(g_qr_payload), "%s", payload);
    return 0;
}

void qr_draw(void *hdcv, int x, int y, int module_px)
{
    HDC hdc = (HDC)hdcv;
    int total = (g_qr_size + QR_MARGIN * 2) * module_px;
    /* white background incl. quiet zone */
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RECT rc = { x, y, x + total, y + total };
    FillRect(hdc, &rc, white);

    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    for (int cy = 0; cy < g_qr_size; cy++) {
        for (int cx = 0; cx < g_qr_size; cx++) {
            if (!qrcodegen_getModule(g_qr, cx, cy))
                continue;
            RECT m = {
                x + (cx + QR_MARGIN) * module_px,
                y + (cy + QR_MARGIN) * module_px,
                x + (cx + QR_MARGIN + 1) * module_px,
                y + (cy + QR_MARGIN + 1) * module_px
            };
            FillRect(hdc, &m, black);
        }
    }
}

int qr_total_px(int module_px)
{
    return (g_qr_size + QR_MARGIN * 2) * module_px;
}

const char *qr_payload(void)
{
    return g_qr_payload;
}
