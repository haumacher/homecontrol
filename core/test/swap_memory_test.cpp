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
#include "fake_image_staging.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/external_values.h"
#include "hc/image/image_body.h"
#include "hc/image/image_service.h"
#include "hc/image/image_store.h"
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
/// service keeps the applied plan for the providers' reloads. Armed (fail_next()), it makes the next
/// apply fail after the model was replaced (homecontrol #71): when the model is loaded it binds the
/// image's datapoint `victim` to a technology of its own, so the KNX adapter refuses its mapping in
/// the step `technologies` and the service returns to the stored image.
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
		if (dp != nullptr && model_->bind(dp->id(), hijack_) == hc::DatapointStatus::ok) {
			++failures_caused;
		}
		model_ = nullptr; // once
	}
	void model_cleared() override {}

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

/// The ImageStaging of the device is flash behind the updater: the fake's buffer is not heap.
class FlashStaging : public hc::ImageStaging {
public:
	size_t capacity() const override { return fake.capacity(); }
	bool begin(size_t size, const char** reason) override {
		hc_test::HeapExclusion flash;
		return fake.begin(size, reason);
	}
	bool append(const void* data, size_t length) override {
		hc_test::HeapExclusion flash;
		return fake.append(data, length);
	}
	const char* finish() override {
		hc_test::HeapExclusion flash;
		return fake.finish();
	}
	void end() override {
		hc_test::HeapExclusion flash;
		fake.end();
	}

	hc_test::FakeImageStaging fake;
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

	/// `start` false: the service is started by start_service() (after the test stored an image).
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
	// The test above for the sun shading (homecontrol #63): k = 0 .. 4 instances deployed over the
	// same image and over the image with one fewer.
	constexpr long kSwapBudget = 9 * 1024;
	constexpr long kLargestBlock = 3 * 1024;
	std::string base = fixture_text("example.json");
	long held[5] = {};
	long over_same[5] = {};
	for (size_t k = 0; k <= 4; ++k) {
		SwapRig rig;
		rig.load(with_shadings(base, 1, k), false);
		held[k] = rig.held();
		rig.load(with_shadings(base, 2, k), true);
		CHECK(rig.held() == held[k]);
		over_same[k] = rig.probe.peak();
		CHECK(rig.probe.largest() <= kLargestBlock);
		MESSAGE(k << " sun shading(s): the image holds " << held[k] << " B, the swap's peak is " << over_same[k]
			<< " B above it, its largest block " << rig.probe.largest() << " B");
	}
	CHECK(over_same[0] <= kSwapBudget);
	for (size_t k = 1; k <= 4; ++k) {
		long block = (held[k] - held[0]) / static_cast<long>(k);
		CHECK(block > 0);
		CHECK(over_same[k] <= kSwapBudget + static_cast<long>(k) * block);
		CHECK(over_same[k] - over_same[0] <= static_cast<long>(k) * block);
		SwapRig rig;
		rig.load(with_shadings(base, 1, k - 1), false);
		long old = rig.held();
		rig.load(with_shadings(base, 2, k), true);
		CHECK(rig.held() == held[k]);
		CHECK(rig.probe.peak() <= kSwapBudget + static_cast<long>(k) * block + (held[k] - old));
		CHECK(rig.probe.largest() <= kLargestBlock);
		MESSAGE(k << " sun shading(s) over " << k - 1 << ": " << rig.probe.peak() << " B above the old image, a block holds "
			<< block << " B");
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

namespace {

/// The largest single allocation of the probe's phases from the `nth` (0-based) phase named `name` on.
long largest_from(const PeakProbe& probe, const char* name, int nth, bool& found) {
	found = false;
	long top = 0;
	int seen = 0;
	for (size_t i = 0; i < probe.count; ++i) {
		if (!found && std::strcmp(probe.phases[i].name, name) == 0 && seen++ == nth) {
			found = true;
		}
		if (found) {
			top = probe.phases[i].largest > top ? probe.phases[i].largest : top;
		}
	}
	return top;
}

int phases_named(const PeakProbe& probe, const char* name) {
	int n = 0;
	for (size_t i = 0; i < probe.count; ++i) {
		n += std::strcmp(probe.phases[i].name, name) == 0 ? 1 : 0;
	}
	return n;
}

/// The heap of the C6 during a swap has no block above 3.5 KB to count on (homecontrol #70, #71).
constexpr size_t kFragmentedLimit = 3584;
/// No allocation of a load of the stored image is larger (homecontrol #71).
constexpr long kStoredLoadLargest = 3 * 1024;

} // namespace

TEST_CASE("Swap memory: a failed apply on a fragmented heap returns to the stored image, read in place (#71)") {
	// The new image arrives in the received staging (as PUT /api/image puts it) and fails after the
	// model was replaced (the KNX adapter refuses its mapping in `technologies`); the service tears it
	// down and loads the stored image again. Every allocation above 3.5 KB fails meanwhile: the stored
	// image (7 KB of text and more) is put together in the stored staging through a piece of
	// ImageStore::kReadPiece and parsed in place, not read into one heap block of its size.
	std::string base = fixture_text("example.json");
	std::vector<std::string> outputs = {"living.light", "hall.light", "hall.blind.move", "away"};
	std::string old_image = with_blocks(base, 1, 2, "living.temperature", outputs);
	std::string next = with_blocks(base, 2, 2, "living.temperature", outputs);
	REQUIRE(old_image.size() > kFragmentedLimit);
	SwapRig rig;
	FlashStaging received;
	FlashStaging stored;
	rig.service.set_staging(&received);
	rig.service.set_stored_staging(&stored);
	REQUIRE(rig.service.load(old_image.data(), old_image.size(), Source::api, true));
	REQUIRE(rig.service.stored_ok());
	rig.settle();
	size_t datapoints = rig.model.size();
	size_t mappings = rig.adapter.size();

	hc::image::ImageBody body;
	REQUIRE(body.begin(next.size(), &received));
	REQUIRE(body.staged());
	REQUIRE(body.append(next.data(), next.size()));
	const char* text = body.finish();
	REQUIRE(text != nullptr);
	rig.zigbee.fail_next(rig.model, "hall.light");
	bool ok = true;
	size_t refused = 0;
	{
		// No doctest call in here: a failed check allocates its message.
		hc_test::BlockLimit limit(kFragmentedLimit);
		rig.service.set_load_probe(&rig.probe);
		ok = rig.service.load(text, next.size(), Source::api, true, nullptr, &body);
		rig.service.set_load_probe(nullptr);
		refused = limit.refused();
	}
	CHECK(received.fake.busy); // the failed image stayed in the received staging during the restore
	body.release();
	CHECK_FALSE(ok);
	CHECK(rig.zigbee.failures_caused == 1);
	CHECK(rig.service.report().kind == hc::image::Report::Kind::apply);
	CHECK(refused == 0);
	if (refused > 0) {
		MESSAGE("refused: " << rig.service.report().refusals[0].reason);
	}
	// The old image is loaded and running.
	CHECK(rig.service.loaded());
	CHECK(rig.service.info().revision == 1);
	CHECK(rig.service.source() == Source::restore);
	CHECK(rig.service.engine().loaded());
	CHECK(rig.service.blocks().size() == 2);
	CHECK(rig.model.size() == datapoints);
	CHECK(rig.adapter.size() == mappings);
	CHECK(stored.fake.begins == 1);
	CHECK(stored.fake.ends == 1);
	CHECK_FALSE(stored.fake.busy);
	// The restore's allocations: from the tear-down of the failed image (its phase `signature`) to its
	// end, none above 3 KB.
	CHECK(phases_named(rig.probe, "signature") == 2);
	bool found = false;
	long largest = largest_from(rig.probe, "signature", 1, found);
	CHECK(found);
	CHECK(largest <= kStoredLoadLargest);
	MESSAGE("the restore of " << old_image.size() << " bytes on a heap without blocks above " << kFragmentedLimit
		<< " B: largest allocation " << largest << " B");
	// The device goes on: the next deploy (not failing) runs.
	rig.settle();
	REQUIRE(rig.service.load(next.data(), next.size(), Source::api, false));
	CHECK(rig.service.info().revision == 2);
}

TEST_CASE("Swap memory: without a staging area for the stored image a restore on a fragmented heap has no image "
	"(the mechanism #71 replaces)") {
	// The probe that the test above bites: the heap buffer of the stored image's size is refused, the
	// device is left without an image, and the refusal says why.
	std::string base = fixture_text("example.json");
	std::vector<std::string> outputs = {"living.light", "hall.light", "hall.blind.move", "away"};
	std::string old_image = with_blocks(base, 1, 2, "living.temperature", outputs);
	std::string next = with_blocks(base, 2, 2, "living.temperature", outputs);
	SwapRig rig;
	REQUIRE(rig.service.load(old_image.data(), old_image.size(), Source::api, true));
	rig.settle();
	rig.zigbee.fail_next(rig.model, "hall.light");
	bool ok = true;
	size_t refused = 0;
	{
		hc_test::BlockLimit limit(kFragmentedLimit);
		ok = rig.service.load(next.data(), next.size(), Source::api, false);
		refused = limit.refused();
	}
	CHECK_FALSE(ok);
	CHECK(refused >= 1);
	CHECK_FALSE(rig.service.loaded());
	bool logged = false;
	for (const std::string& line : rig.sink.sink.lines) {
		logged = logged || line.find("stored image not loaded: no memory") != std::string::npos;
	}
	CHECK(logged);
}

TEST_CASE("Swap memory: the boot load of the stored image on a fragmented heap reads it in place (#71)") {
	std::string base = fixture_text("example.json");
	std::vector<std::string> outputs = {"living.light", "hall.light", "hall.blind.move", "away"};
	std::string image = with_blocks(base, 5, 2, "living.temperature", outputs);
	SwapRig rig(false);
	FlashStaging stored;
	rig.service.set_stored_staging(&stored);
	{
		hc::image::ImageStore store(rig.blobs);
		const char* error = nullptr;
		REQUIRE(store.write(image.data(), image.size(), &error));
	}
	size_t refused = 0;
	{
		hc_test::BlockLimit limit(kFragmentedLimit);
		rig.service.set_load_probe(&rig.probe);
		rig.start_service();
		rig.service.set_load_probe(nullptr);
		refused = limit.refused();
	}
	CHECK(refused == 0);
	CHECK(rig.service.loaded());
	CHECK(rig.service.source() == Source::boot);
	CHECK(rig.service.info().revision == 5);
	CHECK(rig.service.engine().loaded());
	CHECK(rig.service.blocks().size() == 2);
	CHECK(rig.service.stored_ok());
	CHECK(stored.fake.begins == 1);
	CHECK(stored.fake.ends == 1);
	CHECK(rig.probe.largest() <= kStoredLoadLargest);
	MESSAGE("the boot load of " << image.size() << " bytes: largest allocation " << rig.probe.largest() << " B");
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

TEST_CASE("Swap memory: an image whose external member the device refuses returns to the stored image (how to "
	"provoke a restore on the board, #71)") {
	// The loader checks only that `external` is an object; ExternalValues checks its sources in the
	// step `technologies`, after the model, the rules and the blocks were replaced. A source with a URL
	// other than http(s) therefore passes the validation and fails the apply: the service returns to
	// the stored image. On the board the same image (the stored one with this member added, signed if
	// the device holds the installation key) provokes a restore.
	std::string base = fixture_text("example.json");
	std::vector<std::string> outputs = {"living.light", "hall.light", "hall.blind.move", "away"};
	std::string old_image = with_blocks(base, 1, 2, "living.temperature", outputs);
	std::string next = with_blocks(base, 2, 2, "living.temperature", outputs);
	REQUIRE(next.back() == '}');
	next.pop_back();
	next += R"(, "external": {"sources": [{"id": "restore-probe", "url": "ftp://192.0.2.1/x", "interval": 600, )"
		R"("bindings": [{"datapoint": "living.temperature", "path": "a"}]}]}})";
	SwapRig rig;
	NoFetches fetcher;
	hc::ExternalValues external(rig.controller.loop(), rig.controller.time(), rig.controller.settings(),
		rig.controller.connectivity(), rig.model, fetcher);
	rig.service.set_external(&external);
	FlashStaging stored;
	rig.service.set_stored_staging(&stored);
	REQUIRE(rig.service.load(old_image.data(), old_image.size(), Source::api, true));
	rig.settle();
	bool ok = true;
	size_t refused = 0;
	{
		hc_test::BlockLimit limit(kFragmentedLimit);
		ok = rig.service.load(next.data(), next.size(), Source::api, true);
		refused = limit.refused();
	}
	CHECK_FALSE(ok);
	CHECK(refused == 0);
	CHECK(rig.service.report().kind == hc::image::Report::Kind::apply);
	REQUIRE(rig.service.report().count > 0);
	CHECK(std::string(rig.service.report().refusals[0].path) == "external");
	CHECK(rig.service.loaded());
	CHECK(rig.service.info().revision == 1);
	CHECK(rig.service.source() == Source::restore);
	CHECK(stored.fake.begins == 1);
	CHECK(rig.sink.sink.count("loading the stored image again") == 1);
	rig.service.set_external(nullptr);
}
