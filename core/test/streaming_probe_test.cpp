/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the streaming fetch (issue #43): the JSON accumulator takes exactly its capacity and
// names the limit one byte above it, with and without an announced length; a store allows one
// streaming write at a time and a discarded writer leaves nothing behind.
#include <cstring>
#include <string>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/fetch_body.h"
#include "hc/platform/blob_store.h"

TEST_CASE("Streaming probe: the fetch body takes exactly its capacity and names the limit beyond it") {
	char buffer[16];
	hc::FetchBody body(buffer, sizeof(buffer), "the probe answer");
	REQUIRE(body.begin(hc::FetchReceiver::kUnknownLength));
	CHECK(body.append("0123456789", 10));
	CHECK(body.append("abcdef", 6));
	CHECK(body.length() == 16);
	CHECK(std::string(body.data(), body.length()) == "0123456789abcdef");
	CHECK_FALSE(body.append("x", 1));
	REQUIRE(body.refusal()[0] != '\0');
	CHECK(std::string(body.refusal()).find("16") != std::string::npos);
	CHECK(std::string(body.refusal()).find("the probe answer") != std::string::npos);

	// An announced length above the capacity is refused before any byte, naming both figures.
	hc::FetchBody announced(buffer, sizeof(buffer), "the probe answer");
	CHECK_FALSE(announced.begin(17));
	REQUIRE(announced.refusal()[0] != '\0');
	CHECK(std::string(announced.refusal()).find("17") != std::string::npos);
	CHECK(announced.length() == 0);
	hc::FetchBody fitting(buffer, sizeof(buffer), "the probe answer");
	CHECK(fitting.begin(16));
	CHECK(fitting.append("0123456789abcdef", 16));
	CHECK(fitting.refusal()[0] == '\0');
}

TEST_CASE("Streaming probe: one streaming write at a time, and a discarded one leaves nothing") {
	hc_test::FakeBlobStore store;
	hc::BlobStatus status = hc::BlobStatus::ok;
	hc::BlobWriter* first = store.open_writer("pack.gz", 100, status);
	REQUIRE(first != nullptr);
	CHECK(status == hc::BlobStatus::ok);
	hc::BlobWriter* second = store.open_writer("other.gz", 100, status);
	CHECK(second == nullptr);
	CHECK(status == hc::BlobStatus::busy);
	CHECK(first->append("12345", 5) == hc::BlobStatus::ok);
	first->discard();
	CHECK(store.blobs.count("pack.gz") == 0);
	CHECK(store.temporaries.empty());
	// After the discard the store is free again and a full write round trip works.
	hc::BlobWriter* third = store.open_writer("pack.gz", 5, status);
	REQUIRE(third != nullptr);
	CHECK(third->append("abcde", 5) == hc::BlobStatus::ok);
	CHECK(third->commit() == hc::BlobStatus::ok);
	CHECK(store.blobs["pack.gz"] == std::vector<uint8_t>({'a', 'b', 'c', 'd', 'e'}));
	CHECK(store.temporaries.empty());
}
