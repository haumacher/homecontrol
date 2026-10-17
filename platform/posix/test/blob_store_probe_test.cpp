/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the POSIX blob store: chunked writes read back in pieces,
// a replace that keeps the old content until the rename, a stale temporary file at start, and the
// name rules seen through visit().

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/posix/file_blob_store.h"
#include "test_support.h"

using hc::BlobChunk;
using hc::BlobStatus;
using hc::posix::FileBlobStore;

namespace {

struct TempDir {
	std::string path;
	TempDir() {
		char buffer[] = "/tmp/hc-blob-probe-XXXXXX";
		REQUIRE(mkdtemp(buffer) != nullptr);
		path = buffer;
	}
	~TempDir() {
		std::string command = "rm -rf '" + path + "'";
		int rc = std::system(command.c_str());
		(void) rc;
	}
};

struct Names : hc::BlobVisitor {
	void blob(const char* name, size_t size) override { names.push_back(std::string(name) + ":" + std::to_string(size)); }
	std::vector<std::string> names;
};

} // namespace

TEST_CASE("BlobProbe.ChunkedWriteReadInPiecesAndReplaced") {
	hc_test::ScopedCapture capture;
	TempDir dir;
	FileBlobStore store(dir.path + "/state");
	REQUIRE(store.start());
	std::string a(1000, 'a'), b(2000, 'b'), c(3, 'c');
	BlobChunk chunks[3] = {{a.data(), a.size()}, {b.data(), b.size()}, {c.data(), c.size()}};
	REQUIRE(store.write("v.one", chunks, 3) == BlobStatus::ok);
	size_t size = 0;
	REQUIRE(store.size("v.one", size) == BlobStatus::ok);
	CHECK(size == 3003);
	// Read across the chunk boundaries and past the end.
	char buffer[64];
	size_t length = 0;
	REQUIRE(store.read("v.one", 995, buffer, 10, length) == BlobStatus::ok);
	CHECK(length == 10);
	CHECK(std::string(buffer, 10) == "aaaaabbbbb");
	REQUIRE(store.read("v.one", 2998, buffer, sizeof(buffer), length) == BlobStatus::ok);
	CHECK(length == 5);
	CHECK(std::string(buffer, 5) == "bbccc");
	REQUIRE(store.read("v.one", 5000, buffer, sizeof(buffer), length) == BlobStatus::ok);
	CHECK(length == 0);
	// Replace with a shorter blob: the old content is gone entirely.
	std::string d = "new";
	BlobChunk one[1] = {{d.data(), d.size()}};
	REQUIRE(store.write("v.one", one, 1) == BlobStatus::ok);
	REQUIRE(store.size("v.one", size) == BlobStatus::ok);
	CHECK(size == 3);
	// No temp file is left behind, and the listing names exactly one blob.
	Names names;
	store.visit(names);
	CHECK(names.names == std::vector<std::string>{"v.one:3"});
	CHECK(store.remove("v.one") == BlobStatus::ok);
	CHECK(store.remove("v.one") == BlobStatus::not_found);
	CHECK(store.size("v.one", size) == BlobStatus::not_found);
}

TEST_CASE("BlobProbe.StaleTemporaryAndForeignFilesAreIgnoredOrCleaned") {
	hc_test::ScopedCapture capture;
	TempDir dir;
	std::string state = dir.path + "/state";
	{
		FileBlobStore first(state);
		REQUIRE(first.start());
		std::string v = "value";
		BlobChunk one[1] = {{v.data(), v.size()}};
		REQUIRE(first.write("h.ring", one, 1) == BlobStatus::ok);
	}
	// A power loss left a temp file of a newer write, and a stranger dropped a dot file and a folder.
	{
		std::ofstream tmp(state + "/.h.ring.tmp");
		tmp << "half written";
	}
	{
		std::ofstream hidden(state + "/.hidden");
		hidden << "x";
	}
	std::string mk = "mkdir -p '" + state + "/sub'";
	int rc = std::system(mk.c_str());
	(void) rc;
	FileBlobStore second(state);
	REQUIRE(second.start());
	char buffer[32];
	size_t length = 0;
	REQUIRE(second.read("h.ring", 0, buffer, sizeof(buffer), length) == BlobStatus::ok);
	CHECK(std::string(buffer, length) == "value"); // the old blob, not the half-written one
	Names names;
	second.visit(names);
	CHECK(names.names == std::vector<std::string>{"h.ring:5"});
	std::ifstream gone(state + "/.h.ring.tmp");
	CHECK_FALSE(gone.good()); // cleaned at start
	// Names outside the contract are refused before anything is touched.
	std::string v = "v";
	BlobChunk one[1] = {{v.data(), v.size()}};
	CHECK(second.write(".dot", one, 1) != BlobStatus::ok);
	CHECK(second.write("has space", one, 1) != BlobStatus::ok);
	CHECK(second.write("", one, 1) != BlobStatus::ok);
	CHECK(second.write(std::string(32, 'n').c_str(), one, 1) != BlobStatus::ok);
	CHECK(second.write("../escape", one, 1) != BlobStatus::ok);
	CHECK(capture.sink.count("E hc.") + capture.sink.count("W hc.") >= 1);
}
