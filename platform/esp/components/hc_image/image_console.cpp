/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "image_console.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/log.h"

namespace hc {
namespace esp {

namespace {
constexpr const char* kTag = "hc.image";
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
		std::printf("image '%s' revision %u (%s), schema %s, format %u.%u, from %s%s\n", info.project_id,
			static_cast<unsigned>(info.revision), info.name[0] != '\0' ? info.name : "unnamed", info.schema_version,
			static_cast<unsigned>(info.format_major), static_cast<unsigned>(info.format_minor),
			image::source_name(service_.source()), service_.image_degraded() ? ", DEGRADED (KNX tunnel down)" : "");
		std::printf("  %u bytes (CRC %08x, %s); %u datapoints (%u KNX, %u Zigbee), %u rules, %u blocks, "
			"%u schedules, %u functions, %u locations, %u history rings%s\n", static_cast<unsigned>(info.bytes),
			static_cast<unsigned>(info.crc), info.is_signed ? "signed" : "unsigned",
			static_cast<unsigned>(info.datapoints), static_cast<unsigned>(info.knx_datapoints),
			static_cast<unsigned>(info.zigbee_datapoints), static_cast<unsigned>(info.rules),
			static_cast<unsigned>(info.blocks), static_cast<unsigned>(info.schedules),
			static_cast<unsigned>(info.functions), static_cast<unsigned>(info.locations),
			static_cast<unsigned>(info.history_rings), info.time_master ? ", time master" : "");
		if (info.generator[0] != '\0' || info.generated_at[0] != '\0') {
			std::printf("  generated %s by %s\n", info.generated_at, info.generator);
		}
	} else {
		std::printf("no image loaded: the LED shows setup; PUT /api/image or `image load <url>` deploys one\n");
	}
	std::printf("  slot: %u KB%s%s\n", static_cast<unsigned>(service_.slot_size() / 1024),
		service_.slot_size() == 0 ? " (no partition 'image': flash this firmware's partition table)" : "",
		service_.receiving() ? ", an image is being received" : "");
	if (service_.swap_kept()) {
		std::printf("  values of the last image are kept for the next one\n");
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
		std::printf("download in progress: %s (%u of %u bytes)\n", url_, static_cast<unsigned>(received_),
			static_cast<unsigned>(length_));
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
	if (service_.receiving()) {
		std::printf("refused: another image is being received\n");
		return 1;
	}
	if (busy_.exchange(true)) {
		std::printf("refused: a download is in progress (%s)\n", url_);
		return 1;
	}
	std::snprintf(url_, sizeof(url_), "%s", url);
	length_ = 0;
	received_ = 0;
	head_length_ = 0;
	open_ = false;
	write_failed_ = false;
	error_[0] = '\0';
	const char* error = nullptr;
	if (!fetcher_.fetch(url_, *this, &error)) {
		busy_.store(false);
		std::printf("refused: %s\n", error);
		return 1;
	}
	std::printf("downloading %s on the fetch worker into the image slot; the log reports the outcome\n", url_);
	return 0;
}

int ImageConsole::clear() {
	const char* error = nullptr;
	if (!service_.unload(&error)) {
		std::printf("refused or incomplete: %s\n", error != nullptr ? error : "(no reason)");
		return 1;
	}
	std::printf("image cleared: no image, the slot erased, persisted values and history removed\n");
	return 0;
}

bool ImageConsole::fetch_begin(int http_status, size_t content_length) {
	if (http_status != 200) {
		std::snprintf(error_, sizeof(error_), "HTTP status %d", http_status);
		return false;
	}
	if (content_length == FetchReceiver::kUnknownLength) {
		std::snprintf(error_, sizeof(error_), "the server announced no length (Content-Length)");
		return false;
	}
	if (content_length > service_.slot_size()) {
		std::snprintf(error_, sizeof(error_), "the image has %u bytes; the device holds at most %u (its image slot)",
			static_cast<unsigned>(content_length), static_cast<unsigned>(service_.slot_size()));
		return false;
	}
	length_ = content_length;
	return true;
}

bool ImageConsole::begin_receive() {
	if (!service_.receive_begin(head_, head_length_, length_, image::Source::console)) {
		std::snprintf(error_, sizeof(error_), "refused from its header; the running image is untouched");
		return false;
	}
	open_ = true;
	if (!service_.receive_write(head_, head_length_)) {
		write_failed_ = true;
		return false;
	}
	return true;
}

bool ImageConsole::fetch_chunk(const char* data, size_t size) {
	const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
	received_ += size;
	if (!open_) {
		size_t want = (length_ < kHead ? length_ : kHead) - head_length_;
		size_t take = size < want ? size : want;
		std::memcpy(head_ + head_length_, bytes, take);
		head_length_ += take;
		bytes += take;
		size -= take;
		if (head_length_ < (length_ < kHead ? length_ : kHead)) {
			return true;
		}
		if (!begin_receive()) {
			return false;
		}
		if (size == 0) {
			return true;
		}
	}
	if (!service_.receive_write(bytes, size)) {
		write_failed_ = true;
		return false;
	}
	return true;
}

void ImageConsole::log_report(const char* what) {
	const image::Report& report = service_.report();
	if (report.count > 0) {
		Log::warn(kTag, "%s: %s: %s (`image status` lists every problem)", what,
			report.refusals[0].path[0] != '\0' ? report.refusals[0].path : "(image)", report.refusals[0].reason);
	} else {
		Log::warn(kTag, "%s (`image status` lists the problems)", what);
	}
}

void ImageConsole::fetch_end(const FetchResult& result) {
	if (!open_ && error_[0] == '\0' && result.ok) {
		// A body shorter than the header: the header check refuses it with its length.
		begin_receive();
	}
	if (open_) {
		if (write_failed_ || result.ok) {
			// receive_end() reports a write the slot refused or a body that overran.
			if (service_.receive_end()) {
				Log::info(kTag, "image from %s deployed: %u bytes", url_, static_cast<unsigned>(received_));
			} else {
				log_report("the downloaded image was refused; the device has no image");
			}
		} else {
			service_.receive_abort();
			Log::warn(kTag, "image download from %s broke off after %u of %u bytes (%s): the device has no image",
				url_, static_cast<unsigned>(received_), static_cast<unsigned>(length_), result.error);
		}
	} else if (error_[0] != '\0') {
		if (service_.report().ok() || std::strncmp(error_, "refused", 7) != 0) {
			Log::warn(kTag, "image download from %s failed: %s", url_, error_);
		} else {
			log_report("the downloaded image was refused from its header; the running image is untouched");
		}
	} else {
		Log::warn(kTag, "image download from %s failed: %s", url_, result.error);
	}
	open_ = false;
	busy_.store(false);
}

} // namespace esp
} // namespace hc
