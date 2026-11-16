/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The heap of an image swap per phase (homecontrol #70, doc/image.md "Memory of a swap", issue #87):
// an image with k blocks deployed over the same image with k blocks (and over k - 1), the whole heap
// of the test binary counted from before the receive to the end of the apply, and at every phase
// the image service reports (LoadProbe). The image is binary and runs in place from its slot (flash
// on the device, kept out of the counted heap here): during the receive the device holds only the
// swap table of the values and the rules' Engine::Carry (spec/runtime-image.md 12), no image text.
//
// The 32-bit model build (scripts/c6-model) compiles this file with the core; the images are
// written in the JSON source form and compiled by the test compiler (image_compiler.cpp), which that
// build must therefore compile too.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "allocation_counter.h"
#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/external_values.h"
#include "hc/image/image_service.h"
#include "hc/image/memory_image_slot.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "image_support.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"

using hc::image::ImageService;
using hc::image::MemoryImageSlot;
using hc::image::Source;

namespace {

std::string fixture_text(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

/// The test-system template with its "GA:<name>" placeholders replaced by 31/7/n, in order of first
/// appearance (as image_test.cpp fills it: addresses no real project here uses).
std::string filled_template() {
	std::string text = fixture_text("test-system.template.json");
	std::map<std::string, unsigned> names;
	std::string out;
	size_t pos = 0;
	for (;;) {
		size_t at = text.find("\"GA:", pos);
		while (at != std::string::npos && at > 0 && text[at - 1] == '\\') {
			at = text.find("\"GA:", at + 1); // the comment's \"GA:<name>\"
		}
		if (at == std::string::npos) {
			out += text.substr(pos);
			break;
		}
		size_t end = text.find('"', at + 1);
		std::string name = text.substr(at + 4, end - at - 4);
		if (names.count(name) == 0) {
			unsigned n = static_cast<unsigned>(names.size() + 1);
			names[name] = n;
		}
		out += text.substr(pos, at - pos);
		out += "\"31/7/" + std::to_string(names[name]) + "\"";
		pos = end + 1;
	}
	return out;
}

/// The source `json` compiled; the bytes stand for the body arriving from the network (on the
/// device in pieces through the HTTP server's buffer), so they are kept out of the counted heap.
std::vector<uint8_t> compile(const std::string& json) {
	hc_test::HeapExclusion network;
	return hc_test::compiled(json);
}

/// A provider of dynamic datapoints, as the Zigbee adapter is on the C6. Armed (fail_next()), it
/// makes the next apply fail after the model was replaced (homecontrol #71): when the model is loaded
/// it binds the image's datapoint `victim` to a technology of its own, so the KNX adapter refuses its
/// mapping in the step `technologies` and the device is left without an image (spec I9).
class NoDevices : public hc::image::DynamicSpecProvider {
public:
	const char* provider_name() const override { return "zigbee"; }
	bool dynamic_specs(const hc::image::ImageBindings&, const hc::image::DynamicSpec*& specs, size_t& count,
		const char**) override {
		specs = nullptr;
		count = 0;
		return true;
	}
	void model_loaded(const hc::image::ImageBindings&) override {
		if (model_ == nullptr) {
			return;
		}
		const hc::Datapoint* dp = model_->find(victim_);
		if (dp == nullptr) {
			return; // the model without the image (during the receive): armed for the image's
		}
		if (model_->bind(dp->id(), hijack_) == hc::DatapointStatus::ok) {
			++failures_caused;
		}
		model_ = nullptr; // once
	}
	void model_cleared() override {}

	/// A device joined or left (its datapoints are none here): the service reloads the model.
	void changed() { notify_specs_changed(); }

	void fail_next(hc::DatapointModel& model, const char* victim) {
		model_ = &model;
		victim_ = victim;
	}

	int failures_caused = 0;

private:
	hc::DatapointModel* model_ = nullptr;
	const char* victim_ = nullptr;
	hc_test::TestTechnology hijack_;
};

/// The image slot of the device is flash: its RAM stand-in is not heap.
std::unique_ptr<MemoryImageSlot> flash_slot() {
	hc_test::HeapExclusion flash;
	return std::unique_ptr<MemoryImageSlot>(new MemoryImageSlot());
}

/// Records the heap in each phase of a load (in fixed storage: the probe allocates nothing): the
/// absolute peaks, so that the deploy's peak spans the receive before the first phase as well.
struct PeakProbe : hc::image::LoadProbe {
	struct Phase {
		char name[24];
		long peak;    ///< The most held at once during the phase, above the start of the load.
		long after;   ///< Held at its end, above the start of the load.
		long charged; ///< The peak as the C6's heap charges it (allocation_counter.h), above the start.
		long largest; ///< The largest single block allocated in the phase.
		size_t peak_abs;
		size_t charged_abs;
	};
	static constexpr size_t kMaxPhases = 32;
	size_t base = 0;
	size_t charged_base = 0;
	/// The segment before load_begun() (the receive: the swap table, the carry), absolute.
	size_t begun_peak = 0;
	size_t begun_charged = 0;
	size_t begun_largest = 0;
	bool begun = false;
	Phase phases[kMaxPhases] = {};
	size_t count = 0;

	void reset() {
		count = 0;
		begun = false;
	}
	void load_begun() override {
		count = 0;
		begun = true;
		begun_charged = hc_test::heap_charged_peak();
		begun_largest = hc_test::heap_largest();
		begun_peak = hc_test::heap_mark();
		base = hc_test::heap_live();
		charged_base = hc_test::heap_charged();
	}
	void load_phase(const char* name) override {
		size_t charged = hc_test::heap_charged_peak();
		size_t largest = hc_test::heap_largest();
		size_t peak = hc_test::heap_mark();
		size_t live = hc_test::heap_live();
		if (count == kMaxPhases) {
			return; // the test's check of the phase count fails (no doctest call here: it allocates)
		}
		Phase& p = phases[count++];
		std::snprintf(p.name, sizeof(p.name), "%s", name);
		p.peak = static_cast<long>(peak) - static_cast<long>(base);
		p.after = static_cast<long>(live) - static_cast<long>(base);
		p.charged = static_cast<long>(charged) - static_cast<long>(charged_base);
		p.largest = static_cast<long>(largest);
		p.peak_abs = peak;
		p.charged_abs = charged;
	}
	long largest() const {
		long top = begun ? static_cast<long>(begun_largest) : 0;
		for (size_t i = 0; i < count; ++i) {
			top = phases[i].largest > top ? phases[i].largest : top;
		}
		return top;
	}
	const Phase* find(const char* name) const {
		for (size_t i = 0; i < count; ++i) {
			if (std::strcmp(phases[i].name, name) == 0) {
				return &phases[i];
			}
		}
		return nullptr;
	}
};

/// What a deploy cost, relative to the heap before its receive began (the old image in its steady
/// state).
struct SwapCost {
	bool ok = false;
	long peak = 0;      ///< The most held at once above the start.
	long charged = 0;   ///< The same as the C6 charges it.
	long receiving = 0; ///< Held while the slot is written (the old image gone), above the start.
	long receiving_charged = 0;
	long largest = 0; ///< The largest single block of the deploy.
};

/// The blob store of the device keeps its content in the flash: the fake's maps are not heap.
class FlashBlobs : public hc::BlobStore {
public:
	hc::BlobStatus write(const char* name, const hc::BlobChunk* chunks, size_t count) override {
		hc_test::HeapExclusion flash;
		return fake.write(name, chunks, count);
	}
	hc::BlobStatus read(const char* name, size_t offset, void* buffer, size_t size, size_t& length) const override {
		hc_test::HeapExclusion flash;
		return fake.read(name, offset, buffer, size, length);
	}
	hc::BlobStatus size(const char* name, size_t& size) const override {
		hc_test::HeapExclusion flash;
		return fake.size(name, size);
	}
	hc::BlobStatus remove(const char* name) override {
		hc_test::HeapExclusion flash;
		return fake.remove(name);
	}
	void visit(hc::BlobVisitor& visitor) const override {
		hc_test::HeapExclusion flash;
		fake.visit(visitor);
	}
	hc::BlobStatus patch(const char* name, size_t offset, const hc::BlobChunk* chunks, size_t count) override {
		hc_test::HeapExclusion flash;
		return fake.patch(name, offset, chunks, count);
	}
	bool free_space(size_t& bytes) const override {
		hc_test::HeapExclusion flash;
		return fake.free_space(bytes);
	}

	hc_test::FakeBlobStore fake;
};

/// The NVS of the device: not heap either.
class FlashSettings : public hc::Settings {
public:
	hc::SettingsStatus get_string(const char* key, char* buffer, size_t size) const override {
		hc_test::HeapExclusion nvs;
		return fake.get_string(key, buffer, size);
	}
	hc::SettingsStatus set_string(const char* key, const char* value) override {
		hc_test::HeapExclusion nvs;
		return fake.set_string(key, value);
	}
	hc::SettingsStatus get_int(const char* key, int32_t& value) const override {
		hc_test::HeapExclusion nvs;
		return fake.get_int(key, value);
	}
	hc::SettingsStatus set_int(const char* key, int32_t value) override {
		hc_test::HeapExclusion nvs;
		return fake.set_int(key, value);
	}
	hc::SettingsStatus get_bool(const char* key, bool& value) const override {
		hc_test::HeapExclusion nvs;
		return fake.get_bool(key, value);
	}
	hc::SettingsStatus set_bool(const char* key, bool value) override {
		hc_test::HeapExclusion nvs;
		return fake.set_bool(key, value);
	}
	hc::SettingsStatus erase(const char* key) override {
		hc_test::HeapExclusion nvs;
		return fake.erase(key);
	}
	void visit(hc::SettingsVisitor& visitor) const override {
		hc_test::HeapExclusion nvs;
		fake.visit(visitor);
	}

	hc_test::FakeSettings fake;
};

/// The log of the device is a ring of its own: the capture's lines are not heap.
class RingSink : public hc::LogSink {
public:
	void write(hc::LogLevel level, const char* tag, const char* message) override {
		hc_test::HeapExclusion ring;
		sink.write(level, tag, message);
	}
	hc_test::CapturingSink sink;
};

struct SwapRig {
	hc_test::FakeClock clock;
	RingSink sink;
	hc_test::FakeSystemInfo system;
	FlashSettings settings;
	hc_test::FakeTimeZone time_zone;
	hc_test::FakeNetworkDriver network{clock};
	hc::Controller controller{hc::Platform{clock, sink, system, settings, time_zone, network}};
	hc::DatapointModel model{clock};
	hc_test::FakeUdpSocket socket{clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), clock, model, tunnel};
	FlashBlobs blobs;
	std::unique_ptr<MemoryImageSlot> slot = flash_slot();
	NoDevices zigbee;
	ImageService service{controller, model, blobs, *slot, adapter, &tunnel};
	PeakProbe probe;
	size_t empty = 0;         ///< The heap held before the first image.
	size_t empty_charged = 0; ///< The same as the C6 charges it.

	/// `start` false: the service is started by start_service() (after the test wrote the slot).
	explicit SwapRig(bool start = true) {
		controller.start();
		controller.connectivity().link_up("192.0.2.10");
		adapter.start();
		REQUIRE(service.add_provider(zigbee));
		if (start) {
			start_service();
		}
	}

	void start_service() {
		service.start();
		settle();
		empty = hc_test::heap_live();
		empty_charged = hc_test::heap_charged();
	}

	void settle() {
		for (int i = 0; i < 5; ++i) {
			clock.now_ms += 10;
			controller.run_once();
		}
	}

	/// Deploys `bytes` as PUT /api/image does (receive_begin, receive_write, receive_end) and measures
	/// it. No doctest call in here: a check allocates its message (BlockLimit scopes around it).
	SwapCost deploy(const std::vector<uint8_t>& bytes) {
		SwapCost cost;
		probe.reset();
		service.set_load_probe(&probe);
		hc_test::heap_mark();
		size_t start = hc_test::heap_live();
		size_t start_charged = hc_test::heap_charged();
		size_t head = bytes.size() < 32 ? bytes.size() : 32;
		bool begun = service.receive_begin(bytes.data(), head, bytes.size(), Source::api);
		cost.receiving = static_cast<long>(hc_test::heap_live()) - static_cast<long>(start);
		cost.receiving_charged = static_cast<long>(hc_test::heap_charged()) - static_cast<long>(start_charged);
		if (begun) {
			service.receive_write(bytes.data(), bytes.size());
			cost.ok = service.receive_end();
		}
		service.set_load_probe(nullptr);
		size_t peak = hc_test::heap_peak();
		size_t charged = hc_test::heap_charged_peak();
		long largest = static_cast<long>(hc_test::heap_largest());
		if (probe.begun) {
			peak = probe.begun_peak > peak ? probe.begun_peak : peak;
			charged = probe.begun_charged > charged ? probe.begun_charged : charged;
			for (size_t i = 0; i < probe.count; ++i) {
				peak = probe.phases[i].peak_abs > peak ? probe.phases[i].peak_abs : peak;
				charged = probe.phases[i].charged_abs > charged ? probe.phases[i].charged_abs : charged;
			}
		}
		cost.peak = static_cast<long>(peak) - static_cast<long>(start);
		cost.charged = static_cast<long>(charged) - static_cast<long>(start_charged);
		cost.largest = probe.largest() > largest ? probe.largest() : largest;
		return cost;
	}

	/// Compiles and deploys `text`, lets the loop settle (the rules' and blocks' start cascades, the
	/// first writes); the deploy's cost.
	SwapCost load(const std::string& text) {
		std::vector<uint8_t> bytes = compile(text);
		SwapCost cost = deploy(bytes);
		{
			hc_test::HeapExclusion network;
			bytes = std::vector<uint8_t>();
		}
		REQUIRE_MESSAGE(cost.ok, hc_test::describe(service.report()));
		settle();
		return cost;
	}

	/// The heap the loaded image holds in the steady state.
	long held() const { return static_cast<long>(hc_test::heap_live()) - static_cast<long>(empty); }
	long held_charged() const {
		return static_cast<long>(hc_test::heap_charged()) - static_cast<long>(empty_charged);
	}

	/// The phases of the last deploy, one per line.
	std::string phases() const {
		std::string line;
		if (probe.begun) {
			char cell[96];
			std::snprintf(cell, sizeof(cell), "\n  %-14s largest block %5ld", "receive", static_cast<long>(probe.begun_largest));
			line += cell;
		}
		for (size_t i = 0; i < probe.count; ++i) {
			const PeakProbe::Phase& ph = probe.phases[i];
			char cell[128];
			std::snprintf(cell, sizeof(cell), "\n  %-14s peak %6ld (charged %6ld), held after %6ld, largest block %5ld",
				ph.name, ph.peak, ph.charged, ph.after, ph.largest);
			line += cell;
		}
		return line;
	}
};

std::string room_controller(const std::string& id, const std::string& temperature, const std::string& sw) {
	return R"({"id": ")" + id + R"(", "type": "org.homecontrol.room-controller", "version": "1.0", )"
		R"("label": {"en": "Test room", "de": "Testraum"}, "inputs": {"temperature": ")" + temperature +
		R"("}, "outputs": {"switch": ")" + sw + R"("}})";
}

std::string heating_demand(const std::string& id, const std::string& room, const std::string& request) {
	return R"({"id": ")" + id + R"(", "type": "org.homecontrol.heating-demand", "version": "1.0", )"
		R"("label": {"en": "Heating", "de": "Heizung"}, "inputs": {"valves": ["block/)" + room +
		R"(/valve"]}, "outputs": {"request": ")" + request + R"("}})";
}

/// The value of the member `name` (a string or a number) of the image's `image` object replaced by
/// `value` (written as it stands).
void set_member(std::string& text, const std::string& name, const std::string& value) {
	size_t at = text.find("\"" + name + "\"");
	REQUIRE_MESSAGE(at != std::string::npos, name);
	at = text.find(':', at) + 1;
	while (text[at] == ' ') {
		++at;
	}
	size_t end = text[at] == '"' ? text.find('"', at + 1) + 1 : text.find_first_of(",}\n", at);
	text.replace(at, end - at, value);
}

/// `base` (an image without blocks) as schema 1.3 at `revision` with `k` blocks: room controllers and
/// heating demands alternating, as E-Home's revision 13 of the test system (a room controller on
/// `temperature` switching `outputs[0]`, a heating demand over its valve requesting on `outputs[1]`,
/// the next pair on the next two outputs).
std::string with_blocks(std::string base, unsigned revision, size_t k, const std::string& temperature,
	const std::vector<std::string>& outputs) {
	set_member(base, "schemaVersion", "\"1.3\"");
	set_member(base, "revision", std::to_string(revision));
	std::string blocks;
	for (size_t i = 0; i < k; ++i) {
		std::string room = "raumregler" + (i / 2 > 0 ? std::to_string(i / 2 + 1) : std::string());
		std::string demand = "heizbedarf" + (i / 2 > 0 ? std::to_string(i / 2 + 1) : std::string());
		REQUIRE(i < outputs.size());
		blocks += (i > 0 ? ", " : "") + (i % 2 == 0 ? room_controller(room, temperature, outputs[i])
													: heating_demand(demand, room, outputs[i]));
	}
	while (!base.empty() && (base.back() == '\n' || base.back() == ' ' || base.back() == '\t')) {
		base.pop_back();
	}
	REQUIRE(base.back() == '}');
	base.pop_back();
	base += ", \"blocks\": [" + blocks + "]}";
	return base;
}


/// The peak of a deploy above the old image's steady state is at most kSwapBudget for an image
/// without blocks (example.json on the 64-bit host; the 32-bit model build gives the C6's figures, a
/// little less), plus a block's steady cost per block; no single allocation is above kLargestBlock.
constexpr long kSwapBudget = 9 * 1024;
constexpr long kLargestBlock = 3 * 1024;

/// The budget check of one block type: `make(revision, k)` is the image with k instances. k = 0 .. 4
/// deployed over the same image (a new revision) and over the image with k - 1.
void check_budget(const char* what, std::string (*make)(std::string, unsigned, size_t), const std::string& base) {
	long held[5] = {};
	long over_same[5] = {};
	for (size_t k = 0; k <= 4; ++k) {
		SwapRig rig;
		rig.load(make(base, 1, k));
		held[k] = rig.held();
		SwapCost cost = rig.load(make(base, 2, k));
		CHECK(rig.probe.count < PeakProbe::kMaxPhases);
		CHECK(rig.held() == held[k]); // a redeploy of the same image holds what the first deploy held
		over_same[k] = cost.peak;
		// While the slot is written the old image is gone: what is held then is the swap table and
		// the carry (spec/runtime-image.md 12), less than the image held.
		CHECK(cost.receiving < 0);
		CHECK(cost.largest <= kLargestBlock);
		MESSAGE(k << " " << std::string(what) << "(s): the image holds " << held[k] << " B, the swap's peak is " << over_same[k]
			<< " B above it (charged " << cost.charged << "), while receiving " << held[k] + cost.receiving
			<< " B are held, its largest block " << cost.largest << " B"
			<< (cost.largest > kLargestBlock ? rig.phases() : std::string()));
	}
	CHECK(over_same[0] <= kSwapBudget);
	for (size_t k = 1; k <= 4; ++k) {
		long block = (held[k] - held[0]) / static_cast<long>(k); // a block's steady cost in this image
		CHECK(block > 0);
		CHECK(over_same[k] <= kSwapBudget + static_cast<long>(k) * block);
		CHECK(over_same[k] - over_same[0] <= static_cast<long>(k) * block);
		// Deployed over the image with one block fewer: the old image holds less, the peak above it
		// is at most what it is above the same image plus the added block's steady cost.
		SwapRig rig;
		rig.load(make(base, 1, k - 1));
		long old = rig.held();
		CHECK(old == held[k - 1]);
		SwapCost cost = rig.load(make(base, 2, k));
		CHECK(rig.held() == held[k]);
		CHECK(cost.peak <= kSwapBudget + static_cast<long>(k) * block + (held[k] - old));
		CHECK(cost.largest <= kLargestBlock);
		MESSAGE(k << " " << std::string(what) << "(s) over " << k - 1 << ": " << cost.peak << " B above the old image (" << old
			<< " B), a block holds " << block << " B");
	}
}

std::string example_blocks(std::string base, unsigned revision, size_t k) {
	const std::vector<std::string> outputs = {"living.light", "hall.light", "hall.blind.move", "away"};
	return with_blocks(base, revision, k, "living.temperature", outputs);
}

} // namespace

TEST_CASE("Swap memory: the peak of each phase (measurement, HC_SWAP_IMAGE)") {
	const char* path = std::getenv("HC_SWAP_IMAGE");
#ifdef HC_SWAP_IMAGE_DEFAULT
	path = path != nullptr ? path : HC_SWAP_IMAGE_DEFAULT; // the 32-bit model build (doc/image.md)
#endif
	if (path == nullptr) {
		return; // a measurement on a real image (homecontrol #70, its JSON source form); the tests below run always
	}
	std::string base;
	REQUIRE(hc_test::read_text(path, base));
	std::vector<std::string> outputs = {"0/0/13", "0/0/14", "0/0/15", "0/0/16"};
	for (size_t k : {0, 1, 2, 4}) {
		for (bool over_same : {false, true}) {
			if (k == 0 && !over_same) {
				continue;
			}
			SwapRig rig;
			size_t before = k > 0 && !over_same ? k - 1 : k;
			rig.load(with_blocks(base, 12, before, "0/0/28", outputs));
			long held = rig.held();
			long held_charged = rig.held_charged();
			SwapCost cost = rig.load(with_blocks(base, 13, k, "0/0/28", outputs));
			long now = rig.held();
			long now_charged = rig.held_charged();
			MESSAGE(k << " block(s) over " << before << ": the old image held " << held << " B (charged " << held_charged
				<< "), the new " << now << " B (" << now_charged << "); while receiving " << held + cost.receiving
				<< " B; peak above the old " << cost.peak << " B (" << cost.charged << "), above the new "
				<< held + cost.peak - now << " B (" << held_charged + cost.charged - now_charged << ")" << rig.phases());
		}
	}
}

TEST_CASE("Swap memory: the figures of example.json and the test-system template (issue #87)") {
	// What the report of issue #87 quotes: the heap a loaded image holds in its steady state, what the
	// swap holds while the slot is written (the swap table and the carry), the peak of a redeploy
	// above the old image, and the largest block, on the 64-bit host and as the C6's heap charges it
	// (the 32-bit model build gives the C6's sizes).
	struct Image {
		const char* name;
		std::string text;
		std::string next; ///< The same at another revision.
		/// Whether the swap's largest block must stay within kLargestBlock: the template's history ring
		/// (a day at one sample a minute, 11.5 KB) is one block of the image's steady state.
		bool small_blocks;
	};
	std::vector<Image> images = {{"example.json", fixture_text("example.json"), "", true},
		{"test-system.template.json", filled_template(), "", false}};
	for (Image& image : images) {
		image.next = image.text;
		set_member(image.next, "revision", "99");
	}
	for (const Image& image : images) {
		CAPTURE(std::string(image.name));
		SwapRig rig;
		rig.load(image.text);
		long held = rig.held();
		long held_charged = rig.held_charged();
		SwapCost cost = rig.load(image.next);
		CHECK(rig.held() == held);
		CHECK(cost.receiving < 0);
		CHECK(cost.peak <= kSwapBudget);
		if (image.small_blocks) {
			CHECK(cost.largest <= kLargestBlock);
		}
		MESSAGE(std::string(image.name) << " (" << rig.service.info().bytes << " bytes in the slot, " << rig.model.size()
			<< " datapoints, " << rig.service.info().rules << " rules): the loaded image holds " << held
			<< " B of the heap (charged " << held_charged << "); a redeploy holds " << held + cost.receiving
			<< " B while the slot is written (charged " << held_charged + cost.receiving_charged << "), peaks "
			<< cost.peak << " B above the old image (charged " << cost.charged << "), largest block " << cost.largest
			<< " B" << rig.phases());
	}
}

TEST_CASE("Swap memory: a block costs at the peak at most its steady size; the swap stays within its budget and "
	"takes no large block") {
	// The peak of a deploy above the old image's steady state, for an image with k = 0 .. 4 blocks
	// deployed over the same image (a new revision) and over the image with k - 1 blocks: at most the
	// swap's budget for the image without blocks plus k times a block's steady cost, and a block adds
	// to the peak at most its steady cost (doc/image.md "Memory of a swap", doc/blocks.md "Memory").
	// No single allocation of the swap is larger than kLargestBlock: a fragmented heap refuses one
	// block with enough free bytes beside it (homecontrol #70). They cover the core's allocations
	// only: on the board the tasks the load holds back add their own.
	check_budget("block", example_blocks, fixture_text("example.json"));
}

namespace {

/// `base` as schema 1.3 at `revision` with `k` sun shadings (spec/blocks.md 18), one per facade, on
/// the brightness, the button, the presence and the temperature of example.json.
std::string with_shadings(std::string base, unsigned revision, size_t k) {
	set_member(base, "schemaVersion", "\"1.3\"");
	set_member(base, "revision", std::to_string(revision));
	std::string blocks;
	for (size_t i = 0; i < k; ++i) {
		blocks += (i > 0 ? ", " : "") + std::string(R"({"id": "beschattung)") + std::to_string(i + 1) +
			R"(", "type": "org.homecontrol.sun-shading", "version": "1.0", "label": {"en": "Shading", "de": "Beschattung"},
			"inputs": {"brightness": "hall.lux", "manual": ["hall.button", "hall.presence"], "presence": "hall.presence",
			"roomTemperature": "living.temperature"}, "parameters": {"facadeAzimuth": )" + std::to_string(90 * i) +
			R"(, "slatMode": "tracking"}})";
	}
	while (!base.empty() && (base.back() == '\n' || base.back() == ' ' || base.back() == '\t')) {
		base.pop_back();
	}
	REQUIRE(base.back() == '}');
	base.pop_back();
	base += ", \"blocks\": [" + blocks + "]}";
	return base;
}

} // namespace


TEST_CASE("Swap memory: sun shadings keep the swap's budget and take no large block") {
	// The test above for the sun shading (homecontrol #63).
	check_budget("sun shading", with_shadings, fixture_text("example.json"));
}

namespace {

/// `base` as schema 1.3 at `revision` with `k` house modes (spec/blocks.md 19) on the presence, the
/// button, the morning schedule and the away switch of example.json.
std::string with_house_modes(std::string base, unsigned revision, size_t k) {
	set_member(base, "schemaVersion", "\"1.3\"");
	set_member(base, "revision", std::to_string(revision));
	std::string blocks;
	for (size_t i = 0; i < k; ++i) {
		blocks += (i > 0 ? ", " : "") + std::string(R"({"id": "haus)") + std::to_string(i + 1) +
			R"(", "type": "org.homecontrol.house-mode", "version": "1.0", "label": {"en": "House", "de": "Haus"},
			"inputs": {"presence": ["hall.presence", "hall.button"], "awayButton": "hall.button",
			"nightButton": ["hall.button", "away"], "vacation": "away", "morning": "schedule.morning"},
			"parameters": {"awayDelay": )" + std::to_string(600000 * (i + 1)) + "}}";
	}
	while (!base.empty() && (base.back() == '\n' || base.back() == ' ' || base.back() == '\t')) {
		base.pop_back();
	}
	REQUIRE(base.back() == '}');
	base.pop_back();
	base += ", \"blocks\": [" + blocks + "]}";
	return base;
}

} // namespace


TEST_CASE("Swap memory: house modes keep the swap's budget and take no large block") {
	// The test of the sun shadings for the house mode (homecontrol #64).
	check_budget("house mode", with_house_modes, fixture_text("example.json"));
}

namespace {

/// `base` as schema 1.3 at `revision` with `k` PV surplus controls (spec/blocks.md 20) on the real
/// datapoints, the presence and the button of example.json, every output unbound (its mirror only).
std::string with_pv_surplus(std::string base, unsigned revision, size_t k) {
	set_member(base, "schemaVersion", "\"1.3\"");
	set_member(base, "revision", std::to_string(revision));
	std::string blocks;
	for (size_t i = 0; i < k; ++i) {
		blocks += (i > 0 ? ", " : "") + std::string(R"({"id": "pv)") + std::to_string(i + 1) +
			R"(", "type": "org.homecontrol.pv-surplus", "version": "1.0", "label": {"en": "PV surplus", "de": "PV-Überschuss"},
			"inputs": {"gridPower": "living.dimmer.state", "batterySoc": "hall.lux", "enable": "hall.button",
			"ready1": "hall.presence"},
			"parameters": {"power1": )" + std::to_string(1000 * (i + 1)) + R"(, "power2": 3680, "minPower2": 1380,
			"step2": 230, "mustRunFor1": 3600000}})";
	}
	while (!base.empty() && (base.back() == '\n' || base.back() == ' ' || base.back() == '\t')) {
		base.pop_back();
	}
	REQUIRE(base.back() == '}');
	base.pop_back();
	base += ", \"blocks\": [" + blocks + "]}";
	return base;
}

} // namespace


TEST_CASE("Swap memory: PV surplus controls keep the swap's budget and take no large block") {
	// The test of the house modes for the PV surplus control (homecontrol #65).
	check_budget("PV surplus control", with_pv_surplus, fixture_text("example.json"));
}

TEST_CASE("Swap memory: a large image swaps while no block above 3.5 KB can be had (6 KB on the 64-bit host)") {
	// The board refused a deploy for one block of 7 104 bytes (the rule set) with 9.7 KB free
	// (homecontrol #70). An image of 215 datapoints and 52 rules swaps over itself with no allocation
	// above 3.5 KB on the C6's sizes (6 KB with the 64-bit host's larger structures): the rules are
	// read in place from the slot, the rules' runtime takes its per-rule tables in chunks, the model
	// its datapoints in chunks of 32, the block runtime one block per instance.
	std::string base = fixture_text("example.json");
	std::string datapoints;
	std::string rules;
	for (int i = 0; i < 50; ++i) {
		std::string n = std::to_string(i);
		datapoints += R"({"id": "x)" + n + R"(", "type": "boolean", "knx": {"dpt": "1.001", "listen": ["31/7/)" + n +
			R"("]}}, {"id": "y)" + n + R"(", "type": "boolean", "writable": true, "transmit": true, )"
			R"("knx": {"dpt": "1.001", "send": "31/6/)" + n + R"("}}, )";
		rules += R"({"id": "r)" + n + R"(", "name": "Rule )" + n + R"(", "reactions": [{"triggers": [{"kind": )"
			R"("datapoint", "datapoint": "x)" + n + R"("}], "actions": [{"kind": "write", "datapoint": "y)" + n +
			R"(", "value": {"kind": "datapoint", "name": "x)" + n + R"("}}]}]}, )";
	}
	auto insert = [&base](const std::string& after, const std::string& text) {
		size_t at = base.find(after);
		REQUIRE(at != std::string::npos);
		base.insert(at + after.size(), text);
	};
	insert("\"datapoints\": [", datapoints);
	insert("\"rules\": [", rules);
	SwapRig rig;
	rig.load(example_blocks(base, 1, 4));
	std::vector<uint8_t> next = compile(example_blocks(base, 2, 4));
	constexpr size_t kLimit = sizeof(void*) == 4 ? 3584 : 6 * 1024;
	size_t refused = 0;
	SwapCost cost;
	{
		// No doctest call in here: a failed check allocates its message.
		hc_test::BlockLimit limit(kLimit);
		cost = rig.deploy(next);
		refused = limit.refused();
	}
	CHECK(cost.ok);
	CHECK(refused == 0);
	if (refused > 0 || rig.service.report().count > 0) {
		std::string memory;
		for (const std::string& line : rig.sink.sink.lines) {
			if (line.find("memory") != std::string::npos) {
				memory += "\n  " + line;
			}
		}
		MESSAGE("refused: " << hc_test::describe(rig.service.report()) << memory);
	}
	CHECK(rig.service.info().rules >= 50);
	CHECK(rig.model.size() > 200);
	MESSAGE("a swap of " << rig.model.size() << " datapoints and " << rig.service.info().rules
		<< " rules: largest block " << cost.largest << " B, peak " << cost.peak << " B above the old image");
}

namespace {

/// The heap of the C6 during a swap has no block above 3.5 KB to count on (homecontrol #70, #71).
constexpr size_t kFragmentedLimit = 3584;
/// No allocation of a load of the slot's image is larger (homecontrol #71).
constexpr long kSlotLoadLargest = 3 * 1024;

} // namespace

TEST_CASE("Swap memory: a failed apply on a fragmented heap leaves no image, keeps the values for the next deploy "
	"and takes no large block (#71, spec I9)") {
	// The new image fails after the model was replaced (the KNX adapter refuses its mapping in
	// `technologies`); every allocation above 3.5 KB fails meanwhile. The device is left without an
	// image (it was written into the slot), the swap table stays, and E-Home's next deployment of the
	// last valid image finds the values.
	std::string base = fixture_text("example.json");
	std::string old_text = example_blocks(base, 1, 2);
	std::vector<uint8_t> old_image = compile(old_text);
	std::vector<uint8_t> next = compile(example_blocks(base, 2, 2));
	SwapRig rig;
	rig.load(old_text);
	REQUIRE(rig.model.command(rig.model.find("away")->id(), hc::Value::of_bool(true)) == hc::DatapointStatus::ok);
	rig.settle();
	size_t datapoints = rig.model.size();
	size_t mappings = rig.adapter.size();

	rig.zigbee.fail_next(rig.model, "hall.light");
	SwapCost failed;
	size_t refused = 0;
	{
		// No doctest call in here: a failed check allocates its message.
		hc_test::BlockLimit limit(kFragmentedLimit);
		failed = rig.deploy(next);
		refused = limit.refused();
	}
	CHECK_FALSE(failed.ok);
	CHECK(rig.zigbee.failures_caused == 1);
	CHECK(rig.service.report().kind == hc::image::Report::Kind::apply);
	CHECK(refused == 0);
	if (refused > 0) {
		MESSAGE("refused: " << hc_test::describe(rig.service.report()));
	}
	CHECK(failed.largest <= kSlotLoadLargest);
	CHECK_FALSE(rig.service.loaded());
	CHECK(rig.service.swap_kept());
	CHECK(rig.model.find("away") == nullptr);

	// The last valid image again, on the same heap: the values follow their names.
	SwapCost again;
	{
		hc_test::BlockLimit limit(kFragmentedLimit);
		again = rig.deploy(old_image);
		refused = limit.refused();
	}
	CHECK(again.ok);
	CHECK(refused == 0);
	CHECK(again.largest <= kSlotLoadLargest);
	CHECK(rig.service.loaded());
	CHECK(rig.service.info().revision == 1);
	CHECK_FALSE(rig.service.swap_kept());
	CHECK(rig.service.engine().loaded());
	CHECK(rig.service.blocks().size() == 2);
	CHECK(rig.model.size() == datapoints);
	CHECK(rig.adapter.size() == mappings);
	REQUIRE(rig.model.find("away") != nullptr);
	CHECK(rig.model.find("away")->value() == hc::Value::of_bool(true));
	MESSAGE("a failed apply on a heap without blocks above " << kFragmentedLimit << " B: largest allocation "
		<< failed.largest << " B; the redeploy " << again.largest << " B");
	// The device goes on: the next deploy (not failing) runs.
	rig.settle();
	CHECK(rig.deploy(next).ok);
	CHECK(rig.service.info().revision == 2);
}

TEST_CASE("Swap memory: the boot load of the slot's image on a fragmented heap reads it in place (#71)") {
	std::string base = fixture_text("example.json");
	std::vector<uint8_t> image = compile(example_blocks(base, 5, 2));
	SwapRig rig(false);
	REQUIRE(image.size() <= rig.slot->size());
	std::memcpy(rig.slot->bytes(), image.data(), image.size());
	size_t refused = 0;
	{
		hc_test::BlockLimit limit(kFragmentedLimit);
		rig.probe.reset();
		hc_test::heap_mark();
		rig.service.set_load_probe(&rig.probe);
		rig.start_service();
		rig.service.set_load_probe(nullptr);
		refused = limit.refused();
	}
	long largest = static_cast<long>(hc_test::heap_largest());
	largest = rig.probe.largest() > largest ? rig.probe.largest() : largest;
	CHECK(refused == 0);
	CHECK(rig.service.loaded());
	CHECK(rig.service.source() == Source::boot);
	CHECK(rig.service.info().revision == 5);
	CHECK(rig.service.engine().loaded());
	CHECK(rig.service.blocks().size() == 2);
	CHECK(rig.service.image().bytes() == rig.slot->bytes()); // in place
	CHECK(largest <= kSlotLoadLargest);
	MESSAGE("the boot load of " << image.size() << " bytes: largest allocation " << largest << " B");
}


namespace {

class NoFetches : public hc::HttpFetcher {
public:
	bool fetch(const char*, hc::FetchReceiver&, const char** error) override {
		*error = "no network in the test";
		return false;
	}
	bool busy() const override { return false; }
};

} // namespace


TEST_CASE("Swap memory: an image whose external sources the device refuses leaves no image and takes no large "
	"block (how to provoke a failed apply on the board, #71)") {
	// ExternalValues checks its sources in the step `technologies`, after the model, the rules and the
	// blocks were replaced. A source with a URL other than http(s) therefore passes the image's checks
	// and fails the apply: the device has no image then (spec I9), the swap table stays for the next
	// deploy. On the board the same image (the running one with this source added, signed if the device
	// holds the installation key) provokes a failed apply.
	std::string base = fixture_text("example.json");
	std::string old_text = example_blocks(base, 1, 2);
	std::string next_text = example_blocks(base, 2, 2);
	REQUIRE(next_text.back() == '}');
	next_text.pop_back();
	next_text += R"(, "external": {"sources": [{"id": "restore-probe", "url": "ftp://192.0.2.1/x", "interval": 600, )"
		R"("bindings": [{"datapoint": "outside.temperature", "path": "a"}]}]}})";
	size_t at = next_text.find("\"datapoints\": [");
	REQUIRE(at != std::string::npos);
	next_text.insert(at + std::strlen("\"datapoints\": ["), R"({"id": "outside.temperature", "type": "real"}, )");
	std::vector<uint8_t> next = compile(next_text);
	SwapRig rig;
	NoFetches fetcher;
	hc::ExternalValues external(rig.controller.loop(), rig.controller.time(), rig.controller.settings(),
		rig.controller.connectivity(), rig.model, fetcher);
	rig.service.set_external(&external);
	rig.load(old_text);
	SwapCost cost;
	size_t refused = 0;
	{
		hc_test::BlockLimit limit(kFragmentedLimit);
		cost = rig.deploy(next);
		refused = limit.refused();
	}
	CHECK_FALSE(cost.ok);
	CHECK(refused == 0);
	CHECK_MESSAGE(rig.service.report().kind == hc::image::Report::Kind::apply, hc_test::describe(rig.service.report()));
	REQUIRE(rig.service.report().count > 0);
	CHECK(std::string(rig.service.report().refusals[0].path) == "external.sources");
	CHECK_FALSE(rig.service.loaded());
	CHECK(rig.service.swap_kept());
	// The last valid image again.
	rig.load(old_text);
	CHECK(rig.service.loaded());
	CHECK(rig.service.info().revision == 1);
	rig.service.set_external(nullptr);
}

TEST_CASE("Swap memory: a provider reload rebuilds the model from the plan of the slot's image, keeps nothing and "
	"takes no large block (#45 item 9)") {
	// A reload (a device joins) loads the model again from the plan of the image in the slot (which
	// points into the slot) and binds the components again; nothing of it stays.
	std::string base = fixture_text("example.json");
	SwapRig rig;
	rig.load(example_blocks(base, 1, 2));
	rig.clock.now_ms += ImageService::kReloadSpacingMs;
	rig.settle();
	long held = rig.held();
	size_t datapoints = rig.model.size();
	size_t mappings = rig.adapter.size();
	uint32_t reloads = rig.service.dynamic_reloads();

	size_t refused = 0;
	size_t peak = 0;
	size_t largest = 0;
	size_t start = 0;
	{
		// No doctest call in here: a failed check allocates its message.
		hc_test::BlockLimit limit(kFragmentedLimit);
		hc_test::heap_mark();
		start = hc_test::heap_live();
		rig.zigbee.changed();
		rig.settle();
		peak = hc_test::heap_peak();
		largest = hc_test::heap_largest();
		refused = limit.refused();
	}
	CHECK(rig.service.dynamic_reloads() == reloads + 1);
	CHECK(rig.service.dynamic_refusal() == nullptr);
	CHECK(refused == 0);
	CHECK(static_cast<long>(largest) <= kSlotLoadLargest);
	CHECK(rig.model.size() == datapoints);
	CHECK(rig.adapter.size() == mappings);
	CHECK(rig.service.engine().loaded());
	CHECK(rig.service.blocks().size() == 2);
	// Nothing of the reload stays.
	CHECK(rig.held() <= held);
	MESSAGE("a reload's peak " << static_cast<long>(peak) - static_cast<long>(start) << " B above the steady state ("
		<< held << " B), largest allocation " << largest << " B");
}
