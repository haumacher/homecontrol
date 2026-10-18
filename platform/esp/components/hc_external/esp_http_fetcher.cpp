/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_http_fetcher.h"

#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.ext";
constexpr UBaseType_t kTaskPriority = 4;

} // namespace

EspHttpFetcher::EspHttpFetcher(LoopQueue& queue) : queue_(queue) {}

bool EspHttpFetcher::fetch(const char* url, size_t max_bytes, FetchReceiver& receiver, const char** error) {
	if (busy_.load()) {
		*error = "busy: a fetch is in flight";
		return false;
	}
	post_length_ = 0;
	return start(url, max_bytes, receiver, error);
}

bool EspHttpFetcher::post(const char* url, const char* body, size_t length, size_t max_bytes, FetchReceiver& receiver,
	const char** error) {
	if (busy_.load()) {
		*error = "busy: a fetch is in flight";
		return false;
	}
	if (length == 0 || length > kMaxPostBody) {
		*error = "the request body is empty or longer than 384 bytes";
		return false;
	}
	std::memcpy(post_body_, body, length);
	post_length_ = length;
	return start(url, max_bytes, receiver, error);
}

bool EspHttpFetcher::start(const char* url, size_t max_bytes, FetchReceiver& receiver, const char** error) {
	if (busy_.load()) {
		*error = "busy: a fetch is in flight";
		return false;
	}
	size_t length = std::strlen(url);
	if (length > kMaxUrl) {
		*error = "URL longer than 383 bytes";
		return false;
	}
	if (std::strncmp(url, "https://", 8) != 0 && std::strncmp(url, "http://", 7) != 0) {
		*error = "not an http:// or https:// URL";
		return false;
	}
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
	if (std::strncmp(url, "https://", 8) == 0) {
		*error = "https:// needs the certificate bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)";
		return false;
	}
#endif
	std::memcpy(url_, url, length + 1);
	max_bytes_ = max_bytes < kBufferBytes ? max_bytes : kBufferBytes;
	receiver_ = &receiver;
	length_ = 0;
	status_ = 0;
	error_[0] = '\0';
	busy_.store(true);
	if (xTaskCreate(&EspHttpFetcher::task, "hc_ext_fetch", kTaskStack, this, kTaskPriority, nullptr) != pdPASS) {
		busy_.store(false);
		*error = "cannot create the fetch task (memory)";
		return false;
	}
	return true;
}

void EspHttpFetcher::task(void* context) {
	EspHttpFetcher* self = static_cast<EspHttpFetcher*>(context);
	self->run();
	if (!self->queue_.post(&EspHttpFetcher::deliver, self, 0, nullptr, "external fetch result")) {
		// The core's guard (ExternalValues::kFetchTimeoutMs) counts the lost result as a failure.
		self->busy_.store(false);
	}
	vTaskDelete(nullptr);
}

void EspHttpFetcher::run() {
	esp_http_client_config_t config = {};
	config.url = url_;
	config.timeout_ms = kTimeoutMs;
	config.buffer_size = 1024;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
	config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (client == nullptr) {
		std::snprintf(error_, sizeof(error_), "HTTP client not created (memory)");
		return;
	}
	if (post_length_ > 0) {
		esp_http_client_set_method(client, HTTP_METHOD_POST);
		esp_http_client_set_header(client, "Content-Type", "application/json");
	}
	esp_err_t err = esp_http_client_open(client, static_cast<int>(post_length_));
	if (err != ESP_OK) {
		std::snprintf(error_, sizeof(error_), "cannot connect: %s", esp_err_to_name(err));
	} else if (post_length_ > 0 &&
		esp_http_client_write(client, post_body_, static_cast<int>(post_length_)) != static_cast<int>(post_length_)) {
		std::snprintf(error_, sizeof(error_), "cannot send the request body");
	} else {
		int64_t announced = esp_http_client_fetch_headers(client);
		status_ = esp_http_client_get_status_code(client);
		if (status_ != 200 && post_length_ == 0) {
			std::snprintf(error_, sizeof(error_), "HTTP status %d", status_);
		} else if (announced > static_cast<int64_t>(max_bytes_)) {
			std::snprintf(error_, sizeof(error_), "body of %lld bytes larger than %u bytes",
				static_cast<long long>(announced), static_cast<unsigned>(max_bytes_));
		} else {
			read_body(client);
			// A POST delivers the body of any status (a refusal document); the status still fails it.
			if (error_[0] == '\0' && status_ != 200) {
				std::snprintf(error_, sizeof(error_), "HTTP status %d", status_);
			}
		}
	}
	esp_http_client_close(client);
	esp_http_client_cleanup(client);
}

void EspHttpFetcher::read_body(void* handle) {
	esp_http_client_handle_t client = static_cast<esp_http_client_handle_t>(handle);
	// Read one byte more than allowed to notice a body that did not announce its size.
	for (;;) {
		int n = esp_http_client_read(client, body_ + length_, static_cast<int>(max_bytes_ + 1 - length_));
		if (n < 0) {
			std::snprintf(error_, sizeof(error_), "read error after %u bytes", static_cast<unsigned>(length_));
			break;
		}
		if (n == 0) {
			break;
		}
		length_ += static_cast<size_t>(n);
		if (length_ > max_bytes_) {
			std::snprintf(error_, sizeof(error_), "body larger than %u bytes", static_cast<unsigned>(max_bytes_));
			break;
		}
	}
}

void EspHttpFetcher::deliver(void* context, const LoopMessage&) {
	EspHttpFetcher* self = static_cast<EspHttpFetcher*>(context);
	FetchResult result;
	result.ok = self->error_[0] == '\0';
	result.http_status = self->status_;
	// A POST hands over the body of any status that arrived complete (FetchResult::body).
	bool body = result.ok || (self->post_length_ > 0 && self->status_ != 0 && self->length_ <= self->max_bytes_);
	result.body = body ? self->body_ : nullptr;
	result.length = body ? self->length_ : 0;
	result.error = self->error_;
	if (!result.ok) {
		ESP_LOGD(kTag, "fetch of %s failed: %s", self->url_, self->error_);
	}
	FetchReceiver* receiver = self->receiver_;
	receiver->fetch_done(result);
	// Busy until the receiver returned: the body stays valid during its call.
	self->busy_.store(false);
}

} // namespace esp
} // namespace hc
