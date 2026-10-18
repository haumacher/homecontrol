/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/httplib_fetcher.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "hc/log.h"
#include "httplib.h"

namespace hc {
namespace posix {

namespace {

constexpr const char* kTag = "hc.ext";

} // namespace

HttplibFetcher::HttplibFetcher(LoopExecutor& executor) : executor_(executor) {
	thread_ = std::thread([this] { worker(); });
}

HttplibFetcher::~HttplibFetcher() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	wake_.notify_all();
	if (thread_.joinable()) {
		thread_.join();
	}
}

bool HttplibFetcher::https_supported() {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
	return true;
#else
	return false;
#endif
}

bool HttplibFetcher::fetch(const char* url, size_t max_bytes, FetchReceiver& receiver, const char** error) {
	post_ = false;
	return start(url, max_bytes, receiver, error);
}

bool HttplibFetcher::post(const char* url, const char* body, size_t length, size_t max_bytes, FetchReceiver& receiver,
	const char** error) {
	if (busy_.load()) {
		*error = "busy: a fetch is in flight";
		return false;
	}
	if (length > kMaxPostBody) {
		*error = "the request body is longer than 1024 bytes";
		return false;
	}
	std::memcpy(post_body_, body, length);
	post_length_ = length;
	post_ = true;
	return start(url, max_bytes, receiver, error);
}

bool HttplibFetcher::start(const char* url, size_t max_bytes, FetchReceiver& receiver, const char** error) {
	if (busy_.load()) {
		*error = "busy: a fetch is in flight";
		return false;
	}
	size_t length = std::strlen(url);
	if (length > kMaxUrl) {
		*error = "URL longer than 383 bytes";
		return false;
	}
	if (std::strncmp(url, "https://", 8) == 0) {
		if (!https_supported()) {
			*error = "https:// needs OpenSSL, which this POSIX build does not have (install its development files "
				"and reconfigure, or use http://)";
			return false;
		}
	} else if (std::strncmp(url, "http://", 7) != 0) {
		*error = "not an http:// or https:// URL";
		return false;
	}
	std::memcpy(url_, url, length + 1);
	max_bytes_ = max_bytes < kBufferBytes ? max_bytes : kBufferBytes;
	receiver_ = &receiver;
	busy_.store(true);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pending_ = true;
	}
	wake_.notify_all();
	return true;
}

void HttplibFetcher::worker() {
	for (;;) {
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return pending_ || stop_; });
			if (stop_) {
				return;
			}
			pending_ = false;
		}
		run_request();
	}
}

void HttplibFetcher::run_request() {
	// Split "scheme://host[:port]" from the path and query.
	const char* after_scheme = std::strstr(url_, "://") + 3;
	const char* slash = std::strchr(after_scheme, '/');
	std::string origin = slash != nullptr ? std::string(url_, static_cast<size_t>(slash - url_)) : std::string(url_);
	std::string path = slash != nullptr ? std::string(slash) : std::string("/");

	FetchResult result;
	size_t length = 0;
	bool too_large = false;
	error_[0] = '\0';
	httplib::Client client(origin);
	if (!client.is_valid()) {
		std::snprintf(error_, sizeof(error_), "URL refused by the HTTP client");
	} else {
		client.set_connection_timeout(kConnectTimeoutS);
		client.set_read_timeout(kReadTimeoutS);
		client.set_follow_location(true);
		int status = 0;
		if (post_) {
			// The enrollment: the answer's body whatever the status (E-Home's refusal document).
			httplib::Result answer = client.Post(path, std::string(post_body_, post_length_), "application/json");
			if (!answer) {
				std::snprintf(error_, sizeof(error_), "cannot connect: %s", httplib::to_string(answer.error()).c_str());
			} else {
				status = answer->status;
				length = answer->body.size() < max_bytes_ ? answer->body.size() : max_bytes_;
				std::memcpy(body_, answer->body.data(), length);
				if (status != 200) {
					std::snprintf(error_, sizeof(error_), "HTTP status %d", status);
				} else if (answer->body.size() > max_bytes_) {
					std::snprintf(error_, sizeof(error_), "body larger than %zu bytes", max_bytes_);
				}
			}
			result.http_status = status;
			result.ok = error_[0] == '\0';
			result.body = body_;
			result.length = length;
			result.error = error_;
			FetchReceiver* receiver = receiver_;
			if (!executor_.run([&] {
					receiver->fetch_done(result);
					busy_.store(false);
				})) {
				busy_.store(false);
			}
			return;
		}
		httplib::Result response = client.Get(
			path,
			[&](const httplib::Response& head) {
				status = head.status;
				// The body of an error status is not read (a redirect is followed).
				return status == 200 || (status >= 300 && status < 400);
			},
			[&](const char* data, size_t n) {
				if (length + n > max_bytes_) {
					too_large = true;
					return false;
				}
				std::memcpy(body_ + length, data, n);
				length += n;
				return true;
			});
		result.http_status = status;
		if (status != 0 && status != 200) {
			std::snprintf(error_, sizeof(error_), "HTTP status %d", status);
		} else if (too_large) {
			std::snprintf(error_, sizeof(error_), "body larger than %zu bytes", max_bytes_);
		} else if (!response) {
			std::snprintf(error_, sizeof(error_), "cannot fetch: %s", httplib::to_string(response.error()).c_str());
		}
	}
	result.ok = error_[0] == '\0';
	result.body = result.ok ? body_ : nullptr;
	result.length = result.ok ? length : 0;
	result.error = error_;
	FetchReceiver* receiver = receiver_;
	bool delivered = executor_.run([&] {
		receiver->fetch_done(result);
		busy_.store(false); // busy until the receiver returned: the body stays valid during its call
	});
	if (!delivered) {
		busy_.store(false);
		Log::info(kTag, "fetch result not delivered: the loop stopped");
	}
}

} // namespace posix
} // namespace hc
