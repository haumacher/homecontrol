/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_status_light.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.led";

/// The WS2812's colour of set(bool) when lit: a dim cyan (it is bright enough at a few percent).
constexpr uint8_t kRed = 0;
constexpr uint8_t kGreen = 8;
constexpr uint8_t kBlue = 16;

} // namespace

bool EspStatusLight::init() {
#if defined(CONFIG_HC_STATUS_LED_WS2812)
	gpio_ = CONFIG_HC_STATUS_LED_GPIO_NUM;
	led_strip_config_t config = {};
	config.strip_gpio_num = gpio_;
	config.max_leds = 1;
	config.led_model = LED_MODEL_WS2812;
	config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
	led_strip_rmt_config_t rmt = {};
	rmt.clk_src = RMT_CLK_SRC_DEFAULT;
	rmt.resolution_hz = 10 * 1000 * 1000;
	led_strip_handle_t strip = nullptr;
	esp_err_t err = led_strip_new_rmt_device(&config, &rmt, &strip);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "status LED disabled: WS2812 on GPIO%d: %s", gpio_, esp_err_to_name(err));
		return false;
	}
	strip_ = strip;
	led_strip_clear(strip);
	ESP_LOGI(kTag, "status LED: WS2812 on GPIO%d", gpio_);
	return true;
#elif defined(CONFIG_HC_STATUS_LED_GPIO)
	gpio_ = CONFIG_HC_STATUS_LED_GPIO_NUM;
	if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio_)) {
		ESP_LOGE(kTag, "status LED disabled: GPIO%d cannot drive an output", gpio_);
		return false;
	}
	gpio_config_t config = {};
	config.pin_bit_mask = 1ULL << gpio_;
	config.mode = GPIO_MODE_OUTPUT;
	esp_err_t err = gpio_config(&config);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "status LED disabled: GPIO%d: %s", gpio_, esp_err_to_name(err));
		return false;
	}
	set(false);
	ESP_LOGI(kTag, "status LED: GPIO%d%s", gpio_,
#if defined(CONFIG_HC_STATUS_LED_ACTIVE_LOW)
		" (active low)"
#else
		""
#endif
	);
	return true;
#else
	ESP_LOGI(kTag, "no status LED configured (menuconfig: HomeControl status LED)");
	return false;
#endif
}

void EspStatusLight::set(bool on) {
	set(on, kRed, kGreen, kBlue);
}

void EspStatusLight::set(bool on, uint8_t red, uint8_t green, uint8_t blue) {
#if defined(CONFIG_HC_STATUS_LED_WS2812)
	led_strip_handle_t strip = static_cast<led_strip_handle_t>(strip_);
	if (strip == nullptr) {
		return;
	}
	if (on) {
		led_strip_set_pixel(strip, 0, red, green, blue);
		led_strip_refresh(strip);
	} else {
		led_strip_clear(strip);
	}
#elif defined(CONFIG_HC_STATUS_LED_GPIO)
#if defined(CONFIG_HC_STATUS_LED_ACTIVE_LOW)
	gpio_set_level(static_cast<gpio_num_t>(gpio_), on ? 0 : 1);
#else
	gpio_set_level(static_cast<gpio_num_t>(gpio_), on ? 1 : 0);
#endif
#else
	(void)on;
#endif
#if !defined(CONFIG_HC_STATUS_LED_WS2812)
	(void)red;
	(void)green;
	(void)blue;
#endif
}

} // namespace esp
} // namespace hc
