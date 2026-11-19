/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/posix/options.h"

// The controller's options on Linux (homecontrol #95, doc/linux.md): the defaults, the environment
// of the systemd unit (/etc/default/homecontrol) and the command line, which wins.

namespace {

using hc::posix::Options;

struct Run {
	std::map<std::string, std::string> env;
	Options options;
	std::string error;

	bool parse(std::vector<const char*> args) {
		args.insert(args.begin(), "homecontrol");
		options = Options();
		error.clear();
		return hc::posix::parse_options(static_cast<int>(args.size()), args.data(),
			[this](const char* name) -> const char* {
				auto it = env.find(name);
				return it == env.end() ? nullptr : it->second.c_str();
			},
			options, error);
	}
};

} // namespace

TEST_CASE("PosixOptions.DefaultsAreThePackagePortsAndEveryIpv4Address") {
	Run run;
	REQUIRE(run.parse({}));
	CHECK(run.options.action == Options::Action::run);
	CHECK(run.options.http_port == 8081); // E-Home's Tomcat has 8080; the WebSocket is on 8082
	CHECK(run.options.bind_address == "0.0.0.0");
	CHECK(run.options.settings_path == "homecontrol.settings");
	CHECK(run.options.image_path.empty());
	CHECK(run.options.www_dir.empty()); // the build's default
	CHECK_FALSE(run.options.demo);
	// No environment at all (a null lookup) is the same.
	Options plain;
	std::string error;
	const char* argv[] = {"homecontrol"};
	REQUIRE(hc::posix::parse_options(1, argv, nullptr, plain, error));
	CHECK(plain.http_port == 8081);
}

TEST_CASE("PosixOptions.TheEnvironmentSetsPortAndAddressAndTheCommandLineWins") {
	Run run;
	run.env["HOMECONTROL_HTTP_PORT"] = "9000";
	run.env["HOMECONTROL_HTTP_BIND"] = "127.0.0.1";
	REQUIRE(run.parse({"--settings", "/var/lib/homecontrol/homecontrol.settings"}));
	CHECK(run.options.http_port == 9000);
	CHECK(run.options.bind_address == "127.0.0.1");
	CHECK(run.options.settings_path == "/var/lib/homecontrol/homecontrol.settings");

	REQUIRE(run.parse({"--http", "9100", "--bind", "::1"}));
	CHECK(run.options.http_port == 9100);
	CHECK(run.options.bind_address == "::1");

	// The commented-out defaults of /etc/default/homecontrol, set but empty: as if unset.
	run.env["HOMECONTROL_HTTP_PORT"] = "";
	run.env["HOMECONTROL_HTTP_BIND"] = "";
	REQUIRE(run.parse({}));
	CHECK(run.options.http_port == 8081);
	CHECK(run.options.bind_address == "0.0.0.0");
}

TEST_CASE("PosixOptions.ABadValueIsRefusedWithItsSource") {
	Run run;
	run.env["HOMECONTROL_HTTP_PORT"] = "65535"; // the WebSocket would need 65536
	CHECK_FALSE(run.parse({}));
	CHECK(run.error == "HOMECONTROL_HTTP_PORT=65535: the port is 0..65534 (0: no web server; the WebSocket uses the next port)");
	run.env["HOMECONTROL_HTTP_PORT"] = "80x";
	CHECK_FALSE(run.parse({}));
	run.env["HOMECONTROL_HTTP_PORT"] = "-1";
	CHECK_FALSE(run.parse({}));
	run.env["HOMECONTROL_HTTP_PORT"] = " 8081";
	CHECK_FALSE(run.parse({}));
	run.env.erase("HOMECONTROL_HTTP_PORT");

	run.env["HOMECONTROL_HTTP_BIND"] = "localhost"; // a name, not an address
	CHECK_FALSE(run.parse({}));
	CHECK(run.error == "HOMECONTROL_HTTP_BIND=localhost: not a numeric IPv4 or IPv6 address");
	run.env["HOMECONTROL_HTTP_BIND"] = "192.168.1.300";
	CHECK_FALSE(run.parse({}));
	run.env.erase("HOMECONTROL_HTTP_BIND");

	CHECK_FALSE(run.parse({"--http"}));
	CHECK(run.error == "--http needs an argument");
	CHECK_FALSE(run.parse({"--http", "70000"}));
	CHECK(run.error == "--http takes a port 0..65534 (0: no web server; the WebSocket uses the next port)");
	CHECK_FALSE(run.parse({"--bind", "example.org"}));
	CHECK(run.error == "--bind example.org: not a numeric IPv4 or IPv6 address");
	CHECK_FALSE(run.parse({"--port", "1"}));
	CHECK(run.error == "unknown argument: --port");
	CHECK_FALSE(run.parse({"--demo", "--image", "x.bin"}));
	CHECK(run.error == "--demo and --image both fill the datapoint model: choose one");
}

TEST_CASE("PosixOptions.PortZeroSwitchesTheWebServerOffAndTheActionsStopParsing") {
	Run run;
	REQUIRE(run.parse({"--http", "0"}));
	CHECK(run.options.http_port == 0);
	REQUIRE(run.parse({"--version", "--nonsense"}));
	CHECK(run.options.action == Options::Action::version);
	REQUIRE(run.parse({"-h"}));
	CHECK(run.options.action == Options::Action::help);
	REQUIRE(run.parse({"--www", "/usr/share/homecontrol/www", "--image", "a.bin", "--demo"}) == false);
	REQUIRE(run.parse({"--www", "/usr/share/homecontrol/www", "--image", "a.bin"}));
	CHECK(run.options.www_dir == "/usr/share/homecontrol/www");
	CHECK(run.options.image_path == "a.bin");
	CHECK(hc::posix::bind_address_valid("::"));
	CHECK(hc::posix::bind_address_valid("fe80::1"));
	CHECK_FALSE(hc::posix::bind_address_valid(""));
	CHECK_FALSE(hc::posix::bind_address_valid(nullptr));
}
