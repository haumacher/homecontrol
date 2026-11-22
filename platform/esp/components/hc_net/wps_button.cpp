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

WpsButton::WpsButton(EventLoop& loop, const Clock& clock, LoopQueue& queue, Connectivity& connectivity)
	: loop_(loop),
	  clock_(clock),
	  queue_(queue),
	  connectivity_(connectivity),
	  button_(loop, clock, *this, Callback::member<WpsButton, &WpsButton::held>(this, "wps_button.held")) {}

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
	gpio_num_t pin = static_cast<gpio_num_t>(gpio_);
	gpio_config_t config = {};
	config.pin_bit_mask = 1ULL << gpio_;
	config.mode = GPIO_MODE_INPUT;
	config.pull_up_en = GPIO_PULLUP_ENABLE;
	config.pull_down_en = GPIO_PULLDOWN_DISABLE;
	config.intr_type = GPIO_INTR_ANYEDGE;
	esp_err_t err = gpio_config(&config);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "WPS button disabled: cannot configure GPIO%d: %s", gpio_, esp_err_to_name(err));
		return false;
	}
	// The per-pin dispatcher of the GPIO driver; another component may have installed it already.
	// Flags 0: the handler is not in IRAM, so it waits while the flash is written (cache off).
	err = gpio_install_isr_service(0);
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		gpio_intr_disable(pin);
		ESP_LOGE(kTag, "WPS button disabled: cannot install the GPIO interrupt service: %s", esp_err_to_name(err));
		return false;
	}
	button_.start(); // reads the pin: armed when released
	err = gpio_isr_handler_add(pin, &WpsButton::isr, this);
	if (err != ESP_OK) {
		gpio_intr_disable(pin);
		ESP_LOGE(kTag, "WPS button disabled: cannot attach the interrupt of GPIO%d: %s", gpio_, esp_err_to_name(err));
		return false;
	}
	ESP_LOGI(kTag, "hold the button on GPIO%d for %u s to pair with a router again (WPS)", gpio_,
		static_cast<unsigned>(button_.hold_ms() / 1000));
	return true;
}

bool WpsButton::pressed() const {
	return gpio_get_level(static_cast<gpio_num_t>(gpio_)) == 0;
}

void WpsButton::isr(void* context) {
	WpsButton* self = static_cast<WpsButton*>(context);
	if (self->busy_.load()) {
		return; // the edge before is on its way; the loop reads the level when it settled
	}
	// Only when the loop will hear of it, the interrupt goes off: a full queue leaves it on, and the
	// next edge tries again.
	if (self->queue_.post_from_isr(&WpsButton::on_edge, self, 0, "WPS button edge")) {
		self->busy_.store(true);
		gpio_intr_disable(static_cast<gpio_num_t>(self->gpio_));
	}
}

void WpsButton::on_edge(void* context, const LoopMessage&) {
	WpsButton* self = static_cast<WpsButton*>(context);
	self->button_.edge();
	// Due with (and scheduled after) the button's settle timer: the level is read, then the
	// interrupt goes on again.
	self->rearm_ = self->loop_.schedule_at(self->clock_.monotonic_ms() + HoldButton::kDefaultDebounceMs,
		Callback::member<WpsButton, &WpsButton::rearm>(self, "wps_button.rearm"));
	if (!self->rearm_.valid()) {
		self->rearm(); // no timer: at least the interrupt comes back
	}
}

void WpsButton::rearm() {
	rearm_ = TimerHandle();
	busy_.store(false);
	gpio_intr_enable(static_cast<gpio_num_t>(gpio_));
	// An edge between the settle and now raised no interrupt: the level tells.
	if (pressed() != button_.pressed() && !busy_.exchange(true)) {
		gpio_intr_disable(static_cast<gpio_num_t>(gpio_));
		LoopMessage none{};
		on_edge(this, none);
	}
}

void WpsButton::held() {
	ESP_LOGI(kTag, "button on GPIO%d held for %u s", gpio_, static_cast<unsigned>(button_.hold_ms() / 1000));
	connectivity_.request_pairing("button held");
}

} // namespace esp
} // namespace hc
