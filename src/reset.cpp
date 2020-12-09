// Basic Config
#include "globals.h"
#include "reset.h"

// Local logging tag
static const char TAG[] = __FILE__;

// Conversion factor for micro seconds to seconds
#define uS_TO_S_FACTOR 1000000ULL

// variables keep its values after a wakeup from sleep
RTC_DATA_ATTR runmode_t RTC_runmode = RUNMODE_POWERCYCLE;
static RTC_DATA_ATTR struct timeval RTC_sleep_start_time;

const char *runmode[4] = {"powercycle", "normal", "wakeup", "update"};

void do_reset(bool warmstart) {
  if (warmstart) {
    ESP_LOGI(TAG, "restarting device (warmstart), keeping runmode %s",
             runmode[RTC_runmode]);
  } else {
#if (HAS_LORA)
    if (RTC_runmode == RUNMODE_NORMAL) {
      LMIC_shutdown();
    }
#endif
    RTC_runmode = RUNMODE_POWERCYCLE;
    ESP_LOGI(TAG, "restarting device (coldstart), setting runmode %s",
             runmode[RTC_runmode]);
  }
  esp_restart();
}

void do_after_reset(void) {

  struct timeval sleep_stop_time;
  uint64_t sleep_time_ms;

  switch (esp_sleep_get_wakeup_cause()) {
  case ESP_SLEEP_WAKEUP_EXT0:  // Wakeup caused by external signal using RTC_IO
  case ESP_SLEEP_WAKEUP_EXT1:  // Wakeup caused by external signal using
                               // RTC_CNTL
  case ESP_SLEEP_WAKEUP_TIMER: // Wakeup caused by timer
  case ESP_SLEEP_WAKEUP_TOUCHPAD: // Wakeup caused by touchpad
  case ESP_SLEEP_WAKEUP_ULP:      // Wakeup caused by ULP program

    // calculate time spent in deep sleep
    gettimeofday(&sleep_stop_time, NULL);
    sleep_time_ms =
        (sleep_stop_time.tv_sec - RTC_sleep_start_time.tv_sec) * 1000 +
        (sleep_stop_time.tv_usec - RTC_sleep_start_time.tv_usec) / 1000;
    ESP_LOGI(TAG, "Time spent in deep sleep: %d ms", sleep_time_ms);

    RTC_runmode = RUNMODE_WAKEUP;
    break;

  case ESP_SLEEP_WAKEUP_ALL:
  case ESP_SLEEP_WAKEUP_GPIO:
  case ESP_SLEEP_WAKEUP_UART:
  case ESP_SLEEP_WAKEUP_UNDEFINED:
  default:
    // not a deep sleep reset
    RTC_runmode = RUNMODE_POWERCYCLE;
    break;
  } // switch

  ESP_LOGI(TAG, "Starting Software v%s, runmode %s", PROGVERSION,
           runmode[RTC_runmode]);
}

void enter_deepsleep(const uint64_t wakeup_sec = 60,
                     const gpio_num_t wakeup_gpio = GPIO_NUM_MAX) {

  // ensure we are in normal runmode, not udpate or wakeup
  if ((RTC_runmode != RUNMODE_NORMAL)
#if (HAS_LORA)
      || (LMIC.opmode & (OP_JOINING | OP_REJOIN))
#endif
  ) {
    ESP_LOGE(TAG, "Can't go to sleep now");
    return;
  } else {
    ESP_LOGI(TAG, "Attempting to sleep...");
  }

  // wait until all send queues are empty
  ESP_LOGI(TAG, "Waiting until send queues are empty...");
  while (!allQueuesEmtpy())
    vTaskDelay(pdMS_TO_TICKS(100));

#if (HAS_LORA)
  // shutdown LMIC safely
  ESP_LOGI(TAG, "Waiting until LMIC is idle...");
  while ((LMIC.opmode & OP_TXRXPEND) ||
         os_queryTimeCriticalJobs(sec2osticks(wakeup_sec)))
    vTaskDelay(pdMS_TO_TICKS(100));

  SaveLMICToRTC(wakeup_sec);
#endif // (HAS_LORA)

// switch off radio
#if (BLECOUNTER)
  stop_BLEscan();
  btStop();
#endif
#if (WIFICOUNTER)
  switch_wifi_sniffer(0);
#endif

  // halt interrupts accessing i2c bus
  mask_user_IRQ();

// switch off display
#ifdef HAS_DISPLAY
  dp_shutdown();
#endif

// reduce power if has PMU
#ifdef HAS_PMU
  AXP192_power(pmu_power_sleep);
#endif

  // shutdown i2c bus
  i2c_deinit();

  // configure wakeup sources
  // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/sleep_modes.html

  // set up RTC wakeup timer, if we have
  if (wakeup_sec > 0) {
    esp_sleep_enable_timer_wakeup(wakeup_sec * uS_TO_S_FACTOR);
  }

  // set wakeup gpio, if we have
  if (wakeup_gpio != GPIO_NUM_MAX) {
    rtc_gpio_isolate(wakeup_gpio); // minimize deep sleep current
    esp_sleep_enable_ext1_wakeup(1ULL << wakeup_gpio, ESP_EXT1_WAKEUP_ALL_LOW);
  }

  // save sleep start time. Deep sleep.
  gettimeofday(&RTC_sleep_start_time, NULL);
  ESP_LOGI(TAG, "Going to sleep, good bye.");
  esp_deep_sleep_start();
}