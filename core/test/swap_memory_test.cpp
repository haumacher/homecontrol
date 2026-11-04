/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The heap of an image swap per phase (homecontrol #70, doc/image.md "Memory of a swap"): an image
// with k blocks deployed over the same image with k blocks (and over k - 1), the whole heap of the
// test binary counted at every phase the image service reports (LoadProbe).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "allocation_counter.h"
#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/image/image_service.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"

using hc::image::ImageService;
using hc::image::Source;

namespace {

std::string fixture_text(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

/// A provider of dynamic datapoints, as the Zigbee adapter is on the C6: with one registered the
/// service keeps the applied plan for the providers' reloads.
class NoDevices : public hc::image::DynamicSpecProvider {
public:
	const char* provider_name() const override { return "zigbee"; }
	bool dynamic_specs(const hc::image::ImageBindings&, const hc::image::DynamicSpec*& specs, size_t& count,
		const char**) override {
		specs = nullptr;
		count = 0;
		return true;
	}
	void model_loaded(const hc::image::ImageBindings&) override {}
	void model_cleared() override {}
};

/// Records the peak of the whole heap in each phase, relative to what was held when the load began
/// (in fixed storage: the probe allocates nothing).
struct PeakProbe : hc::image::LoadProbe {
	struct Phase {
		char name[24];
		long peak;    ///< The most held at once during the phase, above the start of the load.
		long after;   ///< Held at its end, above the start of the load.
		long charged; ///< The peak as the C6's heap charges it (allocation_counter.h).
		long largest; ///< The largest single block allocated in the phase.
	};
	static constexpr size_t kMaxPhases = 32;
	size_t base = 0;
	size_t charged_base = 0;
	Phase phases[kMaxPhases] = {};
	size_t count = 0;

	void load_begun() override {
		count = 0;
		hc_test::heap_mark();
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
	}
	long peak() const {
		long top = 0;
		for (size_t i = 0; i < count; ++i) {
			top = phases[i].peak > top ? phases[i].peak : top;
		}
		return top;
	}
	long largest() const {
		long top = 0;
		for (size_t i = 0; i < count; ++i) {
			top = phases[i].largest > top ? phases[i].largest : top;
		}
		return top;
	}
	long charged_peak() const {
		long top = 0;
		for (size_t i = 0; i < count; ++i) {
			top = phases[i].charged > top ? phases[i].charged : top;
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
	NoDevices zigbee;
	ImageService service{controller, model, blobs, adapter, &tunnel};
	PeakProbe probe;
	size_t empty = 0;         ///< The heap held before the first image.
	size_t empty_charged = 0; ///< The same as the C6 charges it.

	SwapRig() {
		controller.start();
		controller.connectivity().link_up("192.0.2.10");
		adapter.start();
		REQUIRE(service.add_provider(zigbee));
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

	/// Loads without storing (the store runs off the loop on the device, after the swap) and lets the
	/// loop settle (the rules' and blocks' start cascades, the first writes).
	void load(const std::string& text, bool measure) {
		service.set_load_probe(measure ? &probe : nullptr);
		bool ok = service.load(text.data(), text.size(), Source::api, false);
		service.set_load_probe(nullptr);
		REQUIRE_MESSAGE(ok, (service.report().count > 0 ? service.report().refusals[0].reason : "?"));
		settle();
	}

	/// The heap the loaded image holds in the steady state.
	long held() const { return static_cast<long>(hc_test::heap_live()) - static_cast<long>(empty); }
	long held_charged() const {
		return static_cast<long>(hc_test::heap_charged()) - static_cast<long>(empty_charged);
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

} // namespace

TEST_CASE("Swap memory: the peak of each phase (measurement, HC_SWAP_IMAGE)") {
	const char* path = std::getenv("HC_SWAP_IMAGE");
#ifdef HC_SWAP_IMAGE_DEFAULT
	path = path != nullptr ? path : HC_SWAP_IMAGE_DEFAULT; // the 32-bit model build (doc/image.md)
#endif
	if (path == nullptr) {
		return; // a measurement on a real image (homecontrol #70); the budget test below runs always
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
			rig.load(with_blocks(base, 12, before, "0/0/28", outputs), false);
			long held = rig.held();
			long held_charged = rig.held_charged();
			rig.load(with_blocks(base, 13, k, "0/0/28", outputs), true);
			long now = rig.held();
			long now_charged = rig.held_charged();
			std::string line;
			for (size_t i = 0; i < rig.probe.count; ++i) {
				const PeakProbe::Phase& ph = rig.probe.phases[i];
				char cell[112];
				std::snprintf(cell, sizeof(cell), "\n  %-14s peak %6ld (charged %6ld), held after %6ld, largest block %5ld",
					ph.name, ph.peak, ph.charged, ph.after, ph.largest);
				line += cell;
			}
			MESSAGE(k << " block(s) over " << before << ": the old image held " << held << " B (charged " << held_charged
				<< "), the new " << now << " B (" << now_charged << "); peak above the old " << rig.probe.peak()
				<< " B (" << rig.probe.charged_peak() << "), above the new " << held + rig.probe.peak() - now
				<< " B (" << held_charged + rig.probe.charged_peak() - now_charged << ")" << line);
		}
	}
}

TEST_CASE("Swap memory: a block costs at the peak at most its steady size; the swap stays within its budget and "
	"takes no large block") {
	// The peak of a deploy above the old image's steady state, for an image with k = 0 .. 4 blocks
	// deployed over the same image (a new revision) and over the image with k - 1 blocks: at most the
	// swap's budget for the image without blocks plus k times a block's steady cost, and a block adds
	// to the peak at most its steady cost (doc/image.md "Memory of a swap", doc/blocks.md "Memory").
	// No single allocation of the swap is larger than kLargestBlock: a fragmented heap refuses one
	// block with enough free bytes beside it (homecontrol #70, the rule set's 7 KB on the board).
	// The budgets are of example.json on the 64-bit host (the 32-bit model build of doc/image.md gives
	// the C6's figures, about 10 % less). They cover the core's allocations only: on the board the
	// tasks the load holds back add their own (doc/image.md "Memory of a swap", "Model and board").
	constexpr long kSwapBudget = 9 * 1024;
	constexpr long kLargestBlock = 3 * 1024;
	std::string base = fixture_text("example.json");
	std::vector<std::string> outputs = {"living.light", "hall.light", "hall.blind.move", "away"};
	const size_t ks[] = {0, 1, 2, 3, 4};
	long held[5] = {};
	long over_same[5] = {};
	for (size_t k : ks) {
		SwapRig rig;
		rig.load(with_blocks(base, 1, k, "living.temperature", outputs), false);
		held[k] = rig.held();
		rig.load(with_blocks(base, 2, k, "living.temperature", outputs), true);
		CHECK(rig.probe.count < PeakProbe::kMaxPhases);
		CHECK(rig.held() == held[k]); // a redeploy of the same image holds what the first deploy held
		over_same[k] = rig.probe.peak();
		const PeakProbe::Phase* release = rig.probe.find("release");
		REQUIRE(release != nullptr);
		CHECK(release->after <= 0); // the old image is gone at the release
		CHECK(rig.probe.largest() <= kLargestBlock);
		MESSAGE(k << " block(s): the image holds " << held[k] << " B, the swap's peak is " << over_same[k]
			<< " B above it, its largest block " << rig.probe.largest() << " B");
	}
	CHECK(over_same[0] <= kSwapBudget);
	for (size_t k : ks) {
		if (k == 0) {
			continue;
		}
		long block = (held[k] - held[0]) / static_cast<long>(k); // a block's steady cost in this image
		CHECK(block > 0);
		CHECK(over_same[k] <= kSwapBudget + static_cast<long>(k) * block);
		CHECK(over_same[k] - over_same[0] <= static_cast<long>(k) * block);
		// Deployed over the image with one block fewer: the old image holds less, the peak above it
		// is at most what it is above the same image plus the added block's steady cost.
		SwapRig rig;
		size_t before = k - 1;
		rig.load(with_blocks(base, 1, before, "living.temperature", outputs), false);
		long old = rig.held();
		CHECK(old == held[before]);
		rig.load(with_blocks(base, 2, k, "living.temperature", outputs), true);
		CHECK(rig.held() == held[k]);
		CHECK(rig.probe.peak() <= kSwapBudget + static_cast<long>(k) * block + (held[k] - old));
		MESSAGE(k << " block(s) over " << before << ": " << rig.probe.peak() << " B above the old image ("
			<< old << " B), a block holds " << block << " B");
	}
}

TEST_CASE("Swap memory: a large image swaps while no block above 3.5 KB can be had (6 KB on the 64-bit host)") {
	// The board refused a deploy for one block of 7 104 bytes (the rule set) with 9.7 KB free
	// (homecontrol #70). An image of 215 datapoints and 52 rules swaps over itself with no allocation
	// above 3.5 KB on the C6's sizes (6 KB with the 64-bit host's larger structures; before #70 it
	// failed at the loader's first table of all datapoints' drafts): the rule set takes one block per
	// table, the rules' runtime its per-rule tables in chunks, the model its datapoints in
	// chunks of 32, the loader its per-datapoint scratch in chunks, the block runtime one block per
	// instance.
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
	std::vector<std::string> outputs = {"living.light", "hall.light", "hall.blind.move", "away"};
	SwapRig rig;
	rig.load(with_blocks(base, 1, 4, "living.temperature", outputs), false);
	std::string next = with_blocks(base, 2, 4, "living.temperature", outputs);
	constexpr size_t kLimit = sizeof(void*) == 4 ? 3584 : 6 * 1024;
	size_t refused = 0;
	bool ok = false;
	{
		// No doctest call in here: a failed check allocates its message.
		hc_test::BlockLimit limit(kLimit);
		rig.service.set_load_probe(&rig.probe);
		ok = rig.service.load(next.data(), next.size(), Source::api, false);
		rig.service.set_load_probe(nullptr);
		refused = limit.refused();
	}
	CHECK(ok);
	CHECK(refused == 0);
	if (refused > 0 || rig.service.report().count > 0) {
		MESSAGE("refused: " << rig.service.report().refusals[0].reason);
	}
	CHECK(rig.service.info().rules >= 50);
	CHECK(rig.model.size() > 200);
	MESSAGE("a swap of " << rig.model.size() << " datapoints and " << rig.service.info().rules
		<< " rules: largest block " << rig.probe.largest() << " B, peak " << rig.probe.peak()
		<< " B above the old image");
}
