/*
 * PN532 NFC controller over I2C (new i2c_master driver), as a reader of ISO 14443-3A tags.
 *
 * Measured on this board (PN532 v1.6, 400 kHz): the ACK is ready 0.4-1 ms after a command,
 * a quick answer 1.4 ms after that, and a poll that finds no tag takes 12.6 / 18.8 / 23.8 ms
 * with 0 / 1 / 2 activation retries. About one read in twenty is refused while the chip is
 * busy, which is why a failed read here means "not yet", not "broken".
 *
 * One caller at a time: the driver has no lock, because one task owns the reader.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "nfc_xcvr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PN532_UID_MAX 10

typedef struct {
    int i2c_port;       /* I2C controller number */
    int sda_gpio;
    int scl_gpio;
    uint32_t freq_hz;
    int irq_gpio;       /* P70_IRQ, low when an answer is ready; -1 when not wired */
    int rst_gpio;       /* RSTPD_N, active low; -1 when not wired */
} pn532_config_t;

typedef struct {
    uint8_t ic;         /* 0x32 for a PN532 */
    uint8_t ver;
    uint8_t rev;
    uint8_t support;
} pn532_version_t;

typedef struct {
    uint8_t count;      /* tags found: 0, 1, or 2 (two is as many as the chip reports) */
    uint16_t atqa;      /* the first tag's SENS_RES */
    uint8_t sak;        /* SEL_RES */
    uint8_t uid_len;
    uint8_t uid[PN532_UID_MAX];
} pn532_target_t;

/* Set up the bus. Does not talk to the chip. */
esp_err_t pn532_init(const pn532_config_t *config);

/* Wake the chip (through its reset pin if wired), read its version and configure it as a
 * reader. Safe to call again whenever it has stopped answering. */
esp_err_t pn532_start(pn532_version_t *version);

/* Look for tags once. Returns within about 20 ms when there are none. Finding exactly one
 * leaves it selected, ready for pn532_xcvr(). */
esp_err_t pn532_poll(pn532_target_t *target);

/*
 * The selected tag is still there, by one check: a READ of page 0 for a Type 2 tag, a fresh
 * selection for anything else. One failure is not a departure; the caller decides how many are.
 */
bool pn532_present(bool type2);

/* Deselect whatever is selected. */
esp_err_t pn532_release(void);

/* Exchange Type 2 commands with the tag pn532_poll() selected. */
nfc_xcvr_t *pn532_xcvr(void);

/* After an error: abort the chip's current command and clear the bus. */
void pn532_recover(void);

#ifdef __cplusplus
}
#endif
