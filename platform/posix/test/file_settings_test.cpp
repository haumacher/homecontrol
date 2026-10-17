/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "doctest.h"
#include "hc/posix/file_settings.h"
#include "test_support.h"

using hc::SettingsStatus;
using hc::posix::FileSettings;
using hc_test::ScopedCapture;

namespace {

/// A settings file in the working directory (the build directory under ctest), removed afterwards.
struct TempFile {
	explicit TempFile(const char* name) : path(std::string("hc_test_") + name + ".settings") { remove(); }
	~TempFile() { remove(); }

	void remove() const {
		std::remove(path.c_str());
		std::remove((path + ".tmp").c_str());
	}

	void write(const std::string& content) const {
		std::ofstream out(path);
		out << content;
	}

	std::string read() const {
		std::ifstream in(path);
		std::stringstream content;
		content << in.rdbuf();
		return content.str();
	}

	std::string path;
};

} // namespace

TEST_CASE("FileSettings.MissingFileIsAnEmptyStore") {
	ScopedCapture capture;
	TempFile file("missing");
	FileSettings settings(file.path);
	CHECK(settings.load());
	char buffer[8];
	CHECK(settings.get_string("tz", buffer, sizeof(buffer)) == SettingsStatus::not_found);
	CHECK(capture.sink.contains("does not exist yet, starting empty"));
}

TEST_CASE("FileSettings.RoundTripThroughTheFile") {
	ScopedCapture capture;
	TempFile file("roundtrip");
	{
		FileSettings settings(file.path);
		settings.load();
		CHECK(settings.set_string("wifi.ssid", "Example Net = with ü") == SettingsStatus::ok);
		CHECK(settings.set_string("empty", "") == SettingsStatus::ok);
		CHECK(settings.set_string("trailing", "space ") == SettingsStatus::ok);
		CHECK(settings.set_int("count", -2147483647 - 1) == SettingsStatus::ok);
		CHECK(settings.set_bool("flag", true) == SettingsStatus::ok);
	}
	std::string content = file.read();
	CHECK(content.find("wifi.ssid=s:Example Net = with ü\n") != std::string::npos);
	CHECK(content.find("count=i:-2147483648\n") != std::string::npos);
	CHECK(content.find("flag=b:1\n") != std::string::npos);
	CHECK(content.rfind("# HomeControl settings, format 1", 0) == 0);

	FileSettings again(file.path);
	CHECK(again.load());
	char buffer[64];
	CHECK(again.get_string("wifi.ssid", buffer, sizeof(buffer)) == SettingsStatus::ok);
	CHECK(std::string(buffer) == "Example Net = with ü");
	CHECK(again.get_string("empty", buffer, sizeof(buffer)) == SettingsStatus::ok);
	CHECK(std::string(buffer).empty());
	CHECK(again.get_string("trailing", buffer, sizeof(buffer)) == SettingsStatus::ok);
	CHECK(std::string(buffer) == "space ");
	int32_t number = 0;
	CHECK(again.get_int("count", number) == SettingsStatus::ok);
	CHECK(number == -2147483647 - 1);
	bool flag = false;
	CHECK(again.get_bool("flag", flag) == SettingsStatus::ok);
	CHECK(flag);
	CHECK(again.get_int("flag", number) == SettingsStatus::wrong_type);

	CHECK(again.erase("wifi.ssid") == SettingsStatus::ok);
	FileSettings third(file.path);
	third.load();
	CHECK(third.get_string("wifi.ssid", buffer, sizeof(buffer)) == SettingsStatus::not_found);
}

TEST_CASE("FileSettings.FileIsPrivate") {
	ScopedCapture capture;
	TempFile file("private");
	FileSettings settings(file.path);
	settings.load();
	REQUIRE(settings.set_string("wifi.psk", "placeholder-key") == SettingsStatus::ok);
	struct stat info;
	REQUIRE(stat(file.path.c_str(), &info) == 0);
	CHECK((info.st_mode & 0777) == 0600);
}

TEST_CASE("FileSettings.BadLinesAreLoggedAndSkipped") {
	ScopedCapture capture;
	TempFile file("bad");
	file.write(
		"# comment\n"
		"\n"
		"good=s:value\n"
		"no equals sign\n"
		"bad key=s:x\n"
		"untyped=value\n"
		"kind=x:1\n"
		"number=i:12a\n"
		"big=i:4294967296\n"
		"flag=b:2\n"
		"windows=s:crlf\r\n"
		"last=i:5");
	FileSettings settings(file.path);
	CHECK(settings.load());
	char buffer[16];
	CHECK(settings.get_string("good", buffer, sizeof(buffer)) == SettingsStatus::ok);
	CHECK(settings.get_string("windows", buffer, sizeof(buffer)) == SettingsStatus::ok);
	CHECK(std::string(buffer) == "crlf");
	int32_t number = 0;
	CHECK(settings.get_int("last", number) == SettingsStatus::ok);
	CHECK(number == 5);
	CHECK(capture.sink.count("W hc.settings: " + file.path + " line") == 7);
	CHECK(capture.sink.contains("line 4 ignored (no '=')"));
	CHECK(capture.sink.contains("line 10 ignored (boolean value must be 0 or 1)"));
}

TEST_CASE("FileSettings.RefusalsAndStorageErrors") {
	ScopedCapture capture;
	FileSettings settings("hc_test_no_such_directory/settings");
	settings.load();
	CHECK(settings.set_string("much.too.long.key", "v") == SettingsStatus::invalid_key);
	CHECK(settings.set_string("k", std::string(128, 'x').c_str()) == SettingsStatus::too_long);
	CHECK(settings.set_string("k", "tab\there") == SettingsStatus::invalid_value);
	CHECK(settings.set_string("k", "v") == SettingsStatus::storage_error);
	CHECK(capture.sink.contains("E hc.settings: cannot write settings file hc_test_no_such_directory/settings.tmp"));
	// A failed write leaves the store as it was.
	char buffer[8];
	CHECK(settings.get_string("k", buffer, sizeof(buffer)) == SettingsStatus::not_found);
}
