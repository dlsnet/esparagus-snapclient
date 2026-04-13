#include "led_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "LED_MGR";

typedef struct {
    bool initialized;
    bool active_high;

    gpio_num_t idle_pin;
    gpio_num_t playback_pin;

    led_audio_state_t audio_state;
    led_health_mode_t health_mode;

    // Cached outputs to avoid redundant gpio writes
    bool idle_on;
    bool playback_on;

    SemaphoreHandle_t mutex;
    TaskHandle_t task;
} led_mgr_t;

static led_mgr_t s_led = {
    .initialized = false,
    .active_high = true,
    .idle_pin = (gpio_num_t)IDLE_GPIO,
    .playback_pin = (gpio_num_t)PLAYBACK_EN_GPIO,
    .audio_state = LED_AUDIO_IDLE,
    .health_mode = LED_HEALTH_NORMAL,
    .idle_on = false,
    .playback_on = false,
    .mutex = NULL,
    .task = NULL,
};

static inline int level_for(bool on, bool active_high)
{
    if (active_high) {
        return on ? 1 : 0;
    }
    return on ? 0 : 1;
}

static void apply_outputs(bool idle_on, bool playback_on)
{
    // NOTE: gpio_set_level returns esp_err_t but is safe to ignore in steady-state loops.
    if (s_led.idle_on != idle_on) {
        gpio_set_level(s_led.idle_pin, level_for(idle_on, s_led.active_high));
        s_led.idle_on = idle_on;
    }
    if (s_led.playback_on != playback_on) {
        gpio_set_level(s_led.playback_pin, level_for(playback_on, s_led.active_high));
        s_led.playback_on = playback_on;
    }
}

static void led_manager_task(void *arg)
{
    (void)arg;

    bool blink_phase = false;
    led_health_mode_t prev_health = LED_HEALTH_NORMAL;

    for (;;) {
        led_audio_state_t audio;
        led_health_mode_t health;

        if (s_led.mutex) {
            xSemaphoreTake(s_led.mutex, portMAX_DELAY);
        }
        audio = s_led.audio_state;
        health = s_led.health_mode;
        if (s_led.mutex) {
            xSemaphoreGive(s_led.mutex);
        }

        if (health != prev_health) {
            // Reset blink phase on mode transitions for predictable patterns
            blink_phase = false;
            prev_health = health;
        }

        uint32_t delay_ms = 1000;

        if (health == LED_HEALTH_NORMAL) {
            // Audio-driven
            bool idle_on = false;
            bool playback_on = false;

            switch (audio) {
                case LED_AUDIO_OFF:
                    idle_on = false;
                    playback_on = false;
                    break;
                case LED_AUDIO_IDLE:
                    idle_on = true;
                    playback_on = false;
                    break;
                case LED_AUDIO_PLAYING:
                    idle_on = false;
                    playback_on = true;
                    break;
                default:
                    idle_on = false;
                    playback_on = false;
                    break;
            }

            apply_outputs(idle_on, playback_on);

            // Wait indefinitely for an update
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        // Health override (playback forced off, idle blinks)
        switch (health) {
            case LED_HEALTH_DISCONNECTED:
                delay_ms = 1000;
                break;
            case LED_HEALTH_REBOOTING:
                delay_ms = 200;
                break;
            default:
                delay_ms = 1000;
                break;
        }

        blink_phase = !blink_phase;
        apply_outputs(blink_phase, false);

        // Wait for either the next blink tick or a state change
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }
}

static void notify_task(void)
{
    if (s_led.task) {
        xTaskNotifyGive(s_led.task);
    }
}

void led_manager_init(gpio_num_t idle_pin, gpio_num_t playback_pin, bool active_high)
{
    // First call performs full init; subsequent calls are idempotent and only update pins/active level if changed.
    if (s_led.mutex == NULL) {
        s_led.mutex = xSemaphoreCreateMutex();
        if (s_led.mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return;
        }
    }

    xSemaphoreTake(s_led.mutex, portMAX_DELAY);

    s_led.idle_pin = idle_pin;
    s_led.playback_pin = playback_pin;
    s_led.active_high = active_high;

    if (!s_led.initialized) {
        gpio_config_t io_cfg = {
            .pin_bit_mask = (1ULL << s_led.idle_pin) | (1ULL << s_led.playback_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        esp_err_t err = gpio_config(&io_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
            xSemaphoreGive(s_led.mutex);
            return;
        }

        // Default outputs
        s_led.idle_on = false;
        s_led.playback_on = false;
        gpio_set_level(s_led.idle_pin, level_for(false, s_led.active_high));
        gpio_set_level(s_led.playback_pin, level_for(false, s_led.active_high));

        // Spawn task
        BaseType_t ok = xTaskCreate(led_manager_task, "led_mgr", 2048, NULL, tskIDLE_PRIORITY + 1, &s_led.task);
        if (ok != pdPASS || s_led.task == NULL) {
            ESP_LOGE(TAG, "Failed to create LED manager task");
            xSemaphoreGive(s_led.mutex);
            return;
        }

        s_led.initialized = true;
        ESP_LOGI(TAG, "Initialized: idle_gpio=%d, playback_gpio=%d, active_%s",
                 (int)s_led.idle_pin, (int)s_led.playback_pin, s_led.active_high ? "high" : "low");
    }

    xSemaphoreGive(s_led.mutex);

    // Ensure initial state is applied quickly
    notify_task();
}

void led_manager_set_audio_state(led_audio_state_t state)
{
    if (s_led.mutex == NULL) {
        // Best-effort: allow use even if init wasn't called yet.
        led_manager_init((gpio_num_t)IDLE_GPIO, (gpio_num_t)PLAYBACK_EN_GPIO, true);
        if (s_led.mutex == NULL) {
            return;
        }
    }

    xSemaphoreTake(s_led.mutex, portMAX_DELAY);
    if (s_led.audio_state != state) {
        s_led.audio_state = state;
        xSemaphoreGive(s_led.mutex);
        notify_task();
        return;
    }
    xSemaphoreGive(s_led.mutex);
}

void led_manager_set_health_mode(led_health_mode_t mode)
{
    if (s_led.mutex == NULL) {
        led_manager_init((gpio_num_t)IDLE_GPIO, (gpio_num_t)PLAYBACK_EN_GPIO, true);
        if (s_led.mutex == NULL) {
            return;
        }
    }

    xSemaphoreTake(s_led.mutex, portMAX_DELAY);
    if (s_led.health_mode != mode) {
        s_led.health_mode = mode;
        xSemaphoreGive(s_led.mutex);
        notify_task();
        return;
    }
    xSemaphoreGive(s_led.mutex);
}

led_health_mode_t led_manager_get_health_mode(void)
{
    if (s_led.mutex == NULL) {
        return s_led.health_mode;
    }
    xSemaphoreTake(s_led.mutex, portMAX_DELAY);
    led_health_mode_t mode = s_led.health_mode;
    xSemaphoreGive(s_led.mutex);
    return mode;
}

led_audio_state_t led_manager_get_audio_state(void)
{
    if (s_led.mutex == NULL) {
        return s_led.audio_state;
    }
    xSemaphoreTake(s_led.mutex, portMAX_DELAY);
    led_audio_state_t st = s_led.audio_state;
    xSemaphoreGive(s_led.mutex);
    return st;
}
