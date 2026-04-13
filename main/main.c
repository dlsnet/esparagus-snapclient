/* Play flac file by audio pipeline
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
#include "eth_interface.h"
#else
#include "wifi_interface.h"
#endif

#include "nvs_flash.h"

// Minimum ESP-IDF stuff only hardware abstraction stuff
#include <wifi_provisioning.h>

#include "board.h"
#include "es8388.h"
#include "esp_netif.h"
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "mdns.h"
#include "net_functions.h"

// Web socket server
// #include "websocket_if.h"
// #include "websocket_server.h"

#include <sys/time.h>

#include "driver/i2s_std.h"
#if CONFIG_USE_DSP_PROCESSOR
#include "dsp_processor.h"
#endif

// Opus decoder is implemented as a subcomponet from master git repo
#include "opus.h"

// flac decoder is implemented as a subcomponet from master git repo
#include "FLAC/stream_decoder.h"
#include "ota_server.h"
#include "player.h"
#include "snapcast.h"
#include "ui_http_server.h"

#include "led_manager.h"

typedef struct decoderData_s decoderData_t;
typedef struct {
  tv_t items[16];
  size_t head;
  size_t count;
} flac_timestamp_fifo_t;

// Forward declarations
struct netconn;
extern struct netconn *lwipNetconn; // Declare it as extern since it's defined later

// Function declarations
static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
    void *client_data);
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data);
static void metadata_callback(const FLAC__StreamDecoder *decoder,
                              const FLAC__StreamMetadata *metadata,
                              void *client_data);
static void error_callback(const FLAC__StreamDecoder *decoder,
                           FLAC__StreamDecoderErrorStatus status,
                           void *client_data);
void free_flac_data(decoderData_t *pFlacData);
void cleanup_decoder_resources(void); // Declare this early

// Forward declarations for task functions (needed for health monitor recovery)
static void flac_decoder_task(void *pvParameters);
static void flac_task(void *pvParameters);
static void opus_decoder_task(void *pvParameters);
static bool flac_timestamp_fifo_push(flac_timestamp_fifo_t *fifo,
                                     const tv_t *timestamp);
static bool flac_timestamp_fifo_pop(flac_timestamp_fifo_t *fifo,
                                    tv_t *timestamp);
static void flac_handle_decoder_output(decoderData_t *pFlacData,
                                       snapcastSetting_t *scSet,
                                       flac_timestamp_fifo_t *timestamps);
static void flac_drain_decoder_output(snapcastSetting_t *scSet,
                                      flac_timestamp_fifo_t *timestamps,
                                      TickType_t wait);

// #include "ma120.h"

static FLAC__StreamDecoder *flacDecoder = NULL;
static QueueHandle_t decoderReadQHdl = NULL;
static QueueHandle_t decoderWriteQHdl = NULL;
static QueueHandle_t decoderTaskQHdl = NULL;
SemaphoreHandle_t decoderReadSemaphore = NULL;
SemaphoreHandle_t decoderWriteSemaphore = NULL;

const char *VERSION_STRING = "0.0.3";

#define HTTP_TASK_PRIORITY (configMAX_PRIORITIES - 2)  // 9
#define HTTP_TASK_CORE_ID 1                            // 1  // tskNO_AFFINITY

#define OTA_TASK_PRIORITY 6
#define OTA_TASK_CORE_ID tskNO_AFFINITY
// 1  // tskNO_AFFINITY

#define FLAC_DECODER_TASK_PRIORITY 7
#define FLAC_DECODER_TASK_CORE_ID tskNO_AFFINITY
#define FLAC_DECODER_TASK_STACK_SIZE (4 * 1024)  // Increased stack size

#define FLAC_TASK_PRIORITY 8
#define FLAC_TASK_CORE_ID tskNO_AFFINITY
#define FLAC_TASK_STACK_SIZE (4 * 1024)  // Increased stack size
#define FLAC_PENDING_TIMESTAMPS 16

#define OPUS_TASK_PRIORITY 8
#define OPUS_TASK_CORE_ID tskNO_AFFINITY
#define OPUS_TASK_STACK_SIZE (4 * 1024)  // Increased stack size

// 1  // tskNO_AFFINITY

TaskHandle_t t_ota_task = NULL;
TaskHandle_t t_http_get_task = NULL;
TaskHandle_t t_flac_decoder_task = NULL;
TaskHandle_t dec_task_handle = NULL;

#define FAST_SYNC_LATENCY_BUF 10000      // in µs
#define NORMAL_SYNC_LATENCY_BUF 1000000  // in µs

struct timeval tdif, tavg;
static audio_board_handle_t board_handle = NULL;

/* snapast parameters; configurable in menuconfig */
#define SNAPCAST_SERVER_USE_MDNS CONFIG_SNAPSERVER_USE_MDNS
#define SNAPCAST_SERVER_HOST CONFIG_SNAPSERVER_HOST
#define SNAPCAST_SERVER_PORT CONFIG_SNAPSERVER_PORT
#define SNAPCAST_CLIENT_NAME CONFIG_SNAPCLIENT_NAME
#define SNAPCAST_USE_SOFT_VOL CONFIG_SNAPCLIENT_USE_SOFT_VOL

/* Logging tag */
static const char *TAG = "SC";

// static QueueHandle_t playerChunkQueueHandle = NULL;
SemaphoreHandle_t timeSyncSemaphoreHandle = NULL;

#if CONFIG_USE_DSP_PROCESSOR
#if CONFIG_SNAPCLIENT_DSP_FLOW_STEREO
dspFlows_t dspFlow = dspfStereo;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASSBOOST
dspFlows_t dspFlow = dspfBassBoost;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BIAMP
dspFlows_t dspFlow = dspfBiamp;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASS_TREBLE_EQ
dspFlows_t dspFlow = dspfEQBassTreble;
#endif
#endif

struct decoderData_s {
  uint32_t type;  // should be SNAPCAST_MESSAGE_CODEC_HEADER
                  // or SNAPCAST_MESSAGE_WIRE_CHUNK
  uint8_t *inData;
  tv_t timestamp;
  pcm_chunk_message_t *outData;
  uint32_t bytes;
};

void time_sync_msg_cb(void *args);

static char base_message_serialized[BASE_MESSAGE_SIZE];
static const esp_timer_create_args_t tSyncArgs = {
    .callback = &time_sync_msg_cb,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "tSyncMsg",
    .skip_unhandled_events = false};

struct netconn *lwipNetconn = NULL; // Initialize to NULL

static int id_counter = 0;

static OpusDecoder *opusDecoder = NULL;

// Health Monitor Configuration
#define HEALTH_CHECK_INTERVAL 30000        // Periodic check interval (ms)

// "Soft" health thresholds used by health_monitor_check_connection()
#define HEALTH_NO_DATA_WARN_TIMEOUT_MS 30000
#define HEALTH_NO_STREAM_TIMEOUT_MS_OPUS 60000
#define HEALTH_NO_STREAM_TIMEOUT_MS_FLAC 120000

// Escalation policy
#define MAX_CONSECUTIVE_FAILURES 3
#define MAX_SOFT_RECOVERY_ATTEMPTS 3
#define MIN_RECOVERY_INTERVAL_MS 10000

#define REBOOT_DELAY 5000                  // 5 seconds before reboot for fast flashing

// LED modes (shared with player.c via led_manager)
#define LED_MODE_NORMAL      0                  // Normal: audio state drives LEDs
#define LED_MODE_SLOW_FLASH  1                  // Slow flash: disconnected / reconnecting
#define LED_MODE_FAST_FLASH  2                  // Fast flash: imminent reboot

// Forward declare LED mode helper (wrapper around led_manager health mode)
static void set_led_mode(int mode);

// Health Monitor Structure
typedef struct {
    int64_t last_health_check;
    // Updated on actual data receipt (not on loop iterations). Used as the "known-good" baseline.
    int64_t last_successful_connection;
    int64_t last_data_received;
    int64_t last_stream_data_received;  // Stream chunks only

    int64_t last_recovery_attempt;      // Debounce timestamp for recovery actions (ms)
    bool was_connected;
    int connection_failures;
    int recovery_attempts;              // Soft recovery attempts since last healthy state
    bool recovery_in_progress;
} health_monitor_t;



static health_monitor_t health_monitor = {0};
static const char *HEALTH_TAG = "HealthMonitor";

// Assume netconn_get_state exists for lwIP (adjust based on actual lwIP API)
typedef enum {
    NETCONN_STATE_CONNECTED,
    NETCONN_STATE_CLOSED,
    NETCONN_STATE_ERROR
} netconn_state_t;

// FIXED: Proper netconn_get_state implementation with actual state checking
netconn_state_t netconn_get_state(struct netconn *conn) {
    if (conn == NULL) {
        return NETCONN_STATE_CLOSED;
    }

    // Check if the underlying PCB exists (indicates connection is alive)
    if (conn->pcb.tcp == NULL) {
        return NETCONN_STATE_CLOSED;
    }

    // Check for errors
    err_t err = netconn_err(conn);
    if (err != ERR_OK) {
        return NETCONN_STATE_ERROR;
    }

    return NETCONN_STATE_CONNECTED;
}


static const char *lwip_err_to_str(err_t err) {
    switch (err) {
        case ERR_OK: return "No error";
        case ERR_MEM: return "Out of memory";
        case ERR_BUF: return "Buffer error";
        case ERR_TIMEOUT: return "Timeout";
        case ERR_RTE: return "Routing problem";
        case ERR_INPROGRESS: return "Operation in progress";
        case ERR_VAL: return "Illegal value";
        case ERR_WOULDBLOCK: return "Operation would block";
        case ERR_USE: return "Address in use";
        case ERR_ALREADY: return "Already connecting";
        case ERR_ISCONN: return "Conn already established";
        case ERR_CONN: return "Not connected";
        case ERR_IF: return "Low-level netif error";
        case ERR_ABRT: return "Connection aborted";
        case ERR_RST: return "Connection reset";
        case ERR_CLSD: return "Connection closed";
        case ERR_ARG: return "Illegal argument";
        default: return "Unknown error";
    }
}

static bool snapcast_transport_connected(void) {
#if !CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET && \
    !CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    if (!wifi_is_connected()) {
        return false;
    }
#endif

    return lwipNetconn != NULL &&
           netconn_get_state(lwipNetconn) == NETCONN_STATE_CONNECTED;
}

static bool resolve_snapserver_fallback(ip_addr_t *remote_ip,
                                        uint16_t *remote_port) {
    if (remote_ip == NULL || remote_port == NULL) {
        return false;
    }

    err_t dns_err = netconn_gethostbyname(SNAPCAST_SERVER_HOST, remote_ip);
    if (dns_err != ERR_OK) {
        ESP_LOGW(TAG, "Fallback host lookup failed for %s: %s",
                 SNAPCAST_SERVER_HOST, lwip_err_to_str(dns_err));
        return false;
    }

    *remote_port = SNAPCAST_SERVER_PORT;
    ESP_LOGI(TAG, "Using fallback SnapServer %s:%d (%s)",
             ipaddr_ntoa(remote_ip), *remote_port, SNAPCAST_SERVER_HOST);
    return true;
}

static void set_led_mode(int mode)
{
    switch (mode) {
        case LED_MODE_NORMAL:
            led_manager_set_health_mode(LED_HEALTH_NORMAL);
            break;
        case LED_MODE_SLOW_FLASH:
            led_manager_set_health_mode(LED_HEALTH_DISCONNECTED);
            break;
        case LED_MODE_FAST_FLASH:
            led_manager_set_health_mode(LED_HEALTH_REBOOTING);
            break;
        default:
            ESP_LOGW(TAG, "Unknown LED mode %d (forcing NORMAL)", mode);
            led_manager_set_health_mode(LED_HEALTH_NORMAL);
            break;
    }
}


/**
 * Cleanup decoder resources
 */
static void drain_decoder_queue(QueueHandle_t *queue_handle) {
    decoderData_t *queued_data = NULL;

    if (queue_handle == NULL || *queue_handle == NULL) {
        return;
    }

    while (xQueueReceive(*queue_handle, &queued_data, 0) == pdTRUE) {
        free_flac_data(queued_data);
        queued_data = NULL;
    }

    vQueueDelete(*queue_handle);
    *queue_handle = NULL;
}

void cleanup_decoder_resources(void) {
    if (t_flac_decoder_task != NULL) {
        vTaskDelete(t_flac_decoder_task);
        t_flac_decoder_task = NULL;
    }

    if (dec_task_handle != NULL) {
        vTaskDelete(dec_task_handle);
        dec_task_handle = NULL;
    }

    if (opusDecoder != NULL) {
        opus_decoder_destroy(opusDecoder);
        opusDecoder = NULL;
    }

    if (flacDecoder != NULL) {
        FLAC__stream_decoder_finish(flacDecoder);
        FLAC__stream_decoder_delete(flacDecoder);
        flacDecoder = NULL;
    }

    drain_decoder_queue(&decoderWriteQHdl);
    drain_decoder_queue(&decoderReadQHdl);
    drain_decoder_queue(&decoderTaskQHdl);

    if (decoderWriteSemaphore != NULL) {
        vSemaphoreDelete(decoderWriteSemaphore);
        decoderWriteSemaphore = NULL;
    }

    if (decoderReadSemaphore != NULL) {
        vSemaphoreDelete(decoderReadSemaphore);
        decoderReadSemaphore = NULL;
    }
}



// Health Monitor Functions

/**
 * Initialize the health monitor
 * This function sets up the initial state of the health monitor structure.
 * It records the current time for health checks, successful connections, and data received.
 * It also initializes connection status and failure counters.
 * Why: Ensures all timestamps start from the current moment to accurately track durations,
 * and resets counters to prevent carrying over state from previous runs or resets.
 */
static void health_monitor_init(void) {
    int64_t now = esp_timer_get_time() / 1000; // ms
    health_monitor.last_health_check = now;
    health_monitor.last_successful_connection = now;
    health_monitor.last_data_received = now;
    health_monitor.last_stream_data_received = now;
    health_monitor.last_recovery_attempt = 0;

    health_monitor.was_connected = false;
    health_monitor.connection_failures = 0;
    health_monitor.recovery_attempts = 0;
    health_monitor.recovery_in_progress = false;

    ESP_LOGI(HEALTH_TAG, "Health Monitor initialized");
}

/**
 * Update the last data received timestamp
 * This function is called whenever new data is received to refresh the timestamp.
 * Why: Tracks how recently data was received to detect stale connections where
 * the network link might be up but no actual data is flowing (e.g., server issues).
 */
static void health_monitor_data_received(bool is_stream) {
    int64_t now = esp_timer_get_time() / 1000; // ms

    health_monitor.last_data_received = now;
    health_monitor.last_successful_connection = now;

    if (is_stream) {
        health_monitor.last_stream_data_received = now;
        ESP_LOGD(HEALTH_TAG, "Stream data received");
    } else {
        ESP_LOGD(HEALTH_TAG, "Non-stream data received");
    }

    // Receiving any data is the strongest "healthy" signal we have; clear escalation counters.
    health_monitor.connection_failures = 0;
    health_monitor.recovery_attempts = 0;
}


/**
 * Check if the connection is healthy
 * This function verifies two things:
 * 1. If there's an active network connection (lwipNetconn state).
 * 2. If data has been received recently (within 30 seconds).
 * It logs warnings if either condition fails.
 * Why: Distinguishes between complete connection loss and a "hung" connection
 * where the socket is open but inactive, allowing targeted recovery actions.
 * Returns true if both conditions are satisfied, false otherwise.
 */
static bool health_monitor_check_connection(const snapcastSetting_t *scSet) {
    // Verify netconn state first (distinguishes a closed/broken socket from a "hung" socket).
    if (!snapcast_transport_connected()) {
        ESP_LOGW(HEALTH_TAG, "No active connection");
        return false;
    }

    int64_t now = esp_timer_get_time() / 1000; // ms

    // Any data timeout (covers "hung" connection where socket is open but inactive).
    int64_t no_data_ms = now - health_monitor.last_data_received;
    if (no_data_ms > HEALTH_NO_DATA_WARN_TIMEOUT_MS) {
        ESP_LOGW(HEALTH_TAG, "No data for %lld ms", (long long)no_data_ms);
        return false;
    }

    // Stream-specific timeout (covers cases where control traffic continues but audio stalls).
    int64_t stream_timeout_ms =
        (scSet != NULL && scSet->codec == OPUS) ? HEALTH_NO_STREAM_TIMEOUT_MS_OPUS
                                               : HEALTH_NO_STREAM_TIMEOUT_MS_FLAC;

    int64_t no_stream_ms = now - health_monitor.last_stream_data_received;
    if (no_stream_ms > stream_timeout_ms) {
        ESP_LOGW(HEALTH_TAG, "No stream data for %lld ms", (long long)no_stream_ms);
        return false;
    }

    return true;
}


/**
 * Attempt to recover from a connection failure
 * This function tries to softly recover the system without a full reboot.
 * It limits attempts to 3 to prevent infinite loops.
 * Steps:
 * 1. Reinitialize WiFi (if applicable).
 * 2. Restart decoder tasks if they exist.
 * 3. Clean up decoder resources.
 * 4. Close and delete the existing network connection.
 * After actions, it waits 2 seconds and checks if connection is restored.
 * Why: Allows graceful recovery from transient issues like network glitches
 * or task hangs, avoiding unnecessary reboots while logging progress for debugging.
 * Returns true if recovery succeeds, false otherwise.
 */
static bool health_monitor_attempt_recovery(snapcastSetting_t *scSet) {
    (void)scSet; // Currently recovery is transport-level; codec-specific actions can be added later.

    int64_t now = esp_timer_get_time() / 1000; // ms

    if (health_monitor.recovery_in_progress) {
        ESP_LOGW(HEALTH_TAG, "Recovery already in progress, skipping");
        return true;
    }

    if (health_monitor.recovery_attempts >= MAX_SOFT_RECOVERY_ATTEMPTS) {
        ESP_LOGE(HEALTH_TAG, "Max soft recovery attempts (%d) reached", MAX_SOFT_RECOVERY_ATTEMPTS);
        return false;
    }

    if (health_monitor.last_recovery_attempt != 0 &&
        (now - health_monitor.last_recovery_attempt) < MIN_RECOVERY_INTERVAL_MS) {
        ESP_LOGW(HEALTH_TAG, "Recovery attempt throttled (%lld ms since last attempt)",
                 (long long)(now - health_monitor.last_recovery_attempt));
        return true;
    }

    health_monitor.recovery_in_progress = true;
    health_monitor.last_recovery_attempt = now;
    health_monitor.recovery_attempts++;

    ESP_LOGW(HEALTH_TAG,
             "Triggering reconnect recovery (attempt %d/%d). no_data=%lld ms, no_stream=%lld ms",
             health_monitor.recovery_attempts, MAX_SOFT_RECOVERY_ATTEMPTS,
             (long long)(now - health_monitor.last_data_received),
             (long long)(now - health_monitor.last_stream_data_received));

    // IMPORTANT:
    // Do not delete tasks or free decoder resources here.
    // The normal reconnect path in the main loop already performs a safe cleanup/re-init.
    // We only need to force netconn_recv() to unblock and the loop to restart.
    if (lwipNetconn != NULL) {
        netconn_close(lwipNetconn);
    }

    // Recovery is "in motion" once the connection is closed; the main loop will handle re-connect.
    health_monitor.recovery_in_progress = false;
    health_monitor.connection_failures = 0;

    return true;
}

/**
 * Schedule a system reboot after a delay
 * This function is called when recovery fails critically.
 * It saves the last disconnect time to NVS for potential post-reboot analysis.
 * Then, it sets the LED to fast flash mode and delays for 5 seconds before rebooting.
 * Why: Provides a grace period before reboot, during which the fast flashing
 * indicates an imminent reboot to the user. Saving state helps in debugging persistent issues.
 * The LED flashing is handled by the background LED task during the delay.
 */
static void health_monitor_schedule_reboot(int64_t unhealthy_duration_ms) {
    ESP_LOGE(HEALTH_TAG,
             "Critical failure: reboot in %d seconds (unhealthy for %lld ms, failures=%d)",
             REBOOT_DELAY / 1000, (long long)unhealthy_duration_ms, health_monitor.connection_failures);

    // Persist minimal state for post-mortem/debugging.
    nvs_handle_t nvs_handle;
    if (nvs_open("health_monitor", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        int64_t now = esp_timer_get_time() / 1000;
        nvs_set_i64(nvs_handle, "last_unhealthy_ms", unhealthy_duration_ms);
        nvs_set_i64(nvs_handle, "last_disconnect_time", now);
        nvs_set_i32(nvs_handle, "last_fail_count", health_monitor.connection_failures);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(HEALTH_TAG, "Saved last failure details to NVS");
    } else {
        ESP_LOGE(HEALTH_TAG, "Failed to open NVS for state preservation");
    }

    // Set LED to fast flash mode
    set_led_mode(LED_MODE_FAST_FLASH);

    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY));
    ESP_LOGI(HEALTH_TAG, "Rebooting system...");
    esp_restart();
}

/**
 * Update the health monitor state
 * This function is called periodically or on status changes.
 * It handles timer overflows, updates connection status, resets failure counters on connection,
 * and performs periodic health checks every HEALTH_CHECK_INTERVAL.
 * During checks, if disconnected for too long, it increments failures and triggers recovery/reboot if needed.
 * It also updates the LED mode: normal when connected, slow flash when disconnected.
 * Why: Central function for monitoring; ensures timely detection of issues,
 * handles state transitions, and integrates visual feedback via LED for connection status.
 * The periodic check prevents indefinite hanging in bad states.
 */
static void health_monitor_update(bool is_connected, snapcastSetting_t *scSet) {
    int64_t now = esp_timer_get_time() / 1000; // ms

    // Handle timer overflow (defensive; esp_timer is int64, but keep the guard).
    if (now < health_monitor.last_health_check) {
        ESP_LOGW(HEALTH_TAG, "Timer anomaly detected, resetting timestamps");
        health_monitor.last_health_check = now;
        health_monitor.last_successful_connection = now;
        health_monitor.last_data_received = now;
        health_monitor.last_stream_data_received = now;
        health_monitor.last_recovery_attempt = 0;
        health_monitor.connection_failures = 0;
        health_monitor.recovery_attempts = 0;
        health_monitor.recovery_in_progress = false;
        return;
    }

    // Connection state transitions (keep LED updates event-driven; avoid per-loop churn).
    if (is_connected) {
        if (!health_monitor.was_connected) {
            ESP_LOGI(HEALTH_TAG, "Connection established");
            health_monitor.was_connected = true;

            // Provide a grace baseline after a reconnect; avoids false positives from old timestamps.
            health_monitor.last_successful_connection = now;
            health_monitor.last_data_received = now;
            health_monitor.last_stream_data_received = now;

            health_monitor.connection_failures = 0;
            health_monitor.recovery_attempts = 0;
            health_monitor.recovery_in_progress = false;

            set_led_mode(LED_MODE_NORMAL);
        }
    } else {
        if (health_monitor.was_connected) {
            ESP_LOGW(HEALTH_TAG, "Connection lost");
            set_led_mode(LED_MODE_SLOW_FLASH);
        }
        health_monitor.was_connected = false;
    }

    // Only do deeper health checks when we believe we're connected.
    if (!is_connected) {
        return;
    }

    // Periodic health check
    if ((now - health_monitor.last_health_check) < HEALTH_CHECK_INTERVAL) {
        return;
    }
    health_monitor.last_health_check = now;

    bool healthy = health_monitor_check_connection(scSet);
    if (healthy) {
        // Clear escalation state; data_receive() will also do this, but keep it here for completeness.
        health_monitor.connection_failures = 0;
        health_monitor.recovery_attempts = 0;
        return;
    }

    int64_t no_data_ms = now - health_monitor.last_data_received;
    int64_t no_stream_ms = now - health_monitor.last_stream_data_received;

    int64_t max_disconnect_ms = (scSet != NULL && scSet->codec == OPUS) ? 60000 : 120000;
    int64_t max_stream_ms = max_disconnect_ms;

    bool beyond_grace = (no_data_ms > max_disconnect_ms) || (no_stream_ms > max_stream_ms);
    if (!beyond_grace) {
        // Still within codec-dependent grace; warnings are already logged by health_monitor_check_connection().
        return;
    }

    health_monitor.connection_failures++;
    netconn_state_t state = (lwipNetconn != NULL) ? netconn_get_state(lwipNetconn) : NETCONN_STATE_CLOSED;

    ESP_LOGE(HEALTH_TAG,
             "Health failure %d/%d (no_data=%lld ms, no_stream=%lld ms, netconn_state=%d)",
             health_monitor.connection_failures, MAX_CONSECUTIVE_FAILURES,
             (long long)no_data_ms, (long long)no_stream_ms, (int)state);

    if (health_monitor.connection_failures >= MAX_CONSECUTIVE_FAILURES) {
        int64_t unhealthy_ms = (no_data_ms > no_stream_ms) ? no_data_ms : no_stream_ms;

        // Attempt recovery first. If we are out of soft recovery attempts, schedule a reboot.
        if (!health_monitor_attempt_recovery(scSet)) {
            health_monitor_schedule_reboot(unhealthy_ms);
        }

        // Avoid repeated immediate triggers; next escalation requires fresh failures.
        health_monitor.connection_failures = 0;
    }
}

/**
 *
 */
void time_sync_msg_cb(void *args) {
  base_message_t base_message_tx;
  int64_t now;
  int rc1;

  uint8_t *p_pkt = (uint8_t *)malloc(BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);
  if (p_pkt == NULL) {
    ESP_LOGW(
        TAG,
        "%s: Failed to get memory for time sync message. Skipping this round.",
        __func__);

    return;
  }

  memset(p_pkt, 0, BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);

  base_message_tx.type = SNAPCAST_MESSAGE_TIME;
  base_message_tx.id = id_counter++;
  base_message_tx.refersTo = 0;
  base_message_tx.received.sec = 0;
  base_message_tx.received.usec = 0;
  now = esp_timer_get_time();
  base_message_tx.sent.sec = now / 1000000;
  base_message_tx.sent.usec = now - base_message_tx.sent.sec * 1000000;
  base_message_tx.size = TIME_MESSAGE_SIZE;
  rc1 = base_message_serialize(&base_message_tx, (char *)&p_pkt[0],
                               BASE_MESSAGE_SIZE);
  if (rc1) {
    ESP_LOGE(TAG, "Failed to serialize base message for time");

    return;
  }

  rc1 = netconn_write(lwipNetconn, p_pkt, BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE,
                      NETCONN_NOCOPY);
  if (rc1 != ERR_OK) {
    ESP_LOGW(TAG, "error writing timesync msg");

    return;
  }

  free(p_pkt);
}

/**
 *
 */
void free_flac_data(decoderData_t *pFlacData) {
  if (pFlacData == NULL) return;

  if (pFlacData->inData) {
    free(pFlacData->inData);
    pFlacData->inData = NULL;
  }

  if (pFlacData->outData) {
    free_pcm_chunk(pFlacData->outData);
    pFlacData->outData = NULL;
  }

  free(pFlacData);
}

static bool flac_timestamp_fifo_push(flac_timestamp_fifo_t *fifo,
                                     const tv_t *timestamp) {
  size_t tail;

  if ((fifo == NULL) || (timestamp == NULL)) {
    return false;
  }

  if (fifo->count >= FLAC_PENDING_TIMESTAMPS) {
    return false;
  }

  tail = (fifo->head + fifo->count) % FLAC_PENDING_TIMESTAMPS;
  fifo->items[tail] = *timestamp;
  fifo->count++;

  return true;
}

static bool flac_timestamp_fifo_pop(flac_timestamp_fifo_t *fifo,
                                    tv_t *timestamp) {
  if ((fifo == NULL) || (timestamp == NULL) || (fifo->count == 0)) {
    return false;
  }

  *timestamp = fifo->items[fifo->head];
  fifo->head = (fifo->head + 1U) % FLAC_PENDING_TIMESTAMPS;
  fifo->count--;

  return true;
}

static inline size_t snapcast_pcm_bytes_per_sample(i2s_data_bit_width_t bits) {
  return ((size_t)bits + 7U) / 8U;
}

static inline size_t snapcast_pcm_bytes_per_frame(uint8_t channels,
                                                  i2s_data_bit_width_t bits) {
  return (size_t)channels * snapcast_pcm_bytes_per_sample(bits);
}

typedef struct {
  pcm_chunk_fragment_t *fragment;
  size_t offset;
} pcm_fragment_writer_t;

static bool pcm_fragment_writer_write(pcm_fragment_writer_t *writer,
                                      const uint8_t *data, size_t len) {
  if ((writer == NULL) || (data == NULL)) {
    return false;
  }

  while (len > 0) {
    size_t remaining = 0;
    size_t chunk = 0;

    if ((writer->fragment == NULL) || (writer->fragment->payload == NULL)) {
      return false;
    }

    if (writer->offset >= writer->fragment->size) {
      writer->fragment = writer->fragment->nextFragment;
      writer->offset = 0;
      continue;
    }

    remaining = writer->fragment->size - writer->offset;
    chunk = (len < remaining) ? len : remaining;

    memcpy(writer->fragment->payload + writer->offset, data, chunk);
    writer->offset += chunk;
    data += chunk;
    len -= chunk;
  }

  return true;
}

static bool snapcast_pack_flac_frame(uint8_t *dst, size_t dst_size,
                                     const FLAC__int32 *const buffer[],
                                     uint8_t channels,
                                     i2s_data_bit_width_t bits,
                                     size_t sample_index) {
  size_t sample_bytes = snapcast_pcm_bytes_per_sample(bits);
  size_t frame_bytes = snapcast_pcm_bytes_per_frame(channels, bits);
  size_t offset = 0;

  if ((dst == NULL) || (buffer == NULL) || (dst_size < frame_bytes)) {
    return false;
  }

  for (size_t channel = 0; channel < channels; ++channel) {
    size_t source_channel = (size_t)channels - 1U - channel;
    uint32_t sample = (uint32_t)buffer[source_channel][sample_index];

    for (size_t byte = 0; byte < sample_bytes; ++byte) {
      dst[offset++] = (uint8_t)((sample >> (8U * byte)) & 0xFFU);
    }
  }

  return true;
}

static bool snapcast_pack_pcm_frame(uint8_t *dst, size_t dst_size,
                                    const uint8_t *src, uint8_t channels,
                                    i2s_data_bit_width_t bits) {
  size_t sample_bytes = snapcast_pcm_bytes_per_sample(bits);
  size_t frame_bytes = snapcast_pcm_bytes_per_frame(channels, bits);
  size_t offset = 0;

  if ((dst == NULL) || (src == NULL) || (dst_size < frame_bytes)) {
    return false;
  }

  for (size_t channel = 0; channel < channels; ++channel) {
    size_t source_channel = (size_t)channels - 1U - channel;

    memcpy(dst + offset, src + (source_channel * sample_bytes), sample_bytes);
    offset += sample_bytes;
  }

  return true;
}

/**
 *
 */
static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
    void *client_data) {
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;
  decoderData_t *flacData;

  (void)scSet;

  if (xQueueReceive(decoderReadQHdl, &flacData, portMAX_DELAY) != pdTRUE) {
    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  }

  if (flacData == NULL || flacData->bytes <= 0) {
    if (flacData) free_flac_data(flacData);
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }

  if (flacData->inData == NULL) {
    free_flac_data(flacData);
    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  }

  if (flacData->bytes <= *bytes) {
    memcpy(buffer, flacData->inData, flacData->bytes);
    *bytes = flacData->bytes;
    free_flac_data(flacData);
  } else {
    size_t consumed = *bytes;

    memcpy(buffer, flacData->inData, consumed);
    memmove(flacData->inData, flacData->inData + consumed,
            flacData->bytes - consumed);
    flacData->bytes -= consumed;

    if (xQueueSend(decoderReadQHdl, &flacData, portMAX_DELAY) != pdTRUE) {
      free_flac_data(flacData);
      return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
  }

  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

/**
 *
 */
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data) {
  size_t i;
  decoderData_t *flacData = NULL;
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;
  int ret = 0;
  size_t frame_bytes =
      snapcast_pcm_bytes_per_frame(frame->header.channels,
                                   frame->header.bits_per_sample);

  (void)decoder;

  if (frame->header.channels != scSet->ch) {
    ESP_LOGE(TAG,
             "ERROR: frame header reports different channel count %ld than "
             "previous metadata block %d",
             frame->header.channels, scSet->ch);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (frame->header.bits_per_sample != scSet->bits) {
    ESP_LOGE(TAG,
             "ERROR: frame header reports different bps %ld than previous "
             "metadata block %d",
             frame->header.bits_per_sample, scSet->bits);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (buffer[0] == NULL) {
    ESP_LOGE(TAG, "ERROR: buffer [0] is NULL\n");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (buffer[1] == NULL) {
    ESP_LOGE(TAG, "ERROR: buffer [1] is NULL\n");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  flacData = (decoderData_t *)malloc(sizeof(decoderData_t));
  if (flacData == NULL) {
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  memset(flacData, 0, sizeof(decoderData_t));

  flacData->bytes = frame->header.blocksize * frame_bytes;

  ret = allocate_pcm_chunk_memory(&(flacData->outData), flacData->bytes);

  if (ret == 0) {
    pcm_chunk_fragment_t *fragment = flacData->outData->fragment;

    if (fragment->payload != NULL) {
      pcm_fragment_writer_t writer = {
          .fragment = fragment,
          .offset = 0,
      };
      uint8_t packed_frame[32];

      if (frame_bytes > sizeof(packed_frame)) {
        ESP_LOGE(TAG, "Unsupported FLAC frame size %u", (unsigned)frame_bytes);
        free_flac_data(flacData);
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
      }

      for (i = 0; i < frame->header.blocksize; i++) {
        if (!snapcast_pack_flac_frame(packed_frame, sizeof(packed_frame),
                                      buffer, frame->header.channels,
                                      frame->header.bits_per_sample, i) ||
            !pcm_fragment_writer_write(&writer, packed_frame, frame_bytes)) {
          ESP_LOGE(TAG, "Failed to pack FLAC frame for %d-bit output",
                   (int)frame->header.bits_per_sample);
          free_flac_data(flacData);
          return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
      }
    }
  }

  if (xQueueSend(decoderWriteQHdl, &flacData, portMAX_DELAY) != pdTRUE) {
      free_flac_data(flacData);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/**
 *
 */
void metadata_callback(const FLAC__StreamDecoder *decoder,
                       const FLAC__StreamMetadata *metadata,
                       void *client_data) {
  decoderData_t *flacData;
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;

  (void)decoder;

  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
    flacData = (decoderData_t *)malloc(sizeof(decoderData_t));
    if (flacData == NULL) {
      ESP_LOGE(TAG, "in flac meta cb, malloc failed");
      return;
    }

    memset(flacData, 0, sizeof(decoderData_t));

    scSet->sr = metadata->data.stream_info.sample_rate;
    scSet->ch = metadata->data.stream_info.channels;
    scSet->bits = metadata->data.stream_info.bits_per_sample;

    ESP_LOGI(TAG, "fLaC sampleformat: %ld:%d:%d", scSet->sr, scSet->bits,
             scSet->ch);

    if (xQueueSend(decoderWriteQHdl, &flacData, portMAX_DELAY) != pdTRUE) {
        free(flacData);
    }
  }
}

/**
 *
 */
void error_callback(const FLAC__StreamDecoder *decoder,
                    FLAC__StreamDecoderErrorStatus status, void *client_data) {
  (void)decoder, (void)client_data;

  ESP_LOGE(TAG, "Got error callback: %s\n",
           FLAC__StreamDecoderErrorStatusString[status]);
}

static void flac_decoder_task(void *pvParameters) {
  FLAC__StreamDecoderInitStatus init_status;
  snapcastSetting_t *scSet = (snapcastSetting_t *)pvParameters;
  bool should_run = true;

  if (flacDecoder != NULL) {
    FLAC__stream_decoder_finish(flacDecoder);
    FLAC__stream_decoder_delete(flacDecoder);
    flacDecoder = NULL;
  }

  flacDecoder = FLAC__stream_decoder_new();
  if (flacDecoder == NULL) {
    ESP_LOGE(TAG, "Failed to init flac decoder");
    should_run = false;
  }

  if (should_run) {
    init_status = FLAC__stream_decoder_init_stream(
        flacDecoder, read_callback, NULL, NULL, NULL, NULL, write_callback,
        metadata_callback, error_callback, scSet);
    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      ESP_LOGE(TAG, "ERROR: initializing decoder: %s\n",
               FLAC__StreamDecoderInitStatusString[init_status]);
      should_run = false;
    }
  }

  while (should_run) {
    FLAC__bool result = FLAC__stream_decoder_process_until_end_of_stream(flacDecoder);
    if (!result) {
      FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(flacDecoder);
      if (state == FLAC__STREAM_DECODER_END_OF_STREAM ||
          state == FLAC__STREAM_DECODER_ABORTED) {
        break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  if (flacDecoder != NULL) {
    FLAC__stream_decoder_finish(flacDecoder);
    FLAC__stream_decoder_delete(flacDecoder);
    flacDecoder = NULL;
  }

  ESP_LOGI(TAG, "flac_decoder_task exiting");
  vTaskDelete(NULL);
}

static void flac_handle_decoder_output(decoderData_t *pFlacData,
                                       snapcastSetting_t *scSet,
                                       flac_timestamp_fifo_t *timestamps) {
  pcm_chunk_message_t *pcmData = NULL;
  tv_t currentTimestamp = {0};

  if (pFlacData == NULL) {
    return;
  }

  if (pFlacData->outData == NULL) {
    if (timestamps != NULL && timestamps->count > 0) {
      (void)flac_timestamp_fifo_pop(timestamps, &currentTimestamp);
    }
    free_flac_data(pFlacData);
    return;
  }

  if ((timestamps == NULL) ||
      !flac_timestamp_fifo_pop(timestamps, &currentTimestamp)) {
    ESP_LOGW(TAG, "FLAC output arrived without a matching pending timestamp");
    free_flac_data(pFlacData);
    return;
  }

  pcmData = pFlacData->outData;
  pcmData->timestamp = currentTimestamp;

  {
    size_t decodedSize = pcmData->totalSize;

    scSet->chkInFrames =
        decodedSize / ((size_t)scSet->ch * (size_t)(scSet->bits / 8));
  }

  if (player_send_snapcast_setting(scSet) != pdPASS) {
    ESP_LOGE(TAG,
             "Failed to notify sync task about codec. Did you init player?");
    free_flac_data(pFlacData);
    return;
  }

#if CONFIG_USE_DSP_PROCESSOR
  if (pcmData->fragment != NULL && pcmData->fragment->payload != NULL) {
    dsp_processor_worker(pcmData->fragment->payload, pcmData->fragment->size,
                         scSet->sr, scSet->bits);
  }
#endif

  insert_pcm_chunk(pcmData);

  if (pFlacData->inData) {
    free(pFlacData->inData);
    pFlacData->inData = NULL;
  }
  pFlacData->outData = NULL;
  free(pFlacData);
}

static void flac_drain_decoder_output(snapcastSetting_t *scSet,
                                      flac_timestamp_fifo_t *timestamps,
                                      TickType_t wait) {
  decoderData_t *pFlacData = NULL;

  while (xQueueReceive(decoderWriteQHdl, &pFlacData, wait) == pdTRUE) {
    flac_handle_decoder_output(pFlacData, scSet, timestamps);
    pFlacData = NULL;
    wait = 0;
  }
}

/**
 *
 */
void flac_task(void *pvParameters) {
  tv_t currentTimestamp = {0};
  decoderData_t *pFlacData = NULL;
  snapcastSetting_t *scSet = (snapcastSetting_t *)pvParameters;
  flac_timestamp_fifo_t pendingTimestamps = {0};

  while (1) {
    if (xQueueReceive(decoderTaskQHdl, &pFlacData, pdMS_TO_TICKS(20)) ==
        pdTRUE) {
      if (pFlacData != NULL) {
        currentTimestamp = pFlacData->timestamp;

        if (xQueueSend(decoderReadQHdl, &pFlacData, portMAX_DELAY) != pdTRUE) {
          free_flac_data(pFlacData);
        }
      } else if (!flac_timestamp_fifo_push(&pendingTimestamps,
                                           &currentTimestamp)) {
        tv_t droppedTimestamp;

        ESP_LOGW(TAG, "FLAC timestamp queue full, dropping oldest pending item");
        if (flac_timestamp_fifo_pop(&pendingTimestamps, &droppedTimestamp)) {
          (void)flac_timestamp_fifo_push(&pendingTimestamps, &currentTimestamp);
        }
      }
    }

    flac_drain_decoder_output(
        scSet, &pendingTimestamps,
        (pendingTimestamps.count > 0) ? pdMS_TO_TICKS(20) : 0);
  }
}

/**
 *
 */
void opus_decoder_task(void *pvParameters) {
  tv_t currentTimestamp;
  decoderData_t *pOpusData = NULL;
  snapcastSetting_t *scSet = (snapcastSetting_t *)pvParameters;

  while (1) {
    if (xQueueReceive(decoderTaskQHdl, &pOpusData, portMAX_DELAY) != pdTRUE) {
        continue;
    }

    if (pOpusData) {
      currentTimestamp = pOpusData->timestamp;

      if (pOpusData->inData) {
        int frame_size = 0;
        int samples_per_frame = 0;
        opus_int16 *audio = NULL;

        samples_per_frame =
            opus_packet_get_samples_per_frame(pOpusData->inData, scSet->sr);
        if (samples_per_frame < 0) {
          ESP_LOGE(TAG,
                   "couldn't get samples per frame count "
                   "of packet");
          free(pOpusData->inData);
          free(pOpusData);
          continue;
        }

        scSet->chkInFrames = samples_per_frame;

        size_t bytes = samples_per_frame * scSet->ch * scSet->bits / 8;

        if (samples_per_frame > 480) {
          ESP_LOGE(TAG, "samples_per_frame: %d, pOpusData->bytes %ld, bytes %u",
                   samples_per_frame, pOpusData->bytes, bytes);
        }

        int retry_count = 0;
        while ((audio = (opus_int16 *)malloc(bytes)) == NULL && retry_count < 10) {
          ESP_LOGE(TAG, "couldn't get memory for audio, retry %d", retry_count);
          vTaskDelay(pdMS_TO_TICKS(1));
          retry_count++;
        }

        if (audio == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for audio after %d retries", retry_count);
            free(pOpusData->inData);
            free(pOpusData);
            continue;
        }

        frame_size =
            opus_decode(opusDecoder, pOpusData->inData, pOpusData->bytes,
                        (opus_int16 *)audio, samples_per_frame, 0);

        free(pOpusData->inData);
        pOpusData->inData = NULL;

        if (frame_size < 0) {
          ESP_LOGE(TAG, "Decode error : %d \n", frame_size);
          free(audio);
          free(pOpusData);
        } else {
          pcm_chunk_message_t *pcmData = NULL;

          bytes = frame_size * scSet->ch * scSet->bits / 8;
          if (allocate_pcm_chunk_memory(&pcmData, bytes) < 0) {
            pcmData = NULL;
            ESP_LOGE(TAG, "Failed to allocate PCM chunk memory");
          } else {
            pcmData->timestamp = currentTimestamp;

            if (pcmData->fragment->payload) {
              volatile uint32_t *sample;
              uint32_t tmpData;
              uint32_t cnt = 0;

              for (int i = 0; i < bytes; i += 4) {
                sample =
                    (volatile uint32_t *)(&(pcmData->fragment->payload[i]));
                tmpData = (((uint32_t)audio[cnt] << 16) & 0xFFFF0000) |
                          (((uint32_t)audio[cnt + 1] << 0) & 0x0000FFFF);
                *sample = (volatile uint32_t)tmpData;

                cnt += 2;
              }
            }

            free(audio);
            audio = NULL;
          }

          if (player_send_snapcast_setting(scSet) != pdPASS) {
            ESP_LOGE(TAG,
                     "Failed to notify "
                     "sync task about "
                     "codec. Did you "
                     "init player?");

            if (pcmData) {
                if (pcmData->fragment) {
                    if (pcmData->fragment->payload) free(pcmData->fragment->payload);
                    free(pcmData->fragment);
                }
                free(pcmData);
            }
            free(pOpusData);
            continue;
          }

#if CONFIG_USE_DSP_PROCESSOR
          if (pcmData && pcmData->fragment && pcmData->fragment->payload) {
            dsp_processor_worker(pcmData->fragment->payload,
                                 pcmData->fragment->size, scSet->sr,
                                 scSet->bits);
          }
#endif

          if (pcmData) {
            insert_pcm_chunk(pcmData);
          }
        }
      }

      free(pOpusData);
      pOpusData = NULL;
    }
  }
}

/**
 *
 */
esp_err_t audio_set_mute(bool mute) {
  if (!board_handle) {
    ESP_LOGW(TAG, "audio board not initialized yet");

    return ESP_OK;
  } else {
    return audio_hal_set_mute(board_handle->audio_hal, mute);
  }
}

/**
 *
 */
static void http_get_task(void *pvParameters) {
  char *start;
  base_message_t base_message_rx;
  hello_message_t hello_message;
  wire_chunk_message_t wire_chnk = {{0, 0}, 0, NULL};
  char *hello_message_serialized = NULL;
  int result;
  int64_t now, trx, tdif, ttx;
  time_message_t time_message_rx = {{0, 0}};
  int64_t tmpDiffToServer;
  int64_t lastTimeSync = 0;
  esp_timer_handle_t timeSyncMessageTimer = NULL;
  esp_err_t err = 0;
  server_settings_message_t server_settings_message;
  bool received_header = false;
  mdns_result_t *r;
  codec_type_t codec = NONE;
  snapcastSetting_t scSet;
  decoderData_t *pDecData = NULL;
  pcm_chunk_message_t *pcmData = NULL;
  uint8_t *opusData = NULL;
  ip_addr_t remote_ip;
  uint16_t remotePort = 0;
  int rc1 = ERR_OK, rc2 = ERR_OK;
  struct netbuf *firstNetBuf = NULL;
  struct netbuf *newNetBuf = NULL;
  uint16_t len;
  uint64_t timeout = FAST_SYNC_LATENCY_BUF;
#if !CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET && !CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  uint32_t wifi_disconnect_count_at_connect = 0;
#endif

  // Additional health check for SnapServer reachability
  const int MAX_CONNECT_ATTEMPTS = 3;
  int connect_attempts = 0;

  // Initialize health monitor
  health_monitor_init();

  // create a timer to send time sync messages every x µs
  esp_timer_create(&tSyncArgs, &timeSyncMessageTimer);

  while (1) {
    received_header = false;

    // Update health monitor - not connected
    health_monitor_update(false, &scSet);

    if (reset_latency_buffer() < 0) {
      ESP_LOGE(TAG,
               "reset_diff_buffer: couldn't reset median filter long. STOP");
      return;
    }

    timeout = FAST_SYNC_LATENCY_BUF;

    esp_timer_stop(timeSyncMessageTimer);

    // Clean up decoder resources before reconnecting
    cleanup_decoder_resources();

#if !CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET && !CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    esp_err_t wifi_status = wifi_wait_for_connection(pdMS_TO_TICKS(15000));
    if (wifi_status != ESP_OK) {
      ESP_LOGW(TAG, "WiFi not ready for SnapServer reconnect (err=%d)",
               wifi_status);
      if (wifi_status == ESP_FAIL) {
        esp_err_t reconnect_err = wifi_reconnect();
        if (reconnect_err != ESP_OK) {
          ESP_LOGW(TAG, "Failed to request WiFi reconnect (err=%d)",
                   reconnect_err);
        }
      }
      set_led_mode(LED_MODE_SLOW_FLASH);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
#endif

#if SNAPCAST_SERVER_USE_MDNS
    int mdns_attempts = 0;
    net_mdns_register(SNAPCAST_CLIENT_NAME);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Find snapcast server
    // Connect to first snapcast server found
    r = NULL;
    err = 0;
    while ((!r || err) && mdns_attempts < 3) {
      mdns_attempts++;
      ESP_LOGI(TAG, "Lookup snapcast service on network");
      err = mdns_query_ptr("_snapcast", "_tcp", 3000, 20, &r);
      if (err) {
        ESP_LOGE(TAG, "Query Failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_INVALID_STATE) {
          net_mdns_register(SNAPCAST_CLIENT_NAME);
          vTaskDelay(pdMS_TO_TICKS(500));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
      }

      if (!r) {
        ESP_LOGW(TAG, "No results found!");
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    if (r && r->addr) {
      mdns_ip_addr_t *a = r->addr;
      ip_addr_copy(remote_ip, (a->addr));
      remote_ip.type = a->addr.type;
      remotePort = r->port;
      ESP_LOGI(TAG, "Found %s:%d", ipaddr_ntoa(&remote_ip), remotePort);

      mdns_query_results_free(r);
    } else {
      if (r) {
        mdns_query_results_free(r);
      }

      ESP_LOGW(TAG, "mDNS discovery failed, trying configured fallback");
      if (!resolve_snapserver_fallback(&remote_ip, &remotePort)) {
        continue;
      }
    }
#else
    // configure a failsafe snapserver according to CONFIG values
    if (!resolve_snapserver_fallback(&remote_ip, &remotePort)) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
#endif

    if (lwipNetconn != NULL) {
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;
    }

    lwipNetconn = netconn_new(NETCONN_TCP);
    if (lwipNetconn == NULL) {
      ESP_LOGE(TAG, "can't create netconn");

      continue;
    }

    rc1 = netconn_bind(lwipNetconn, IPADDR_ANY, 0);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "can't bind local IP");
    }

    rc2 = netconn_connect(lwipNetconn, &remote_ip, remotePort);
    if (rc2 != ERR_OK) {
      ESP_LOGE(TAG, "can't connect to remote %s:%d, err %d",
               ipaddr_ntoa(&remote_ip), remotePort, rc2);
      connect_attempts++;
      set_led_mode(LED_MODE_SLOW_FLASH); // Start slow flashing on connection failure
      if (connect_attempts >= MAX_CONNECT_ATTEMPTS) {
        ESP_LOGE(TAG, "Failed to connect to SnapServer after %d attempts, rebooting...", MAX_CONNECT_ATTEMPTS);
        set_led_mode(LED_MODE_FAST_FLASH); // Switch to fast flashing before reboot
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
      }
      ESP_LOGW(TAG, "Connect attempt failed (%d/%d), retrying in 10 seconds...", connect_attempts, MAX_CONNECT_ATTEMPTS);
#if !CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET && !CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
      if (!wifi_is_connected()) {
        esp_err_t reconnect_err = wifi_reconnect();
        if (reconnect_err != ESP_OK) {
          ESP_LOGW(TAG, "Failed to request WiFi reconnect (err=%d)",
                   reconnect_err);
        }
      }
#endif
      vTaskDelay(pdMS_TO_TICKS(10000));
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;
      continue;
    } else {
      connect_attempts = 0; // Reset attempts on successful connection
      set_led_mode(LED_MODE_NORMAL); // Stop flashing on successful connection
    }

    if (rc1 != ERR_OK) {
      netconn_close(lwipNetconn);
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;

      continue;
    }

    // Keep the receive timeout short enough to react to transient WiFi flaps.
    netconn_set_recvtimeout(lwipNetconn, 3000);

    ESP_LOGI(TAG, "netconn connected");

    // Update health monitor - connected
    health_monitor_update(snapcast_transport_connected(), &scSet);
#if !CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET && !CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    wifi_disconnect_count_at_connect = wifi_get_disconnect_count();
#endif
    char mac_address[18];
    uint8_t base_mac[6];
    // Get MAC address for WiFi station
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    esp_read_mac(base_mac, ESP_MAC_ETH);
#else
    esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
#endif
    sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
            base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);

    now = esp_timer_get_time();

    // init base message
    base_message_rx.type = SNAPCAST_MESSAGE_HELLO;
    base_message_rx.id = 0x0000;
    base_message_rx.refersTo = 0x0000;
    base_message_rx.sent.sec = now / 1000000;
    base_message_rx.sent.usec = now - base_message_rx.sent.sec * 1000000;
    base_message_rx.received.sec = 0;
    base_message_rx.received.usec = 0;
    base_message_rx.size = 0x00000000;

    // init hello message
    hello_message.mac = mac_address;
    hello_message.hostname = SNAPCAST_CLIENT_NAME;
    hello_message.version = (char *)VERSION_STRING;
    hello_message.client_name = "libsnapcast";
    hello_message.os = "esp32";
    hello_message.arch = "xtensa";
    hello_message.instance = 1;
    hello_message.id = mac_address;
    hello_message.protocol_version = 2;

    if (hello_message_serialized == NULL) {
      hello_message_serialized = hello_message_serialize(
          &hello_message, (size_t *)&(base_message_rx.size));
      if (!hello_message_serialized) {
        ESP_LOGE(TAG, "Failed to serialize hello message");

        return;
      }
    }

    result = base_message_serialize(&base_message_rx, base_message_serialized,
                                    BASE_MESSAGE_SIZE);
    if (result) {
      ESP_LOGE(TAG, "Failed to serialize base message");

      return;
    }

    rc1 = netconn_write(lwipNetconn, base_message_serialized, BASE_MESSAGE_SIZE,
                        NETCONN_NOCOPY);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "netconn failed to send base message: %s", lwip_err_to_str(rc1));
      free(hello_message_serialized);
      hello_message_serialized = NULL;

      continue;
    }
    rc1 = netconn_write(lwipNetconn, hello_message_serialized,
                        base_message_rx.size, NETCONN_NOCOPY);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "netconn failed to send hello message: %s", lwip_err_to_str(rc1));
      free(hello_message_serialized);
      hello_message_serialized = NULL;

      continue;
    }

    ESP_LOGI(TAG, "netconn sent hello message");

    free(hello_message_serialized);
    hello_message_serialized = NULL;

    // init default setting
    scSet.buf_ms = 0;
    scSet.codec = NONE;
    scSet.bits = 0;
    scSet.ch = 0;
    scSet.sr = 0;
    scSet.chkInFrames = 0;
    scSet.volume = 0;
    scSet.muted = true;

    char *p_tmp = NULL;
    size_t currentPos = 0;
    size_t typedMsgCurrentPos = 0;
    uint32_t typedMsgLen = 0;
    uint32_t offset = 0;
    uint32_t payloadOffset = 0;
    uint8_t pcmFrameBuffer[32] = {0};
    size_t pcmFrameBufferFill = 0;

#define BASE_MESSAGE_STATE 0
#define TYPED_MESSAGE_STATE 1

    // 0 ... base message, 1 ... typed message
    uint32_t state = BASE_MESSAGE_STATE;
    uint32_t internalState = 0;

    firstNetBuf = NULL;

#define TEST_DECODER_TASK 1

    // Create semaphores and queues with proper error checking
    decoderWriteSemaphore = xSemaphoreCreateMutex();
    if (decoderWriteSemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create decoderWriteSemaphore");
        continue;
    }
    xSemaphoreTake(decoderWriteSemaphore, portMAX_DELAY);

    decoderReadSemaphore = xSemaphoreCreateMutex();
    if (decoderReadSemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create decoderReadSemaphore");
        vSemaphoreDelete(decoderWriteSemaphore);
        continue;
    }
    xSemaphoreGive(decoderReadSemaphore);

    while (1) {
      // Update health monitor - connected
      health_monitor_update(snapcast_transport_connected(), &scSet);

      rc2 = netconn_recv(lwipNetconn, &firstNetBuf);
      if (rc2 != ERR_OK) {
        if (rc2 == ERR_TIMEOUT) {
#if !CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET && !CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
          if (wifi_get_disconnect_count() != wifi_disconnect_count_at_connect) {
            ESP_LOGW(TAG, "WiFi link bounced; reconnecting SnapServer socket");
            netconn_close(lwipNetconn);
            break;
          }
          if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "WiFi link dropped while waiting for SnapServer data");
            netconn_close(lwipNetconn);
            break;
          }
#endif
          ESP_LOGD(TAG, "netconn_recv timeout, continuing");
          continue;
        }
        if (rc2 == ERR_CONN || rc2 == ERR_CLSD || rc2 == ERR_RST ||
            rc2 == ERR_ABRT || rc2 == ERR_IF) {
          ESP_LOGW(TAG, "Connection closed or reset, reconnecting...");
          netconn_close(lwipNetconn);
          break;
        }

        if (firstNetBuf != NULL) {
          netbuf_delete(firstNetBuf);
          firstNetBuf = NULL;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }

      // now parse the data
      netbuf_first(firstNetBuf);
      do {
        currentPos = 0;

        rc1 = netbuf_data(firstNetBuf, (void **)&start, &len);
        if (rc1 == ERR_OK) {
        } else {
          ESP_LOGE(TAG, "netconn rx, couldn't get data");

          continue;
        }

        while (len > 0) {
          rc1 = ERR_OK;

          switch (state) {
            // decode base message
            case BASE_MESSAGE_STATE: {
              switch (internalState) {
                case 0:
                  base_message_rx.type = *start & 0xFF;
                  internalState++;
                  break;

                case 1:
                  base_message_rx.type |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 2:
                  base_message_rx.id = *start & 0xFF;
                  internalState++;
                  break;

                case 3:
                  base_message_rx.id |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 4:
                  base_message_rx.refersTo = *start & 0xFF;
                  internalState++;
                  break;

                case 5:
                  base_message_rx.refersTo |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 6:
                  base_message_rx.sent.sec = *start & 0xFF;
                  internalState++;
                  break;

                case 7:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 8:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 9:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 10:
                  base_message_rx.sent.usec = *start & 0xFF;
                  internalState++;
                  break;

                case 11:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 12:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 13:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 14:
                  base_message_rx.received.sec = *start & 0xFF;
                  internalState++;
                  break;

                case 15:
                  base_message_rx.received.sec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 16:
                  base_message_rx.received.sec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 17:
                  base_message_rx.received.sec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 18:
                  base_message_rx.received.usec = *start & 0xFF;
                  internalState++;
                  break;

                case 19:
                  base_message_rx.received.usec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 20:
                  base_message_rx.received.usec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 21:
                  base_message_rx.received.usec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 22:
                  base_message_rx.size = *start & 0xFF;
                  internalState++;
                  break;

                case 23:
                  base_message_rx.size |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 24:
                  base_message_rx.size |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 25:
                  base_message_rx.size |= (*start & 0xFF) << 24;
                  internalState = 0;

                  now = esp_timer_get_time();

                  base_message_rx.received.sec = now / 1000000;
                  base_message_rx.received.usec =
                      now - base_message_rx.received.sec * 1000000;

                  typedMsgCurrentPos = 0;

                  state = TYPED_MESSAGE_STATE;
                  break;
              }

              currentPos++;
              len--;
              start++;

              break;
            }

            // decode typed message
            case TYPED_MESSAGE_STATE: {
              switch (base_message_rx.type) {
                case SNAPCAST_MESSAGE_WIRE_CHUNK: {
                  switch (internalState) {
                    case 0: {
                      wire_chnk.timestamp.sec = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 1: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 2: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 3: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 24;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 4: {
                      wire_chnk.timestamp.usec = *start;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 5: {
                      wire_chnk.timestamp.usec |= (int32_t)*start << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 6: {
                      wire_chnk.timestamp.usec |= (int32_t)*start << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 7: {
                      wire_chnk.timestamp.usec |= (int32_t)*start << 24;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 8: {
                      wire_chnk.size = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 9: {
                      wire_chnk.size |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 10: {
                      wire_chnk.size |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 11: {
                      wire_chnk.size |= (*start & 0xFF) << 24;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 12: {
                      size_t tmp_size;

                      if ((base_message_rx.size - typedMsgCurrentPos) <= len) {
                        tmp_size = base_message_rx.size - typedMsgCurrentPos;
                      } else {
                        tmp_size = len;
                      }

                      if (received_header == true) {
                        switch (codec) {
                          case OPUS: {
                            if (opusData == NULL) {
                              int retry_count = 0;
                              while ((opusData = (uint8_t *)malloc(
                                          wire_chnk.size)) == NULL && retry_count < 10) {
                                ESP_LOGE(TAG, "couldn't memory for opusData, retry %d", retry_count);
                                vTaskDelay(pdMS_TO_TICKS(1));
                                retry_count++;
                              }

                              if (opusData == NULL) {
                                  ESP_LOGE(TAG, "Failed to allocate opusData after %d retries", retry_count);
                                  break;
                              }

                              payloadOffset = 0;
                            }

                            memcpy(&opusData[payloadOffset], start, tmp_size);
                            payloadOffset += tmp_size;

                            if (payloadOffset >= wire_chnk.size) {
                              pDecData = NULL;
                              int retry_count = 0;
                              while (!pDecData && retry_count < 10) {
                                pDecData = (decoderData_t *)malloc(
                                    sizeof(decoderData_t));
                                if (!pDecData) {
                                  vTaskDelay(pdMS_TO_TICKS(1));
                                  retry_count++;
                                }
                              }

                              if (!pDecData) {
                                  ESP_LOGE(TAG, "Failed to allocate pDecData after %d retries", retry_count);
                                  free(opusData);
                                  opusData = NULL;
                                  break;
                              }

                              pDecData->timestamp = wire_chnk.timestamp;
                              pDecData->inData = opusData;
                              pDecData->bytes = wire_chnk.size;
                              pDecData->outData = NULL;
                              pDecData->type = SNAPCAST_MESSAGE_WIRE_CHUNK;

                              if (xQueueSend(decoderTaskQHdl, &pDecData, portMAX_DELAY) != pdTRUE) {
                                  free(opusData);
                                  free(pDecData);
                              }

                              opusData = NULL;
                              pDecData = NULL;
                            }

                            break;
                          }

                          case FLAC: {
#if TEST_DECODER_TASK
                            pDecData = NULL;
                            int retry_count = 0;
                            while (!pDecData && retry_count < 10) {
                              pDecData = (decoderData_t *)malloc(
                                  sizeof(decoderData_t));
                              if (!pDecData) {
                                vTaskDelay(pdMS_TO_TICKS(1));
                                retry_count++;
                              }
                            }

                            if (!pDecData) {
                                ESP_LOGE(TAG, "Failed to allocate pDecData after %d retries", retry_count);
                                break;
                            }

                            pDecData->bytes = tmp_size;
                            pDecData->timestamp = wire_chnk.timestamp;
                            pDecData->inData = NULL;

                            retry_count = 0;
                            while (!pDecData->inData && retry_count < 10) {
                              pDecData->inData =
                                  (uint8_t *)malloc(pDecData->bytes);
                              if (!pDecData->inData) {
                                vTaskDelay(pdMS_TO_TICKS(1));
                                retry_count++;
                              }
                            }

                            if (!pDecData->inData) {
                                ESP_LOGE(TAG, "Failed to allocate inData after %d retries", retry_count);
                                free(pDecData);
                                break;
                            }

                            if (pDecData->inData) {
                              memcpy(pDecData->inData, start, tmp_size);
                              pDecData->outData = NULL;
                              pDecData->type = SNAPCAST_MESSAGE_WIRE_CHUNK;

                              if (xQueueSend(decoderTaskQHdl, &pDecData, portMAX_DELAY) != pdTRUE) {
                                  free(pDecData->inData);
                                  free(pDecData);
                              }
                            }
#endif
                            break;
                          }

                          case PCM: {
                            size_t _tmp = tmp_size;
                            size_t frame_bytes =
                                snapcast_pcm_bytes_per_frame(scSet.ch,
                                                             scSet.bits);

                            offset = 0;

                            if ((frame_bytes == 0) ||
                                (frame_bytes > sizeof(pcmFrameBuffer))) {
                              ESP_LOGE(TAG,
                                       "Unsupported PCM frame size %u for %u-bit/%u-ch stream",
                                       (unsigned)frame_bytes,
                                       (unsigned)scSet.bits,
                                       (unsigned)scSet.ch);
                              break;
                            }

                            if (pcmData == NULL) {
                              if (allocate_pcm_chunk_memory(
                                      &pcmData, wire_chnk.size) < 0) {
                                pcmData = NULL;
                                ESP_LOGE(TAG, "Failed to allocate PCM chunk memory");
                                break;
                              }

                              payloadOffset = 0;
                              pcmFrameBufferFill = 0;
                            }

                            while (_tmp--) {
                              pcmFrameBuffer[pcmFrameBufferFill++] =
                                  start[offset++];

                              if (pcmFrameBufferFill == frame_bytes) {
                                uint8_t packed_frame[32];

                                if ((pcmData) && (pcmData->fragment->payload)) {
                                  if (!snapcast_pack_pcm_frame(
                                          packed_frame, sizeof(packed_frame),
                                          pcmFrameBuffer, scSet.ch,
                                          scSet.bits)) {
                                    ESP_LOGE(TAG,
                                             "Failed to repack PCM frame for %d-bit output",
                                             (int)scSet.bits);
                                    break;
                                  }

                                  memcpy(&(pcmData->fragment->payload[payloadOffset]),
                                         packed_frame, frame_bytes);
                                  payloadOffset += frame_bytes;
                                }

                                pcmFrameBufferFill = 0;
                              }
                            }

                            break;
                          }

                          default: {
                            ESP_LOGE(TAG, "Decoder (1) not supported");

                            return;

                            break;
                          }
                        }
                      }

                      typedMsgCurrentPos += tmp_size;
                      start += tmp_size;
                      currentPos += tmp_size;
                      len -= tmp_size;

                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        if (received_header == true) {
                          switch (codec) {
                            case OPUS: {
                              break;
                            }

                            case FLAC: {
#if TEST_DECODER_TASK
                              pDecData = NULL;

                              xQueueSend(decoderTaskQHdl, &pDecData,
                                         portMAX_DELAY);
#endif
                              break;
                            }

                            case PCM: {
                              size_t decodedSize = wire_chnk.size;

                              if (pcmFrameBufferFill != 0) {
                                ESP_LOGW(TAG,
                                         "Discarding incomplete PCM frame (%u bytes buffered)",
                                         (unsigned)pcmFrameBufferFill);
                                pcmFrameBufferFill = 0;
                              }

                              if (pcmData) {
                                pcmData->timestamp = wire_chnk.timestamp;
                              }

                              scSet.chkInFrames =
                                  decodedSize /
                                  ((size_t)scSet.ch * (size_t)(scSet.bits / 8));

                              if (player_send_snapcast_setting(&scSet) !=
                                  pdPASS) {
                                ESP_LOGE(TAG,
                                         "Failed to notify "
                                         "sync task about "
                                         "codec. Did you "
                                         "init player?");

                                return;
                              }

#if CONFIG_USE_DSP_PROCESSOR
                              if ((pcmData) && (pcmData->fragment->payload)) {
                                dsp_processor_worker(pcmData->fragment->payload,
                                                     pcmData->fragment->size,
                                                     scSet.sr, scSet.bits);
                              }
#endif

                              if (pcmData) {
                                insert_pcm_chunk(pcmData);
                              }

                              pcmData = NULL;

                              break;
                            }

                            default: {
                              ESP_LOGE(TAG,
                                       "Decoder (2) not "
                                       "supported");

                              return;

                              break;
                            }
                          }
                        }

                        health_monitor_data_received(true);
                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        typedMsgCurrentPos = 0;
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "wire chunk decoder "
                               "shouldn't get here");

                      break;
                    }
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_CODEC_HEADER: {
                  switch (internalState) {
                    case 0: {
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 1: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 2: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 3: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      p_tmp = malloc(typedMsgLen + 1);
                      if (p_tmp == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory "
                                 "for codec string");

                        return;
                      }

                      offset = 0;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 4: {
                      if (len >= typedMsgLen) {
                        memcpy(&p_tmp[offset], start, typedMsgLen);

                        offset += typedMsgLen;

                        typedMsgCurrentPos += typedMsgLen;
                        start += typedMsgLen;
                        currentPos += typedMsgLen;
                        len -= typedMsgLen;
                      } else {
                        memcpy(&p_tmp[offset], start, typedMsgLen);

                        offset += len;

                        typedMsgCurrentPos += len;
                        start += len;
                        currentPos += len;
                        len -= len;
                      }

                      if (offset == typedMsgLen) {
                        p_tmp[typedMsgLen] = 0;

                        if (strcmp(p_tmp, "opus") == 0) {
                          codec = OPUS;
                        } else if (strcmp(p_tmp, "flac") == 0) {
                          codec = FLAC;
                        } else if (strcmp(p_tmp, "pcm") == 0) {
                          codec = PCM;
                        } else {
                          codec = NONE;

                          ESP_LOGI(TAG, "Codec : %s not supported", p_tmp);
                          ESP_LOGI(TAG,
                                   "Change encoder codec to "
                                   "opus, flac or pcm in "
                                   "/etc/snapserver.conf on "
                                   "server");

                          free(p_tmp);
                          return;
                        }

                        free(p_tmp);
                        p_tmp = NULL;

                        internalState++;
                      }

                      break;
                    }

                    case 5: {
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 6: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 7: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 8: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      p_tmp = malloc(typedMsgLen);
                      if (p_tmp == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory "
                                 "for codec string");

                        return;
                      }

                      offset = 0;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 9: {
                      if (len >= typedMsgLen) {
                        memcpy(&p_tmp[offset], start, typedMsgLen);

                        offset += typedMsgLen;

                        typedMsgCurrentPos += typedMsgLen;
                        start += typedMsgLen;
                        currentPos += typedMsgLen;
                        len -= typedMsgLen;
                      } else {
                        memcpy(&p_tmp[offset], start, typedMsgLen);

                        offset += len;

                        typedMsgCurrentPos += len;
                        start += len;
                        currentPos += len;
                        len -= len;
                      }

                      if (offset == typedMsgLen) {
                        cleanup_decoder_resources();

                        if (codec == OPUS) {
                          decoderTaskQHdl =
                              xQueueCreate(8, sizeof(decoderData_t *));
                          if (decoderTaskQHdl == NULL) {
                            ESP_LOGE(TAG, "Failed to create decoderTaskQHdl");
                            free(p_tmp);
                            return;
                          }

                          uint16_t channels;
                          uint32_t rate;
                          uint16_t bits;

                          memcpy(&rate, p_tmp + 4, sizeof(rate));
                          memcpy(&bits, p_tmp + 8, sizeof(bits));
                          memcpy(&channels, p_tmp + 10, sizeof(channels));

                          scSet.codec = codec;
                          if (bits != 16) {
                            ESP_LOGW(TAG,
                                     "Opus output is 16-bit PCM on this client; "
                                     "forcing 16-bit playback instead of %u-bit",
                                     (unsigned)bits);
                            bits = 16;
                          }
                          scSet.bits = bits;
                          scSet.ch = channels;
                          scSet.sr = rate;

                          ESP_LOGI(TAG, "Opus sample format: %ld:%d:%d\n", rate,
                                   bits, channels);

                          int error = 0;

                          opusDecoder =
                              opus_decoder_create(scSet.sr, scSet.ch, &error);
                          if (error != 0) {
                            ESP_LOGI(TAG, "Failed to init opus coder");
                            free(p_tmp);
                            return;
                          }

                          ESP_LOGI(TAG, "Initialized opus Decoder: %d", error);

                          if (dec_task_handle == NULL) {
                            if (xTaskCreatePinnedToCore(
                                &opus_decoder_task, "opus_task", OPUS_TASK_STACK_SIZE,
                                &scSet, OPUS_TASK_PRIORITY, &dec_task_handle,
                                OPUS_TASK_CORE_ID) != pdPASS) {
                                ESP_LOGE(TAG, "Failed to create opus_task");
                            }
                          }
                        } else if (codec == FLAC) {
                          decoderTaskQHdl =
                              xQueueCreate(8, sizeof(decoderData_t *));
                          if (decoderTaskQHdl == NULL) {
                            ESP_LOGE(TAG, "Failed to create decoderTaskQHdl");
                            free(p_tmp);
                            return;
                          }

                          decoderReadQHdl =
                              xQueueCreate(8, sizeof(decoderData_t *));
                          if (decoderReadQHdl == NULL) {
                            ESP_LOGE(TAG, "Failed to create flac read queue");
                            free(p_tmp);
                            return;
                          }

                          decoderWriteQHdl =
                              xQueueCreate(8, sizeof(decoderData_t *));
                          if (decoderWriteQHdl == NULL) {
                            ESP_LOGE(TAG, "Failed to create flac write queue");
                            free(p_tmp);
                            return;
                          }

                          if (t_flac_decoder_task == NULL) {
                            if (xTaskCreatePinnedToCore(
                                &flac_decoder_task, "flac_decoder_task",
                                FLAC_DECODER_TASK_STACK_SIZE, &scSet, FLAC_DECODER_TASK_PRIORITY,
                                &t_flac_decoder_task,
                                FLAC_DECODER_TASK_CORE_ID) != pdPASS) {
                                ESP_LOGE(TAG, "Failed to create flac_decoder_task");
                            }
                          }

#if TEST_DECODER_TASK
                          if (dec_task_handle == NULL) {
                            if (xTaskCreatePinnedToCore(
                                &flac_task, "flac_task", FLAC_TASK_STACK_SIZE, &scSet,
                                FLAC_TASK_PRIORITY, &dec_task_handle,
                                FLAC_TASK_CORE_ID) != pdPASS) {
                                ESP_LOGE(TAG, "Failed to create flac_task");
                            }
                          }

                          pDecData =
                              (decoderData_t *)malloc(sizeof(decoderData_t));
                          if (pDecData == NULL) {
                              ESP_LOGE(TAG, "Failed to allocate pDecData for codec header");
                              free(p_tmp);
                              break;
                          }
                          memset(pDecData, 0, sizeof(decoderData_t));

                          pDecData->bytes = typedMsgLen;
                          pDecData->inData = (uint8_t *)malloc(typedMsgLen);
                          if (pDecData->inData == NULL) {
                              ESP_LOGE(TAG, "Failed to allocate inData for codec header");
                              free(pDecData);
                              free(p_tmp);
                              break;
                          }
                          memcpy(pDecData->inData, p_tmp, typedMsgLen);
                          pDecData->outData = NULL;
                          pDecData->type = SNAPCAST_MESSAGE_CODEC_HEADER;

                          if (xQueueSend(decoderTaskQHdl, &pDecData, portMAX_DELAY) != pdTRUE) {
                              free(pDecData->inData);
                              free(pDecData);
                          }

                          pDecData = NULL;
                          xQueueSend(decoderTaskQHdl, &pDecData, portMAX_DELAY);

#endif
                        } else if (codec == PCM) {
                          uint16_t channels;
                          uint32_t rate;
                          uint16_t bits;

                          memcpy(&channels, p_tmp + 22, sizeof(channels));
                          memcpy(&rate, p_tmp + 24, sizeof(rate));
                          memcpy(&bits, p_tmp + 34, sizeof(bits));

                          scSet.codec = codec;
                          scSet.bits = bits;
                          scSet.ch = channels;
                          scSet.sr = rate;

                          ESP_LOGI(TAG, "pcm sampleformat: %ld:%d:%d", scSet.sr,
                                   scSet.bits, scSet.ch);
                        } else {
                          ESP_LOGE(TAG,
                                   "codec header decoder "
                                   "shouldn't get here after "
                                   "codec string was detected");

                          free(p_tmp);
                          return;
                        }

                        free(p_tmp);
                        p_tmp = NULL;

                        health_monitor_data_received(false);
                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        received_header = true;
                        esp_timer_stop(timeSyncMessageTimer);
                        if (!esp_timer_is_active(timeSyncMessageTimer)) {
                          esp_timer_start_periodic(timeSyncMessageTimer,
                                                   timeout);
                        }
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "codec header decoder "
                               "shouldn't get here");

                      break;
                    }
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_SERVER_SETTINGS: {
                  switch (internalState) {
                    case 0: {
                      while ((netbuf_len(firstNetBuf) - currentPos) <
                             base_message_rx.size) {
                        ESP_LOGI(TAG, "need more data");

                        rc1 = netconn_recv(lwipNetconn, &newNetBuf);
                        if (rc1 != ERR_OK) {
                          ESP_LOGE(TAG, "rx error for need more data");

                          if (rc1 == ERR_CONN) {
                            break;
                          }

                          if (newNetBuf != NULL) {
                            netbuf_delete(newNetBuf);

                            newNetBuf = NULL;
                          }

                          continue;
                        }

                        netbuf_chain(firstNetBuf, newNetBuf);
                        newNetBuf = NULL;
                      }

                      if (rc1 == ERR_OK) {
                        typedMsgLen = *start & 0xFF;

                        typedMsgCurrentPos++;
                        start++;
                        currentPos++;
                        len--;

                        internalState++;
                      } else {
                        ESP_LOGE(TAG, "some error");
                      }

                      break;
                    }

                    case 1: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 2: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 3: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 4: {
                      p_tmp = malloc(typedMsgLen + 1);

                      if (p_tmp == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory for "
                                 "server settings string");
                      } else {
                        netbuf_copy_partial(firstNetBuf, p_tmp, typedMsgLen,
                                            currentPos);

                        p_tmp[typedMsgLen] = 0;

                        result = server_settings_message_deserialize(
                            &server_settings_message, p_tmp);
                        if (result) {
                          ESP_LOGE(TAG,
                                   "Failed to read server "
                                   "settings: %d",
                                   result);
                        } else {
                          ESP_LOGI(TAG, "Buffer length:  %ld",
                                   server_settings_message.buffer_ms);
                          ESP_LOGI(TAG, "Latency:        %ld",
                                   server_settings_message.latency);
                          ESP_LOGI(TAG, "Mute:           %d",
                                   server_settings_message.muted);
                          ESP_LOGI(TAG, "Setting volume: %ld",
                                   server_settings_message.volume);
                        }

                        if (scSet.muted != server_settings_message.muted) {
#if SNAPCAST_USE_SOFT_VOL
                          if (server_settings_message.muted) {
                            dsp_processor_set_volome(0.0);
                          } else {
                            dsp_processor_set_volome(
                                (double)server_settings_message.volume / 100);
                          }
#endif
                          audio_hal_set_mute(board_handle->audio_hal,
                                             server_settings_message.muted);
                        }

                        if (scSet.volume != server_settings_message.volume) {
#if SNAPCAST_USE_SOFT_VOL
                          if (!server_settings_message.muted) {
                            dsp_processor_set_volome(
                                (double)server_settings_message.volume / 100);
                          }
#else
                          audio_hal_set_volume(board_handle->audio_hal,
                                               server_settings_message.volume);
#endif
                        }

                        scSet.cDacLat_ms = server_settings_message.latency;
                        scSet.buf_ms = server_settings_message.buffer_ms;
                        scSet.muted = server_settings_message.muted;
                        scSet.volume = server_settings_message.volume;

                        if (player_send_snapcast_setting(&scSet) != pdPASS) {
                          ESP_LOGE(TAG,
                                   "Failed to notify sync task. "
                                   "Did you init player?");

                          free(p_tmp);
                          return;
                        }

                        free(p_tmp);
                        p_tmp = NULL;
                      }

                      internalState++;
                      /* fall through */
                    }

                    case 5: {
                      size_t tmpSize =
                          base_message_rx.size - typedMsgCurrentPos;

                      if (len > 0) {
                        if (tmpSize < len) {
                          start += tmpSize;
                          currentPos += tmpSize;
                          typedMsgCurrentPos += tmpSize;
                          len -= tmpSize;
                        } else {
                          start += len;
                          currentPos += len;
                          typedMsgCurrentPos += len;
                          len = 0;
                        }
                      }

                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        health_monitor_data_received(false);
                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        typedMsgCurrentPos = 0;
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "server settings decoder "
                               "shouldn't get here");

                      break;
                    }
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_STREAM_TAGS: {
                  size_t tmpSize = base_message_rx.size - typedMsgCurrentPos;

                  if (tmpSize < len) {
                    start += tmpSize;
                    currentPos += tmpSize;
                    typedMsgCurrentPos += tmpSize;
                    len -= tmpSize;
                  } else {
                    start += len;
                    currentPos += len;

                    typedMsgCurrentPos += len;
                    len = 0;
                  }

                  if (typedMsgCurrentPos >= base_message_rx.size) {
                    health_monitor_data_received(false);
                    typedMsgCurrentPos = 0;

                    state = BASE_MESSAGE_STATE;
                    internalState = 0;
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_TIME: {
                  switch (internalState) {
                    case 0: {
                      time_message_rx.latency.sec = *start;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 1: {
                      time_message_rx.latency.sec |= (int32_t)*start << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 2: {
                      time_message_rx.latency.sec |= (int32_t)*start << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 3: {
                      time_message_rx.latency.sec |= (int32_t)*start << 24;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 4: {
                      time_message_rx.latency.usec = *start;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 5: {
                      time_message_rx.latency.usec |= (int32_t)*start << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 6: {
                      time_message_rx.latency.usec |= (int32_t)*start << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 7: {
                      time_message_rx.latency.usec |= (int32_t)*start << 24;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;
                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        health_monitor_data_received(false);
                        typedMsgCurrentPos = 0;

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        trx =
                            (int64_t)base_message_rx.received.sec * 1000000LL +
                            (int64_t)base_message_rx.received.usec;
                        ttx = (int64_t)base_message_rx.sent.sec * 1000000LL +
                              (int64_t)base_message_rx.sent.usec;
                        tdif = trx - ttx;
                        trx = (int64_t)time_message_rx.latency.sec * 1000000LL +
                              (int64_t)time_message_rx.latency.usec;
                        tmpDiffToServer = (trx - tdif) / 2;

                        int64_t diff;

                        diff = now - lastTimeSync;
                        if (diff > 60000000LL) {
                          ESP_LOGW(TAG,
                                   "Last time sync older "
                                   "than a minute. "
                                   "Clearing time buffer");

                          reset_latency_buffer();

                          timeout = FAST_SYNC_LATENCY_BUF;

                          esp_timer_stop(timeSyncMessageTimer);
                          if (received_header == true) {
                            if (!esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_start_periodic(timeSyncMessageTimer,
                                                       timeout);
                            }
                          }
                        }

                        player_latency_insert(tmpDiffToServer);

                        lastTimeSync = now;

                        if (received_header == true) {
                          if (!esp_timer_is_active(timeSyncMessageTimer)) {
                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          }

                          bool is_full = false;
                          latency_buffer_full(&is_full, portMAX_DELAY);
                          if ((is_full == true) &&
                              (timeout < NORMAL_SYNC_LATENCY_BUF)) {
                            timeout = NORMAL_SYNC_LATENCY_BUF;

                            ESP_LOGI(TAG, "latency buffer full");

                            if (esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_stop(timeSyncMessageTimer);
                            }

                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          } else if ((is_full == false) &&
                                     (timeout > FAST_SYNC_LATENCY_BUF)) {
                            timeout = FAST_SYNC_LATENCY_BUF;

                            ESP_LOGI(TAG, "latency buffer not full");

                            if (esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_stop(timeSyncMessageTimer);
                            }

                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          }
                        }
                      } else {
                        ESP_LOGE(TAG,
                                 "error time message, this "
                                 "shouldn't happen! %d %ld",
                                 typedMsgCurrentPos, base_message_rx.size);

                        typedMsgCurrentPos = 0;

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "time message decoder shouldn't "
                               "get here %d %ld %ld",
                               typedMsgCurrentPos, base_message_rx.size,
                               internalState);

                      break;
                    }
                  }

                  break;
                }

                default: {
                  typedMsgCurrentPos++;
                  start++;
                  currentPos++;
                  len--;

                  if (typedMsgCurrentPos >= base_message_rx.size) {
                    health_monitor_data_received(false);
                    ESP_LOGI(TAG, "done unknown typed message %d",
                             base_message_rx.type);

                    state = BASE_MESSAGE_STATE;
                    internalState = 0;

                    typedMsgCurrentPos = 0;
                  }

                  break;
                }
              }

              break;
            }

            default: {
              break;
            }
          }

          if (rc1 != ERR_OK) {
            break;
          }
        }
      } while (netbuf_next(firstNetBuf) >= 0);

      if (firstNetBuf != NULL) {
        netbuf_delete(firstNetBuf);
        firstNetBuf = NULL;
      }

      if (rc1 != ERR_OK) {
        ESP_LOGE(TAG, "Data error, closing netconn");

        netconn_close(lwipNetconn);

        break;
      }
    }

    if (p_tmp != NULL) {
      free(p_tmp);
      p_tmp = NULL;
    }

    if (opusData != NULL) {
      free(opusData);
      opusData = NULL;
    }

    if (pcmData != NULL) {
      free_pcm_chunk(pcmData);
      pcmData = NULL;
    }

    if (pDecData != NULL) {
      free_flac_data(pDecData);
      pDecData = NULL;
    }

    if (newNetBuf != NULL) {
      netbuf_delete(newNetBuf);
      newNetBuf = NULL;
    }
  }
}

/**
 *
 */
void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  esp_log_level_set("*", ESP_LOG_INFO);

  // if enabled these cause a timer srv stack overflow
  esp_log_level_set("HEADPHONE", ESP_LOG_NONE);
  esp_log_level_set("gpio", ESP_LOG_WARN);
  esp_log_level_set("uart", ESP_LOG_WARN);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi_init", ESP_LOG_WARN);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi_init", ESP_LOG_WARN);

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  gpio_config_t cfg = {.pin_bit_mask = BIT64(GPIO_NUM_5),
                       .mode = GPIO_MODE_DEF_INPUT,
                       .pull_up_en = GPIO_PULLUP_DISABLE,
                       .pull_down_en = GPIO_PULLDOWN_ENABLE,
                       .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&cfg);
#endif

#if CONFIG_AUDIO_BOARD_CUSTOM && CONFIG_DAC_ADAU1961
  i2s_chan_handle_t tx_chan;

  i2s_chan_config_t tx_chan_cfg = {
      .id = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 2,
      .dma_frame_num = 128,
      .auto_clear = true,
  };
  ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));

  board_i2s_pin_t pin_config0;
  get_i2s_pins(I2S_NUM_0, &pin_config0);

  i2s_std_clk_config_t i2s_clkcfg = {
    .sample_rate_hz = 44100,
    .clk_src = I2S_CLK_SRC_APLL,
    .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };

  i2s_std_config_t tx_std_cfg = {
      .clk_cfg = i2s_clkcfg,
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = pin_config0.mck_io_num,
              .bclk = pin_config0.bck_io_num,
              .ws = pin_config0.ws_io_num,
              .dout = pin_config0.data_out_num,
              .din = pin_config0.data_in_num,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
  i2s_channel_enable(tx_chan);
#endif

  ESP_LOGI(TAG, "Start codec chip");
  board_handle = audio_board_init();
  if (board_handle) {
    ESP_LOGI(TAG, "Audio board_init done");
  } else {
    ESP_LOGE(TAG,
             "Audio board couldn't be initialized. Check menuconfig if project "
             "is configured right or check your wiring!");

    vTaskDelay(portMAX_DELAY);
  }

  audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE,
                       AUDIO_HAL_CTRL_START);
  audio_hal_set_mute(board_handle->audio_hal,
                     true);

#if CONFIG_AUDIO_BOARD_CUSTOM && CONFIG_DAC_ADAU1961
  if (tx_chan) {
    i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
  }
#endif

  ESP_LOGI(TAG, "init player");
  init_player();

  {
    board_i2s_pin_t pin_config0;
    get_i2s_pins(I2S_NUM_0, &pin_config0);

    uint64_t pin_mask = 0;
    if (pin_config0.mck_io_num >= 0) pin_mask |= BIT64(pin_config0.mck_io_num);
    if (pin_config0.data_out_num >= 0) pin_mask |= BIT64(pin_config0.data_out_num);
    if (pin_config0.bck_io_num >= 0) pin_mask |= BIT64(pin_config0.bck_io_num);
    if (pin_config0.ws_io_num >= 0) pin_mask |= BIT64(pin_config0.ws_io_num);

    if (pin_mask > 0) {
        gpio_config_t gpioCfg = {
            .pin_bit_mask = pin_mask,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&gpioCfg);

        if (pin_config0.mck_io_num >= 0) gpio_set_level(pin_config0.mck_io_num, 0);
        if (pin_config0.data_out_num >= 0) gpio_set_level(pin_config0.data_out_num, 0);
        if (pin_config0.bck_io_num >= 0) gpio_set_level(pin_config0.bck_io_num, 0);
        if (pin_config0.ws_io_num >= 0) gpio_set_level(pin_config0.ws_io_num, 0);
    }

    if (pin_config0.data_in_num >= 0) {
        gpio_config_t gpioCfg = {
            .pin_bit_mask = BIT64(pin_config0.data_in_num),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&gpioCfg);
    }
  }

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  eth_init();
  init_http_server_task("ETH_DEF");
#else
  wifi_init();
  ESP_LOGI(TAG, "Connected to AP");
  init_http_server_task("WIFI_STA_DEF");
#endif

  net_mdns_register(SNAPCAST_CLIENT_NAME);
#ifdef CONFIG_SNAPCLIENT_SNTP_ENABLE
  set_time_from_sntp();
#endif

#if CONFIG_USE_DSP_PROCESSOR
  dsp_processor_init();
#endif
  // Initialize shared LED manager to arbitrate between player and health monitor
  // Normal mode: player controls playback/idle. Health monitor may temporarily override.
  led_manager_init(IDLE_GPIO, PLAYBACK_EN_GPIO, true);
  led_manager_set_health_mode(LED_HEALTH_NORMAL);



  xTaskCreatePinnedToCore(&ota_server_task, "ota", 14 * 256, NULL,
                          OTA_TASK_PRIORITY, &t_ota_task, OTA_TASK_CORE_ID);

  xTaskCreatePinnedToCore(&http_get_task, "http", 4 * 1024, NULL,
                          HTTP_TASK_PRIORITY, &t_http_get_task,
                          HTTP_TASK_CORE_ID);
}
