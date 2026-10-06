/*
 * What the device shows and sounds: the onboard RGB LED and an optional piezo buzzer.
 *
 * The LED rests in a colour for the current state and flashes for moments:
 *
 *   idle            dim blue                 tag read        green flash, one quick rising chirp
 *   job waiting     amber, breathing         tag unknown     white flash, two flat pips
 *   reader fault    red, slow blink          writing         blue, fast blink
 *   bootloader      magenta                  job done        green, two rising chirps
 *                                            job failed      red blinks, one long falling tone
 *
 * With no buzzer fitted (gpio -1) the sound half does nothing and the rest is unchanged.
 *
 * The piezo can sit between one GPIO and ground, or between two GPIOs. With two, they are
 * driven in antiphase, so it sees twice the voltage swing and is louder; at rest both are low.
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
    int buzzer_gpio_b;      /* the piezo's other leg, driven in antiphase; -1 when that leg is on GND */
    uint32_t buzzer_hz;     /* the piezo's resonant frequency: every sound is pitched around it */
} feedback_config_t;

esp_err_t feedback_init(const feedback_config_t *config);

/* Safe from any task; never blocks. */
void feedback_show(app_feedback_t fb);

bool feedback_buzzer_fitted(void);

#ifdef __cplusplus
}
#endif
