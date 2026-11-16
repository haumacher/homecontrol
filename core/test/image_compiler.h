/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TEST_IMAGE_COMPILER_H
#define HC_TEST_IMAGE_COMPILER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hc::blocks {
class BlockRegistry;
} // namespace hc::blocks

/// A test-only compiler of runtime images (issue #87): the JSON source form (spec/runtime-image.md
/// sections 2 to 8) into the binary image (section 9), in the canonical layout of 9.18, so the host
/// tests can keep writing their images as JSON. E-Home's compiler (Java) is the real one; both
/// write the same bytes for the same source.
///
/// It validates nothing beyond what compiling needs (E-Home validates; the inputs are valid): a
/// reference it cannot resolve (an unknown datapoint, a JSON text that is no JSON) fails the
/// compilation with a reason. What the device refuses by its own catalogues (spec/runtime-image.md
/// 10.8: an unknown block type, a block's ports and parameters against the manifest, a DPT, a
/// limit) is compiled anyway, as long as the bytes can be written (the corpus mode of 13.1).
namespace hc::test {

struct CompileOptions {
	/// A 32-byte Ed25519 seed: the image is signed (9.17), the header flag `signed` set and the
	/// signature appended. nullptr: unsigned.
	const uint8_t* sign_seed = nullptr;
	/// The block types the manifests of the blocks come from (ports, parameters and the implied
	/// datapoints, spec/blocks.md 6). nullptr: hc::blocks::BlockRegistry::device(). The corpus needs
	/// hc_test::corpus_registry() (the conformance type of spec/blocks.md 15).
	const hc::blocks::BlockRegistry* registry = nullptr;
	/// compile_case(): the case's file name without `.json` (`05-timer-presence-light`), the image's
	/// projectId (spec/runtime-image.md 13.1). nullptr: "case".
	const char* case_name = nullptr;
};

/// Compiles the source `json` (`length` bytes) into the binary image `out` (replaced). False with a
/// reason in `error` (if given) when it cannot be compiled.
bool compile_image(const char* json, size_t length, std::vector<uint8_t>& out, std::string* error = nullptr,
	const CompileOptions& options = CompileOptions());

/// Compiles a rule or block conformance case (spec/fixtures/rules, spec/fixtures/blocks) into an image
/// as spec/runtime-image.md 13.1 maps it: `step` -1 the case's own image (revision 0), `step` k >= 0
/// the image of the timeline step at the 0-based position k, which must be an `image` step (revision
/// k + 1; the file `NN-<name>.step-<k>.bin`). False when the step is no image step or the image
/// cannot be compiled.
bool compile_case(const char* case_json, size_t length, int step, std::vector<uint8_t>& out,
	std::string* error = nullptr, const CompileOptions& options = CompileOptions());

} // namespace hc::test

#endif // HC_TEST_IMAGE_COMPILER_H
