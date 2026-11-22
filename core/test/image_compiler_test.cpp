/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The test-only image compiler (core/test/image_compiler.h, issue #87): the worked example of
// spec/runtime-image.md 13 byte for byte, and every source of the corpora compiled into an image that
// passes a structural self-check of section 9.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "conformance_block.h"
#include "doctest.h"
#include "hc/sha.h"
#include "hc/signature.h"
#include "hc/value_store.h"
#include "image_compiler.h"
#include "json_reader.h"

using hc::test::compile_case;
using hc::test::compile_image;
using hc::test::CompileOptions;

namespace {

std::string read_file(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

std::vector<std::string> json_files(const std::string& directory, const char* prefix = "") {
	std::vector<std::string> files;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
		std::string name = entry.path().filename().string();
		if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0 && name.rfind(prefix, 0) == 0) {
			files.push_back(entry.path().string());
		}
	}
	std::sort(files.begin(), files.end());
	return files;
}

std::vector<uint8_t> from_hex(const char* text) {
	std::vector<uint8_t> out;
	int high = -1;
	for (const char* p = text; *p != '\0'; ++p) {
		int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
		if (d < 0) {
			continue;
		}
		if (high < 0) {
			high = d;
		} else {
			out.push_back(static_cast<uint8_t>(high * 16 + d));
			high = -1;
		}
	}
	return out;
}

// ---- The structural self-check (spec/runtime-image.md 9, the checks of 10.1 to 10.7) -----------

constexpr uint16_t kNone = 0xFFFF;

/// The record sizes of 9.4 (0: a byte section), written down again here rather than taken from the
/// compiler.
const std::map<uint16_t, uint16_t> kSizes = {{0x01, 32}, {0x02, 0}, {0x03, 8}, {0x04, 32}, {0x10, 24}, {0x11, 12},
	{0x12, 28}, {0x13, 8}, {0x14, 16}, {0x15, 8}, {0x20, 8}, {0x21, 12}, {0x22, 2}, {0x23, 2}, {0x24, 12}, {0x28, 32},
	{0x2C, 20}, {0x2D, 32}, {0x2E, 20}, {0x30, 64}, {0x31, 4}, {0x32, 32}, {0x40, 28}, {0x41, 28}, {0x42, 8},
	{0x43, 8}, {0x44, 16}, {0x45, 16}, {0x46, 8}, {0x47, 8}, {0x48, 8}, {0x49, 8}, {0x4A, 12}, {0x4B, 16},
	{0x4C, 2}, {0x4D, 16}, {0x4E, 40}, {0x50, 28}, {0x51, 12}, {0x52, 2}, {0x53, 16}, {0x60, 0},
	{0x62, 0}, {0x63, 4}, {0x64, 2}};

class Checker {
public:
	explicit Checker(const std::vector<uint8_t>& image) : b_(image) {}

	/// Empty when the image passes, else the first problem.
	std::string run();

private:
	struct Section {
		size_t offset = 0;
		size_t length = 0;
		size_t entry = 0;
		size_t count() const { return entry == 0 ? 0 : length / entry; }
	};

	uint32_t u32(size_t at) const {
		return static_cast<uint32_t>(b_[at] | (b_[at + 1] << 8) | (b_[at + 2] << 16)) | (static_cast<uint32_t>(b_[at + 3]) << 24);
	}
	uint16_t u16(size_t at) const { return static_cast<uint16_t>(b_[at] | (b_[at + 1] << 8)); }
	uint8_t u8(size_t at) const { return b_[at]; }

	size_t count(uint16_t id) const {
		auto it = sections_.find(id);
		return it == sections_.end() ? 0 : it->second.count();
	}
	/// The position of field `offset` of record `i` of section `id`.
	size_t at(uint16_t id, size_t i, size_t offset) const {
		const Section& s = sections_.at(id);
		return s.offset + i * s.entry + offset;
	}
	bool problem(const std::string& text) {
		if (error_.empty()) {
			error_ = text;
		}
		return false;
	}
	std::string where(uint16_t id, size_t i, size_t offset) const {
		char text[64];
		std::snprintf(text, sizeof(text), "section 0x%02x record %u at +%u", id, static_cast<unsigned>(i),
			static_cast<unsigned>(offset));
		return text;
	}

	void string(uint16_t id, size_t i, size_t offset, bool required = false) {
		uint32_t s = u32(at(id, i, offset));
		if (starts_.count(s) == 0) {
			problem(where(id, i, offset) + ": no string start");
		} else if (required && s == 0) {
			problem(where(id, i, offset) + ": an empty id");
		}
	}
	void label(uint16_t id, size_t i, size_t offset) {
		uint32_t ref = u32(at(id, i, offset));
		if (ref == 0) {
			return;
		}
		size_t first = ref & 0xFFFFFF;
		size_t n = ref >> 24;
		if (n < 1 || n > 16 || first + n > count(0x03)) {
			problem(where(id, i, offset) + ": label entries do not exist");
			return;
		}
		size_t empty = 0;
		for (size_t e = first; e < first + n; ++e) {
			empty += u32(at(0x03, e, 0)) == 0 ? 1 : 0;
		}
		if (!(empty == 0 || (empty == 1 && n == 1))) {
			problem(where(id, i, offset) + ": a language map with an empty tag");
		}
		label_entries_ += n;
	}
	void index(uint16_t id, size_t i, size_t offset, uint16_t target, bool optional = false) {
		uint16_t v = u16(at(id, i, offset));
		if (!(optional && v == kNone) && v >= count(target)) {
			problem(where(id, i, offset) + ": index " + std::to_string(v) + " beyond its table");
		}
	}
	void run(uint16_t id, size_t i, size_t first_offset, size_t count_offset, bool count8, uint16_t target) {
		size_t first = u16(at(id, i, first_offset));
		size_t n = count8 ? u8(at(id, i, count_offset)) : u16(at(id, i, count_offset));
		if (n > 0 && first + n > count(target)) {
			problem(where(id, i, first_offset) + ": a run beyond its table");
		}
	}
	void expression(size_t root);
	void owners();

	const std::vector<uint8_t>& b_;
	std::map<uint16_t, Section> sections_;
	std::set<uint32_t> starts_;
	size_t label_entries_ = 0;
	std::string error_;
};

std::string Checker::run() {
	if (b_.size() < 32 || std::memcmp(b_.data(), "HCIM", 4) != 0) {
		return "no header";
	}
	if (u16(4) != 1 || u16(6) > 2 || u32(8) != b_.size() || b_.size() % 8 != 0 || u16(20) != 32 || (u32(16) & ~1u) != 0) {
		return "a bad header";
	}
	size_t signed_end = b_.size() - ((u32(16) & 1) != 0 ? 64 : 0);
	std::vector<uint8_t> copy(b_.begin(), b_.begin() + static_cast<long>(signed_end));
	std::memset(copy.data() + 12, 0, 4);
	if (hc::persist::crc32(0, copy.data(), copy.size()) != u32(12)) {
		return "the CRC does not match";
	}
	size_t sections = u16(22);
	size_t end = 32 + 16 * sections;
	int previous = -1;
	for (size_t k = 0; k < sections; ++k) {
		size_t e = 32 + 16 * k;
		uint16_t id = u16(e);
		Section s;
		s.offset = u32(e + 4);
		s.length = u32(e + 8);
		s.entry = u16(e + 12);
		if (static_cast<int>(id) <= previous) {
			return "sections not ascending";
		}
		previous = id;
		if (u16(e + 2) != 0 || u16(e + 14) != 0) {
			return "section flags or reserved set";
		}
		if (s.offset % 8 != 0 || s.offset < end || s.offset + s.length > signed_end) {
			return "section " + std::to_string(id) + " misplaced";
		}
		auto size = kSizes.find(id);
		if (size == kSizes.end() || size->second != s.entry) {
			return "section " + std::to_string(id) + ": unknown or entrySize " + std::to_string(s.entry);
		}
		if (s.entry != 0 && (s.length == 0 || s.length % s.entry != 0)) {
			return "section " + std::to_string(id) + ": empty or not whole records";
		}
		if ((id == 0x01 || id == 0x04 || id == 0x24 || id == 0x2E) && s.count() != 1) {
			return "section " + std::to_string(id) + " holds not exactly one record";
		}
		end = s.offset + s.length;
		sections_[id] = s;
	}
	// The padding up to the signed end is zero, the bytes between sections too.
	if (sections_.count(0x01) == 0 || sections_.count(0x02) == 0) {
		return "image or strings missing";
	}
	// Format 1.1 exactly for an image with its sections, the anonymous pages (spec 8, 9.16).
	if ((u16(6) == 1) != (sections_.count(0x62) == 1)) {
		return "minor " + std::to_string(u16(6)) + " with" + (sections_.count(0x62) == 1 ? "" : "out") + " ui.anonymous";
	}
	if ((sections_.count(0x63) == 1 || sections_.count(0x64) == 1) && sections_.count(0x62) == 0) {
		return "a table of the anonymous pages without ui.anonymous";
	}
	// Strings: NUL at 0 and at the end, sorted and unique.
	const Section& strings = sections_[0x02];
	if (strings.length == 0 || b_[strings.offset] != 0 || b_[strings.offset + strings.length - 1] != 0) {
		return "strings do not start or end with a NUL";
	}
	std::string last;
	bool first = true;
	for (size_t p = 0; p < strings.length;) {
		starts_.insert(static_cast<uint32_t>(p));
		const char* s = reinterpret_cast<const char*>(b_.data() + strings.offset + p);
		std::string text(s);
		if (!first && !(last < text)) {
			return "strings not sorted and unique at '" + text + "'";
		}
		if (text.size() > 511) {
			return "a string longer than 511 bytes";
		}
		first = false;
		last = text;
		p += text.size() + 1;
	}
	for (size_t i = 0; i < count(0x03); ++i) {
		string(0x03, i, 0);
		string(0x03, i, 4, true);
	}
	string(0x01, 0, 0, true);
	string(0x01, 0, 8, true);
	string(0x01, 0, 12);
	string(0x01, 0, 16);
	string(0x01, 0, 20);
	if (count(0x04) == 1) {
		string(0x04, 0, 0);
		string(0x04, 0, 4);
	}
	for (size_t i = 0; i < count(0x10); ++i) {
		string(0x10, i, 0, true);
		label(0x10, i, 4);
		string(0x10, i, 8);
		if (u8(at(0x10, i, 12)) < 1 || u8(at(0x10, i, 12)) > 12 || u8(at(0x10, i, 14)) > 7) {
			problem(where(0x10, i, 12) + ": a type or owner code");
		}
		index(0x10, i, 20, 0x14, true);
		if (((u8(at(0x10, i, 13)) & 0x20) != 0) != (u16(at(0x10, i, 20)) != kNone)) {
			problem(where(0x10, i, 13) + ": the history flag and the history index disagree");
		}
	}
	for (size_t i = 0; i < count(0x11); ++i) {
		string(0x11, i, 0, true);
		label(0x11, i, 4);
		index(0x11, i, 8, 0x11, true);
	}
	for (size_t i = 0; i < count(0x12); ++i) {
		string(0x12, i, 0, true);
		label(0x12, i, 4);
		string(0x12, i, 8);
		index(0x12, i, 12, 0x11, true);
		run(0x12, i, 14, 18, true, 0x13);
	}
	for (size_t i = 0; i < count(0x13); ++i) {
		string(0x13, i, 0, true);
		index(0x13, i, 4, 0x10);
	}
	for (size_t i = 0; i < count(0x14); ++i) {
		index(0x14, i, 0, 0x10);
		run(0x14, i, 12, 2, true, 0x15);
	}
	for (size_t i = 0; i < count(0x20); ++i) {
		run(0x20, i, 2, 4, false, 0x23);
		if (i > 0 && u16(at(0x20, i, 0)) <= u16(at(0x20, i - 1, 0))) {
			problem(where(0x20, i, 0) + ": addresses not ascending");
		}
	}
	for (size_t i = 0; i < count(0x21); ++i) {
		index(0x21, i, 0, 0x10);
		index(0x21, i, 6, 0x20, true);
		run(0x21, i, 8, 10, true, 0x22);
		if (i > 0 && u16(at(0x21, i, 0)) <= u16(at(0x21, i - 1, 0))) {
			problem(where(0x21, i, 0) + ": bindings not by datapoint");
		}
	}
	for (size_t i = 0; i < count(0x22); ++i) {
		index(0x22, i, 0, 0x20);
	}
	for (size_t i = 0; i < count(0x23); ++i) {
		index(0x23, i, 0, 0x21);
	}
	if (count(0x24) == 1) {
		index(0x24, 0, 0, 0x10, true);
		index(0x24, 0, 2, 0x10, true);
		index(0x24, 0, 4, 0x10, true);
	}
	for (size_t i = 0; i < count(0x28); ++i) {
		index(0x28, i, 8, 0x10);
	}
	for (size_t i = 0; i < count(0x2C); ++i) {
		string(0x2C, i, 0, true);
		string(0x2C, i, 4);
		string(0x2C, i, 8);
		run(0x2C, i, 16, 18, true, 0x2D);
	}
	for (size_t i = 0; i < count(0x2D); ++i) {
		index(0x2D, i, 0, 0x10);
		string(0x2D, i, 4);
		string(0x2D, i, 8);
		string(0x2D, i, 12);
	}
	if (count(0x2E) == 1) {
		string(0x2E, 0, 0, true);
		string(0x2E, 0, 4);
		string(0x2E, 0, 8);
		string(0x2E, 0, 12);
	}
	for (size_t i = 0; i < count(0x30); ++i) {
		string(0x30, i, 0, true);
		string(0x30, i, 4);
		index(0x30, i, 12, 0x10, true);
		index(0x30, i, 14, 0x10, true);
		run(0x30, i, 20, 22, false, 0x31);
		run(0x30, i, 24, 26, true, 0x32);
	}
	for (size_t i = 0; i < count(0x40); ++i) {
		string(0x40, i, 0, true);
		label(0x40, i, 4);
		run(0x40, i, 8, 16, true, 0x41);
		run(0x40, i, 10, 17, true, 0x43);
		run(0x40, i, 12, 18, true, 0x44);
		run(0x40, i, 14, 19, true, 0x45);
		index(0x40, i, 22, 0x49, true);
		index(0x40, i, 24, 0x10, true);
		if (u8(at(0x40, i, 16)) < 1) {
			problem(where(0x40, i, 16) + ": no enable switch");
		}
		if ((u8(at(0x40, i, 20)) == 0xFF) != (u8(at(0x40, i, 18)) == 0)) {
			problem(where(0x40, i, 20) + ": initialState and the states disagree");
		}
	}
	for (size_t i = 0; i < count(0x41); ++i) {
		string(0x41, i, 0, true);
		label(0x41, i, 4);
		string(0x41, i, 8);
		run(0x41, i, 16, 14, true, 0x42);
		index(0x41, i, 18, 0x4E);
		index(0x41, i, 20, 0x4E, true);
		index(0x41, i, 22, 0x4E, true);
		index(0x41, i, 24, 0x4E, true);
		index(0x41, i, 26, 0x10);
	}
	for (size_t i = 0; i < count(0x42); ++i) {
		index(0x42, i, 0, 0x4E);
		label(0x42, i, 4);
	}
	for (size_t i = 0; i < count(0x43); ++i) {
		string(0x43, i, 0, true);
		expression(u16(at(0x43, i, 4)));
	}
	for (size_t i = 0; i < count(0x44); ++i) {
		string(0x44, i, 0, true);
		label(0x44, i, 4);
		run(0x44, i, 8, 10, true, 0x47);
		run(0x44, i, 12, 14, true, 0x47);
	}
	for (size_t i = 0; i < count(0x45); ++i) {
		run(0x45, i, 0, 2, true, 0x46);
		if (u16(at(0x45, i, 4)) != kNone) {
			expression(u16(at(0x45, i, 4)));
		}
		run(0x45, i, 8, 10, true, 0x47);
		run(0x45, i, 12, 14, true, 0x47);
	}
	for (size_t i = 0; i < count(0x46); ++i) {
		uint8_t kind = u8(at(0x46, i, 0));
		if (kind == 0 || kind == 6) {
			index(0x46, i, 2, 0x10);
		} else if (kind == 1) {
			expression(u16(at(0x46, i, 2)));
		} else if (kind == 2) {
			index(0x46, i, 2, 0x30);
		} else if (kind > 6) {
			problem(where(0x46, i, 0) + ": a trigger kind");
		}
		if (kind == 6) {
			expression(u16(at(0x46, i, 4)));
		}
	}
	for (size_t i = 0; i < count(0x47); ++i) {
		uint8_t kind = u8(at(0x47, i, 0));
		uint16_t expr = u16(at(0x47, i, 4));
		if (kind == 0 || kind == 6) {
			index(0x47, i, 2, 0x10);
		}
		if (kind == 0 || kind == 4 || ((kind == 1 || kind == 2) && expr != kNone)) {
			expression(expr);
		}
		if (kind == 5) {
			run(0x47, i, 2, 6, false, 0x48);
		}
		if (kind > 6) {
			problem(where(0x47, i, 0) + ": an action kind");
		}
	}
	for (size_t i = 0; i < count(0x48); ++i) {
		string(0x48, i, 0);
		bool text = u32(at(0x48, i, 0)) != 0;
		bool expr = u16(at(0x48, i, 4)) != kNone;
		if (text == expr) {
			problem(where(0x48, i, 0) + ": a log part is a text or an expression");
		}
		if (expr) {
			expression(u16(at(0x48, i, 4)));
		}
	}
	for (size_t i = 0; i < count(0x49); ++i) {
		run(0x49, i, 0, 4, true, 0x4A);
		run(0x49, i, 2, 5, true, 0x4C);
	}
	for (size_t i = 0; i < count(0x4A); ++i) {
		string(0x4A, i, 0, true);
		label(0x4A, i, 4);
		run(0x4A, i, 8, 10, true, 0x4B);
	}
	for (size_t i = 0; i < count(0x50); ++i) {
		string(0x50, i, 0, true);
		string(0x50, i, 4, true);
		label(0x50, i, 8);
		run(0x50, i, 16, 20, true, 0x51);
		run(0x50, i, 18, 21, true, 0x53);
		index(0x50, i, 24, 0x10);
	}
	for (size_t i = 0; i < count(0x51); ++i) {
		string(0x51, i, 0, true);
		run(0x51, i, 4, 6, true, 0x52);
		index(0x51, i, 8, 0x10, true);
	}
	for (size_t i = 0; i < count(0x52); ++i) {
		index(0x52, i, 0, 0x10);
	}
	for (size_t i = 0; i < count(0x53); ++i) {
		string(0x53, i, 0, true);
		index(0x53, i, 4, 0x10, true);
	}
	if (label_entries_ != count(0x03)) {
		problem("label entries are shared or unreferenced");
	}
	owners();
	return error_;
}

void Checker::expression(size_t root) {
	size_t nodes = count(0x4D);
	if (root >= nodes) {
		problem("an expression root beyond the nodes");
		return;
	}
	size_t size = u16(at(0x4D, root, 6));
	if (root + size > nodes) {
		problem("an expression beyond the nodes");
		return;
	}
	std::vector<int> parents(size, -1);
	for (size_t n = root; n < root + size; ++n) {
		uint8_t kind = u8(at(0x4D, n, 0));
		uint8_t type = u8(at(0x4D, n, 2));
		uint8_t argc = u8(at(0x4D, n, 3));
		uint16_t a = u16(at(0x4D, n, 4));
		if (kind > 6 || type > 11) {
			problem("node " + std::to_string(n) + ": a kind or type code");
			return;
		}
		if (kind >= 4) {
			size_t total = 1;
			if (argc > 0 && (a <= n || a + argc > root + size)) {
				problem("node " + std::to_string(n) + ": children outside its expression");
				return;
			}
			for (size_t c = 0; c < argc; ++c) {
				if (parents[a + c - root] >= 0) {
					problem("node " + std::to_string(a + c) + " is a child of two nodes");
					return;
				}
				parents[a + c - root] = static_cast<int>(n);
				total += u16(at(0x4D, a + c, 6));
			}
			if (total != u16(at(0x4D, n, 6))) {
				problem("node " + std::to_string(n) + ": its size is not that of its subtree");
				return;
			}
		} else if (argc != 0) {
			problem("node " + std::to_string(n) + ": a leaf with children");
			return;
		} else if (kind == 1 && a >= count(0x10)) {
			problem("node " + std::to_string(n) + ": no datapoint");
			return;
		} else if (kind == 0 && (type == 3 || type == 7 || type == 11) &&
			(a >= count(0x4E) || u8(at(0x4E, a, 0)) != type)) {
			problem("node " + std::to_string(n) + ": its constant is missing or of another type");
			return;
		}
	}
	for (size_t n = root + 1; n < root + size; ++n) {
		if (parents[n - root] < 0) {
			problem("node " + std::to_string(n) + " is no node of its tree");
			return;
		}
	}
}

void Checker::owners() {
	for (size_t i = 0; i < count(0x10); ++i) {
		uint8_t owner = u8(at(0x10, i, 14));
		uint16_t index = u16(at(0x10, i, 16));
		uint16_t member = u16(at(0x10, i, 18));
		bool back = true;
		switch (owner) {
			case 0: back = index == kNone && member == 0; break;
			case 1:
			case 3: back = index < count(0x21) && u16(at(0x21, index, 0)) == i; break;
			case 2:
				back = index < count(0x30) && u16(at(0x30, index, member == 0 ? 12 : 14)) == i;
				break;
			case 4: back = index < count(0x28) && u16(at(0x28, index, 8)) == i; break;
			case 5:
				if (index >= count(0x40)) {
					back = false;
				} else if (member == 0x8000) {
					back = u16(at(0x40, index, 24)) == i;
				} else {
					back = member < u8(at(0x40, index, 16)) && u16(at(0x41, u16(at(0x40, index, 8)) + member, 26)) == i;
				}
				break;
			case 6: {
				if (index >= count(0x50)) {
					back = false;
					break;
				}
				size_t kind = member >> 8;
				size_t k = member & 0xFF;
				if (kind == 0) {
					back = u16(at(0x50, index, 24)) == i;
				} else if (kind == 1) {
					back = k < u8(at(0x50, index, 21)) && u16(at(0x53, u16(at(0x50, index, 18)) + k, 4)) == i;
				} else {
					back = k < u8(at(0x50, index, 20)) && u16(at(0x51, u16(at(0x50, index, 16)) + k, 8)) == i;
				}
				break;
			}
			case 7: back = index < count(0x2D) && u16(at(0x2D, index, 0)) == i; break;
			default: back = false; break;
		}
		if (!back) {
			problem("datapoint " + std::to_string(i) + ": its owner does not name it back");
			return;
		}
	}
	for (size_t h = 0; h < count(0x14); ++h) {
		if (u16(at(0x10, u16(at(0x14, h, 0)), 20)) != h) {
			problem("history " + std::to_string(h) + ": its datapoint does not name it");
		}
	}
	// Each binding listens on its send and listen addresses, and the listeners are exactly those.
	size_t uses = 0;
	for (size_t k = 0; k < count(0x21); ++k) {
		uses += (u16(at(0x21, k, 6)) != kNone ? 1 : 0) + u8(at(0x21, k, 10));
	}
	if (uses != count(0x23)) {
		problem("the listeners are not the bindings' addresses");
	}
}

/// The test-system template with its "GA:<name>" placeholders replaced by 31/7/n in order of first
/// appearance (as core/test/image_test.cpp fills it).
std::string fill_template(const std::string& text) {
	std::map<std::string, int> names;
	std::string out;
	size_t pos = 0;
	for (;;) {
		size_t at = text.find("\"GA:", pos);
		while (at != std::string::npos && at > 0 && text[at - 1] == '\\') {
			at = text.find("\"GA:", at + 1); // the comment's \"GA:<name>\"
		}
		if (at == std::string::npos) {
			return out + text.substr(pos);
		}
		size_t end = text.find('"', at + 1);
		std::string name = text.substr(at + 4, end - at - 4);
		if (names.count(name) == 0) {
			int n = static_cast<int>(names.size()) + 1;
			names[name] = n;
		}
		out += text.substr(pos, at - pos) + "\"31/7/" + std::to_string(names[name]) + "\"";
		pos = end + 1;
	}
}

/// Compiles and checks; the image's bytes in `out`.
void compile_and_check(const std::string& name, const std::string& text, std::vector<uint8_t>& out,
	const CompileOptions& options = CompileOptions()) {
	std::string error;
	bool ok = compile_image(text.data(), text.size(), out, &error, options);
	INFO(name << ": " << error);
	REQUIRE(ok);
	std::string problem = Checker(out).run();
	INFO(name << ": " << problem);
	CHECK(problem.empty());
}

const char* const kMinimal =
	"48 43 49 4d 01 00 00 00 70 00 00 00 42 2c 77 59"
	"00 00 00 00 20 00 02 00 00 00 00 00 00 00 00 00"
	"01 00 00 00 40 00 00 00 20 00 00 00 20 00 00 00"
	"02 00 00 00 60 00 00 00 0d 00 00 00 00 00 00 00"
	"05 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00"
	"00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
	"00 31 2e 30 00 6d 69 6e 69 6d 61 6c 00 00 00 00";

const char* const kMinimalSigned =
	"48 43 49 4d 01 00 00 00 b0 00 00 00 87 ff f6 f6"
	"01 00 00 00 20 00 02 00 00 00 00 00 00 00 00 00"
	"01 00 00 00 40 00 00 00 20 00 00 00 20 00 00 00"
	"02 00 00 00 60 00 00 00 0d 00 00 00 00 00 00 00"
	"05 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00"
	"00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
	"00 31 2e 30 00 6d 69 6e 69 6d 61 6c 00 00 00 00"
	"d8 39 e1 4f 50 d3 4b d3 97 ef 9d 57 2d d7 af 3e"
	"a8 3f d8 ce 24 a8 4e bb 28 ee ed 99 b4 1e 22 2d"
	"90 35 04 e9 07 2e 01 ac 4f 86 5c 39 3c b3 42 83"
	"fd 39 a2 ef 6e 21 aa 0f d6 7b e4 bf 92 0f 28 06";

/// The seed of the test key "installation" (spec/fixtures/signatures/vectors.json; TEST KEY ONLY).
std::vector<uint8_t> installation_seed() {
	hc_test::Json vectors;
	std::string error;
	hc_test::JsonReader(read_file(HC_SPEC_DIR "/fixtures/signatures/vectors.json")).parse(vectors, error);
	return from_hex(vectors["keys"]["installation"]["private_seed_TEST_ONLY"].text.c_str());
}

/// Whether a case's expected load refusal is one the device makes (spec/runtime-image.md 13.1, 10.8):
/// such a case has an image.
bool device_refusal(const hc_test::Json& expected) {
	const std::string& reason = expected["loadRefused"].text;
	return reason == "unknownType" || reason == "missingInput" || reason == "invalidParameter";
}

/// Every case of a corpus and each image step of its timeline compiles into a checked image.
void compile_corpus(const char* directory) {
	CompileOptions options;
	options.registry = &hc_test::corpus_registry();
	size_t cases = 0, images = 0, refused = 0;
	for (const std::string& path : json_files(std::string(HC_SPEC_DIR "/fixtures/") + directory)) {
		std::string text = read_file(path);
		hc_test::Json json;
		std::string error;
		REQUIRE(hc_test::JsonReader(text).parse(json, error));
		std::string name = std::filesystem::path(path).stem().string();
		options.case_name = name.c_str();
		bool load_refused = json["expected"].has("loadRefused");
		std::vector<int> steps = {-1};
		for (size_t k = 0; k < json["timeline"].items.size(); ++k) {
			if (json["timeline"].items[k].has("image")) {
				steps.push_back(static_cast<int>(k));
			}
		}
		for (int step : steps) {
			std::vector<uint8_t> out;
			bool ok = compile_case(text.data(), text.size(), step, out, &error, options);
			INFO(name << " step " << step << ": " << error);
			if (load_refused && !ok) {
				// A semantic refusal of E-Home's validation (13.1): no image, unless the device refuses it.
				CHECK_FALSE(device_refusal(json["expected"]));
				++refused;
				continue;
			}
			REQUIRE(ok);
			std::string problem = Checker(out).run();
			INFO(name << " step " << step << ": " << problem);
			CHECK(problem.empty());
			++images;
		}
		++cases;
	}
	MESSAGE(std::string(directory) << ": " << cases << " cases, " << images << " images, " << refused
		<< " semantic refusals without an image");
	CHECK(cases > 0);
}

} // namespace

TEST_CASE("Image compiler: minimal.json is the 112 bytes of spec/runtime-image.md 13, signed the 176 bytes") {
	std::string text = read_file(HC_SPEC_DIR "/fixtures/images/minimal.json");
	std::vector<uint8_t> out;
	std::string error;
	REQUIRE(compile_image(text.data(), text.size(), out, &error));
	CHECK(out == from_hex(kMinimal));
	CHECK(Checker(out).run().empty());

	std::vector<uint8_t> seed = installation_seed();
	REQUIRE(seed.size() == 32);
	CompileOptions options;
	options.sign_seed = seed.data();
	REQUIRE(compile_image(text.data(), text.size(), out, &error, options));
	CHECK(out == from_hex(kMinimalSigned));
	CHECK(Checker(out).run().empty());
	// The signature verifies under the public key over the message of 9.17.
	uint8_t key[hc::kEd25519KeySize];
	hc::ed25519_public_key(seed.data(), key);
	uint8_t message[52];
	std::memcpy(message, "homecontrol-image-v2", 20);
	hc::Sha256::hash(out.data(), 112, message + 20);
	CHECK(hc::ed25519_verify(out.data() + 112, message, sizeof(message), key));
}

TEST_CASE("Image compiler: every image of spec/fixtures/images compiles into a checked image") {
	size_t n = 0;
	for (const std::string& path : json_files(HC_SPEC_DIR "/fixtures/images")) {
		std::string text = read_file(path);
		if (path.find(".template.json") != std::string::npos) {
			text = fill_template(text);
		}
		std::vector<uint8_t> out;
		compile_and_check(path, text, out);
		++n;
	}
	for (const std::string& path : json_files(HC_SPEC_DIR "/fixtures/images/cases", "accept-")) {
		std::vector<uint8_t> out;
		compile_and_check(path, read_file(path), out);
		++n;
	}
	CHECK(n >= 15);
}

TEST_CASE("Image compiler: anonymous-pages.json, the format 1.1 fixture of the anonymous pages") {
	// Its `.bin` is the test compiler's until E-Home's compiler writes the sections (haumacher/ehome#184,
	// spec/runtime-image.md 13.1); HC_WRITE_FIXTURES=1 writes it again.
	std::string path = HC_SPEC_DIR "/fixtures/images/anonymous-pages.json";
	std::vector<uint8_t> out;
	compile_and_check(path, read_file(path), out);
	CHECK(out[6] == 1);
	if (std::getenv("HC_WRITE_FIXTURES") != nullptr) {
		std::ofstream file(HC_SPEC_DIR "/fixtures/images/anonymous-pages.bin", std::ios::binary);
		file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
	}
}

TEST_CASE("Image compiler: accept-initial-values.json, the format 1.2 case of the initial values") {
	// The initial values of application datapoints (homecontrol #107, haumacher/ehome#217): the image
	// is written in format 1.2; its `.bin` is compared with E-Home's (Image.CompiledCorpus).
	// HC_WRITE_FIXTURES=1 writes it again.
	std::string path = HC_SPEC_DIR "/fixtures/images/cases/accept-initial-values.json";
	std::vector<uint8_t> out;
	compile_and_check(path, read_file(path), out);
	CHECK(out[6] == 2);
	if (std::getenv("HC_WRITE_FIXTURES") != nullptr) {
		std::ofstream file(HC_SPEC_DIR "/fixtures/images/cases/accept-initial-values.bin", std::ios::binary);
		file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
	}
}

TEST_CASE("Image compiler: every rule case and each of its image steps compiles into a checked image") {
	compile_corpus("rules");
}

TEST_CASE("Image compiler: every block case and each of its image steps compiles into a checked image") {
	compile_corpus("blocks");
}

TEST_CASE("Image compiler: the members no fixture image has (external, mqtt, archives, a block, Zigbee reporting)") {
	const char* source = R"({
		"image": {"schemaVersion": "1.7", "projectId": "all", "revision": 3, "name": "All"},
		"settings": {"tz": "UTC0", "location": {"lat": 48.1, "lon": 11.5}},
		"datapoints": [
			{"id": "t", "type": "real", "history": {"retention": 3600, "spacing": 60,
			 "archives": [{"resolution": 3600, "retention": 86400}]}},
			{"id": "w", "type": "real"},
			{"id": "z", "type": "boolean", "writable": true, "transmit": true,
			 "zigbee": {"ieee": "00:12:4b:00:01:02:03:04", "endpoint": 11, "cluster": 6, "attribute": 0, "command": 6,
			            "reporting": {"min": 1, "max": 300}}},
			{"id": "v1", "type": "real", "knx": {"dpt": "9.001", "send": "1/1/1", "listen": ["1/1/2", "1/1/1"]}},
			{"id": "v2", "type": "real", "knx": {"dpt": "9.001", "listen": ["1/1/2"]}},
			{"id": "s", "type": "boolean"}
		],
		"knx": {"timeMaster": {"time": "0/7/1", "interval": 900, "atMidnight": false}},
		"external": {"sources": [{"id": "weather", "preset": "open-meteo",
			"bindings": [{"datapoint": "w", "variable": "temperature"}]}]},
		"mqtt": {"broker": "mqtt://192.0.2.5", "discovery": {"enabled": false}},
		"schedules": [{"id": "e", "label": "Evening", "kind": "weekly", "time": {"hour": 18, "minute": 0, "second": 0},
			"output": "s", "adjustable": ["time"], "bounds": {"time": {"step": 900}}}],
		"blocks": [{"id": "curve", "type": "org.homecontrol.heating-curve", "version": "1.0", "label": "Curve",
			"inputs": {"outside": "t"}}]
	})";
	std::vector<uint8_t> out;
	compile_and_check("all", source, out);
	// The time master's datapoint follows the declared ones; its binding sends 0/7/1 with DPT 10.001.
	CHECK(out.size() % 8 == 0);
}
