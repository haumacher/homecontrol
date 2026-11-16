/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TEST_IMAGE_SUPPORT_H
#define HC_TEST_IMAGE_SUPPORT_H

// The tests' way to the binary runtime image (issue #87): they keep writing images in the JSON
// source form, which the test-only compiler (image_compiler.h) turns into the binary image the
// device reads; the corpora's compiled `.bin` files are read as they are.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/image/binary.h"
#include "hc/image/image_service.h"
#include "hc/image/validator.h"
#include "hc/rules/rule_set.h"
#include "image_compiler.h"

namespace hc_test {

/// The source `json` compiled into a binary image; a failure fails the test with the compiler's error.
inline std::vector<uint8_t> compiled(const std::string& json, const hc::test::CompileOptions& options = {}) {
	std::vector<uint8_t> out;
	std::string error;
	bool ok = hc::test::compile_image(json.data(), json.size(), out, &error, options);
	REQUIRE_MESSAGE(ok, "the test compiler refused the image: " << error);
	return out;
}

/// Deploys the binary `bytes` into `service` as PUT /api/image does (the receive path: header,
/// swap, slot, checks, apply).
inline bool deploy_bytes(hc::image::ImageService& service, const std::vector<uint8_t>& bytes,
	hc::image::Source source = hc::image::Source::api) {
	return service.load(bytes.data(), bytes.size(), source);
}

/// Compiles the source `json` and deploys it into `service`.
inline bool deploy(hc::image::ImageService& service, const std::string& json,
	hc::image::Source source = hc::image::Source::api, const hc::test::CompileOptions& options = {}) {
	return deploy_bytes(service, compiled(json, options), source);
}

/// The refusals of a report, one per line, for messages.
inline std::string describe(const hc::image::Report& report) {
	std::string out = std::string(hc::image::report_kind_name(report.kind)) + ":";
	for (size_t i = 0; i < report.count; ++i) {
		out += std::string("\n  ") + report.refusals[i].path + ": " + report.refusals[i].reason;
	}
	return out;
}

/// A compiled image held in memory and read in place, after the device's structural checks.
struct CompiledImage {
	std::vector<uint8_t> bytes;
	hc::image::bin::View view;

	/// Compiles `json`, checks it as the device does (CRC, structure) and opens the view.
	bool compile(const std::string& json, const hc::test::CompileOptions& options = {}) {
		bytes = compiled(json, options);
		return open();
	}
	/// Checks and opens `bytes` (set by the caller).
	bool open() {
		hc::image::Report report;
		bool ok = hc::image::check_header(bytes.data(), bytes.size(), bytes.size(), report) &&
			hc::image::check_crc(bytes.data(), report) && hc::image::check_structure(bytes.data(), report);
		CHECK_MESSAGE(ok, describe(report));
		if (ok) {
			view.open(bytes.data());
		}
		return ok;
	}
	/// The datapoint specs of the image (the model's first datapoints), names in the image.
	std::vector<hc::DatapointSpec> specs() const {
		std::vector<hc::DatapointSpec> out;
		for (size_t i = 0; i < view.count(hc::image::bin::kDatapoints); ++i) {
			const auto& d = view.at<hc::image::bin::DatapointRec>(hc::image::bin::kDatapoints, i);
			out.push_back(hc::DatapointSpec{view.str(d.name), static_cast<hc::ValueType>(d.type),
				static_cast<uint8_t>(d.flags & 0x7F)});
		}
		return out;
	}
	/// Binds the image's rules into `set`.
	hc::rules::LoadResult bind(hc::rules::RuleSet& set, const char* language = "en") const {
		return set.bind(view, language);
	}
};

} // namespace hc_test

#endif // HC_TEST_IMAGE_SUPPORT_H
