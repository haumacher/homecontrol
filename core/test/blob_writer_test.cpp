/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The streaming write of the blob stores (issue #43): the RAM store and the tests' fake keep the
// contract the LittleFS and the POSIX store keep (platform/posix/test/file_blob_store_test.cpp).
#include <string>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/memory_blob_store.h"
#include "test_support.h"

using hc::BlobChunk;
using hc::BlobStatus;
using hc::BlobWriter;

namespace {

std::string read_all(const hc::BlobStore& store, const char* name) {
	size_t size = 0;
	if (store.size(name, size) != BlobStatus::ok) {
		return "<none>";
	}
	std::string content(size, '\0');
	size_t length = 0;
	REQUIRE(store.read(name, 0, &content[0], size, length) == BlobStatus::ok);
	content.resize(length);
	return content;
}

std::string pattern(size_t size) {
	std::string content(size, '\0');
	for (size_t i = 0; i < size; ++i) {
		content[i] = static_cast<char>(i * 13 + 1);
	}
	return content;
}

/// Streams `content` in pieces of `piece` bytes; returns the commit's status.
BlobStatus stream(hc::BlobStore& store, const char* name, const std::string& content, size_t expected, size_t piece) {
	BlobStatus status = BlobStatus::storage_error;
	BlobWriter* writer = store.open_writer(name, expected, status);
	if (writer == nullptr) {
		return status;
	}
	for (size_t at = 0; at < content.size(); at += piece) {
		size_t n = content.size() - at < piece ? content.size() - at : piece;
		status = writer->append(content.data() + at, n);
		if (status != BlobStatus::ok) {
			writer->discard();
			return status;
		}
	}
	return writer->commit();
}

} // namespace

TEST_CASE("MemoryBlobStore: a streaming write grows its buffer, installs it on commit and keeps the old on discard") {
	hc_test::ScopedCapture capture;
	hc::image::MemoryBlobStore store(100 * 1024);
	BlobChunk old{"old", 3};
	REQUIRE(store.write("pack", &old, 1) == BlobStatus::ok);
	std::string content = pattern(40 * 1024);

	// Announced exactly, and unannounced (the buffer doubles).
	CHECK(stream(store, "pack", content, content.size(), 1024) == BlobStatus::ok);
	CHECK(read_all(store, "pack") == content);
	CHECK(store.used() == content.size());
	std::string other = pattern(30000);
	CHECK(stream(store, "other", other, 0, 777) == BlobStatus::ok);
	CHECK(read_all(store, "other") == other);
	CHECK(store.used() == content.size() + other.size());

	// Discard: the previous content stays, nothing is accounted.
	BlobStatus status = BlobStatus::storage_error;
	BlobWriter* writer = store.open_writer("pack", 0, status);
	REQUIRE(writer != nullptr);
	CHECK(store.open_writer("x", 0, status) == nullptr);
	CHECK(status == BlobStatus::busy);
	REQUIRE(writer->append("abc", 3) == BlobStatus::ok);
	CHECK(writer->written() == 3);
	writer->discard();
	CHECK(read_all(store, "pack") == content);
	CHECK(store.used() == content.size() + other.size());

	// The capacity: an announced size above the free space is refused at open, an unannounced one at
	// the append that crosses it; the old content stays.
	size_t free = 0;
	REQUIRE(store.free_space(free));
	CHECK(free == 100 * 1024 - content.size() - other.size());
	CHECK(store.open_writer("third", free + 1, status) == nullptr);
	CHECK(status == BlobStatus::no_space);
	CHECK(stream(store, "third", pattern(free + 1), 0, 1024) == BlobStatus::no_space);
	CHECK(read_all(store, "third") == "<none>");
	// Replacing a blob counts its old content as freed.
	CHECK(stream(store, "other", pattern(free + other.size()), 0, 4096) == BlobStatus::ok);
	CHECK(store.used() == 100 * 1024);
	CHECK(stream(store, "more", "x", 0, 1) == BlobStatus::no_space);
	CHECK(stream(store, "empty", "", 0, 1) == BlobStatus::ok); // an empty blob takes no content bytes
	CHECK(read_all(store, "empty").empty());
	CHECK(store.open_writer(".tmp", 0, status) == nullptr);
	CHECK(status == BlobStatus::invalid_name);
}

TEST_CASE("FakeBlobStore: the streaming write keeps its content under the temporary name until commit") {
	hc_test::FakeBlobStore store;
	store.blobs["pack"] = {'o', 'l', 'd'};
	BlobStatus status = BlobStatus::storage_error;
	BlobWriter* writer = store.open_writer("pack", 6, status);
	REQUIRE(writer != nullptr);
	REQUIRE(writer->append("new", 3) == BlobStatus::ok);
	CHECK(store.temporaries[".pack.tmp"].size() == 3);
	CHECK(read_all(store, "pack") == "old");
	REQUIRE(writer->append("new", 3) == BlobStatus::ok);
	REQUIRE(writer->commit() == BlobStatus::ok);
	CHECK(read_all(store, "pack") == "newnew");
	CHECK(store.temporaries.empty());
	// A copy of the store does not take an open writer along.
	writer = store.open_writer("pack", 0, status);
	REQUIRE(writer != nullptr);
	hc_test::FakeBlobStore copy(store);
	CHECK_FALSE(copy.writing());
	CHECK(store.writing());
	writer->discard();
	CHECK_FALSE(store.writing());
}
