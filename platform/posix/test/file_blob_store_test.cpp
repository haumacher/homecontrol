/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/posix/file_blob_store.h"
#include "test_support.h"

using hc::BlobChunk;
using hc::BlobStatus;
using hc::BlobStore;
using hc::posix::FileBlobStore;
using hc_test::ScopedCapture;

namespace {

/// A fresh directory in the working directory (the build directory under ctest), removed with its
/// files afterwards. The store's directory is `path + "/state"`, so the tests also see it created.
struct TempDir {
	TempDir() {
		char pattern[] = "hc_test_blobs_XXXXXX";
		const char* made = ::mkdtemp(pattern);
		REQUIRE(made != nullptr);
		path = made;
		state = path + "/state";
	}
	~TempDir() {
		clear(state);
		::rmdir(state.c_str());
		::rmdir(path.c_str());
	}

	static void clear(const std::string& dir) {
		DIR* d = ::opendir(dir.c_str());
		if (d == nullptr) {
			return;
		}
		while (dirent* entry = ::readdir(d)) {
			if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
				std::string file = dir + "/" + entry->d_name;
				if (::unlink(file.c_str()) != 0) {
					::rmdir(file.c_str());
				}
			}
		}
		::closedir(d);
	}

	void put(const std::string& name, const std::string& content) const {
		std::ofstream out(state + "/" + name, std::ios::binary);
		out << content;
	}

	bool exists(const std::string& name) const {
		struct stat info;
		return ::stat((state + "/" + name).c_str(), &info) == 0;
	}

	std::vector<std::string> files() const {
		std::vector<std::string> names;
		DIR* d = ::opendir(state.c_str());
		if (d == nullptr) {
			return names;
		}
		while (dirent* entry = ::readdir(d)) {
			if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
				names.push_back(entry->d_name);
			}
		}
		::closedir(d);
		return names;
	}

	std::string path;
	std::string state;
};

BlobStatus put(BlobStore& store, const char* name, const std::string& content) {
	BlobChunk chunk{content.data(), content.size()};
	return store.write(name, &chunk, 1);
}

std::string get(const BlobStore& store, const char* name, BlobStatus* status = nullptr) {
	char buffer[BlobStore::kMaxBlobSize + 16];
	size_t length = 99;
	BlobStatus result = store.read(name, 0, buffer, sizeof(buffer), length);
	if (status != nullptr) {
		*status = result;
	}
	return std::string(buffer, length);
}

struct Collect : hc::BlobVisitor {
	void blob(const char* name, size_t size) override { blobs[name] = size; }
	std::map<std::string, size_t> blobs;
};

} // namespace

TEST_CASE("FileBlobStore: write, read, size round trip in a directory it creates") {
	ScopedCapture capture;
	TempDir dir;
	FileBlobStore store(dir.state + "/");
	CHECK(store.directory() == dir.state);
	std::string header = "HV\x01";
	std::string body(1000, 'x');
	body[500] = '\0';
	BlobChunk chunks[] = {{header.data(), header.size()}, {nullptr, 0}, {body.data(), body.size()}};
	REQUIRE(store.write("v.1a2b3c4d", chunks, 3) == BlobStatus::ok);
	CHECK(dir.exists("v.1a2b3c4d"));
	CHECK(get(store, "v.1a2b3c4d") == header + body);

	size_t size = 0;
	CHECK(store.size("v.1a2b3c4d", size) == BlobStatus::ok);
	CHECK(size == 1003);

	// Read at an offset, a short read at the end, nothing at and past the end.
	char buffer[10];
	size_t length = 0;
	CHECK(store.read("v.1a2b3c4d", 3, buffer, 5, length) == BlobStatus::ok);
	CHECK(length == 5);
	CHECK(std::string(buffer, 5) == "xxxxx");
	CHECK(store.read("v.1a2b3c4d", 1000, buffer, sizeof(buffer), length) == BlobStatus::ok);
	CHECK(length == 3);
	CHECK(store.read("v.1a2b3c4d", 1003, buffer, sizeof(buffer), length) == BlobStatus::ok);
	CHECK(length == 0);
	CHECK(store.read("v.1a2b3c4d", 5000, buffer, sizeof(buffer), length) == BlobStatus::ok);
	CHECK(length == 0);

	// An empty blob exists.
	REQUIRE(store.write("empty", nullptr, 0) == BlobStatus::ok);
	CHECK(store.size("empty", size) == BlobStatus::ok);
	CHECK(size == 0);

	// A second store on the same directory (a restart) sees the blobs.
	FileBlobStore again(dir.state);
	CHECK(get(again, "v.1a2b3c4d") == header + body);
	CHECK(capture.sink.contains("blob store " + dir.state + ": 2 blob(s)"));
}

TEST_CASE("FileBlobStore: replace and remove") {
	TempDir dir;
	FileBlobStore store(dir.state);
	REQUIRE(put(store, "h.00000001", std::string(5000, 'a')) == BlobStatus::ok);
	REQUIRE(put(store, "h.00000001", "short") == BlobStatus::ok);
	CHECK(get(store, "h.00000001") == "short");
	size_t size = 0;
	CHECK(store.size("h.00000001", size) == BlobStatus::ok);
	CHECK(size == 5);
	CHECK(dir.files() == std::vector<std::string>{"h.00000001"}); // no temporary left

	CHECK(store.remove("h.00000001") == BlobStatus::ok);
	CHECK_FALSE(dir.exists("h.00000001"));
	CHECK(store.remove("h.00000001") == BlobStatus::not_found);
	BlobStatus status = BlobStatus::ok;
	CHECK(get(store, "h.00000001", &status).empty());
	CHECK(status == BlobStatus::not_found);
	CHECK(store.size("h.00000001", size) == BlobStatus::not_found);
}

TEST_CASE("FileBlobStore: visit lists the blobs with their sizes, not temporaries or foreign files") {
	TempDir dir;
	FileBlobStore store(dir.state);
	REQUIRE(put(store, "a", "1") == BlobStatus::ok);
	REQUIRE(put(store, "b.2", "22") == BlobStatus::ok);
	REQUIRE(put(store, "c-3_x", std::string(300, 'c')) == BlobStatus::ok);
	dir.put("bad name", "x");                                                   // breaks the name rule
	REQUIRE(::mkdir((dir.state + "/subdir").c_str(), 0700) == 0);              // a directory is no blob
	dir.put(".hidden", "x");                                                    // a dot file, not a temporary
	Collect collect;
	store.visit(collect);
	CHECK(collect.blobs == std::map<std::string, size_t>{{"a", 1}, {"b.2", 2}, {"c-3_x", 300}});
}

TEST_CASE("FileBlobStore: a temporary file left by a crash is ignored and deleted at start") {
	ScopedCapture capture;
	TempDir dir;
	{
		FileBlobStore store(dir.state);
		REQUIRE(put(store, "v.00000001", "old content") == BlobStatus::ok);
	}
	// Power loss in the middle of the next write: the temporary is written (partly), the rename
	// never happened.
	dir.put(".v.00000001.tmp", "new con");
	dir.put(".v.00000002.tmp", "never renamed");

	FileBlobStore store(dir.state);
	CHECK(get(store, "v.00000001") == "old content");
	BlobStatus status = BlobStatus::ok;
	get(store, "v.00000002", &status);
	CHECK(status == BlobStatus::not_found);
	CHECK_FALSE(dir.exists(".v.00000001.tmp"));
	CHECK_FALSE(dir.exists(".v.00000002.tmp"));
	CHECK(capture.sink.contains("deleted 2 unfinished write(s)"));
	Collect collect;
	store.visit(collect);
	CHECK(collect.blobs == std::map<std::string, size_t>{{"v.00000001", 11}});

	// The next write completes normally.
	REQUIRE(put(store, "v.00000001", "new content") == BlobStatus::ok);
	CHECK(get(store, "v.00000001") == "new content");
}

TEST_CASE("FileBlobStore: a blob name that ends in .tmp is an ordinary blob") {
	TempDir dir;
	{
		FileBlobStore store(dir.state);
		REQUIRE(put(store, "x.tmp", "kept") == BlobStatus::ok);
	}
	FileBlobStore store(dir.state);
	CHECK(get(store, "x.tmp") == "kept");
}

TEST_CASE("FileBlobStore: names outside the contract are refused before the store is touched") {
	ScopedCapture capture;
	TempDir dir;
	FileBlobStore store(dir.state);
	const char* bad[] = {nullptr, "", ".hidden", "..", "a/b", "../escape", "sp ace", "umlaut\xc3\xa4",
		"12345678901234567890123456789012"};
	for (const char* name : bad) {
		CAPTURE(name == nullptr ? "(null)" : name);
		CHECK(put(store, name, "x") == BlobStatus::invalid_name);
		BlobStatus status = BlobStatus::ok;
		get(store, name, &status);
		CHECK(status == BlobStatus::invalid_name);
		size_t size = 0;
		CHECK(store.size(name, size) == BlobStatus::invalid_name);
		CHECK(store.remove(name) == BlobStatus::invalid_name);
	}
	CHECK(capture.sink.contains("write refused: blob name invalid"));
	struct stat info;
	CHECK(::stat(dir.state.c_str(), &info) != 0); // never created
	CHECK(put(store, "1234567890123456789012345678901", "31 characters") == BlobStatus::ok);
}

TEST_CASE("FileBlobStore: a blob over the size limit is refused and the old content kept") {
	ScopedCapture capture;
	TempDir dir;
	FileBlobStore store(dir.state);
	REQUIRE(put(store, "big", "old") == BlobStatus::ok);
	std::string half(BlobStore::kMaxBlobSize / 2, 'h');
	BlobChunk chunks[] = {{half.data(), half.size()}, {half.data(), half.size()}, {"!", 1}};
	CHECK(store.write("big", chunks, 3) == BlobStatus::too_large);
	CHECK(capture.sink.contains("write of big refused: 20481 bytes, at most 20480 allowed"));
	CHECK(get(store, "big") == "old");
	CHECK(dir.files() == std::vector<std::string>{"big"});
	CHECK(store.write("big", chunks, 2) == BlobStatus::ok); // exactly the limit
	size_t size = 0;
	CHECK(store.size("big", size) == BlobStatus::ok);
	CHECK(size == BlobStore::kMaxBlobSize);
}

TEST_CASE("FileBlobStore: an unusable directory refuses every call with storage_error, logged once") {
	ScopedCapture capture;
	TempDir dir;
	FileBlobStore store(dir.path + "/missing/parent");
	CHECK_FALSE(store.start());
	CHECK(put(store, "a", "x") == BlobStatus::storage_error);
	BlobStatus status = BlobStatus::ok;
	get(store, "a", &status);
	CHECK(status == BlobStatus::storage_error);
	size_t size = 0;
	CHECK(store.size("a", size) == BlobStatus::storage_error);
	CHECK(store.remove("a") == BlobStatus::storage_error);
	Collect collect;
	store.visit(collect);
	CHECK(collect.blobs.empty());
	CHECK(capture.sink.count("blob store unusable: cannot create the directory") == 1);

	// A file where the directory should be.
	std::ofstream(dir.path + "/afile") << "x";
	FileBlobStore on_file(dir.path + "/afile");
	CHECK(put(on_file, "a", "x") == BlobStatus::storage_error);
	CHECK(capture.sink.contains("blob store unusable: cannot open the directory " + dir.path + "/afile"));
	::unlink((dir.path + "/afile").c_str());
}

TEST_CASE("FileBlobStore: the default directory sits beside the settings file") {
	CHECK(FileBlobStore::directory_for_settings("homecontrol.settings") == "homecontrol.state");
	CHECK(FileBlobStore::directory_for_settings("/etc/hc/home.settings") == "/etc/hc/home.state");
	CHECK(FileBlobStore::directory_for_settings("config") == "config.state");
	CHECK(FileBlobStore::directory_for_settings("dir.settings/x") == "dir.settings/x.state");
	CHECK(FileBlobStore::directory_for_settings("/a/.settings") == "/a/.settings.state");
}
