#pragma once

#include <stddef.h>
#include <stdint.h>

/* The two-record message an InvenTree bin tag carries: a URI record for the location page and
 * a Text record with the short barcode. `uri_rest` is what follows "https://". Returns the
 * message length. This is the C twin of what tools/nfcprog.py builds. */
size_t make_inventree_ndef(uint8_t *out, size_t cap, const char *uri_rest, const char *text);

/* A single Text record of `text_len` filler characters, long-form when it has to be. */
size_t make_text_ndef(uint8_t *out, size_t cap, size_t text_len);

void run_ndef_tests(void);
void run_pn532_frame_tests(void);
void run_ntag21x_tests(void);
void run_proto_tests(void);
void run_app_core_tests(void);
void run_net_sync_tests(void);
void run_wifi_policy_tests(void);
void run_ota_stream_tests(void);
