/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "wps_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.wps";

} // namespace

WpsButton::WpsButton(EventLoop& loop, const Clock& clock, Connectivity& connectivity)
	: loop_(loop), clock_(clock), connectivity_(connectivity) {}

bool WpsButton::start() {
	gpio_ = CONFIG_HC_WPS_BUTTON_GPIO;
	if (gpio_ < 0) {
		ESP_LOGI(kTag, "no WPS button configured (CONFIG_HC_WPS_BUTTON_GPIO=-1): WPS only through 'wifi wps'");
		return false;
	}
	if (!GPIO_IS_VALID_GPIO(gpio_)) {
		ESP_LOGE(kTag, "WPS button disabled: GPIO%d does not exist on this chip (CONFIG_HC_WPS_BUTTON_GPIO)", gpio_);
		return false;
	}
	gpio_config_t config = {};
	config.pin_bit_mask = 1ULL << gpio_;
	config.mode = GPIO_MODE_INPUT;
	config.pull_up_en = GPIO_PULLUP_ENABLE;
	config.pull_down_en = GPIO_PULLDOWN_DISABLE;
	config.intr_type = GPIO_INTR_DISABLE;
	esp_err_t err = gpio_config(&config);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "WPS button disabled: cannot configure GPIO%d: %s", gpio_, esp_err_to_name(err));
		return false;
	}
	TimerHandle timer = loop_.schedule_periodic(clock_.monotonic_ms() + kPollPeriodMs, kPollPeriodMs,
		Callback::member<WpsButton, &WpsButton::poll>(this));
	if (!timer.valid()) {
		ESP_LOGE(kTag, "WPS button disabled: no timer for polling");
		return false;
	}
	ESP_LOGI(kTag, "hold the button on GPIO%d for %u s to pair with a router again (WPS)", gpio_,
		static_cast<unsigned>(button_.hold_ms() / 1000));
	return true;
}

void WpsButton::poll() {
	bool pressed = gpio_get_level(static_cast<gpio_num_t>(gpio_)) == 0;
	if (button_.sample(pressed, clock_.monotonic_ms())) {
		ESP_LOGI(kTag, "button on GPIO%d held for %u s", gpio_, static_cast<unsigned>(button_.hold_ms() / 1000));
		connectivity_.request_pairing("button held");
	}
}

} // namespace esp
} // namespace hc
