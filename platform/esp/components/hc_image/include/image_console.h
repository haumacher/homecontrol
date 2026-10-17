/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_IMAGE_CONSOLE_H
#define HC_ESP_IMAGE_CONSOLE_H

#include <atomic>
#include <cstddef>

#include "hc/image/image_service.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The console command `image` (runs on the event loop, like every console command):
///
///   image status        what is loaded, the stored copy, the last refusal, the loop's stack reserve
///   image load <url>    downloads an image (http or https) and loads it like PUT /api/image
///   image clear         unloads the image and removes it with its persisted state (DELETE /api/image)
///
/// The download runs in a task of its own into a heap buffer of the announced length (at most
/// image::kMaxImageBytes); the event loop then loads it and frees the buffer. One at a time.
class ImageConsole {
public:
	static constexpr uint32_t kTaskStack = 6144;
	static constexpr uint32_t kHttpTimeoutMs = 10000;
	/// Buffer growth when the server sends no Content-Length.
	static constexpr size_t kGrowth = 16384;
	static constexpr size_t kMaxUrl = 160;

	ImageConsole(image::ImageService& service, LoopQueue& queue) : service_(service), queue_(queue) {}

	ImageConsole(const ImageConsole&) = delete;
	ImageConsole& operator=(const ImageConsole&) = delete;

	/// Prints to stdout; 0 on success, 1 on a refusal (printed with its reason).
	int command(int argc, char** argv);

	static int run(void* context, int argc, char** argv) {
		return static_cast<ImageConsole*>(context)->command(argc, argv);
	}

private:
	int status();
	int load(const char* url);
	int clear();

	static void download_task(void* context);
	static void downloaded(void* context, const LoopMessage& message);
	void download();
	void finish();

	image::ImageService& service_;
	LoopQueue& queue_;
	std::atomic<bool> busy_{false};
	char url_[kMaxUrl + 1] = {};
	char* buffer_ = nullptr;
	size_t length_ = 0;
	char error_[96] = {};
};

/// For the console's command table.
constexpr const char* kImageCommandHint = "status | load <url> | clear";
constexpr const char* kImageCommandHelp =
	"Runtime image (doc/image.md): show what is loaded, download and load an image from an http(s) URL, or "
	"unload it and remove its persisted state";

} // namespace esp
} // namespace hc

#endif // HC_ESP_IMAGE_CONSOLE_H
