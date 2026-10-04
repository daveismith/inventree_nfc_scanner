/*
 * What the device shows and sounds: the onboard RGB LED and an optional piezo buzzer.
 *
 * The LED rests in a colour for the current state and flashes for moments:
 *
 *   idle            dim blue                 tag read        green flash, one chirp
 *   job waiting     amber, breathing         tag unknown     white flash, low blip
 *   reader fault    red, slow blink          writing         blue, fast blink
 *   bootloader      magenta                  job done        green, two rising chirps
 *                                            job failed      red blinks, low buzz
 *
 * With no buzzer fitted (gpio -1) the sound half does nothing and the rest is unchanged.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "app_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int led_gpio;           /* WS2812 data; -1 for none */
    bool led_grb;           /* colour order on the wire: GRB (most WS2812) or RGB */
    int buzzer_gpio;        /* passive piezo, driven with a square wave; -1 for none */
} feedback_config_t;

esp_err_t feedback_init(const feedback_config_t *config);

/* Safe from any task; never blocks. */
void feedback_show(app_feedback_t fb);

bool feedback_buzzer_fitted(void);

#ifdef __cplusplus
}
#endif
