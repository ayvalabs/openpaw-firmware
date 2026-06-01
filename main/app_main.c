// OpenPaw firmware entry point.
// Instinct layer: command dispatch + motion + sensors. AI runs off-MCU.
// License: MIT (c) 2026 aeropriest

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "openpaw";

// TODO(#14): init servo, imu, sensors, command dispatcher, net, ota
void app_main(void)
{
    ESP_LOGI(TAG, "OpenPaw firmware booting (board=%s)", CONFIG_OPENPAW_BOARD_NAME);

    // command_dispatcher_start();   // UART/BLE token loop (k/m/i/d)
    // motion_engine_init();         // load gait tables, start playback timer
    // sensors_init();               // temp, laser pointer
    // ota_mark_valid_after_health_check();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
