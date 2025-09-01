// ESP32-S3 RTU-over-TCP Bridge (ESP-IDF 5.5-3, Managed Components)
// - USB CDC-ACM Host (Meltem per USB, 19200 8E1) — RX per Callback
// - TCP-Server (rtuovertcp) für Home Assistant auf Port 5020
// - WLAN + Hostname, DHCP/Static IP aus app_config.h
// - mDNS (_modbus._tcp), LED-Kill-Pin, Boot: erst IP, dann USB/TCP
// - Netz-Watchdog (Reboot falls zu lange ohne IP)
// - Akzeptiert TCP-Frames mit/ohne CRC; ergänzt CRC bei Bedarf

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_pm.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#include "driver/gpio.h"

// USB Host + CDC-ACM
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"      // usb_device_desc_t
#include "usb/cdc_acm_host.h"

// RGB LED
#include "led_strip.h"

// mDNS
#include "mdns.h"

// ---------- Konfig ----------
#include "app_config.h"

// Defaults, falls app_config.h fehlt/Einträge fehlen
#ifndef APP_WIFI_SSID
#  define APP_WIFI_SSID   "DEIN_SSID"
#endif
#ifndef APP_WIFI_PASS
#  define APP_WIFI_PASS   "DEIN_PASSWORT"
#endif
#ifndef APP_HOSTNAME
#  define APP_HOSTNAME    "meltem-bridge"
#endif
#ifndef APP_NET_USE_DHCP
#  define APP_NET_USE_DHCP 1
#endif
#ifndef APP_BOOT_IP_WAIT_S
#  define APP_BOOT_IP_WAIT_S 15      // Sekunden auf IP warten vor USB/TCP
#endif
#ifndef APP_NET_WATCHDOG_S
#  define APP_NET_WATCHDOG_S 60      // Sekunden ohne IP -> Reboot (0=aus)
#endif
#ifndef APP_LED_KILL_ENABLE
#  define APP_LED_KILL_ENABLE 0
#endif
#ifndef APP_LED_KILL_PIN
#  define APP_LED_KILL_PIN 46
#endif
#ifndef APP_MDNS_ENABLE
#  define APP_MDNS_ENABLE 1
#endif
#ifndef APP_MDNS_INSTANCE
#  define APP_MDNS_INSTANCE "Meltem RTU Bridge"
#endif
#ifndef APP_MDNS_SVC_TYPE
#  define APP_MDNS_SVC_TYPE "_modbus"
#endif
#ifndef APP_MDNS_SVC_PROTO
#  define APP_MDNS_SVC_PROTO "_tcp"
#endif

#if (APP_NET_USE_DHCP==0)
#  ifndef APP_NET_STATIC_IP_ADDR
#    define APP_NET_STATIC_IP_ADDR 192,168,178,230
#  endif
#  ifndef APP_NET_STATIC_NETMASK
#    define APP_NET_STATIC_NETMASK 255,255,255,0
#  endif
#  ifndef APP_NET_STATIC_GATEWAY
#    define APP_NET_STATIC_GATEWAY 192,168,178,1
#  endif
#  ifndef APP_NET_STATIC_DNS1
#    define APP_NET_STATIC_DNS1 192,168,178,1
#  endif
#  ifndef APP_NET_STATIC_DNS2
#    define APP_NET_STATIC_DNS2 8,8,8,8
#  endif
#endif

// ================= Fach-Parameter =================
#define LED_STRIP_GPIO 48
#define LED_STRIP_LEN  1

#define TCP_LISTEN_PORT       5020
#define TCP_RX_IDLE_GAP_MS    10
#define TCP_CLIENT_TIMEOUT_S  120

#define MB_BAUDRATE    19200
#define MB_PARITY      2    // 0=N,1=O,2=E
#define MB_STOP_BITS   1
#define MB_TIMEOUT_MS  700
#define MB_MAX_FRAME   256

#define WIFI_TX_PWR_DBM_QL    34

static const char *TAG = "rtu_tcp_bridge";

// ===== LED =====
static led_strip_handle_t s_strip;

static inline bool led_is_muted(void) {
#if APP_LED_KILL_ENABLE
    return gpio_get_level(APP_LED_KILL_PIN) == 0;  // GND => mute
#else
    return false;
#endif
}

#ifndef APP_LED_BRIGHTNESS
#define APP_LED_BRIGHTNESS 255
#endif

static inline uint8_t apply_brightness(uint8_t v) {
    // linear; falls du später Gamma willst, kann man hier eine LUT verwenden
    return (uint8_t)((uint16_t)v * (uint16_t)APP_LED_BRIGHTNESS / 255);
}



static inline void led_show(uint8_t r, uint8_t g, uint8_t b) {
    if (led_is_muted()) return;
    led_strip_set_pixel(s_strip, 0,
        apply_brightness(r), apply_brightness(g), apply_brightness(b));
    led_strip_refresh(s_strip);
}
static void led_blink(uint8_t r, uint8_t g, uint8_t b, int on_ms) {
    if (led_is_muted()) return;
    led_strip_set_pixel(s_strip, 0,
        apply_brightness(r), apply_brightness(g), apply_brightness(b));
    led_strip_refresh(s_strip);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
}static void led_init(void) {
#if APP_LED_KILL_ENABLE
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << APP_LED_KILL_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
#endif
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_LEN,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);
}

// ===== CRC16 (Modbus) =====
static uint16_t mb_crc16(const uint8_t *buf, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i=0;i<len;i++) {
        crc ^= buf[i];
        for (int b=0;b<8;b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}
static inline bool mb_frame_has_crc(const uint8_t *buf, size_t len) {
    if (len < 4) return false;
    uint16_t crc_in = (uint16_t)buf[len-2] | ((uint16_t)buf[len-1] << 8);
    return mb_crc16(buf, len-2) == crc_in;
}

// ===== CDC Globals (Callback-RX) =====
static cdc_acm_dev_hdl_t   s_cdc = NULL;
static SemaphoreHandle_t   s_cdc_mutex;
static StreamBufferHandle_t s_rx_stream;
#define RX_STREAM_CAPACITY  1024

static volatile uint16_t s_vid = 0, s_pid = 0;
static volatile bool     s_have_vidpid = false;

// ===== USB Host Library Event Task =====
static TaskHandle_t s_usb_evt_task = NULL;
static void usb_host_event_task(void *arg) {
    while (1) {
        uint32_t events = 0;
        esp_err_t er = usb_host_lib_handle_events(portMAX_DELAY, &events);
        if (er != ESP_OK && er != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "usb_host_lib_handle_events: %s", esp_err_to_name(er));
        }
    }
}

// ---- Device-Event (Disconnect etc.) ----
static void dev_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_arg) {
    switch (event->type) {
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGW(TAG, "CDC device disconnected");
            xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
            s_cdc = NULL;
            xSemaphoreGive(s_cdc_mutex);
            s_have_vidpid = false;
            break;
        case CDC_ACM_HOST_SERIAL_STATE:
            ESP_LOGI(TAG, "CDC serial state: 0x%04x", event->data.serial_state.val);
            break;
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG, "CDC host error: %d", event->data.error);
            break;
        default:
            break;
    }
}

// ---- Data-RX Callback → in StreamBuffer ----
static void data_rx_cb(uint8_t *data, size_t data_len, void *user_arg) {
    if (!s_rx_stream || data_len == 0) return;
    size_t off = 0;
    while (off < data_len) {
        size_t wrote = xStreamBufferSend(s_rx_stream, data + off, data_len - off, 0);
        if (wrote == 0) break;
        off += wrote;
    }
}

// ---- New-Device-Callback: VID/PID auslesen ----
static void new_dev_cb(usb_device_handle_t usb_dev) {
    const usb_device_desc_t *dd = NULL;
    if (usb_host_get_device_descriptor(usb_dev, &dd) == ESP_OK && dd) {
        s_vid = dd->idVendor;
        s_pid = dd->idProduct;
        s_have_vidpid = true;
        ESP_LOGI(TAG, "USB new device: VID=0x%04x, PID=0x%04x", s_vid, s_pid);
    } else {
        ESP_LOGW(TAG, "usb_host_get_device_descriptor() failed");
    }
}

// USB Host + CDC initialisieren und ggf. öffnen
static esp_err_t cdc_open_if_needed(void) {
    if (s_cdc) return ESP_OK;

    static bool usb_host_ready = false;
    if (!usb_host_ready) {
        usb_host_config_t host_cfg = {0};
        ESP_ERROR_CHECK(usb_host_install(&host_cfg));
        usb_host_ready = true;
        ESP_LOGI(TAG, "USB Host installiert");
        if (!s_usb_evt_task) {
            xTaskCreatePinnedToCore(usb_host_event_task, "usb_evt", 4096, NULL, 6, &s_usb_evt_task, 0);
        }
    }

    static bool cdc_driver_ready = false;
    if (!cdc_driver_ready) {
        cdc_acm_host_driver_config_t cfg = {
            .driver_task_stack_size = 4096,
            .driver_task_priority   = 5,
            .xCoreID                = 0,
            .new_dev_cb             = new_dev_cb,
        };
        ESP_ERROR_CHECK(cdc_acm_host_install(&cfg));
        cdc_driver_ready = true;
        ESP_LOGI(TAG, "CDC-ACM Host installiert");
    }

    if (!s_have_vidpid) return ESP_ERR_NOT_FOUND;

    cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 1000,
        .out_buffer_size       = 256,
        .event_cb              = dev_event_cb,
        .data_cb               = data_rx_cb,
        .user_arg              = NULL,
    };

    for (int ifx = 0; ifx < 4; ++ifx) {
        cdc_acm_dev_hdl_t dev = NULL;
        esp_err_t er = cdc_acm_host_open(s_vid, s_pid, (uint8_t)ifx, &dev_cfg, &dev);
        if (er == ESP_OK && dev) {
            cdc_acm_line_coding_t lc = {
                .dwDTERate   = MB_BAUDRATE,
                .bCharFormat = (MB_STOP_BITS == 2) ? 2 : 0,
                .bParityType = MB_PARITY,
                .bDataBits   = 8,
            };
            ESP_ERROR_CHECK(cdc_acm_host_line_coding_set(dev, &lc));
            ESP_ERROR_CHECK(cdc_acm_host_set_control_line_state(dev, true, true)); // DTR/RTS

            xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
            s_cdc = dev;
            xSemaphoreGive(s_cdc_mutex);

            ESP_LOGI(TAG, "CDC geöffnet (VID=0x%04x PID=0x%04x IF=%d)", s_vid, s_pid, ifx);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "cdc_acm_host_open: Kein passendes Interface (VID=0x%04x PID=0x%04x)", s_vid, s_pid);
    return ESP_ERR_NOT_FOUND;
}

static void cdc_close_if_open(void) {
    xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
    if (s_cdc) {
        cdc_acm_host_close(s_cdc);
        s_cdc = NULL;
        ESP_LOGW(TAG, "CDC geschlossen");
    }
    xSemaphoreGive(s_cdc_mutex);
}

// ===== WLAN & Netz =====
static esp_netif_t *s_netif = NULL;
static volatile bool s_wifi_up = false;

static void on_wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        esp_wifi_disconnect();
        esp_wifi_connect();   // reconnect
    }
}
static void on_ip_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_up = true;
    }
}

static void net_apply_config(void) {
    if (s_netif) esp_netif_set_hostname(s_netif, APP_HOSTNAME);

#if (APP_NET_USE_DHCP==0)
    if (s_netif) {
        esp_netif_ip_info_t ipi = {0};
        ip4_addr_t ip, gw, mask;
        IP4_ADDR(&ip,   APP_NET_STATIC_IP_ADDR);
        IP4_ADDR(&gw,   APP_NET_STATIC_GATEWAY);
        IP4_ADDR(&mask, APP_NET_STATIC_NETMASK);

        ipi.ip = ip; ipi.gw = gw; ipi.netmask = mask;

        (void)esp_netif_dhcpc_stop(s_netif);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ipi));

        esp_netif_dns_info_t d1 = {0}, d2 = {0};
        IP4_ADDR(&d1.ip.u_addr.ip4, APP_NET_STATIC_DNS1); d1.ip.type = IPADDR_TYPE_V4;
        IP4_ADDR(&d2.ip.u_addr.ip4, APP_NET_STATIC_DNS2); d2.ip.type = IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN,   &d1);
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &d2);
    }
#else
    if (s_netif) (void)esp_netif_dhcpc_start(s_netif);
#endif
}

static void wifi_init_and_connect(void) {
    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));

    wifi_config_t wcfg = {0};
    strncpy((char*)wcfg.sta.ssid, APP_WIFI_SSID, sizeof(wcfg.sta.ssid)-1);
    strncpy((char*)wcfg.sta.password, APP_WIFI_PASS, sizeof(wcfg.sta.password)-1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));

    net_apply_config();

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    // esp_wifi_set_ps(WIFI_PS_NONE); // falls Startprobleme mit PS auftreten

    ESP_LOGI(TAG, "WiFi connecting to \"%s\" as \"%s\" ...", APP_WIFI_SSID, APP_HOSTNAME);
}
static void wifi_powersave(void) {
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    esp_wifi_set_max_tx_power(WIFI_TX_PWR_DBM_QL);
}
static void cpu_powersave(void) {
#if CONFIG_PM_ENABLE
    esp_pm_config_t pmcfg = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false };
    esp_pm_configure(&pmcfg);
#endif
}

// ===== mDNS =====
static void start_mdns(void) {
#if APP_MDNS_ENABLE
    esp_err_t e = mdns_init();
    if (e == ESP_ERR_INVALID_STATE) {
        // bereits initialisiert → okay
    } else if (e != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(e));
        return;
    }
    mdns_hostname_set(APP_HOSTNAME);
    mdns_instance_name_set(APP_MDNS_INSTANCE);
    mdns_service_add(APP_MDNS_INSTANCE, APP_MDNS_SVC_TYPE, APP_MDNS_SVC_PROTO,
                     TCP_LISTEN_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS aktiv: %s.local (%s.%s auf %d)",
             APP_HOSTNAME, APP_MDNS_SVC_TYPE, APP_MDNS_SVC_PROTO, TCP_LISTEN_PORT);
#endif
}

// ===== RTU-over-TCP Utils =====
static ssize_t recv_rtu_frame_with_gap(int sock, uint8_t *buf, size_t maxlen,
                                       uint32_t gap_ms, uint32_t overall_ms)
{
    size_t got = 0;
    uint32_t t_start = (uint32_t)xTaskGetTickCount();
    uint32_t last_byte_tick = t_start;

    while (got < maxlen) {
        uint8_t b;
        int n = recv(sock, &b, 1, 0);
        if (n == 1) { buf[got++] = b; last_byte_tick = (uint32_t)xTaskGetTickCount(); continue; }
        if (n == 0) return 0; // peer closed

        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ETIMEDOUT) {
            uint32_t now = (uint32_t)xTaskGetTickCount();
            uint32_t since_byte  = (now - last_byte_tick) * portTICK_PERIOD_MS;
            uint32_t since_start = (now - t_start)       * portTICK_PERIOD_MS;
            if (got > 0 && since_byte >= gap_ms) break;
            if (since_start >= overall_ms)  return -2;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        return -1; // echter Fehler
    }
    return (ssize_t)got;
}

// Blocking-Forward: TX + RX (Callback-Stream, Idle-Gap). TCP-Request darf ohne CRC kommen.
static int forward_rtu_over_cdc(const uint8_t *req, size_t reqlen,
                                uint8_t *resp, size_t respmax, uint32_t mb_timeout_ms)
{
    xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
    cdc_acm_dev_hdl_t dev = s_cdc;
    xSemaphoreGive(s_cdc_mutex);
    if (!dev) return -10;
    if (reqlen < 2) return -11;

    uint8_t txbuf[MB_MAX_FRAME];
    size_t  txlen = 0;

    if (mb_frame_has_crc(req, reqlen)) {
        if (reqlen > MB_MAX_FRAME) return -18;
        memcpy(txbuf, req, reqlen);
        txlen = reqlen;
    } else {
        if (reqlen + 2 > MB_MAX_FRAME) return -18;
        memcpy(txbuf, req, reqlen);
        uint16_t crc = mb_crc16(req, reqlen);
        txbuf[reqlen]   = (uint8_t)(crc & 0xFF);
        txbuf[reqlen+1] = (uint8_t)(crc >> 8);
        txlen = reqlen + 2;
    }

    esp_err_t er = cdc_acm_host_data_tx_blocking(dev, txbuf, txlen, mb_timeout_ms);
    if (er != ESP_OK) { cdc_close_if_open(); return -13; }

    size_t got = 0;
    uint32_t t0 = (uint32_t)xTaskGetTickCount();
    uint32_t last_rx_tick = t0;

    while (got < respmax) {
        size_t n = xStreamBufferReceive(s_rx_stream, resp + got, respmax - got, pdMS_TO_TICKS(30));
        if (n > 0) { got += n; last_rx_tick = (uint32_t)xTaskGetTickCount(); }
        else {
            uint32_t since_rx = ((uint32_t)xTaskGetTickCount() - last_rx_tick) * portTICK_PERIOD_MS;
            uint32_t since_all = ((uint32_t)xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
            if (got > 0 && since_rx >= TCP_RX_IDLE_GAP_MS) break;
            if (since_all >= mb_timeout_ms) return -15;
        }
    }
    if (got < 4) return -16;

    uint16_t rcrc = (uint16_t)resp[got-2] | ((uint16_t)resp[got-1] << 8);
    if (mb_crc16(resp, got-2) != rcrc) return -17;

    return (int)got;
}

// USB offen halten (auch ohne TCP-Client)
static void usb_keeper_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(3000)); // WLAN stabilisieren lassen
    for (;;) { cdc_open_if_needed(); vTaskDelay(pdMS_TO_TICKS(500)); }
}

// Netz-Watchdog
static void net_watchdog_task(void* arg) {
#if APP_NET_WATCHDOG_S == 0
    vTaskDelete(NULL);
#else
    TickType_t last_ok = xTaskGetTickCount();
    for (;;) {
        if (s_wifi_up) last_ok = xTaskGetTickCount();
        else { esp_wifi_disconnect(); esp_wifi_connect(); }
        if ((xTaskGetTickCount() - last_ok) > pdMS_TO_TICKS((APP_NET_WATCHDOG_S)*1000)) {
            ESP_LOGE(TAG, "No IP for %d s → reboot", APP_NET_WATCHDOG_S);
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
#endif
}

static void tcp_server_task(void *arg) {
    int listenfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listenfd < 0) { ESP_LOGE(TAG, "socket()"); vTaskDelete(NULL); return; }

    int yes = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(TCP_LISTEN_PORT);

    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { ESP_LOGE(TAG, "bind()"); close(listenfd); vTaskDelete(NULL); }
    if (listen(listenfd, 1) < 0)                                     { ESP_LOGE(TAG, "listen()"); close(listenfd); vTaskDelete(NULL); }

    ESP_LOGI(TAG, "RTU-over-TCP Server lauscht auf *:%d", TCP_LISTEN_PORT);

    while (1) {
        //led_show(255,150,0); // gelb: idle
        led_show(0,0,180);  // Blau = "warte auf IP"
        struct sockaddr_in cli; socklen_t clen = sizeof(cli);
        int cli_fd = accept(listenfd, (struct sockaddr*)&cli, &clen);
        if (cli_fd < 0) continue;

        ESP_LOGI(TAG, "Client %s:%d verbunden", inet_ntoa(cli.sin_addr), ntohs(cli.sin_port));
        struct timeval tvc; tvc.tv_sec = TCP_CLIENT_TIMEOUT_S; tvc.tv_usec = 0;
        setsockopt(cli_fd, SOL_SOCKET, SO_RCVTIMEO, &tvc, sizeof(tvc));

        for (;;) {
            if (cdc_open_if_needed() != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

            struct timeval tvgap; tvgap.tv_sec = 0; tvgap.tv_usec = 20000; // 20 ms
            setsockopt(cli_fd, SOL_SOCKET, SO_RCVTIMEO, &tvgap, sizeof(tvgap));

            uint8_t req[MB_MAX_FRAME];
            ssize_t rlen = recv_rtu_frame_with_gap(cli_fd, req, sizeof(req), TCP_RX_IDLE_GAP_MS, MB_TIMEOUT_MS);
            if (rlen == 0) { ESP_LOGI(TAG, "Client disconnect"); break; }
            if (rlen < 0) {
                if (rlen == -2) continue;
                ESP_LOGW(TAG, "recv error (%d), Client schließen", (int)rlen);
                break;
            }

            uint8_t resp[MB_MAX_FRAME];
            int resp_len = forward_rtu_over_cdc(req, (size_t)rlen, resp, sizeof(resp), MB_TIMEOUT_MS);
            if (resp_len < 0) { led_blink(255,0,0,50); ESP_LOGE(TAG, "forward_rtu_over_cdc: %d", resp_len); continue; }

            int wn = send(cli_fd, resp, resp_len, 0);
            if (wn != resp_len) ESP_LOGW(TAG, "send short (%d/%d)", wn, resp_len);
            else                led_blink(0,255,0,40);   // grün kurz
        }

        close(cli_fd);
        ESP_LOGI(TAG, "Client getrennt");
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    s_cdc_mutex  = xSemaphoreCreateMutex();
    s_rx_stream  = xStreamBufferCreate(RX_STREAM_CAPACITY, 1);
    configASSERT(s_rx_stream);

    led_init();
    led_show(255,150,0); // warten

    // System/Netif/Eventloop EINMAL hier
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    wifi_init_and_connect();
    wifi_powersave();
    cpu_powersave();

    // --- Boot-Reihenfolge: erst IP abwarten, DANN mDNS/USB/TCP ---
    uint32_t t0 = xTaskGetTickCount();
    while (!s_wifi_up && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(APP_BOOT_IP_WAIT_S*1000)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_wifi_up) {
        ESP_LOGE(TAG, "No IP after %d s → restart", APP_BOOT_IP_WAIT_S);
        esp_restart();
    }

    start_mdns(); // mDNS erst nach IP

    led_show(255,150,0);    // Gelb = Idle (bereit, noch keine Requests)
    
    // Jetzt USB/TCP/Watchdog starten
    xTaskCreatePinnedToCore(usb_keeper_task, "usb_keeper", 4096, NULL, 7, NULL, 0);
    xTaskCreatePinnedToCore(tcp_server_task, "tcp_server_task", 6144, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(net_watchdog_task, "net_wd", 3072, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Boot OK — ESP32-S3 RTU-over-TCP Bridge läuft");
}
