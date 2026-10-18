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

EspHttpFetcher::EspHttpFetcher(LoopQueue& queue) : queue_(queue) {
	consumed_ = xSemaphoreCreateBinaryStatic(&consumed_storage_);
}

EspHttpFetcher::~EspHttpFetcher() {
	vSemaphoreDelete(consumed_);
}

bool EspHttpFetcher::fetch(const char* url, FetchReceiver& receiver, const char** error) {
	if (busy_.load()) {
		*error = "busy: a fetch is in flight";
		return false;
	}
	post_length_ = 0;
	return start(url, receiver, error);
}

bool EspHttpFetcher::post(const char* url, const char* body, size_t length, FetchReceiver& receiver,
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
	return start(url, receiver, error);
}

bool EspHttpFetcher::start(const char* url, FetchReceiver& receiver, const char** error) {
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
	if (consumed_ == nullptr) {
		*error = "no hand-over semaphore";
		return false;
	}
	std::memcpy(url_, url, length + 1);
	receiver_ = &receiver;
	status_ = 0;
	announced_ = FetchReceiver::kUnknownLength;
	chunk_length_ = 0;
	delivered_ = 0;
	pieces_ = 0;
	begun_ = false;
	aborted_ = false;
	complete_ = false;
	error_[0] = '\0';
	xSemaphoreTake(consumed_, 0); // no stale token from an earlier fetch
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
	self->stack_unused_ = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
	// The last touch of `self`: the loop delivers the end and frees the fetcher for the next one.
	self->hand_over(kEnd);
	vTaskDelete(nullptr);
}

bool EspHttpFetcher::hand_over(Event event) {
	// Post until the queue takes it (it is full only for moments).
	while (!queue_.post(&EspHttpFetcher::on_event, this, event, nullptr, "HTTP fetch piece")) {
		vTaskDelay(pdMS_TO_TICKS(20));
	}
	if (event == kEnd) {
		return true; // nobody waits: the loop may free the fetcher from now on
	}
	xSemaphoreTake(consumed_, portMAX_DELAY);
	return answer_;
}

void EspHttpFetcher::on_event(void* context, const LoopMessage& message) {
	EspHttpFetcher* self = static_cast<EspHttpFetcher*>(context);
	switch (message.number) {
		case kBegin:
			self->answer_ = self->receiver_->fetch_begin(self->status_, self->announced_);
			break;
		case kChunk:
			self->answer_ = self->receiver_->fetch_chunk(self->chunk_, self->chunk_length_);
			break;
		default:
			self->deliver_end();
			return;
	}
	xSemaphoreGive(self->consumed_); // the worker reads on (or stops)
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
		if (announced < 0) {
			std::snprintf(error_, sizeof(error_), "no response headers");
		} else {
			status_ = esp_http_client_get_status_code(client);
			// 0: no Content-Length (or chunked); an empty body then ends at the first read.
			announced_ = announced > 0 && !esp_http_client_is_chunked_response(client) ? static_cast<size_t>(announced)
																						: FetchReceiver::kUnknownLength;
			begun_ = true;
			if (!hand_over(kBegin)) {
				aborted_ = true;
			} else {
				read_body(client);
			}
		}
	}
	esp_http_client_close(client);
	esp_http_client_cleanup(client);
}

void EspHttpFetcher::read_body(void* handle) {
	esp_http_client_handle_t client = static_cast<esp_http_client_handle_t>(handle);
	for (;;) {
		int n = esp_http_client_read(client, chunk_, static_cast<int>(kChunkBytes));
		if (n < 0) {
			std::snprintf(error_, sizeof(error_), "read error after %u bytes", static_cast<unsigned>(delivered_));
			return;
		}
		if (n == 0) {
			break;
		}
		chunk_length_ = static_cast<size_t>(n);
		++pieces_;
		if (!hand_over(kChunk)) {
			aborted_ = true;
			return;
		}
		delivered_ += chunk_length_;
	}
	if (announced_ != FetchReceiver::kUnknownLength ? delivered_ == announced_
													: esp_http_client_is_complete_data_received(client)) {
		complete_ = true;
	} else if (announced_ != FetchReceiver::kUnknownLength) {
		std::snprintf(error_, sizeof(error_), "body ended after %u of %u bytes", static_cast<unsigned>(delivered_),
			static_cast<unsigned>(announced_));
	} else {
		std::snprintf(error_, sizeof(error_), "body ended early after %u bytes", static_cast<unsigned>(delivered_));
	}
}

void EspHttpFetcher::deliver_end() {
	if (error_[0] == '\0') {
		if (begun_ && status_ != 200) {
			std::snprintf(error_, sizeof(error_), "HTTP status %d", status_);
		} else if (aborted_) {
			std::snprintf(error_, sizeof(error_), "aborted by the receiver");
		}
	}
	FetchResult result;
	result.http_status = status_;
	result.length = delivered_;
	result.aborted = aborted_;
	result.complete = complete_;
	result.ok = complete_ && status_ == 200 && error_[0] == '\0';
	result.error = error_;
	if (result.ok) {
		ESP_LOGD(kTag, "fetched %s: %u bytes in %u piece(s); task stack %u of %u B unused", url_,
			static_cast<unsigned>(delivered_), static_cast<unsigned>(pieces_), static_cast<unsigned>(stack_unused_),
			static_cast<unsigned>(kTaskStack));
	} else {
		ESP_LOGD(kTag, "fetch of %s failed after %u bytes: %s; task stack %u of %u B unused", url_,
			static_cast<unsigned>(delivered_), error_, static_cast<unsigned>(stack_unused_),
			static_cast<unsigned>(kTaskStack));
	}
	FetchReceiver* receiver = receiver_;
	receiver->fetch_end(result);
	// Busy until the receiver returned.
	busy_.store(false);
}

} // namespace esp
} // namespace hc
