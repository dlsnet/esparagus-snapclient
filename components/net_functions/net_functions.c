/*
   Network related functions
*/

#include "net_functions.h"

#include <ctype.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "netdb.h"
#include "wifi_interface.h"

static const char *TAG = "NETF";

extern EventGroupHandle_t s_wifi_event_group;

static void net_mdns_make_hostname(const char *clientname, char *hostname,
                                   size_t hostname_len)
{
    const char *fallback = "snapclient";
    const char *source = (clientname && clientname[0]) ? clientname : fallback;
    uint8_t mac[6] = {0};
    size_t written = 0;
    bool last_was_dash = false;

    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    if (hostname_len == 0) {
        return;
    }

    memset(hostname, 0, hostname_len);

    for (size_t i = 0; source[i] != '\0' && written + 8 < hostname_len; ++i) {
        unsigned char c = (unsigned char)source[i];

        if (isalnum(c)) {
            hostname[written++] = (char)tolower(c);
            last_was_dash = false;
        } else if (!last_was_dash && written > 0) {
            hostname[written++] = '-';
            last_was_dash = true;
        }
    }

    if (written == 0) {
        strncpy(hostname, fallback, hostname_len - 1);
        written = strlen(hostname);
    }

    if (hostname[written - 1] == '-') {
        hostname[--written] = '\0';
    }

    snprintf(hostname + written, hostname_len - written, "-%02x%02x%02x",
             mac[3], mac[4], mac[5]);
}

static const char *mdns_ip_protocol_to_str(int proto)
{
    /*
     * mdns_result_t::ip_protocol exists across mdns versions, but the enum
     * values may differ slightly. This mapping is guarded so it compiles
     * cleanly regardless of the managed mdns component version.
     */
    switch (proto) {
#ifdef MDNS_IP_PROTOCOL_V4
        case MDNS_IP_PROTOCOL_V4:
            return "V4";
#endif
#ifdef MDNS_IP_PROTOCOL_V6
        case MDNS_IP_PROTOCOL_V6:
            return "V6";
#endif
#ifdef MDNS_IP_PROTOCOL_ANY
        case MDNS_IP_PROTOCOL_ANY:
            return "ANY";
#endif
        default:
            return "UNK";
    }
}

void net_mdns_register(const char *clientname)
{
    char hostname[64];
    esp_err_t err;

    net_mdns_make_hostname(clientname, hostname, sizeof(hostname));

    ESP_LOGI(TAG, "Setup mdns hostname=%s instance=%s", hostname,
             (clientname && clientname[0]) ? clientname : "ESP32 SNAPcast client OTA");

    mdns_free();

    err = mdns_init();
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set(
        (clientname && clientname[0]) ? clientname : "ESP32 SNAPcast client OTA"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 8032, NULL, 0));
}

void mdns_print_results(mdns_result_t *results)
{
    mdns_result_t *r = results;
    int i = 1;

    while (r) {
        printf("%d: Type: %s\n", i++, mdns_ip_protocol_to_str((int)r->ip_protocol));

        if (r->instance_name) {
            printf("  PTR : %s\n", r->instance_name);
        }
        if (r->hostname) {
            printf("  SRV : %s.local:%u\n", r->hostname, r->port);
        }
        if (r->txt_count) {
            printf("  TXT : [%u] ", r->txt_count);
            for (int t = 0; t < r->txt_count; t++) {
                printf("%s=%s; ", r->txt[t].key, r->txt[t].value);
            }
            printf("\n");
        }

        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type == IPADDR_TYPE_V6) {
                printf("  AAAA: " IPV6STR "\n", IPV62STR(a->addr.u_addr.ip6));
            } else {
                printf("  A   : " IPSTR "\n", IP2STR(&(a->addr.u_addr.ip4)));
            }
        }

        r = r->next;
    }
}

uint32_t find_mdns_service(const char *service_name, const char *proto)
{
    ESP_LOGI(TAG, "Query PTR: %s.%s.local", service_name, proto);

    mdns_result_t *r = NULL;
    esp_err_t err = mdns_query_ptr(service_name, proto, 3000, 20, &r);
    if (err) {
        ESP_LOGE(TAG, "Query Failed (err=0x%x)", err);
        return (uint32_t)-1;
    }
    if (!r) {
        ESP_LOGW(TAG, "No results found!");
        return (uint32_t)-1;
    }

    uint32_t port = 0;
    if (r->instance_name) {
        printf("  PTR : %s\n", r->instance_name);
    }
    if (r->hostname) {
        printf("  SRV : %s.local:%u\n", r->hostname, r->port);
        port = r->port;
    }

    mdns_query_results_free(r);
    return port;
}

void sntp_cb(struct timeval *tv)
{
    struct tm timeinfo = {0};
    time_t now = tv->tv_sec;
    localtime_r(&now, &timeinfo);

    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "sntp_cb called: %s", strftime_buf);
}

void set_time_from_sntp(void)
{
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

    ESP_LOGI(TAG, "Initializing SNTP");

    /*
     * Prefer esp_sntp_* wrappers (IDF v5+) to avoid deprecation warnings
     * and keep us aligned with ESP-IDF expectations.
     */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_SNTP_SERVER);
    /* Optional: enable callback for diagnostics */
    // esp_sntp_set_time_sync_notification_cb(sntp_cb);
    esp_sntp_init();

    setenv("TZ", SNTP_TIMEZONE, 1);
    tzset();

    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;

    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time in UTC is: %s", strftime_buf);
}
