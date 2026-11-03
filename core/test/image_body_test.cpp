/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The receivers' body buffer (homecontrol #66): the staging area first, the heap as the fallback.
#include <cstring>
#include <string>

#include "doctest.h"
#include "fake_image_staging.h"
#include "hc/image/image_body.h"

using hc::image::ImageBody;
using hc_test::FakeImageStaging;

TEST_CASE("ImageBody: a body goes to the staging area and reads back as one NUL-terminated text") {
	FakeImageStaging staging;
	ImageBody body;
	const char* refusal = nullptr;
	std::string text = "{\"image\": {\"projectId\": \"P\"}}";
	REQUIRE(body.begin(text.size(), &staging, &refusal));
	CHECK(refusal == nullptr);
	CHECK(body.staged());
	CHECK(staging.expected == text.size() + 1); // with the NUL
	CHECK(body.append(text.data(), 5));
	CHECK_FALSE(body.finish()); // not before the last byte
	CHECK(body.append(text.data() + 5, text.size() - 5));
	CHECK_FALSE(body.append("x", 1)); // beyond the announced size
	const char* read = body.finish();
	REQUIRE(read != nullptr);
	CHECK(std::string(read) == text);
	CHECK(std::strlen(read) == text.size());
	body.release();
	CHECK(staging.ends == 1);
	CHECK_FALSE(staging.busy);
	CHECK_FALSE(body.active());
}

TEST_CASE("ImageBody: without a staging area, or when it declines, the body goes to the heap with the reason") {
	std::string text(5000, 'a');
	{
		ImageBody body;
		REQUIRE(body.begin(text.size(), nullptr));
		CHECK_FALSE(body.staged());
		CHECK(body.append(text.data(), text.size()));
		CHECK(std::string(body.finish()) == text);
	}
	FakeImageStaging staging;
	staging.capacity_bytes = 4096; // smaller than the body
	ImageBody body;
	const char* refusal = nullptr;
	REQUIRE(body.begin(text.size(), &staging, &refusal));
	CHECK_FALSE(body.staged());
	REQUIRE(refusal != nullptr);
	CHECK(std::string(refusal) == "the image is larger than the staging area");
	CHECK(body.append(text.data(), text.size()));
	CHECK(std::string(body.finish()) == text);
	body.release();
	CHECK(staging.ends == 0); // never began

	// A second receiver while the area is in use takes the heap.
	ImageBody first;
	ImageBody second;
	staging.capacity_bytes = 1 << 20;
	REQUIRE(first.begin(10, &staging));
	REQUIRE(second.begin(10, &staging, &refusal));
	CHECK(first.staged());
	CHECK_FALSE(second.staged());
	CHECK(std::string(refusal) == "another image is being received");
	first.release();
	CHECK_FALSE(staging.busy);
}

TEST_CASE("ImageBody: a failing staging area fails the append and the read; begin() starts over") {
	FakeImageStaging staging;
	staging.fail_after = 1;
	ImageBody body;
	REQUIRE(body.begin(10, &staging));
	CHECK(body.append("01234", 5));
	CHECK_FALSE(body.append("56789", 5));
	body.release();
	staging.fail_after = 0;
	staging.refuse_read = true;
	REQUIRE(body.begin(3, &staging));
	CHECK(body.append("abc", 3));
	CHECK(body.finish() == nullptr);
	body.release();
	CHECK(staging.ends == 2);
}
