/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/options.h"

#include <arpa/inet.h>

#include <cstdlib>
#include <cstring>

namespace hc {
namespace posix {

namespace {

/// Parses a port 0..65534 (the WebSocket takes the one above); false for anything else.
bool parse_port(const char* text, unsigned& out) {
	if (text == nullptr || *text < '0' || *text > '9') {
		return false;
	}
	char* end = nullptr;
	unsigned long port = std::strtoul(text, &end, 10);
	if (*end != '\0' || port > 65534) {
		return false;
	}
	out = static_cast<unsigned>(port);
	return true;
}

const char* set(const Environment& env, const char* name) {
	const char* value = env ? env(name) : nullptr;
	return value != nullptr && *value != '\0' ? value : nullptr;
}

} // namespace

bool bind_address_valid(const char* address) {
	if (address == nullptr || *address == '\0') {
		return false;
	}
	unsigned char buffer[16];
	return inet_pton(AF_INET, address, buffer) == 1 || inet_pton(AF_INET6, address, buffer) == 1;
}

std::string usage(const char* program) {
	return std::string("usage: ") + program +
		" [--settings <file>] [--image <file>] [--http <port>] [--bind <address>] [--www <dir>] [--demo] [--version]";
}

bool parse_options(int argc, const char* const* argv, const Environment& env, Options& out, std::string& error) {
	if (const char* port = set(env, kEnvHttpPort)) {
		if (!parse_port(port, out.http_port)) {
			error = std::string(kEnvHttpPort) + "=" + port +
				": the port is 0..65534 (0: no web server; the WebSocket uses the next port)";
			return false;
		}
	}
	if (const char* bind = set(env, kEnvHttpBind)) {
		if (!bind_address_valid(bind)) {
			error = std::string(kEnvHttpBind) + "=" + bind + ": not a numeric IPv4 or IPv6 address";
			return false;
		}
		out.bind_address = bind;
	}
	for (int i = 1; i < argc; ++i) {
		const char* arg = argv[i];
		bool takes_value = std::strcmp(arg, "--settings") == 0 || std::strcmp(arg, "--image") == 0 ||
			std::strcmp(arg, "--http") == 0 || std::strcmp(arg, "--bind") == 0 || std::strcmp(arg, "--www") == 0;
		if (takes_value) {
			if (i + 1 >= argc) {
				error = std::string(arg) + " needs an argument";
				return false;
			}
			const char* value = argv[++i];
			if (std::strcmp(arg, "--settings") == 0) {
				out.settings_path = value;
			} else if (std::strcmp(arg, "--image") == 0) {
				out.image_path = value;
			} else if (std::strcmp(arg, "--www") == 0) {
				out.www_dir = value;
			} else if (std::strcmp(arg, "--http") == 0) {
				if (!parse_port(value, out.http_port)) {
					error = "--http takes a port 0..65534 (0: no web server; the WebSocket uses the next port)";
					return false;
				}
			} else {
				if (!bind_address_valid(value)) {
					error = std::string("--bind ") + value + ": not a numeric IPv4 or IPv6 address";
					return false;
				}
				out.bind_address = value;
			}
			continue;
		}
		if (std::strcmp(arg, "--demo") == 0) {
			out.demo = true;
			continue;
		}
		if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
			out.action = Options::Action::help;
			return true;
		}
		if (std::strcmp(arg, "--version") == 0) {
			out.action = Options::Action::version;
			return true;
		}
		error = std::string("unknown argument: ") + arg;
		return false;
	}
	if (out.demo && !out.image_path.empty()) {
		error = "--demo and --image both fill the datapoint model: choose one";
		return false;
	}
	return true;
}

} // namespace posix
} // namespace hc
