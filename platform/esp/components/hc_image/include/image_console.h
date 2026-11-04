/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_IMAGE_CONSOLE_H
#define HC_ESP_IMAGE_CONSOLE_H

#include <atomic>
#include <cstddef>

#include "hc/image/image_service.h"
#include "hc/platform/http_fetcher.h"
#include "hc/signature.h"

namespace hc {
namespace esp {

/// The console command `image` (runs on the event loop, like every console command):
///
///   image status        what is loaded, the stored copy, the last refusal, the loop's stack reserve
///   image load <url>    downloads an image (http or https) and loads it like PUT /api/image
///   image clear         unloads the image and removes it with its persisted state (DELETE /api/image)
///
/// The download runs on its lane of the one fetch worker (FetchService, issue #45 item 13: first in
/// line) into a heap buffer of the announced length (at most image::kMaxImageBytes, grown in
/// kGrowth steps without one); the event loop then loads it and frees the buffer. One at a time.
class ImageConsole : private FetchReceiver {
public:
	/// Buffer growth when the server sends no Content-Length.
	static constexpr size_t kGrowth = 16384;
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
	int status();
	int load(const char* url, const char* signature);
	int clear();

	// FetchReceiver
	bool fetch_begin(int http_status, size_t content_length) override;
	bool fetch_chunk(const char* data, size_t size) override;
	void fetch_end(const FetchResult& result) override;
	void finish();

	image::ImageService& service_;
	HttpFetcher& fetcher_;
	std::atomic<bool> busy_{false};
	char url_[kMaxUrl + 1] = {};
	char* buffer_ = nullptr;
	size_t length_ = 0;
	size_t capacity_ = 0;
	bool announced_ = false;
	/// The image's signature from the command line (spec/api.md "Signed runtime images").
	uint8_t signature_[kEd25519SignatureSize] = {};
	bool has_signature_ = false;
	char error_[96] = {};
};

/// For the console's command table.
constexpr const char* kImageCommandHint = "status | load <url> [<signature>] | clear";
constexpr const char* kImageCommandHelp =
	"Runtime image (doc/image.md): show what is loaded, download and load an image from an http(s) URL (with "
	"its Ed25519 signature in 128 hex digits once the device holds the installation key), or unload it and "
	"remove its persisted state";

} // namespace esp
} // namespace hc

#endif // HC_ESP_IMAGE_CONSOLE_H
