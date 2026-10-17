/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "image_console.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "hc/log.h"

namespace hc {
namespace esp {

namespace {
constexpr const char* kTag = "hc.image";
constexpr UBaseType_t kTaskPriority = 4;
} // namespace

int ImageConsole::command(int argc, char** argv) {
	if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
		return status();
	}
	if (argc == 3 && std::strcmp(argv[1], "load") == 0) {
		return load(argv[2]);
	}
	if (argc == 2 && std::strcmp(argv[1], "clear") == 0) {
		return clear();
	}
	std::printf("usage: image %s\n", kImageCommandHint);
	return 1;
}

int ImageConsole::status() {
	const image::ImageInfo& info = service_.info();
	if (service_.loaded()) {
		std::printf("image '%s' revision %u (%s), schema %s, from %s%s\n", info.project_id,
			static_cast<unsigned>(info.revision), info.name[0] != '\0' ? info.name : "unnamed", info.schema_version,
			image::source_name(service_.source()), service_.image_degraded() ? ", DEGRADED (KNX tunnel down)" : "");
		std::printf("  %u bytes; %u datapoints (%u KNX), %u rules, %u schedules, %u functions, %u locations, "
			"%u history rings%s\n", static_cast<unsigned>(info.bytes), static_cast<unsigned>(info.datapoints),
			static_cast<unsigned>(info.knx_datapoints), static_cast<unsigned>(info.rules),
			static_cast<unsigned>(info.schedules), static_cast<unsigned>(info.functions),
			static_cast<unsigned>(info.locations), static_cast<unsigned>(info.history_rings),
			info.time_master ? ", time master" : "");
		if (info.generator[0] != '\0' || info.generated_at[0] != '\0') {
			std::printf("  generated %s by %s\n", info.generated_at, info.generator);
		}
	} else {
		std::printf("no image loaded: the LED shows setup; PUT /api/image or `image load <url>` deploys one\n");
	}
	size_t bytes = 0;
	if (service_.stored(bytes)) {
		std::printf("  stored copy: %u bytes (RAM stand-in: lost on restart)\n", static_cast<unsigned>(bytes));
	} else if (service_.loaded()) {
		std::printf("  not stored%s%s\n", service_.store_error()[0] != '\0' ? ": " : "", service_.store_error());
	}
	const image::Report& report = service_.report();
	if (!report.ok()) {
		std::printf("last refusal (%s), %u problem%s:\n", image::report_kind_name(report.kind),
			static_cast<unsigned>(report.total), report.total == 1 ? "" : "s");
		for (size_t i = 0; i < report.count; ++i) {
			std::printf("  %s: %s\n", report.refusals[i].path[0] != '\0' ? report.refusals[i].path : "(image)",
				report.refusals[i].reason);
		}
	}
	if (busy_.load()) {
		std::printf("download in progress: %s\n", url_);
	}
	// The console runs on the event loop: this is the loop task's reserve.
	std::printf("event-loop stack: %u B never used\n", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
	return 0;
}

int ImageConsole::load(const char* url) {
	if (std::strncmp(url, "http://", 7) != 0 && std::strncmp(url, "https://", 8) != 0) {
		std::printf("refused: the URL must start with http:// or https://\n");
		return 1;
	}
	if (std::strlen(url) > kMaxUrl) {
		std::printf("refused: the URL is longer than %u characters\n", static_cast<unsigned>(kMaxUrl));
		return 1;
	}
	if (busy_.exchange(true)) {
		std::printf("refused: a download is in progress (%s)\n", url_);
		return 1;
	}
	std::snprintf(url_, sizeof(url_), "%s", url);
	buffer_ = nullptr;
	length_ = 0;
	error_[0] = '\0';
	if (xTaskCreate(&ImageConsole::download_task, "hc_image_get", kTaskStack, this, kTaskPriority, nullptr) != pdPASS) {
		busy_.store(false);
		std::printf("refused: cannot create the download task (memory)\n");
		return 1;
	}
	std::printf("downloading %s; the log reports the outcome\n", url_);
	return 0;
}

int ImageConsole::clear() {
	const char* error = nullptr;
	if (!service_.unload(&error)) {
		std::printf("the image is unloaded, but its stored copy was not removed: %s\n", error);
		return 1;
	}
	std::printf("image cleared: no image, persisted values and history removed\n");
	return 0;
}

void ImageConsole::download_task(void* context) {
	ImageConsole* self = static_cast<ImageConsole*>(context);
	self->download();
	if (!self->queue_.post(&ImageConsole::downloaded, self, 0, nullptr, "image download")) {
		std::free(self->buffer_);
		self->buffer_ = nullptr;
		self->busy_.store(false);
	}
	vTaskDelete(nullptr);
}

void ImageConsole::download() {
	esp_http_client_config_t config = {};
	config.url = url_;
	config.timeout_ms = kHttpTimeoutMs;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
	config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (client == nullptr) {
		std::snprintf(error_, sizeof(error_), "HTTP client not created");
		return;
	}
	esp_err_t err = esp_http_client_open(client, 0);
	if (err != ESP_OK) {
		std::snprintf(error_, sizeof(error_), "cannot connect: %s", esp_err_to_name(err));
	} else {
		int64_t announced = esp_http_client_fetch_headers(client);
		int status = esp_http_client_get_status_code(client);
		size_t capacity = announced > 0 ? static_cast<size_t>(announced) : kGrowth;
		if (status != 200) {
			std::snprintf(error_, sizeof(error_), "HTTP status %d", status);
		} else if (announced > static_cast<int64_t>(image::kMaxImageBytes)) {
			std::snprintf(error_, sizeof(error_), "the image has %lld bytes, the limit is %u",
				static_cast<long long>(announced), static_cast<unsigned>(image::kMaxImageBytes));
		} else if ((buffer_ = static_cast<char*>(std::malloc(capacity))) == nullptr) {
			std::snprintf(error_, sizeof(error_), "no memory for %u bytes", static_cast<unsigned>(capacity));
		} else {
			for (;;) {
				if (length_ == capacity) {
					if (announced > 0 || capacity >= image::kMaxImageBytes) {
						break; // all announced bytes, or the limit: the loader refuses what is too long
					}
					size_t grown = capacity + kGrowth;
					char* bigger = static_cast<char*>(std::realloc(buffer_, grown));
					if (bigger == nullptr) {
						std::snprintf(error_, sizeof(error_), "no memory for %u bytes", static_cast<unsigned>(grown));
						break;
					}
					buffer_ = bigger;
					capacity = grown;
				}
				int n = esp_http_client_read(client, buffer_ + length_, static_cast<int>(capacity - length_));
				if (n < 0) {
					std::snprintf(error_, sizeof(error_), "read error after %u bytes", static_cast<unsigned>(length_));
					break;
				}
				if (n == 0) {
					break;
				}
				length_ += static_cast<size_t>(n);
			}
		}
	}
	esp_http_client_close(client);
	esp_http_client_cleanup(client);
}

void ImageConsole::downloaded(void* context, const LoopMessage&) {
	static_cast<ImageConsole*>(context)->finish();
}

void ImageConsole::finish() {
	if (error_[0] != '\0') {
		Log::warn(kTag, "image download from %s failed: %s", url_, error_);
	} else {
		Log::info(kTag, "image downloaded from %s: %u bytes", url_, static_cast<unsigned>(length_));
		if (!service_.load(buffer_, length_, image::Source::console, true)) {
			Log::warn(kTag, "the downloaded image was refused (`image status` lists the problems)");
		}
	}
	std::free(buffer_);
	buffer_ = nullptr;
	length_ = 0;
	busy_.store(false);
}

} // namespace esp
} // namespace hc
