#include "pn532.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pn532_frame.h"

#define PN532_I2C_ADDR          0x24

#define CMD_GET_FIRMWARE_VERSION    0x02
#define CMD_SAM_CONFIGURATION       0x14
#define CMD_RF_CONFIGURATION        0x32
#define CMD_IN_DATA_EXCHANGE        0x40
#define CMD_IN_COMMUNICATE_THRU     0x42
#define CMD_IN_LIST_PASSIVE_TARGET  0x4A
#define CMD_IN_RELEASE              0x52

/* Type 2 commands the PN532 knows how to frame itself; the rest go through untouched. */
#define T2_READ     0x30
#define T2_WRITE    0xA2

#define ACK_TIMEOUT_MS      30      /* measured: under 1 ms */
#define FAST_TIMEOUT_MS     100     /* commands that involve no tag: measured 1.4 ms */
/* InListPassiveTarget with one retry: 19 ms measured with no tag. With a tag arriving it has
 * been seen to take over 300 ms: a tag that answers the first request and then fumbles the
 * selection is retried on the chip's own, much longer, timeouts. Slow is not the same as dead. */
#define POLL_TIMEOUT_MS     1500
#define TAG_TIMEOUT_MS      300     /* an exchange with a tag; the chip gives up at about 54 ms */
#define XFER_TIMEOUT_MS     50      /* one I2C transaction */
#define READY_POLL_MS       1

/* One activation retry: a second attempt before reporting "no tag" (19 ms rather than 13). */
#define PASSIVE_ACTIVATION_RETRIES  0x01

#define RSP_MAX     96              /* the longest frame read: two ISO-DEP targets from a poll, with their ATS */

static const char *TAG = "pn532";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static pn532_config_t s_cfg;
static nfc_xcvr_t s_xcvr;

static bool s_selected;
static uint8_t s_uid_len;
static uint8_t s_uid[PN532_UID_MAX];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* At least one tick, whatever the tick rate. */
static void delay_ms(uint32_t ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

/*
 * Wait for the chip to have something to say. With the IRQ line wired that is a pin going
 * low; without it, the first byte of every read is a status byte whose low bit means ready.
 * A read the chip refuses outright is the same answer as "not ready".
 */
static esp_err_t wait_ready(uint32_t timeout_ms)
{
    const int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        if (s_cfg.irq_gpio >= 0) {
            if (gpio_get_level(s_cfg.irq_gpio) == 0) {
                return ESP_OK;
            }
        } else {
            uint8_t status = 0;
            if (i2c_master_receive(s_dev, &status, 1, XFER_TIMEOUT_MS) == ESP_OK && (status & 0x01)) {
                return ESP_OK;
            }
        }
        if (now_ms() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        delay_ms(READY_POLL_MS);
    }
}

/* A read whose first byte is the ready status. The chip can still refuse it; try a few times. */
static esp_err_t read_frame(uint8_t *buf, size_t len)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 5; attempt++) {
        err = i2c_master_receive(s_dev, buf, len, XFER_TIMEOUT_MS);
        if (err == ESP_OK && (buf[0] & 0x01)) {
            return ESP_OK;
        }
        delay_ms(2);
    }
    return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
}

/*
 * One command and its answer. On ESP_OK, `rsp` holds what followed the response code.
 * ESP_ERR_TIMEOUT: no ACK or no answer in time. ESP_ERR_INVALID_RESPONSE: a damaged frame, or
 * the chip rejecting the command.
 */
static esp_err_t command(uint8_t cmd, const uint8_t *params, size_t params_len,
                         uint8_t *rsp, size_t rsp_cap, size_t *rsp_len, uint32_t timeout_ms)
{
    uint8_t frame[PN532_FRAME_OVERHEAD + 1 + 32];
    const size_t n = pn532_frame_build(cmd, params, params_len, frame, sizeof(frame));
    ESP_RETURN_ON_FALSE(n > 0, ESP_ERR_INVALID_SIZE, TAG, "command 0x%02x too long", cmd);

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = i2c_master_transmit(s_dev, frame, n, XFER_TIMEOUT_MS);
        if (err == ESP_OK) {
            break;
        }
        delay_ms(2);
    }
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "command 0x%02x: write failed (%s)", cmd, esp_err_to_name(err));
        return err;
    }

    uint8_t buf[1 + RSP_MAX];
    err = wait_ready(ACK_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = read_frame(buf, 1 + PN532_ACK_LEN);
    }
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "command 0x%02x: no ACK (%s)", cmd, esp_err_to_name(err));
        return err == ESP_ERR_TIMEOUT ? err : ESP_ERR_INVALID_RESPONSE;
    }
    if (pn532_frame_parse(buf + 1, PN532_ACK_LEN, NULL, NULL) != PN532_FRAME_ACK) {
        ESP_LOGD(TAG, "command 0x%02x: not an ACK", cmd);
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = wait_ready(timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "command 0x%02x: no answer in %u ms", cmd, (unsigned)timeout_ms);
        return err;
    }
    /* The chip pads a read longer than its frame, so one fixed-size read is enough. */
    const size_t want = 1 + PN532_FRAME_OVERHEAD + 1 + rsp_cap;
    const size_t len = want < sizeof(buf) ? want : sizeof(buf);
    err = read_frame(buf, len);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    const pn532_frame_kind_t kind = pn532_frame_parse(buf + 1, len - 1, &payload, &payload_len);
    if (kind != PN532_FRAME_DATA || payload[0] != (uint8_t)(cmd + 1)) {
        ESP_LOGD(TAG, "command 0x%02x: unexpected frame (kind %d)", cmd, (int)kind);
        return ESP_ERR_INVALID_RESPONSE;
    }
    payload++;
    payload_len--;
    if (payload_len > rsp_cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (payload_len) {
        memcpy(rsp, payload, payload_len);
    }
    if (rsp_len) {
        *rsp_len = payload_len;
    }
    return ESP_OK;
}

void pn532_recover(void)
{
    /* An ACK frame from the host makes the chip drop whatever it was doing. */
    i2c_master_transmit(s_dev, PN532_ACK_FRAME, PN532_ACK_LEN, XFER_TIMEOUT_MS);
    i2c_master_bus_reset(s_bus);
    delay_ms(5);
    s_selected = false;
}

esp_err_t pn532_start(pn532_version_t *version)
{
    s_selected = false;

    if (s_cfg.rst_gpio >= 0) {
        gpio_set_level(s_cfg.rst_gpio, 0);
        delay_ms(10);
        gpio_set_level(s_cfg.rst_gpio, 1);
        delay_ms(20);
    }
    /* Addressing the chip wakes it; it needs a moment before the first command. */
    pn532_recover();
    delay_ms(50);

    uint8_t rsp[8];
    size_t n = 0;
    esp_err_t err = command(CMD_GET_FIRMWARE_VERSION, NULL, 0, rsp, sizeof(rsp), &n, FAST_TIMEOUT_MS);
    if (err == ESP_OK && n < 4) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "GetFirmwareVersion");
    if (version) {
        version->ic = rsp[0];
        version->ver = rsp[1];
        version->rev = rsp[2];
        version->support = rsp[3];
    }

    /* Normal mode, no SAM; drive the IRQ pin, which costs nothing when it is not wired. */
    const uint8_t sam[] = { 0x01, 0x14, 0x01 };
    ESP_RETURN_ON_ERROR(command(CMD_SAM_CONFIGURATION, sam, sizeof(sam), rsp, sizeof(rsp), &n, FAST_TIMEOUT_MS),
                        TAG, "SAMConfiguration");

    /* MaxRetries. The default for passive activation is to try forever, which would make a
     * poll with no tag never return. */
    const uint8_t retries[] = { 0x05, 0xFF, 0x01, PASSIVE_ACTIVATION_RETRIES };
    ESP_RETURN_ON_ERROR(command(CMD_RF_CONFIGURATION, retries, sizeof(retries), rsp, sizeof(rsp), &n, FAST_TIMEOUT_MS),
                        TAG, "RFConfiguration");
    return ESP_OK;
}

esp_err_t pn532_poll(pn532_target_t *target)
{
    memset(target, 0, sizeof(*target));
    s_selected = false;

    /* Up to two targets at 106 kbps type A: asking for two is how a second tag is noticed. */
    const uint8_t params[] = { 0x02, 0x00 };
    uint8_t rsp[32];
    size_t n = 0;
    const esp_err_t err = command(CMD_IN_LIST_PASSIVE_TARGET, params, sizeof(params), rsp, sizeof(rsp), &n, POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    if (n < 1) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    target->count = rsp[0];
    if (target->count == 0) {
        return ESP_OK;
    }
    /* Tg, SENS_RES (2), SEL_RES, NFCIDLength, NFCID */
    if (n < 6 || rsp[5] > PN532_UID_MAX || n < (size_t)6 + rsp[5]) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    target->atqa = ((uint16_t)rsp[2] << 8) | rsp[3];
    target->sak = rsp[4];
    target->uid_len = rsp[5];
    memcpy(target->uid, rsp + 6, target->uid_len);

    if (target->count == 1) {
        s_selected = true;
        s_uid_len = target->uid_len;
        memcpy(s_uid, target->uid, s_uid_len);
    }
    return ESP_OK;
}

esp_err_t pn532_release(void)
{
    s_selected = false;
    const uint8_t all = 0x00;
    uint8_t rsp[4];
    size_t n = 0;
    return command(CMD_IN_RELEASE, &all, 1, rsp, sizeof(rsp), &n, FAST_TIMEOUT_MS);
}

static nfc_err_t xcvr_transceive(nfc_xcvr_t *x, const uint8_t *tx, size_t tx_len,
                                 uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    (void)x;
    *rx_len = 0;
    if (!s_selected || tx_len == 0 || tx_len > 16) {
        return NFC_ERR_TIMEOUT;
    }

    /* READ and WRITE go as InDataExchange, which understands the tag's bare ACK to a write.
     * Everything else (GET_VERSION, PWD_AUTH) the chip does not know, and passes through. */
    uint8_t params[1 + 16];
    size_t params_len = 0;
    uint8_t cmd;
    if (tx[0] == T2_READ || tx[0] == T2_WRITE) {
        cmd = CMD_IN_DATA_EXCHANGE;
        params[params_len++] = 0x01;        /* target 1 */
    } else {
        cmd = CMD_IN_COMMUNICATE_THRU;
    }
    memcpy(params + params_len, tx, tx_len);
    params_len += tx_len;

    uint8_t rsp[1 + 24];
    size_t n = 0;
    const esp_err_t err = command(cmd, params, params_len, rsp, sizeof(rsp), &n, TAG_TIMEOUT_MS);
    if (err != ESP_OK || n < 1) {
        pn532_recover();
        return NFC_ERR_IO;
    }

    /* Timeout, CRC, parity, framing and RF protocol errors: the exchange was lost in the air,
     * which is what a tag at the edge of the field or pressed against the antenna does. Worth
     * trying again, unlike a tag that answered and said no. */
    const uint8_t status = rsp[0] & 0x3F;
    if (status == 0x01 || status == 0x02 || status == 0x03 || status == 0x05 || status == 0x0B) {
        ESP_LOGD(TAG, "tag command 0x%02x: lost in the air (status 0x%02x)", tx[0], status);
        s_selected = false;
        return NFC_ERR_TIMEOUT;
    }
    if (status != 0x00) {
        ESP_LOGD(TAG, "tag command 0x%02x: status 0x%02x", tx[0], status);
        s_selected = false;
        return NFC_ERR_NAK;
    }
    n -= 1;
    if (n > rx_cap) {
        n = rx_cap;
    }
    memcpy(rx, rsp + 1, n);
    *rx_len = n;
    return NFC_OK;
}

/* Select the same tag again: release, look, and check that what answers is the one we had. */
static nfc_err_t xcvr_reselect(nfc_xcvr_t *x)
{
    (void)x;
    uint8_t uid[PN532_UID_MAX];
    const uint8_t uid_len = s_uid_len;
    memcpy(uid, s_uid, sizeof(uid));

    if (pn532_release() != ESP_OK) {
        pn532_recover();
    }
    pn532_target_t t;
    if (pn532_poll(&t) != ESP_OK) {
        pn532_recover();
        return NFC_ERR_IO;
    }
    if (t.count != 1 || t.uid_len != uid_len || memcmp(t.uid, uid, uid_len) != 0) {
        s_selected = false;
        return NFC_ERR_TIMEOUT;
    }
    return NFC_OK;
}

bool pn532_present(bool type2)
{
    if (!type2) {
        /* No command known to work on it: selecting it again is the test. */
        return xcvr_reselect(&s_xcvr) == NFC_OK;
    }
    if (!s_selected && xcvr_reselect(&s_xcvr) != NFC_OK) {
        return false;
    }
    const uint8_t read0[] = { T2_READ, 0x00 };
    uint8_t rx[16];
    size_t n = 0;
    if (xcvr_transceive(&s_xcvr, read0, sizeof(read0), rx, sizeof(rx), &n) == NFC_OK) {
        return true;
    }
    /* One miss is not a departure. */
    return xcvr_reselect(&s_xcvr) == NFC_OK;
}

nfc_xcvr_t *pn532_xcvr(void)
{
    return &s_xcvr;
}

esp_err_t pn532_init(const pn532_config_t *config)
{
    s_cfg = *config;
    s_xcvr.transceive = xcvr_transceive;
    s_xcvr.reselect = xcvr_reselect;

    if (s_cfg.rst_gpio >= 0) {
        const gpio_config_t rst = {
            .pin_bit_mask = 1ULL << s_cfg.rst_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "reset pin");
        gpio_set_level(s_cfg.rst_gpio, 1);
    }
    if (s_cfg.irq_gpio >= 0) {
        const gpio_config_t irq = {
            .pin_bit_mask = 1ULL << s_cfg.irq_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&irq), TAG, "IRQ pin");
    }

    const i2c_master_bus_config_t bus = {
        .i2c_port = s_cfg.i2c_port,
        .sda_io_num = s_cfg.sda_gpio,
        .scl_io_num = s_cfg.scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_bus), TAG, "I2C bus");

    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PN532_I2C_ADDR,
        .scl_speed_hz = s_cfg.freq_hz,
        /* The chip holds SCL low while it thinks; the driver's default gives up after 2 ms. */
        .scl_wait_us = 20000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev, &s_dev), TAG, "I2C device");

    /* The I2C driver logs an error for every refused read. For this chip a refusal is routine
     * (see the top of pn532.h), and the outcome is reported here where it means something. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    return ESP_OK;
}
