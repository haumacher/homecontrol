/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/httplib_fetcher.h"

#include <cstdio>
#include <cstring>
#include <functional>
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

bool HttplibFetcher::fetch(const char* url, FetchReceiver& receiver, const char** error) {
	if (busy_.load()) {
		*error = "busy: a fetch is in flight";
		return false;
	}
	post_ = false;
	return start(url, receiver, error);
}

bool HttplibFetcher::post(const char* url, const char* body, size_t length, FetchReceiver& receiver,
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
	return start(url, receiver, error);
}

bool HttplibFetcher::start(const char* url, FetchReceiver& receiver, const char** error) {
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

	FetchReceiver* receiver = receiver_;
	int status = 0;
	size_t announced = FetchReceiver::kUnknownLength;
	size_t length = 0;
	bool begun = false;   // fetch_begin() ran
	bool aborted = false; // the receiver said no
	bool stopped = false; // the loop stopped: nothing more is delivered
	bool transported = false;
	error_[0] = '\0';

	// Runs one receiver call on the loop and waits for it.
	auto on_loop = [&](const std::function<bool()>& call) {
		bool go = false;
		if (!executor_.run([&] { go = call(); })) {
			stopped = true;
			return false;
		}
		if (!go) {
			aborted = true;
		}
		return go;
	};
	auto begin = [&](const httplib::Response& head) {
		status = head.status;
		if (head.has_header("Content-Length")) {
			announced = static_cast<size_t>(head.get_header_value_u64("Content-Length"));
		}
		begun = true;
		return on_loop([&] { return receiver->fetch_begin(status, announced); });
	};

	httplib::Client client(origin);
	if (!client.is_valid()) {
		std::snprintf(error_, sizeof(error_), "URL refused by the HTTP client");
	} else {
		client.set_connection_timeout(kConnectTimeoutS);
		client.set_read_timeout(kReadTimeoutS);
		client.set_follow_location(true);
		httplib::Request request;
		request.method = post_ ? "POST" : "GET";
		request.path = path;
		if (post_) {
			request.body.assign(post_body_, post_length_);
			request.set_header("Content-Type", "application/json");
		}
		// Not called for a redirect that is followed, nor for a response without a body (204).
		request.response_handler = [&](const httplib::Response& head) { return begin(head); };
		request.content_receiver = [&](const char* data, size_t n, size_t, size_t) {
			if (!on_loop([&] { return receiver->fetch_chunk(data, n); })) {
				return false;
			}
			length += n;
			return true;
		};
		httplib::Result response = client.send(request);
		if (response && !begun && !stopped) {
			begin(*response);
		}
		transported = static_cast<bool>(response);
		if (!response && !aborted && !stopped) {
			std::snprintf(error_, sizeof(error_), "%s: %s", begun ? "read error" : "cannot connect",
				httplib::to_string(response.error()).c_str());
		}
	}
	if (stopped) {
		busy_.store(false);
		Log::info(kTag, "fetch result not delivered: the loop stopped");
		return;
	}
	FetchResult result;
	result.http_status = status;
	result.length = length;
	result.aborted = aborted;
	result.complete = transported && begun && !aborted &&
		(announced == FetchReceiver::kUnknownLength || length == announced);
	if (error_[0] == '\0') {
		if (transported && begun && !aborted && !result.complete) {
			std::snprintf(error_, sizeof(error_), "body ended after %zu of %zu bytes", length, announced);
		} else if (begun && status != 200) {
			std::snprintf(error_, sizeof(error_), "HTTP status %d", status);
		} else if (aborted) {
			std::snprintf(error_, sizeof(error_), "aborted by the receiver");
		}
	}
	result.ok = result.complete && status == 200 && error_[0] == '\0';
	result.error = error_;
	bool delivered = executor_.run([&] {
		receiver->fetch_end(result);
		busy_.store(false); // busy until the receiver returned
	});
	if (!delivered) {
		busy_.store(false);
		Log::info(kTag, "fetch result not delivered: the loop stopped");
	}
}

} // namespace posix
} // namespace hc
