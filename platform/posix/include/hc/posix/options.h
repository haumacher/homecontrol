/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_OPTIONS_H
#define HC_POSIX_OPTIONS_H

#include <functional>
#include <string>

namespace hc {
namespace posix {

/// The default HTTP port of the controller on Linux (homecontrol #95): not 8080, which E-Home's
/// Tomcat takes on the same server, and not below 1024, which needs privileges. The WebSocket
/// listens on the port above (8082).
constexpr unsigned kDefaultHttpPort = 8081;

/// The address the web server and the WebSocket listen on by default: every IPv4 address.
constexpr const char* kDefaultBindAddress = "0.0.0.0";

/// The settings file by default, in the working directory (doc/network.md).
constexpr const char* kDefaultSettingsFile = "homecontrol.settings";

/// The environment of the systemd unit (/etc/default/homecontrol, doc/linux.md): the HTTP port
/// and the bind address. An empty value counts as unset; the command line wins over both.
constexpr const char* kEnvHttpPort = "HOMECONTROL_HTTP_PORT";
constexpr const char* kEnvHttpBind = "HOMECONTROL_HTTP_BIND";

/// What the command line and the environment say the controller runs with (main.cpp).
struct Options {
	enum class Action { run, help, version };

	Action action = Action::run;
	/// The settings file; the image slot, the blobs and the run marker live beside it.
	std::string settings_path = kDefaultSettingsFile;
	/// A binary runtime image loaded at start ("" for none).
	std::string image_path;
	/// The HTTP port, 0 for no web server; the WebSocket uses the next port.
	unsigned http_port = kDefaultHttpPort;
	/// A numeric IPv4 or IPv6 address the web server and the WebSocket bind to.
	std::string bind_address = kDefaultBindAddress;
	/// The directory of the web UI files ("" for the build's default).
	std::string www_dir;
	bool demo = false;
};

/// Looks up an environment variable (nullptr if unset); getenv in main, a map in the tests.
using Environment = std::function<const char*(const char*)>;

/// Reads the options: the defaults, then the environment (kEnvHttpPort, kEnvHttpBind), then the
/// command line
///
///     [--settings <file>] [--image <file>] [--http <port>] [--bind <address>] [--www <dir>] [--demo]
///     [--version] [--help]
///
/// False with the reason in `error` for an unknown argument, a missing value, a port outside
/// 0..65534 or an address that is not a numeric IPv4 or IPv6 address (each names its source: the
/// option or the environment variable), and for --demo together with --image.
bool parse_options(int argc, const char* const* argv, const Environment& env, Options& out, std::string& error);

/// Whether `address` is a numeric IPv4 or IPv6 address (no host name: the bind must not depend on
/// a name service at start).
bool bind_address_valid(const char* address);

/// The usage line of the executable.
std::string usage(const char* program);

} // namespace posix
} // namespace hc

#endif // HC_POSIX_OPTIONS_H
