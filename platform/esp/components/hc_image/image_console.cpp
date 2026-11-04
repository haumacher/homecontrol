/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "image_console.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/log.h"
#include "hc/signature.h"

namespace hc {
namespace esp {

namespace {
constexpr const char* kTag = "hc.image";
} // namespace

int ImageConsole::command(int argc, char** argv) {
	if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
		return status();
	}
	if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "load") == 0) {
		return load(argv[2], argc == 4 ? argv[3] : nullptr);
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

int ImageConsole::load(const char* url, const char* signature) {
	if (std::strncmp(url, "http://", 7) != 0 && std::strncmp(url, "https://", 8) != 0) {
		std::printf("refused: the URL must start with http:// or https://\n");
		return 1;
	}
	if (std::strlen(url) > kMaxUrl) {
		std::printf("refused: the URL is longer than %u characters\n", static_cast<unsigned>(kMaxUrl));
		return 1;
	}
	uint8_t parsed[kEd25519SignatureSize];
	if (signature != nullptr &&
		(std::strlen(signature) != 2 * kEd25519SignatureSize || !parse_signature(signature, std::strlen(signature), parsed))) {
		std::printf("refused: the signature is the image's Ed25519 signature in 128 hex digits\n");
		return 1;
	}
	if (busy_.exchange(true)) {
		std::printf("refused: a download is in progress (%s)\n", url_);
		return 1;
	}
	has_signature_ = signature != nullptr;
	if (has_signature_) {
		std::memcpy(signature_, parsed, sizeof(signature_));
	}
	std::snprintf(url_, sizeof(url_), "%s", url);
	buffer_ = nullptr;
	length_ = 0;
	error_[0] = '\0';
	capacity_ = 0;
	const char* error = nullptr;
	if (!fetcher_.fetch(url_, *this, &error)) {
		busy_.store(false);
		std::printf("refused: %s\n", error);
		return 1;
	}
	std::printf("downloading %s on the fetch worker; the log reports the outcome\n", url_);
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

bool ImageConsole::fetch_begin(int http_status, size_t content_length) {
	if (http_status != 200) {
		std::snprintf(error_, sizeof(error_), "HTTP status %d", http_status);
		return false;
	}
	bool announced = content_length != FetchReceiver::kUnknownLength;
	if (announced && content_length > image::kMaxImageBytes) {
		std::snprintf(error_, sizeof(error_), "the image has %u bytes, the limit is %u",
			static_cast<unsigned>(content_length), static_cast<unsigned>(image::kMaxImageBytes));
		return false;
	}
	capacity_ = announced && content_length > 0 ? content_length : kGrowth;
	buffer_ = static_cast<char*>(std::malloc(capacity_));
	if (buffer_ == nullptr) {
		std::snprintf(error_, sizeof(error_), "no memory for %u bytes", static_cast<unsigned>(capacity_));
		return false;
	}
	announced_ = announced;
	return true;
}

bool ImageConsole::fetch_chunk(const char* data, size_t size) {
	if (length_ + size > capacity_) {
		size_t grown = capacity_ + (size > kGrowth ? size : kGrowth);
		if (announced_ || grown > image::kMaxImageBytes) {
			std::snprintf(error_, sizeof(error_), "the image is longer than %s",
				announced_ ? "announced" : "the limit");
			return false;
		}
		char* bigger = static_cast<char*>(std::realloc(buffer_, grown));
		if (bigger == nullptr) {
			std::snprintf(error_, sizeof(error_), "no memory for %u bytes", static_cast<unsigned>(grown));
			return false;
		}
		buffer_ = bigger;
		capacity_ = grown;
	}
	std::memcpy(buffer_ + length_, data, size);
	length_ += size;
	return true;
}

void ImageConsole::fetch_end(const FetchResult& result) {
	if (error_[0] == '\0' && !result.ok) {
		std::snprintf(error_, sizeof(error_), "%s", result.error);
	}
	finish();
}

void ImageConsole::finish() {
	if (error_[0] != '\0') {
		Log::warn(kTag, "image download from %s failed: %s", url_, error_);
	} else {
		Log::info(kTag, "image downloaded from %s: %u bytes", url_, static_cast<unsigned>(length_));
		// The service may keep the buffer for its store off the loop (#68); otherwise it is freed below.
		image::ImageBody body;
		body.adopt(buffer_, length_);
		buffer_ = nullptr;
		if (!service_.load(body.text(), length_, image::Source::console, true, has_signature_ ? signature_ : nullptr,
				&body)) {
			Log::warn(kTag, "the downloaded image was refused (`image status` lists the problems)");
		} else if (service_.store_pending()) {
			Log::info(kTag, "the image runs; it is being stored (the log says when it is)");
		}
	}
	std::free(buffer_);
	buffer_ = nullptr;
	length_ = 0;
	busy_.store(false);
}

} // namespace esp
} // namespace hc
