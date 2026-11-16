/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_BINARY_H
#define HC_IMAGE_BINARY_H

#include <cstddef>
#include <cstdint>

namespace hc {
class JsonWriter;
}

/// The binary runtime image (spec/runtime-image.md sections 9 to 13, issue #87): the records as C++
/// structs of exactly the layout the specification gives, read in place from the mapped slot. Every
/// struct has natural alignment and no implicit padding (static_asserts below); the device reads a
/// record at `offset + i * entrySize` of its section, so a later minor version may lengthen records.
namespace hc::image::bin {

/// The format this firmware reads (section 11): major 1, every minor up to 0.
constexpr uint16_t kFormatMajor = 1;
constexpr uint16_t kFormatMinor = 0;
/// "1.0".
const char* format_text();

constexpr uint16_t kNone = 0xFFFF;
constexpr uint8_t kNone8 = 0xFF;

constexpr uint8_t kMagic[4] = {0x48, 0x43, 0x49, 0x4D}; ///< "HCIM"
constexpr size_t kHeaderSize = 32;
constexpr size_t kSignatureSize = 64;
constexpr size_t kMaxString = 511;

/// The header's flags.
constexpr uint32_t kFlagSigned = 0x01;
/// A section's flags.
constexpr uint16_t kSectionRequired = 0x01;

/// The header (9.2).
struct Header {
	uint8_t magic[4];
	uint16_t major;
	uint16_t minor;
	uint32_t length;
	uint32_t crc;
	uint32_t flags;
	uint16_t header_size;
	uint16_t section_count;
	uint32_t reserved[2];
};
static_assert(sizeof(Header) == 32, "header");

/// An entry of the section table (9.3).
struct SectionEntry {
	uint16_t id;
	uint16_t flags;
	uint32_t offset;
	uint32_t length;
	uint16_t entry_size;
	uint16_t reserved;
};
static_assert(sizeof(SectionEntry) == 16, "section entry");

/// The sections (9.4).
enum Section : uint16_t {
	kImage = 0x01,
	kStrings = 0x02,
	kLabels = 0x03,
	kSettings = 0x04,
	kDatapoints = 0x10,
	kLocations = 0x11,
	kFunctions = 0x12,
	kRoles = 0x13,
	kHistory = 0x14,
	kArchives = 0x15,
	kKnxAddresses = 0x20,
	kKnxBindings = 0x21,
	kKnxListen = 0x22,
	kKnxListeners = 0x23,
	kKnxTimeMaster = 0x24,
	kZigbeeBindings = 0x28,
	kExternalSources = 0x2C,
	kExternalBindings = 0x2D,
	kMqtt = 0x2E,
	kSchedules = 0x30,
	kScheduleDates = 0x31,
	kScheduleBounds = 0x32,
	kRules = 0x40,
	kRuleParameters = 0x41,
	kRuleChoices = 0x42,
	kRuleTimers = 0x43,
	kRuleStates = 0x44,
	kRuleReactions = 0x45,
	kRuleTriggers = 0x46,
	kRuleActions = 0x47,
	kRuleLogParts = 0x48,
	kRuleTimelines = 0x49,
	kRulePlans = 0x4A,
	kRulePoints = 0x4B,
	kRuleAssignments = 0x4C,
	kNodes = 0x4D,
	kConstants = 0x4E,
	kBlocks = 0x50,
	kBlockPorts = 0x51,
	kBlockBound = 0x52,
	kBlockParameters = 0x53,
	kUi = 0x60,
};

/// What this firmware knows of a section: its name (the paths of section 10), its record's size
/// (0: a byte section) and alignment, whether it holds exactly one record.
struct SectionInfo {
	uint16_t id;
	const char* name;
	uint16_t size;
	uint8_t align;
	bool single;
};
/// The known sections, by id; nullptr for an unknown one.
const SectionInfo* section_info(uint16_t id);
/// All known sections in ascending order of their ids.
const SectionInfo* sections(size_t& count);

// ---- 9.5 to 9.16: the records ----

struct LabelEntry {
	uint32_t tag;
	uint32_t text;
};
static_assert(sizeof(LabelEntry) == 8, "label");

/// A label reference: bits 0..23 the first entry, 24..31 the count.
inline uint32_t label_first(uint32_t ref) { return ref & 0xFFFFFFu; }
inline uint32_t label_count(uint32_t ref) { return ref >> 24; }

struct ImageRec {
	uint32_t project_id;
	uint32_t revision;
	uint32_t schema_version;
	uint32_t name;
	uint32_t generator;
	uint32_t generated_at;
	uint32_t history_budget;
	uint32_t archive_budget;
};
static_assert(sizeof(ImageRec) == 32, "image");

struct SettingsRec {
	uint32_t tz;
	uint32_t ntp;
	uint8_t flags; ///< 0x01 location, 0x02 elev.
	uint8_t reserved[3];
	int32_t elev;
	double lat;
	double lon;
};
static_assert(sizeof(SettingsRec) == 32, "settings");

/// Owners of a datapoint (9.7).
enum Owner : uint8_t {
	kOwnerNone = 0,
	kOwnerKnx = 1,
	kOwnerScheduler = 2,
	kOwnerTimeMaster = 3,
	kOwnerZigbee = 4,
	kOwnerRule = 5,
	kOwnerBlock = 6,
	kOwnerExternal = 7,
};
constexpr uint16_t kRuleStateMember = 0x8000;
/// The flag of a datapoint whose source has a `virtual` member (bit 7 of `flags`).
constexpr uint8_t kDatapointVirtual = 0x80;

struct DatapointRec {
	uint32_t name;
	uint32_t label;
	uint32_t unit;
	uint8_t type;
	uint8_t flags;
	uint8_t owner;
	uint8_t reserved;
	uint16_t owner_index;
	uint16_t owner_member;
	uint16_t history;
	uint16_t reserved2;
};
static_assert(sizeof(DatapointRec) == 24, "datapoint");

struct LocationRec {
	uint32_t id;
	uint32_t label;
	uint16_t parent;
	uint8_t kind;
	uint8_t reserved;
};
static_assert(sizeof(LocationRec) == 12, "location");

struct FunctionRec {
	uint32_t id;
	uint32_t label;
	uint32_t unit;
	uint16_t room;
	uint16_t first_role;
	uint8_t kind;
	uint8_t scene;
	uint8_t role_count;
	uint8_t reserved;
	uint8_t presses[6];
	uint16_t reserved2;
};
static_assert(sizeof(FunctionRec) == 28, "function");

struct RoleRec {
	uint32_t name;
	uint16_t datapoint;
	uint16_t reserved;
};
static_assert(sizeof(RoleRec) == 8, "role");

struct HistoryRec {
	uint16_t datapoint;
	uint8_t archive_count;
	uint8_t reserved;
	uint32_t retention;
	uint32_t spacing;
	uint16_t first_archive;
	uint16_t reserved2;
};
static_assert(sizeof(HistoryRec) == 16, "history");

struct ArchiveRec {
	uint32_t resolution;
	uint32_t retention;
};
static_assert(sizeof(ArchiveRec) == 8, "archive");

struct KnxAddressRec {
	uint16_t address;
	uint16_t first_listener;
	uint16_t listener_count;
	uint16_t reserved;
};
static_assert(sizeof(KnxAddressRec) == 8, "address");

struct KnxBindingRec {
	uint16_t datapoint;
	uint16_t dpt_main;
	uint16_t dpt_sub;
	uint16_t send;
	uint16_t first_listen;
	uint8_t listen_count;
	uint8_t reserved;
};
static_assert(sizeof(KnxBindingRec) == 12, "knx binding");

struct TimeMasterRec {
	uint16_t time;
	uint16_t date;
	uint16_t date_time;
	uint8_t flags; ///< 0x01 atMidnight, 0x02 onSynchronized.
	uint8_t reserved;
	uint32_t interval;
};
static_assert(sizeof(TimeMasterRec) == 12, "time master");

struct ZigbeeBindingRec {
	uint64_t ieee;
	uint16_t datapoint;
	uint16_t cluster;
	uint16_t attribute;
	uint16_t command;
	uint16_t report_min;
	uint16_t report_max;
	uint8_t endpoint;
	uint8_t flags; ///< 0x01 client, 0x02 min, 0x04 max, 0x08 change.
	uint16_t reserved;
	double report_change;
};
static_assert(sizeof(ZigbeeBindingRec) == 32, "zigbee binding");

struct ExternalSourceRec {
	uint32_t id;
	uint32_t url;
	uint32_t preset;
	uint32_t interval;
	uint16_t first_binding;
	uint8_t binding_count;
	uint8_t reserved;
};
static_assert(sizeof(ExternalSourceRec) == 20, "external source");

struct ExternalBindingRec {
	uint16_t datapoint;
	uint16_t reserved;
	uint32_t path;
	uint32_t variable;
	uint32_t unit;
	double scale;
	double offset;
};
static_assert(sizeof(ExternalBindingRec) == 32, "external binding");

struct MqttRec {
	uint32_t broker;
	uint32_t username;
	uint32_t base_topic;
	uint32_t discovery_prefix;
	uint16_t history_interval;
	uint8_t flags; ///< 0x01 enabled, 0x02 retain, 0x04 discovery, 0x08 history.
	uint8_t reserved;
};
static_assert(sizeof(MqttRec) == 20, "mqtt");

struct ScheduleRec {
	uint32_t id;
	uint32_t label;
	uint8_t kind;
	uint8_t flags; ///< 0x01 enabled, 0x02 enabledAdjustable, 0x04 catchUp, 0x08 notBefore, 0x10 notAfter.
	uint8_t days;
	uint8_t sun;
	uint16_t output;
	uint16_t time_datapoint;
	uint16_t adjustable;
	uint16_t every;
	uint16_t first_date;
	uint16_t date_count;
	uint16_t first_bound;
	uint8_t bound_count;
	uint8_t month;
	uint8_t day;
	uint8_t reserved[3];
	int32_t offset;
	uint32_t time;
	uint32_t not_before;
	uint32_t not_after;
	uint32_t from;
	uint32_t until;
	uint16_t at_year;
	uint8_t at_month;
	uint8_t at_day;
	uint32_t at_time;
};
static_assert(sizeof(ScheduleRec) == 64, "schedule");

struct ScheduleDateRec {
	uint16_t year;
	uint8_t month;
	uint8_t day;
};
static_assert(sizeof(ScheduleDateRec) == 4, "date");

struct ScheduleBoundRec {
	uint16_t member;
	uint8_t reserved[6];
	int64_t min;
	int64_t max;
	int64_t step;
};
static_assert(sizeof(ScheduleBoundRec) == 32, "bound");

// The rules' records (9.13) and the expressions (9.14) are the rule engine's own structs
// (hc/rules/rule_set.h), which have exactly these layouts.

struct BlockRec {
	uint32_t id;
	uint32_t type;
	uint32_t label;
	uint16_t major;
	uint16_t minor;
	uint16_t first_port;
	uint16_t first_parameter;
	uint8_t port_count;
	uint8_t parameter_count;
	uint8_t flags; ///< 0x01 enabled, 0x02 enabledAdjustable.
	uint8_t reserved;
	uint16_t enabled_datapoint;
	uint16_t reserved2;
};
static_assert(sizeof(BlockRec) == 28, "block");

struct BlockPortRec {
	uint32_t id;
	uint16_t first_bound;
	uint8_t bound_count;
	uint8_t aggregation;
	uint16_t mirror;
	uint16_t reserved;
};
static_assert(sizeof(BlockPortRec) == 12, "port");

struct BlockParameterRec {
	uint32_t id;
	uint16_t datapoint;
	uint8_t flags; ///< 0x01 the image gives the value.
	uint8_t reserved;
	double value;
};
static_assert(sizeof(BlockParameterRec) == 16, "block parameter");

/// CRC-32 (zlib's) of the bytes [0, signed_end) with the header's `crc` field taken as 0.
uint32_t image_crc(const uint8_t* bytes, size_t signed_end);

/// FNV-1a, 64 bits (section 12): the hash the values of a swap are kept under.
uint64_t fnv1a(const char* text);
uint64_t fnv1a(const void* data, size_t length, uint64_t basis = 0xcbf29ce484222325ULL);

/// A table section as the device reads it: its first byte, its record stride and count. An absent
/// section is empty.
struct Table {
	const uint8_t* base = nullptr;
	uint32_t stride = 0;
	uint32_t count = 0;
};

/// A validated image, read in place. Built by View::open() on bytes the validator accepted (it
/// trusts them: no access leaves its section, section 10). Copyable: it is pointers.
class View {
public:
	View() = default;

	/// Points the view at `bytes` (the image's `length` bytes). Assumes the header and the section
	/// table are valid (validator); unknown sections are skipped.
	void open(const uint8_t* bytes);
	void close() { *this = View(); }
	bool open() const { return bytes_ != nullptr; }

	const uint8_t* bytes() const { return bytes_; }
	const Header& header() const { return *reinterpret_cast<const Header*>(bytes_); }
	uint32_t length() const { return header().length; }
	bool is_signed() const { return (header().flags & kFlagSigned) != 0; }

	/// The section `id` (empty when absent or unknown).
	const Table& table(uint16_t id) const;
	size_t count(uint16_t id) const { return table(id).count; }
	/// Record `i` of section `id` as `T` (the caller knows `i` is below the count).
	template <class T>
	const T& at(uint16_t id, size_t i) const {
		const Table& t = table(id);
		return *reinterpret_cast<const T*>(t.base + static_cast<size_t>(t.stride) * i);
	}

	/// A string by its offset (0: the empty string).
	const char* str(uint32_t offset) const { return strings_ + offset; }
	/// A string or nullptr for the empty one (an optional member that is absent).
	const char* opt(uint32_t offset) const { return offset == 0 ? nullptr : strings_ + offset; }

	/// The label entries of a reference; count 0 for none.
	const LabelEntry* label(uint32_t ref, size_t& count) const;
	/// The label's text in `language` (spec/rules.md 2.2: the tag, its primary language, `en`, the
	/// first entry); the plain label's text; "" for none.
	const char* label_text(uint32_t ref, const char* language) const;
	/// Whether the label is a language map (written as an object by the API).
	bool label_is_map(uint32_t ref) const;

	const ImageRec& image() const { return at<ImageRec>(kImage, 0); }
	/// The `ui` section's bytes (nullptr, 0 without).
	const char* ui(size_t& length) const;

private:
	const uint8_t* bytes_ = nullptr;
	const char* strings_ = "";
	/// The known sections (indexed by their position in sections()).
	static constexpr size_t kKnown = 42;
	Table tables_[kKnown] = {};
};

/// Writes a label as the source gave it (spec/runtime-image.md 9.5): a string, or an object of its
/// language tags to texts; `null` for none.
void write_label(JsonWriter& json, const View& view, uint32_t ref);

} // namespace hc::image::bin

#endif // HC_IMAGE_BINARY_H
