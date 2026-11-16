/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_IMAGE_CONSOLE_H
#define HC_ESP_IMAGE_CONSOLE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hc/image/image_service.h"
#include "hc/platform/http_fetcher.h"

namespace hc {
namespace esp {

/// The console command `image` (runs on the event loop, like every console command):
///
///   image status        what is loaded, the slot, the last refusal, the loop's stack reserve
///   image load <url>    downloads a binary image (http or https) and deploys it like PUT /api/image
///   image clear         unloads the image, erases it with its persisted state (DELETE /api/image)
///
/// The download runs on its lane of the one fetch worker (FetchService, issue #45 item 13: first in
/// line); its pieces arrive on the event loop and go straight into the image slot (issue #87,
/// spec/runtime-image.md 6): the first min(32, length) bytes open the receive
/// (ImageService::receive_begin(), the header check; the running image stays when it refuses), the
/// rest is written as it arrives, and the end of the download applies the image
/// (ImageService::receive_end()) or, when it broke off, leaves no image (receive_abort()). The
/// server must announce the length (Content-Length). The signature is part of the binary image. One
/// download at a time.
class ImageConsole : private FetchReceiver {
public:
	static constexpr size_t kMaxUrl = 160;

	/// `fetcher`: the `image` lane of the FetchService (its timeout 10 s).
	ImageConsole(image::ImageService& service, HttpFetcher& fetcher) : service_(service), fetcher_(fetcher) {}

	ImageConsole(const ImageConsole&) = delete;
	ImageConsole& operator=(const ImageConsole&) = delete;

	/// Prints to stdout; 0 on success, 1 on a refusal (printed with its reason).
	int command(int argc, char** argv);

	static int run(void* context, int argc, char** argv) {
		return static_cast<ImageConsole*>(context)->command(argc, argv);
	}

private:
	/// The header's size: the bytes receive_begin() checks.
	static constexpr size_t kHead = 32;

	int status();
	int load(const char* url);
	int clear();

	// FetchReceiver
	bool fetch_begin(int http_status, size_t content_length) override;
	bool fetch_chunk(const char* data, size_t size) override;
	void fetch_end(const FetchResult& result) override;
	/// Opens the receive with the head and writes it; false (error_ set) when refused.
	bool begin_receive();
	void log_report(const char* what);

	image::ImageService& service_;
	HttpFetcher& fetcher_;
	std::atomic<bool> busy_{false};
	char url_[kMaxUrl + 1] = {};
	/// The announced length of the body.
	size_t length_ = 0;
	/// Body bytes taken so far.
	size_t received_ = 0;
	/// The first min(kHead, length_) bytes, until the receive is open.
	uint8_t head_[kHead] = {};
	size_t head_length_ = 0;
	/// The receive is open (receive_begin() accepted the header).
	bool open_ = false;
	/// receive_write() refused (the slot or an overrun): receive_end() reports it.
	bool write_failed_ = false;
	char error_[96] = {};
};

/// For the console's command table.
constexpr const char* kImageCommandHint = "status | load <url> | clear";
constexpr const char* kImageCommandHelp =
	"Runtime image (doc/image.md): show what is loaded and the slot, download a binary image from an http(s) URL "
	"and deploy it (signed with the installation key once the device holds one; the signature is in the image), "
	"or unload it and erase it with its persisted state";

} // namespace esp
} // namespace hc

#endif // HC_ESP_IMAGE_CONSOLE_H
