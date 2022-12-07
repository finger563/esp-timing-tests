#include "sdkconfig.h"

#include <algorithm>
#include <chrono>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "nvs_flash.h"

#include "format.hpp"
#include "task.hpp"
#include "tcp_socket.hpp"
#include "udp_socket.hpp"
#include "wifi_sta.hpp"

using namespace std::chrono_literals;

/* Variable holding number of times ESP32 restarted since first boot.
 * It is placed into RTC memory using RTC_DATA_ATTR and
 * maintains its value when ESP32 wakes from deep sleep.
 */
RTC_DATA_ATTR static int boot_count = 0;

static const char * TAG = "ESP Timing Task";

static void time_sync_notification_cb(struct timeval *tv) {
  ESP_LOGI(TAG, "Notification of a time synchronization event");
}

static void initialize_sntp(void)
{
  ESP_LOGD(TAG, "Initializing SNTP");
  sntp_setoperatingmode(SNTP_OPMODE_POLL);

  /*
   * If 'NTP over DHCP' is enabled, we set dynamic pool address
   * as a 'secondary' server. It will act as a fallback server in case that address
   * provided via NTP over DHCP is not accessible
   */
  sntp_setservername(0, "pool.ntp.org");

  sntp_set_time_sync_notification_cb(time_sync_notification_cb);
  sntp_init();

  ESP_LOGD(TAG, "List of configured NTP servers:");

  for (uint8_t i = 0; i < SNTP_MAX_SERVERS; ++i){
    if (sntp_getservername(i)){
      ESP_LOGD(TAG, "server %d: %s", i, sntp_getservername(i));
    } else {
      // we have either IPv4 or IPv6 address, let's print it
      char buff[INET6_ADDRSTRLEN];
      ip_addr_t const *ip = sntp_getserver(i);
      if (ipaddr_ntoa_r(ip, buff, INET6_ADDRSTRLEN) != NULL)
        ESP_LOGD(TAG, "server %d: %s", i, buff);
    }
  }
}

static void obtain_time() {
  initialize_sntp();

  // wait for time to be set
  time_t now = 0;
  struct tm timeinfo = { 0 };
  int retry = 0;
  const int retry_count = 15;
  while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
    ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
  time(&now);
  localtime_r(&now, &timeinfo);
}

extern "C" void app_main(void) {
  esp_err_t err;
  espp::Logger logger({.tag = TAG, .level = espp::Logger::Verbosity::INFO});
  logger.info("Bootup");

  // initialize NVS, needed for WiFi
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    logger.warn("Erasing NVS flash...");
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // initialize WiFi
  logger.info("Initializing WiFi");
  espp::WifiSta wifi_sta({
      .ssid = CONFIG_ESP_WIFI_SSID,
        .password = CONFIG_ESP_WIFI_PASSWORD,
        .num_connect_retries = CONFIG_ESP_MAXIMUM_RETRY,
        .on_connected = nullptr,
        .on_disconnected = nullptr,
        .on_got_ip = [&logger](ip_event_got_ip_t* eventdata) {
          logger.info("got IP: {}.{}.{}.{}", IP2STR(&eventdata->ip_info.ip));
        }
        });

  // wait for network
  while (!wifi_sta.is_connected()) {
    logger.info("waiting for wifi connection...");
    std::this_thread::sleep_for(1s);
  }

  logger.info("synchronizing...");
  obtain_time();

  // multicast our receiver info over UDP
  // create threads
  auto client_task_fn = [](auto&, auto&) {
    static espp::UdpSocket client_socket({});
    static std::string multicast_group = "239.1.1.1";
    static size_t multicast_port = 5000;
    static auto send_config = espp::UdpSocket::SendConfig{
      .ip_address = multicast_group,
      .port = multicast_port,
      .is_multicast_endpoint = true,
    };
    auto now = std::chrono::high_resolution_clock::now();
    std::string payload = fmt::format("{0:%FT%H:%M:}{1:%S}", now, now.time_since_epoch());
    fmt::print("Sending current time '{}'\n", payload);
    // NOTE: now this call blocks until the response is received
    client_socket.send(payload, send_config);
    auto last_second = std::chrono::time_point_cast<std::chrono::seconds>(now);
    auto next_send_time = last_second + 1s;
    std::this_thread::sleep_until(next_send_time);
  };
  auto client_task = espp::Task::make_unique({
      .name = "Client Task",
      .callback = client_task_fn,
      .stack_size_bytes = 6*1024
    });
  client_task->start();

  auto start = std::chrono::high_resolution_clock::now();
  while (true) {
    auto end = std::chrono::high_resolution_clock::now();
    float current_time = std::chrono::duration<float>(end-start).count();
    std::this_thread::sleep_for(1s);
  }
}
