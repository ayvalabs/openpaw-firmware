#include <string.h>
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "VL53L0X.h"

static const char *TAG = "camera_web";
static volatile uint16_t g_distance_mm = 0;

/* Seeed Studio XIAO ESP32S3 Sense camera pins */
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

#define WIFI_SSID "ESP32-CAMERA"
#define WIFI_PASS "12345678"

static esp_err_t index_handler(httpd_req_t *req)
{
    const char *html =
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 Camera OTA v2</title>"
        "<style>body{margin:0;background:#111;color:#eee;font-family:Arial;text-align:center}"
        "h1{font-size:22px;margin:16px}a{color:#7cc7ff}img{width:100%;max-width:720px}</style>"
        "</head><body><h1>ESP32 Camera OTA v2</h1><p><a href='/update'>OTA Update</a></p>"
        "<img src='/stream'></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t update_page_handler(httpd_req_t *req)
{
    const char *html =
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>OTA Update</title>"
        "<style>body{background:#111;color:#eee;font-family:Arial;padding:24px}"
        "button,input{font-size:16px;margin-top:12px}</style>"
        "</head><body><h1>OTA Update</h1>"
        "<input id='file' type='file' accept='.bin'>"
        "<br><button onclick='upload()'>Upload firmware</button>"
        "<pre id='status'></pre>"
        "<script>"
        "async function upload(){"
        "const f=document.getElementById('file').files[0];"
        "if(!f){status.textContent='Choose a .bin file first';return;}"
        "status.textContent='Uploading...';"
        "const r=await fetch('/ota',{method:'POST',body:f});"
        "status.textContent=await r.text();"
        "}"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing OTA update to partition: %s", update_partition->label);

    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char buffer[1024];
    int remaining = req->content_len;

    while (remaining > 0) {
        int received = httpd_req_recv(req, buffer, sizeof(buffer));
        if (received <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buffer, received);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        remaining -= received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "Update successful. Rebooting...");
    ESP_LOGI(TAG, "OTA update successful, restarting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            return ESP_FAIL;
        }

        res = httpd_resp_send_chunk(req, "--frame\r\n", HTTPD_RESP_USE_STRLEN);
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, "Content-Type: image/jpeg\r\n\r\n", HTTPD_RESP_USE_STRLEN);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, "\r\n", HTTPD_RESP_USE_STRLEN);
        }

        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(80));
    }

    return res;
}

static void start_camera(void)
{
    camera_config_t config = {
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,
        .pin_d7 = Y9_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d0 = Y2_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    ESP_ERROR_CHECK(esp_camera_init(&config));
    ESP_LOGI(TAG, "Camera ready");
}

static void start_wifi_ap(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {};

strcpy((char *)wifi_config.ap.ssid, WIFI_SSID);
strcpy((char *)wifi_config.ap.password, WIFI_PASS);

wifi_config.ap.ssid_len = strlen(WIFI_SSID);
wifi_config.ap.channel = 1;
wifi_config.ap.max_connection = 4;
wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started");
    ESP_LOGI(TAG, "SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG, "Password: %s", WIFI_PASS);
    ESP_LOGI(TAG, "Open: http://192.168.4.1");
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
    };

    httpd_uri_t update_uri = {
        .uri = "/update",
        .method = HTTP_GET,
        .handler = update_page_handler,
    };

    httpd_uri_t ota_uri = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &stream_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &update_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_uri));

    ESP_LOGI(TAG, "Web server started");
}
static void vl53_task(void *pv)
{
    VL53L0X vl(I2C_NUM_0);


    vl.i2cMasterInit(GPIO_NUM_5, GPIO_NUM_6);

    if (!vl.init()) {
        ESP_LOGE(TAG, "Failed to initialize VL53L0X");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "VL53L0X initialized successfully");

    while (true) {
        uint16_t distance = 0;

        if (vl.read(&distance)) {
            g_distance_mm = distance;
            ESP_LOGI(TAG, "Distance: %u mm", distance);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


extern "C" void app_main(void)
{
    esp_err_t ota_valid_err = esp_ota_mark_app_valid_cancel_rollback();

    if (ota_valid_err == ESP_OK) {
        ESP_LOGI(TAG, "OTA app marked valid");
    } else if (ota_valid_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "OTA validation skipped: 0x%x", ota_valid_err);
    }

    start_camera();
    start_wifi_ap();
    start_webserver();
    xTaskCreate(
    vl53_task,
    "vl53",
    8192,
    NULL,
    5,
    NULL
);
}