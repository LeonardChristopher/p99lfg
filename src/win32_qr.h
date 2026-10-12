#ifndef P99LFG_WIN32_QR_H
#define P99LFG_WIN32_QR_H

/* Render the given text as a QR code (payload limited to ~64 hex chars).
 * Returns 0 on success. Must be called before qr_draw/qr_total_px. */
int qr_encode_payload(const char *payload);

/* Draw the encoded QR at (x,y) with the given module size in pixels. */
void qr_draw(void *hdc, int x, int y, int module_px);

/* Total drawn size in pixels for a given module size. */
int qr_total_px(int module_px);

/* The payload that was encoded. */
const char *qr_payload(void);

#endif
