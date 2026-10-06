#include "feedback.h"

#include <stdint.h>

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"

#define STEP_MS         20
#define TASK_STACK      3072
#define TASK_PRIO       3

#define BUZZER_TIMER    LEDC_TIMER_0
#define BUZZER_CHANNEL  LEDC_CHANNEL_0
#define BUZZER_CHANNEL_B LEDC_CHANNEL_1
#define BUZZER_DUTY_RES LEDC_TIMER_10_BIT
#define BUZZER_DUTY_ON  512         /* half of 2^10: a square wave */
#define BUZZER_HALF_PERIOD 512      /* where in the period the second leg goes high */

typedef struct {
    uint8_t r, g, b;
} rgb_t;

/* One step of a sound. The pitch is a percentage of the piezo's resonant frequency, because a
 * bare piezo is only loud near it: a tone an octave away is barely audible. */
typedef struct {
    uint8_t percent;                /* of the resonant frequency; 0 is silence */
    uint16_t ms;
} tone_t;

static const char *TAG = "feedback";

static QueueHandle_t s_queue;
static led_strip_handle_t s_led;
static bool s_buzzer;
static bool s_buzzer_two_pin;
static uint32_t s_buzzer_hz;

/*
 * Sounds, each ending in a zero-length step. They stay within about a tenth of the resonant
 * frequency either side, and sweep across it rather than sit on it: a part's resonance is
 * only known to within that much, and a sweep is sure to pass through the loud spot. With so
 * narrow a band they are told apart by rhythm and direction, not by pitch.
 */
static const tone_t s_chirp[]  = { { 94, 20 }, { 100, 20 }, { 106, 40 }, { 0, 0 } };       /* one quick rise */
static const tone_t s_blip[]   = { { 100, 40 }, { 0, 60 }, { 100, 40 }, { 0, 0 } };        /* two flat pips */
static const tone_t s_done[]   = { { 92, 60 }, { 100, 60 }, { 0, 60 },
                                   { 100, 60 }, { 108, 140 }, { 0, 0 } };                  /* two rises, the second higher */
static const tone_t s_failed[] = { { 108, 120 }, { 100, 120 }, { 92, 260 }, { 0, 0 } };    /* one long fall */

static bool is_resting(app_feedback_t fb)
{
    return fb == APP_FB_IDLE || fb == APP_FB_JOB_WAITING || fb == APP_FB_NFC_ERROR || fb == APP_FB_BOOTLOADER;
}

/* How long a momentary indication lasts. Writing lasts until something replaces it. */
static uint32_t moment_ms(app_feedback_t fb)
{
    switch (fb) {
    case APP_FB_TAG_OK:      return 200;
    case APP_FB_TAG_UNKNOWN: return 200;
    case APP_FB_JOB_DONE:    return 700;
    case APP_FB_JOB_FAILED:  return 700;
    default:                 return UINT32_MAX;
    }
}

static const tone_t *sound_of(app_feedback_t fb)
{
    switch (fb) {
    case APP_FB_TAG_OK:      return s_chirp;
    case APP_FB_TAG_UNKNOWN: return s_blip;
    case APP_FB_JOB_DONE:    return s_done;
    case APP_FB_JOB_FAILED:  return s_failed;
    default:                 return NULL;
    }
}

/* A triangle wave from `lo` to `hi` and back over `period_ms`. */
static uint8_t breathe(uint32_t t_ms, uint32_t period_ms, uint8_t lo, uint8_t hi)
{
    const uint32_t half = period_ms / 2;
    const uint32_t phase = t_ms % period_ms;
    const uint32_t up = phase < half ? phase : period_ms - phase;
    return (uint8_t)(lo + (hi - lo) * up / half);
}

/* The LED is bright and bare on this board; these are deliberately low. */
static rgb_t colour_of(app_feedback_t fb, uint32_t t_ms)
{
    switch (fb) {
    case APP_FB_IDLE:
        return (rgb_t){ 0, 0, 6 };
    case APP_FB_JOB_WAITING: {
        const uint8_t v = breathe(t_ms, 1200, 2, 40);
        return (rgb_t){ v, (uint8_t)(v / 2), 0 };
    }
    case APP_FB_NFC_ERROR:
        return (t_ms % 1000) < 500 ? (rgb_t){ 30, 0, 0 } : (rgb_t){ 0, 0, 0 };
    case APP_FB_BOOTLOADER:
        return (rgb_t){ 24, 0, 24 };
    case APP_FB_TAG_OK:
        return (rgb_t){ 0, 40, 0 };
    case APP_FB_TAG_UNKNOWN:
        return (rgb_t){ 20, 20, 20 };
    case APP_FB_JOB_WRITING:
        return (t_ms % 160) < 80 ? (rgb_t){ 0, 0, 48 } : (rgb_t){ 0, 0, 0 };
    case APP_FB_JOB_DONE:
        return (rgb_t){ 0, 48, 0 };
    case APP_FB_JOB_FAILED:
        return (t_ms % 240) < 120 ? (rgb_t){ 48, 0, 0 } : (rgb_t){ 0, 0, 0 };
    }
    return (rgb_t){ 0, 0, 0 };
}

static void set_led(rgb_t c)
{
    static rgb_t shown = { 1, 1, 1 };       /* not black, so the first call always writes */
    if (s_led == NULL || (c.r == shown.r && c.g == shown.g && c.b == shown.b)) {
        return;
    }
    shown = c;
    led_strip_set_pixel(s_led, 0, c.r, c.g, c.b);
    led_strip_refresh(s_led);
}

static void set_tone(uint32_t freq_hz)
{
    static uint32_t playing;
    if (!s_buzzer || freq_hz == playing) {
        return;
    }
    playing = freq_hz;
    const uint32_t duty = freq_hz ? BUZZER_DUTY_ON : 0;
    if (freq_hz) {
        ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER, freq_hz);
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
    if (s_buzzer_two_pin) {
        /* The same square wave, half a period later: high exactly when the first leg is low.
         * Silent, both legs sit low, so nothing is held across the piezo. */
        ledc_set_duty_with_hpoint(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL_B, duty, BUZZER_HALF_PERIOD);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL_B);
    }
}

static void feedback_task(void *arg)
{
    (void)arg;
    app_feedback_t resting = APP_FB_IDLE;
    app_feedback_t showing = APP_FB_IDLE;
    uint32_t t_ms = 0;                      /* since `showing` began */
    const tone_t *sound = NULL;
    uint32_t sound_left_ms = 0;

    for (;;) {
        app_feedback_t fb;
        if (xQueueReceive(s_queue, &fb, pdMS_TO_TICKS(STEP_MS)) == pdTRUE) {
            if (is_resting(fb)) {
                resting = fb;
                /* A moment in progress is allowed to finish; writing is not a moment. */
                if (is_resting(showing) || showing == APP_FB_JOB_WRITING) {
                    showing = fb;
                    t_ms = 0;
                }
            } else {
                showing = fb;
                t_ms = 0;
                sound = sound_of(fb);
                sound_left_ms = sound ? sound->ms : 0;
            }
            continue;                       /* drain the queue before drawing */
        }

        t_ms += STEP_MS;
        if (!is_resting(showing) && t_ms >= moment_ms(showing)) {
            showing = resting;
            t_ms = 0;
        }
        set_led(colour_of(showing, t_ms));

        if (sound) {
            set_tone(s_buzzer_hz * sound->percent / 100);
            if (sound_left_ms > STEP_MS) {
                sound_left_ms -= STEP_MS;
            } else {
                sound++;
                sound_left_ms = sound->ms;
                if (sound->ms == 0) {
                    sound = NULL;
                }
            }
        } else {
            set_tone(0);
        }
    }
}

static esp_err_t led_init(const feedback_config_t *config)
{
    const led_strip_config_t strip = {
        .strip_gpio_num = config->led_gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = config->led_grb ? LED_STRIP_COLOR_COMPONENT_FMT_GRB
                                                  : LED_STRIP_COLOR_COMPONENT_FMT_RGB,
    };
    const led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip, &rmt, &s_led), TAG, "LED");
    return led_strip_clear(s_led);
}

static esp_err_t buzzer_init(int gpio, int gpio_b, uint32_t resonant_hz)
{
    s_buzzer_hz = resonant_hz;
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BUZZER_DUTY_RES,
        .timer_num = BUZZER_TIMER,
        .freq_hz = resonant_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "buzzer timer");
    const ledc_channel_config_t channel = {
        .gpio_num = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_CHANNEL,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "buzzer channel");

    if (gpio_b >= 0) {
        const ledc_channel_config_t channel_b = {
            .gpio_num = gpio_b,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = BUZZER_CHANNEL_B,
            .timer_sel = BUZZER_TIMER,
            .duty = 0,
            .hpoint = BUZZER_HALF_PERIOD,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_b), TAG, "buzzer second channel");
        s_buzzer_two_pin = true;
    }
    return ESP_OK;
}

esp_err_t feedback_init(const feedback_config_t *config)
{
    /* Neither is worth failing the boot for: a device with a dead LED still reads tags. */
    if (config->led_gpio >= 0 && led_init(config) != ESP_OK) {
        s_led = NULL;
    }
    if (config->buzzer_gpio >= 0) {
        s_buzzer = buzzer_init(config->buzzer_gpio, config->buzzer_gpio_b, config->buzzer_hz) == ESP_OK;
    }

    s_queue = xQueueCreate(8, sizeof(app_feedback_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_FALSE(xTaskCreate(feedback_task, "feedback", TASK_STACK, NULL, TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "cannot create the task");
    return ESP_OK;
}

void feedback_show(app_feedback_t fb)
{
    if (s_queue) {
        xQueueSend(s_queue, &fb, 0);
    }
}

bool feedback_buzzer_fitted(void)
{
    return s_buzzer;
}
