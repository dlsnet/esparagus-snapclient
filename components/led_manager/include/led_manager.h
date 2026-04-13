#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Shared LED manager for snapclient.
 *
 * Purpose:
 * - Prevent GPIO contention between main.c (health/recovery indication) and player.c (idle/playing indication).
 * - Provide a single ownership point for the physical LEDs (GPIO config + runtime driving).
 *
 * Policy:
 * - In LED_HEALTH_NORMAL mode, the "audio" state (idle/playing/off) drives the LEDs.
 * - In non-NORMAL health modes, health indication overrides audio indication:
 *     - Playback LED is forced OFF
 *     - Idle LED blinks according to the selected health pattern
 */

#ifndef PLAYBACK_EN_GPIO
#define PLAYBACK_EN_GPIO 5
#endif

#ifndef IDLE_GPIO
#define IDLE_GPIO 2
#endif

typedef enum {
    LED_AUDIO_OFF = 0,      // Both LEDs off
    LED_AUDIO_IDLE,         // Idle LED on, Playback LED off
    LED_AUDIO_PLAYING,      // Playback LED on, Idle LED off
} led_audio_state_t;

typedef enum {
    LED_HEALTH_NORMAL = 0,        // Audio state drives LEDs
    LED_HEALTH_DISCONNECTED,      // Slow blink on idle LED (reconnect/connection lost)
    LED_HEALTH_REBOOTING,         // Fast blink on idle LED (imminent reboot)
} led_health_mode_t;

/**
 * Initialize the LED manager.
 *
 * This function is idempotent: calling it more than once is safe.
 *
 * @param idle_pin       GPIO used for "idle" indicator.
 * @param playback_pin   GPIO used for "playing" indicator.
 * @param active_high    true if GPIO high turns the LED on; false if the LED is active-low.
 */
void led_manager_init(gpio_num_t idle_pin, gpio_num_t playback_pin, bool active_high);

/**
 * Update the audio indication state (used primarily by player.c).
 */
void led_manager_set_audio_state(led_audio_state_t state);

/**
 * Update the health indication mode (used primarily by main.c health monitor).
 * Non-NORMAL modes override the audio state.
 */
void led_manager_set_health_mode(led_health_mode_t mode);

/**
 * Get the currently applied health mode (thread-safe).
 */
led_health_mode_t led_manager_get_health_mode(void);

/**
 * Get the currently applied audio state (thread-safe).
 */
led_audio_state_t led_manager_get_audio_state(void);

#ifdef __cplusplus
}
#endif
