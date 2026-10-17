/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "zigbee_service.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "esp_coexist.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hc/log.h"
#include "nvs_blob_store.h"
#include "sdkconfig.h"

namespace hc {
namespace esp {

namespace {

using zigbee::AttributeRecord;
using zigbee::Ieee;
using zigbee::NodeAddress;
using zigbee::SimpleDescriptor;
using zigbee::ZigbeeAdapter;

constexpr const char* kTag = "hc.zigbee";

/// The coordinator's own endpoint: the source of every request, the destination of the bindings.
constexpr uint8_t kEndpoint = 1;
constexpr uint32_t kTaskStack = 6144;
constexpr UBaseType_t kTaskPriority = 5;
constexpr uint32_t kRetryMs = 1000;
/// How long a request from the event loop waits for the stack's lock.
constexpr TickType_t kLockTicks = pdMS_TO_TICKS(500);

constexpr const char* kStoragePartition = "zb_storage";
constexpr const char* kFactoryPartition = "zb_fct";
constexpr const char* kDatabasePartition = "zb_db";
constexpr const char* kDatabaseNamespace = "zigbee";

// ---- The ring between the stack's task and the event loop ----

enum class Kind : uint8_t {
	network,
	permit,
	announce,
	leave,
	active_endpoints,
	simple_descriptor,
	attributes,
	command,
	reporting,
	bind,
};

/// One frame or state change of the stack, copied out of the stack's buffers.
struct Event {
	static constexpr size_t kMaxRecords = 4;
	/// Room for the Basic cluster's two names (length octet and 32 characters each) and more.
	static constexpr size_t kPayload = 136;

	struct Record {
		uint16_t attribute;
		uint8_t status;
		uint8_t type;
		uint8_t offset;
		uint8_t length;
	};

	Kind kind;
	uint8_t status;
	uint8_t endpoint;
	uint8_t command;
	uint8_t count; ///< endpoints, records, input clusters, payload bytes
	uint8_t out_count;
	bool up;
	uint16_t short_addr;
	uint16_t cluster;
	uint16_t profile; ///< also the PAN id
	uint16_t device; ///< also the channel
	Ieee ieee;
	Ieee extended_pan;
	Record records[kMaxRecords];
	union {
		uint8_t bytes[kPayload];
		uint16_t clusters[kPayload / 2];
	} payload;
};

constexpr size_t kRingSize = 16;

class EspZigbeeDriver;

/// Everything of the running coordinator; one instance, static.
struct State {
	ZigbeeService* service = nullptr;
	ZigbeeAdapter* adapter = nullptr;
	EspZigbeeDriver* driver = nullptr;
	NvsBlobStore store;
	const char* reason = "not initialized (ZigbeeService::init)";
	bool available = false;

	SemaphoreHandle_t mutex = nullptr;
	StaticSemaphore_t mutex_buffer;
	Event ring[kRingSize];
	size_t head = 0;
	size_t count = 0;
	bool drain_posted = false;
	std::atomic<uint32_t> dropped{0};
	Event current; ///< The event being delivered, on the loop task.

	/// Set once the network is formed or restored: requests before are refused.
	std::atomic<bool> ready{false};
	uint8_t zone_id = 0;
};

State g_state;

Ieee ieee_of(const uint8_t* bytes) {
	Ieee value = 0;
	for (int i = 7; i >= 0; --i) {
		value = (value << 8) | bytes[i];
	}
	return value;
}

void bytes_of(Ieee ieee, uint8_t* bytes) {
	for (int i = 0; i < 8; ++i) {
		bytes[i] = static_cast<uint8_t>(ieee >> (8 * i));
	}
}

/// The IEEE address the stack knows for `short_addr` (0 if none). Stack task only.
Ieee ieee_by_short(uint16_t short_addr) {
	uint8_t bytes[8] = {};
	if (esp_zb_ieee_address_by_short(short_addr, bytes) != ESP_OK) {
		return 0;
	}
	return ieee_of(bytes);
}

void drain(void* context, const LoopMessage& message);

/// Claims a free slot (stack task); nullptr when the ring is full (counted as dropped). Finish
/// with commit().
Event* claim(Kind kind) {
	xSemaphoreTake(g_state.mutex, portMAX_DELAY);
	if (g_state.count == kRingSize) {
		xSemaphoreGive(g_state.mutex);
		g_state.dropped.fetch_add(1);
		return nullptr;
	}
	Event* event = &g_state.ring[(g_state.head + g_state.count) % kRingSize];
	xSemaphoreGive(g_state.mutex);
	std::memset(event, 0, sizeof(Event));
	event->kind = kind;
	return event;
}

void commit() {
	xSemaphoreTake(g_state.mutex, portMAX_DELAY);
	++g_state.count;
	bool post = !g_state.drain_posted;
	g_state.drain_posted = true;
	xSemaphoreGive(g_state.mutex);
	if (post && !g_state.service->queue().post(&drain, nullptr, 0, nullptr, "zigbee")) {
		xSemaphoreTake(g_state.mutex, portMAX_DELAY);
		g_state.drain_posted = false; // the next event posts again
		xSemaphoreGive(g_state.mutex);
	}
}

/// Copies one ZCL attribute value into the event. The stack hands strings with their length
/// octet first; their length is taken from that octet.
void add_record(Event& event, uint16_t attribute, uint8_t status, const esp_zb_zcl_attribute_data_t& data) {
	if (event.count >= Event::kMaxRecords) {
		Log::debug(kTag, "attribute 0x%04x beyond %u records dropped", static_cast<unsigned>(attribute),
			static_cast<unsigned>(Event::kMaxRecords));
		return;
	}
	size_t used = 0;
	for (size_t i = 0; i < event.count; ++i) {
		used += event.records[i].length;
	}
	const uint8_t* value = static_cast<const uint8_t*>(data.value);
	uint8_t type = static_cast<uint8_t>(data.type);
	size_t length = 0;
	if (status == zigbee::zcl_status::kSuccess && value != nullptr) {
		if (type == zigbee::zcl_type::kCharString || type == 0x41) { // character or octet string
			length = value[0] == 0xFF ? 1 : static_cast<size_t>(value[0]) + 1;
		} else {
			length = zigbee::zcl_type_size(type);
			if (length == 0) {
				length = data.size;
			}
		}
	}
	if (used + length > Event::kPayload) {
		Log::debug(kTag, "attribute 0x%04x (%u bytes) does not fit the event", static_cast<unsigned>(attribute),
			static_cast<unsigned>(length));
		return;
	}
	Event::Record& record = event.records[event.count++];
	record.attribute = attribute;
	record.status = status;
	record.type = type;
	record.offset = static_cast<uint8_t>(used);
	record.length = static_cast<uint8_t>(length);
	if (length > 0) {
		std::memcpy(event.payload.bytes + used, value, length);
	}
}

// ---- Delivery on the event loop ----

void deliver(ZigbeeAdapter& adapter, const Event& event) {
	NodeAddress address{event.ieee, event.short_addr};
	switch (event.kind) {
		case Kind::network:
			adapter.network_state(event.up, event.profile, static_cast<uint8_t>(event.device), event.extended_pan);
			break;
		case Kind::permit:
			adapter.permit_join_status(event.count);
			break;
		case Kind::announce:
			adapter.device_announced(address);
			break;
		case Kind::leave:
			adapter.device_left(address);
			break;
		case Kind::active_endpoints:
			adapter.active_endpoints(address, event.status, event.payload.bytes, event.count);
			break;
		case Kind::simple_descriptor: {
			SimpleDescriptor descriptor;
			descriptor.endpoint = event.endpoint;
			descriptor.profile = event.profile;
			descriptor.device = event.device;
			descriptor.in = event.payload.clusters;
			descriptor.in_count = event.count;
			descriptor.out = event.payload.clusters + event.count;
			descriptor.out_count = event.out_count;
			adapter.simple_descriptor(address, event.status, descriptor);
			break;
		}
		case Kind::attributes: {
			AttributeRecord records[Event::kMaxRecords];
			for (size_t i = 0; i < event.count; ++i) {
				records[i].attribute = event.records[i].attribute;
				records[i].status = event.records[i].status;
				records[i].type = event.records[i].type;
				records[i].data = event.payload.bytes + event.records[i].offset;
				records[i].length = event.records[i].length;
			}
			adapter.attributes(address, event.endpoint, event.cluster, records, event.count);
			break;
		}
		case Kind::command:
			adapter.command_received(address, event.endpoint, event.cluster, event.command, event.payload.bytes,
				event.count);
			break;
		case Kind::reporting:
			adapter.reporting_configured(address, event.endpoint, event.cluster, event.status);
			break;
		case Kind::bind:
			adapter.bind_result(address, event.status);
			break;
	}
}

void drain(void*, const LoopMessage&) {
	for (;;) {
		xSemaphoreTake(g_state.mutex, portMAX_DELAY);
		if (g_state.count == 0) {
			g_state.drain_posted = false;
			xSemaphoreGive(g_state.mutex);
			return;
		}
		std::memcpy(&g_state.current, &g_state.ring[g_state.head], sizeof(Event));
		g_state.head = (g_state.head + 1) % kRingSize;
		--g_state.count;
		xSemaphoreGive(g_state.mutex);
		if (g_state.adapter != nullptr) {
			deliver(*g_state.adapter, g_state.current);
		}
	}
}

// ---- Callbacks of the stack (its task) ----

void push_network(bool up) {
	Event* event = claim(Kind::network);
	if (event == nullptr) {
		return;
	}
	event->up = up;
	if (up) {
		uint8_t extended[8] = {};
		esp_zb_get_extended_pan_id(extended);
		event->extended_pan = ieee_of(extended);
		event->profile = esp_zb_get_pan_id();
		event->device = esp_zb_get_current_channel();
	}
	commit();
}

uintptr_t context_of(uint16_t short_addr, uint8_t endpoint = 0) {
	return (static_cast<uintptr_t>(short_addr) << 8) | endpoint;
}

void active_endpoints_answer(esp_zb_zdp_status_t status, uint8_t count, uint8_t* list, void* context) {
	Event* event = claim(Kind::active_endpoints);
	if (event == nullptr) {
		return;
	}
	event->short_addr = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(context) >> 8);
	event->ieee = ieee_by_short(event->short_addr);
	event->status = static_cast<uint8_t>(status);
	if (status == ESP_ZB_ZDP_STATUS_SUCCESS && list != nullptr) {
		size_t n = count < Event::kPayload ? count : Event::kPayload;
		std::memcpy(event->payload.bytes, list, n);
		event->count = static_cast<uint8_t>(n);
	}
	commit();
}

void simple_descriptor_answer(esp_zb_zdp_status_t status, esp_zb_af_simple_desc_1_1_t* descriptor, void* context) {
	Event* event = claim(Kind::simple_descriptor);
	if (event == nullptr) {
		return;
	}
	uintptr_t packed = reinterpret_cast<uintptr_t>(context);
	event->short_addr = static_cast<uint16_t>(packed >> 8);
	event->endpoint = static_cast<uint8_t>(packed & 0xFF);
	event->ieee = ieee_by_short(event->short_addr);
	event->status = static_cast<uint8_t>(status);
	if (status == ESP_ZB_ZDP_STATUS_SUCCESS && descriptor != nullptr) {
		constexpr size_t kMax = Event::kPayload / 2;
		event->endpoint = descriptor->endpoint;
		event->profile = descriptor->app_profile_id;
		event->device = descriptor->app_device_id;
		size_t in = descriptor->app_input_cluster_count;
		size_t out = descriptor->app_output_cluster_count;
		const uint16_t* list = descriptor->app_cluster_list;
		size_t keep_in = in < kMax ? in : kMax;
		size_t keep_out = out < kMax - keep_in ? out : kMax - keep_in;
		if (list != nullptr) {
			for (size_t i = 0; i < keep_in; ++i) {
				event->payload.clusters[i] = list[i];
			}
			for (size_t i = 0; i < keep_out; ++i) {
				event->payload.clusters[keep_in + i] = list[in + i];
			}
			event->count = static_cast<uint8_t>(keep_in);
			event->out_count = static_cast<uint8_t>(keep_out);
		}
		if (keep_in < in || keep_out < out) {
			Log::warn(kTag, "endpoint %u of 0x%04x lists %u clusters, %u kept",
				static_cast<unsigned>(descriptor->endpoint), static_cast<unsigned>(event->short_addr),
				static_cast<unsigned>(in + out), static_cast<unsigned>(keep_in + keep_out));
		}
	}
	commit();
}

void bind_answer(esp_zb_zdp_status_t status, void* context) {
	Event* event = claim(Kind::bind);
	if (event == nullptr) {
		return;
	}
	event->short_addr = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(context) >> 8);
	event->ieee = ieee_by_short(event->short_addr);
	event->status = static_cast<uint8_t>(status);
	commit();
}

void leave_answer(esp_zb_zdp_status_t status, void* context) {
	Log::info(kTag, "leave request to 0x%04x: status 0x%02x",
		static_cast<unsigned>(reinterpret_cast<uintptr_t>(context) >> 8), static_cast<unsigned>(status));
}

void fill_source(Event& event, const esp_zb_zcl_cmd_info_t& info) {
	event.short_addr = info.src_address.u.short_addr;
	event.ieee = ieee_by_short(event.short_addr);
	event.endpoint = info.src_endpoint;
	event.cluster = info.cluster;
}

void answer_enroll(const esp_zb_zcl_ias_zone_enroll_request_message_t& request) {
	esp_zb_zcl_ias_zone_enroll_response_cmd_t response = {};
	response.zcl_basic_cmd.dst_addr_u.addr_short = request.info.src_address.u.short_addr;
	response.zcl_basic_cmd.dst_endpoint = request.info.src_endpoint;
	response.zcl_basic_cmd.src_endpoint = kEndpoint;
	response.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
	response.enroll_rsp_code = 0x00; // success
	response.zone_id = g_state.zone_id;
	g_state.zone_id = static_cast<uint8_t>((g_state.zone_id + 1) % 0xFF);
	esp_zb_zcl_ias_zone_enroll_cmd_resp(&response);
	Log::info(kTag, "IAS zone 0x%04x endpoint %u (type 0x%04x) enrolled as zone %u",
		static_cast<unsigned>(request.info.src_address.u.short_addr), static_cast<unsigned>(request.info.src_endpoint),
		static_cast<unsigned>(request.zone_type), static_cast<unsigned>(response.zone_id));
}

esp_err_t action_handler(esp_zb_core_action_callback_id_t id, const void* message) {
	if (message == nullptr) {
		return ESP_OK;
	}
	switch (id) {
		case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
			auto* report = static_cast<const esp_zb_zcl_report_attr_message_t*>(message);
			Event* event = claim(Kind::attributes);
			if (event == nullptr) {
				break;
			}
			event->short_addr = report->src_address.u.short_addr;
			event->ieee = ieee_by_short(event->short_addr);
			event->endpoint = report->src_endpoint;
			event->cluster = report->cluster;
			add_record(*event, report->attribute.id, zigbee::zcl_status::kSuccess, report->attribute.data);
			commit();
			break;
		}
		case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID: {
			auto* response = static_cast<const esp_zb_zcl_cmd_read_attr_resp_message_t*>(message);
			Event* event = claim(Kind::attributes);
			if (event == nullptr) {
				break;
			}
			fill_source(*event, response->info);
			for (const esp_zb_zcl_read_attr_resp_variable_t* v = response->variables; v != nullptr; v = v->next) {
				add_record(*event, v->attribute.id, static_cast<uint8_t>(v->status), v->attribute.data);
			}
			commit();
			break;
		}
		case ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID: {
			auto* response = static_cast<const esp_zb_zcl_cmd_config_report_resp_message_t*>(message);
			Event* event = claim(Kind::reporting);
			if (event == nullptr) {
				break;
			}
			fill_source(*event, response->info);
			event->status = static_cast<uint8_t>(response->info.status);
			for (const esp_zb_zcl_config_report_resp_variable_t* v = response->variables; v != nullptr; v = v->next) {
				if (v->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
					event->status = static_cast<uint8_t>(v->status);
				}
			}
			commit();
			break;
		}
		case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_STATUS_CHANGE_NOT_ID: {
			auto* change = static_cast<const esp_zb_zcl_ias_zone_status_change_notification_message_t*>(message);
			Event* event = claim(Kind::command);
			if (event == nullptr) {
				break;
			}
			fill_source(*event, change->info);
			event->command = 0x00;
			event->payload.bytes[0] = static_cast<uint8_t>(change->zone_status & 0xFF);
			event->payload.bytes[1] = static_cast<uint8_t>(change->zone_status >> 8);
			event->payload.bytes[2] = change->extended_status;
			event->payload.bytes[3] = change->zone_id;
			event->count = 4;
			commit();
			break;
		}
		case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_ENROLL_REQUEST_ID:
			answer_enroll(*static_cast<const esp_zb_zcl_ias_zone_enroll_request_message_t*>(message));
			break;
		case ESP_ZB_CORE_CMD_PRIVILEGE_COMMAND_REQ_CB_ID: {
			auto* command = static_cast<const esp_zb_zcl_privilege_command_message_t*>(message);
			Event* event = claim(Kind::command);
			if (event == nullptr) {
				break;
			}
			fill_source(*event, command->info);
			event->command = command->info.command.id;
			size_t n = command->size < Event::kPayload ? command->size : Event::kPayload;
			if (command->data != nullptr && n > 0) {
				std::memcpy(event->payload.bytes, command->data, n);
				event->count = static_cast<uint8_t>(n);
			}
			commit();
			break;
		}
		default:
			Log::debug(kTag, "stack callback 0x%04x", static_cast<unsigned>(id));
			break;
	}
	return ESP_OK;
}

void start_commissioning(uint8_t mode) {
	if (esp_zb_bdb_start_top_level_commissioning(mode) != ESP_OK) {
		Log::error(kTag, "commissioning (mode 0x%02x) not started, retrying", static_cast<unsigned>(mode));
		esp_zb_scheduler_alarm(&start_commissioning, mode, kRetryMs);
	}
}

void network_up(const char* how) {
	uint8_t extended[8] = {};
	esp_zb_get_extended_pan_id(extended);
	char text[zigbee::kIeeeText];
	zigbee::format_ieee(ieee_of(extended), text);
	Log::info(kTag, "network %s: PAN 0x%04x, channel %u, extended PAN %s", how, static_cast<unsigned>(esp_zb_get_pan_id()),
		static_cast<unsigned>(esp_zb_get_current_channel()), text);
	wifi_ps_type_t ps = WIFI_PS_NONE;
	if (esp_wifi_get_ps(&ps) == ESP_OK && ps == WIFI_PS_NONE) {
		Log::warn(kTag, "WiFi power save is off: the 802.15.4 radio only gets the air between WiFi frames "
						"(doc/zigbee.md, coexistence)");
	}
	g_state.ready.store(true);
	push_network(true);
}

// ---- The stack's task ----

void zigbee_task(void*) {
	esp_zb_platform_config_t platform = {};
	platform.radio_config.radio_mode = ZB_RADIO_MODE_NATIVE;
	platform.host_config.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE;
	if (esp_zb_platform_config(&platform) != ESP_OK) {
		Log::error(kTag, "the Zigbee platform configuration was refused; the coordinator stays off");
		vTaskDelete(nullptr);
		return;
	}

	esp_zb_cfg_t config = {};
	config.esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR;
	config.install_code_policy = false;
	config.nwk_cfg.zczr_cfg.max_children = CONFIG_HC_ZIGBEE_MAX_CHILDREN;
	esp_zb_init(&config);
	esp_zb_set_primary_network_channel_set(CONFIG_HC_ZIGBEE_CHANNEL_MASK);
	if (CONFIG_HC_ZIGBEE_PAN_ID != 0) {
		esp_zb_set_pan_id(static_cast<uint16_t>(CONFIG_HC_ZIGBEE_PAN_ID));
	}

	// Endpoint 1: a home gateway with the Basic and Identify servers, On/Off and Level Control
	// servers (they receive the buttons' commands, privileged so that the application sees them),
	// and the clients of everything the coordinator commands, reads or receives reports of.
	esp_zb_cluster_list_t* clusters = esp_zb_zcl_cluster_list_create();
	esp_zb_basic_cluster_cfg_t basic = {};
	basic.zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE;
	basic.power_source = 0x01; // mains, single phase
	esp_zb_cluster_list_add_basic_cluster(clusters, esp_zb_basic_cluster_create(&basic), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
	esp_zb_identify_cluster_cfg_t identify = {};
	identify.identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;
	esp_zb_cluster_list_add_identify_cluster(clusters, esp_zb_identify_cluster_create(&identify),
		ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
	esp_zb_cluster_list_add_identify_cluster(clusters, esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY),
		ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
	esp_zb_on_off_cluster_cfg_t on_off = {};
	esp_zb_cluster_list_add_on_off_cluster(clusters, esp_zb_on_off_cluster_create(&on_off), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
	esp_zb_cluster_list_add_on_off_cluster(clusters, esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF),
		ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
	esp_zb_level_cluster_cfg_t level = {};
	esp_zb_cluster_list_add_level_cluster(clusters, esp_zb_level_cluster_create(&level), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
	esp_zb_cluster_list_add_level_cluster(clusters, esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL),
		ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
	esp_zb_cluster_list_add_color_control_cluster(clusters,
		esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
	esp_zb_cluster_list_add_power_config_cluster(clusters,
		esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
	esp_zb_cluster_list_add_ias_zone_cluster(clusters, esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE),
		ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
	esp_zb_cluster_list_add_temperature_meas_cluster(clusters,
		esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
	esp_zb_cluster_list_add_humidity_meas_cluster(clusters,
		esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
	esp_zb_cluster_list_add_illuminance_meas_cluster(clusters,
		esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
	esp_zb_cluster_list_add_occupancy_sensing_cluster(clusters,
		esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
	esp_zb_cluster_list_add_multistate_input_cluster(clusters,
		esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_MULTI_INPUT), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

	esp_zb_ep_list_t* endpoints = esp_zb_ep_list_create();
	esp_zb_endpoint_config_t endpoint = {};
	endpoint.endpoint = kEndpoint;
	endpoint.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID;
	endpoint.app_device_id = ESP_ZB_HA_HOME_GATEWAY_DEVICE_ID;
	endpoint.app_device_version = 0;
	esp_zb_ep_list_add_gateway_ep(endpoints, clusters, endpoint);
	esp_zb_device_register(endpoints);

	// The buttons' commands: On/Off (off, on, toggle, off with effect, on with recall, on with
	// timed off) and Level Control (move, step, stop and their with-on/off variants).
	static const uint8_t kOnOffCommands[] = {0x00, 0x01, 0x02, 0x40, 0x41, 0x42};
	for (uint8_t command : kOnOffCommands) {
		esp_zb_zcl_add_privilege_command(kEndpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, command);
	}
	for (uint8_t command = 0x01; command <= 0x07; ++command) {
		esp_zb_zcl_add_privilege_command(kEndpoint, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, command);
	}
	esp_zb_core_action_handler_register(&action_handler);

	if (esp_zb_start(false) != ESP_OK) {
		Log::error(kTag, "the Zigbee stack did not start; the coordinator stays off");
		vTaskDelete(nullptr);
		return;
	}
	esp_zb_stack_main_loop();
	vTaskDelete(nullptr);
}

// ---- The driver: the adapter's requests, on the event loop's task ----

class Lock {
public:
	Lock() : held_(g_state.ready.load() && esp_zb_lock_acquire(kLockTicks)) {}
	~Lock() {
		if (held_) {
			esp_zb_lock_release();
		}
	}
	explicit operator bool() const { return held_; }

private:
	bool held_;
};

void basic_command(esp_zb_zcl_basic_cmd_t& basic, uint16_t short_addr, uint8_t endpoint) {
	basic.dst_addr_u.addr_short = short_addr;
	basic.dst_endpoint = endpoint;
	basic.src_endpoint = kEndpoint;
}

class EspZigbeeDriver : public ZigbeeDriver {
public:
	bool permit_join(uint8_t seconds) override {
		Lock lock;
		if (!lock) {
			return false;
		}
		esp_err_t result = seconds == 0 ? esp_zb_bdb_close_network() : esp_zb_bdb_open_network(seconds);
		if (result != ESP_OK) {
			Log::warn(kTag, "permit join %u: %s", static_cast<unsigned>(seconds), esp_err_to_name(result));
		}
		return result == ESP_OK;
	}

	bool request_active_endpoints(Ieee, uint16_t short_addr) override {
		Lock lock;
		if (!lock) {
			return false;
		}
		esp_zb_zdo_active_ep_req_param_t request = {};
		request.addr_of_interest = short_addr;
		esp_zb_zdo_active_ep_req(&request, &active_endpoints_answer, reinterpret_cast<void*>(context_of(short_addr)));
		return true;
	}

	bool request_simple_descriptor(Ieee, uint16_t short_addr, uint8_t endpoint) override {
		Lock lock;
		if (!lock) {
			return false;
		}
		esp_zb_zdo_simple_desc_req_param_t request = {};
		request.addr_of_interest = short_addr;
		request.endpoint = endpoint;
		esp_zb_zdo_simple_desc_req(&request, &simple_descriptor_answer,
			reinterpret_cast<void*>(context_of(short_addr, endpoint)));
		return true;
	}

	bool read_attributes(Ieee, uint16_t short_addr, uint8_t endpoint, uint16_t cluster, const uint16_t* attributes,
		size_t count) override {
		constexpr size_t kMax = Event::kMaxRecords;
		if (count == 0 || count > kMax) {
			Log::warn(kTag, "read of %u attributes refused (1..%u per request)", static_cast<unsigned>(count),
				static_cast<unsigned>(kMax));
			return false;
		}
		Lock lock;
		if (!lock) {
			return false;
		}
		uint16_t list[kMax];
		std::memcpy(list, attributes, count * sizeof(uint16_t));
		esp_zb_zcl_read_attr_cmd_t request = {};
		basic_command(request.zcl_basic_cmd, short_addr, endpoint);
		request.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
		request.clusterID = cluster;
		request.attr_number = static_cast<uint8_t>(count);
		request.attr_field = list;
		esp_zb_zcl_read_attr_cmd_req(&request);
		return true;
	}

	bool configure_reporting(Ieee, uint16_t short_addr, uint8_t endpoint, uint16_t cluster,
		const ReportingConfig& config) override {
		Lock lock;
		if (!lock) {
			return false;
		}
		uint32_t change = config.change; // little endian: the first bytes hold a narrower type's value
		esp_zb_zcl_config_report_record_t record = {};
		record.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
		record.attributeID = config.attribute;
		record.attrType = config.type;
		record.min_interval = config.min_s;
		record.max_interval = config.max_s;
		record.reportable_change = &change;
		esp_zb_zcl_config_report_cmd_t request = {};
		basic_command(request.zcl_basic_cmd, short_addr, endpoint);
		request.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
		request.clusterID = cluster;
		request.record_number = 1;
		request.record_field = &record;
		esp_zb_zcl_config_report_cmd_req(&request);
		return true;
	}

	bool bind(Ieee ieee, uint16_t short_addr, uint8_t endpoint, uint16_t cluster) override {
		Lock lock;
		if (!lock) {
			return false;
		}
		esp_zb_zdo_bind_req_param_t request = {};
		bytes_of(ieee, request.src_address);
		request.src_endp = endpoint;
		request.cluster_id = cluster;
		request.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
		esp_zb_get_long_address(request.dst_address_u.addr_long);
		request.dst_endp = kEndpoint;
		request.req_dst_addr = short_addr;
		esp_zb_zdo_device_bind_req(&request, &bind_answer, reinterpret_cast<void*>(context_of(short_addr)));
		return true;
	}

	bool ias_enroll(Ieee, uint16_t short_addr, uint8_t endpoint) override {
		Lock lock;
		if (!lock) {
			return false;
		}
		// Writing the CIE address makes the zone send its Zone Enroll Request (answered by
		// answer_enroll()) and its status change notifications to the coordinator.
		esp_zb_ieee_addr_t own = {};
		esp_zb_get_long_address(own);
		esp_zb_zcl_attribute_t attribute = {};
		attribute.id = ESP_ZB_ZCL_ATTR_IAS_ZONE_IAS_CIE_ADDRESS_ID;
		attribute.data.type = ESP_ZB_ZCL_ATTR_TYPE_IEEE_ADDR;
		attribute.data.size = sizeof(own);
		attribute.data.value = own;
		esp_zb_zcl_write_attr_cmd_t request = {};
		basic_command(request.zcl_basic_cmd, short_addr, endpoint);
		request.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
		request.clusterID = ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE;
		request.attr_number = 1;
		request.attr_field = &attribute;
		esp_zb_zcl_write_attr_cmd_req(&request);
		return true;
	}

	bool on_off(Ieee, uint16_t short_addr, uint8_t endpoint, OnOffCommand command) override {
		Lock lock;
		if (!lock) {
			return false;
		}
		esp_zb_zcl_on_off_cmd_t request = {};
		basic_command(request.zcl_basic_cmd, short_addr, endpoint);
		request.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
		request.on_off_cmd_id = command == OnOffCommand::off ? ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID
			: command == OnOffCommand::on                   ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID
															: ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID;
		esp_zb_zcl_on_off_cmd_req(&request);
		return true;
	}

	bool move_to_level(Ieee, uint16_t short_addr, uint8_t endpoint, uint8_t level, uint16_t transition_ds) override {
		Lock lock;
		if (!lock) {
			return false;
		}
		esp_zb_zcl_move_to_level_cmd_t request = {};
		basic_command(request.zcl_basic_cmd, short_addr, endpoint);
		request.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
		request.level = level;
		request.transition_time = transition_ds;
		esp_zb_zcl_level_move_to_level_with_onoff_cmd_req(&request);
		return true;
	}

	bool move_to_color_temperature(Ieee, uint16_t short_addr, uint8_t endpoint, uint16_t mireds,
		uint16_t transition_ds) override {
		Lock lock;
		if (!lock) {
			return false;
		}
		esp_zb_zcl_color_move_to_color_temperature_cmd_t request = {};
		basic_command(request.zcl_basic_cmd, short_addr, endpoint);
		request.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
		request.color_temperature = mireds;
		request.transition_time = transition_ds;
		esp_zb_zcl_color_move_to_color_temperature_cmd_req(&request);
		return true;
	}

	bool identify(Ieee, uint16_t short_addr, uint8_t endpoint, uint16_t seconds) override {
		Lock lock;
		if (!lock) {
			return false;
		}
		esp_zb_zcl_identify_cmd_t request = {};
		basic_command(request.zcl_basic_cmd, short_addr, endpoint);
		request.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
		request.identify_time = seconds;
		esp_zb_zcl_identify_cmd_req(&request);
		return true;
	}

	bool leave(Ieee ieee, uint16_t short_addr) override {
		Lock lock;
		if (!lock) {
			return false;
		}
		esp_zb_zdo_mgmt_leave_req_param_t request = {};
		bytes_of(ieee, request.device_address);
		request.dst_nwk_addr = short_addr;
		request.remove_children = 0;
		request.rejoin = 0;
		esp_zb_zdo_device_leave_req(&request, &leave_answer, reinterpret_cast<void*>(context_of(short_addr)));
		return true;
	}
};

bool partition_present(const char* label) {
	return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label) != nullptr;
}

} // namespace

// ---- The stack's signal handler (C linkage, called by the stack in its task) ----

extern "C" void esp_zb_app_signal_handler(esp_zb_app_signal_t* signal) {
	uint32_t* pointer = signal->p_app_signal;
	esp_err_t status = signal->esp_err_status;
	auto type = static_cast<esp_zb_app_signal_type_t>(*pointer);
	switch (type) {
		case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
			if (esp_coex_wifi_i154_enable() != ESP_OK) {
				Log::warn(kTag, "WiFi/802.15.4 coexistence not enabled");
			}
#endif
			esp_zb_set_tx_power(static_cast<int8_t>(CONFIG_HC_ZIGBEE_TX_POWER));
			Log::info(kTag, "stack initialized, starting commissioning");
			start_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
			break;
		case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
		case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
			if (status != ESP_OK) {
				Log::warn(kTag, "%s failed (%s), retrying", esp_zb_zdo_signal_to_string(type), esp_err_to_name(status));
				esp_zb_scheduler_alarm(&start_commissioning, ESP_ZB_BDB_MODE_INITIALIZATION, kRetryMs);
			} else if (esp_zb_bdb_is_factory_new()) {
				Log::info(kTag, "factory new: forming the network");
				start_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
			} else {
				network_up("restored");
			}
			break;
		case ESP_ZB_BDB_SIGNAL_FORMATION:
			if (status != ESP_OK) {
				Log::warn(kTag, "network formation failed (%s), retrying", esp_err_to_name(status));
				esp_zb_scheduler_alarm(&start_commissioning, ESP_ZB_BDB_MODE_NETWORK_FORMATION, kRetryMs);
			} else {
				network_up("formed");
			}
			break;
		case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
			auto* params = static_cast<esp_zb_zdo_signal_device_annce_params_t*>(esp_zb_app_signal_get_params(pointer));
			Event* event = claim(Kind::announce);
			if (event != nullptr) {
				event->short_addr = params->device_short_addr;
				event->ieee = ieee_of(params->ieee_addr);
				commit();
			}
			break;
		}
		case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION: {
			auto* params =
				static_cast<esp_zb_zdo_signal_leave_indication_params_t*>(esp_zb_app_signal_get_params(pointer));
			if (params->rejoin == 0) {
				Event* event = claim(Kind::leave);
				if (event != nullptr) {
					event->short_addr = params->short_addr;
					event->ieee = ieee_of(params->device_addr);
					commit();
				}
			}
			break;
		}
		case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
			if (status == ESP_OK) {
				Event* event = claim(Kind::permit);
				if (event != nullptr) {
					event->count = *static_cast<uint8_t*>(esp_zb_app_signal_get_params(pointer));
					commit();
				}
			}
			break;
		default:
			Log::debug(kTag, "signal %s (0x%x): %s", esp_zb_zdo_signal_to_string(type), static_cast<unsigned>(type),
				esp_err_to_name(status));
			break;
	}
}

// ---- ZigbeeService ----

ZigbeeService::ZigbeeService(Controller& controller, LoopQueue& queue, DatapointModel& model)
	: controller_(controller), queue_(queue), model_(model) {}

void ZigbeeService::init(BlobStore* store) {
	start(nullptr, store);
}

void ZigbeeService::init(image::ImageService& images, BlobStore* store) {
	start(&images, store);
}

void ZigbeeService::start(image::ImageService* images, BlobStore* store) {
	if (g_state.service != nullptr) {
		Log::warn(kTag, "init called twice, ignored");
		return;
	}
	g_state.service = this;
	if (!partition_present(kStoragePartition) || !partition_present(kFactoryPartition)) {
		g_state.reason = "the partitions zb_storage/zb_fct are missing (flash the new partition table, doc/zigbee.md)";
		Log::error(kTag, "%s", g_state.reason);
		return;
	}
	if (store == nullptr) {
		if (g_state.store.open(kDatabasePartition, kDatabaseNamespace)) {
			store = &g_state.store;
		} else {
			Log::error(kTag, "device database partition %s unusable: devices are kept until the next restart only",
				kDatabasePartition);
		}
	}
	g_state.mutex = xSemaphoreCreateMutexStatic(&g_state.mutex_buffer);
	g_state.driver = new EspZigbeeDriver();
	g_state.adapter = new ZigbeeAdapter(controller_.loop(), controller_.clock(), model_, *g_state.driver, store);
	if (images != nullptr && !images->add_provider(*g_state.adapter)) {
		Log::error(kTag, "not registered with the image service: the adapter loads the model alone");
	}
	const char* error = nullptr;
	if (!g_state.adapter->start(&error)) {
		Log::warn(kTag, "adapter: %s", error);
	}
	if (xTaskCreate(&zigbee_task, "hc_zigbee", kTaskStack, nullptr, kTaskPriority, nullptr) != pdPASS) {
		g_state.reason = "the Zigbee task could not be created";
		Log::error(kTag, "%s", g_state.reason);
		return;
	}
	g_state.available = true;
	g_state.reason = "";
	Log::info(kTag, "coordinator starting (channels 0x%08x)", static_cast<unsigned>(CONFIG_HC_ZIGBEE_CHANNEL_MASK));
}

bool ZigbeeService::available() const {
	return g_state.available;
}

const char* ZigbeeService::unavailable_reason() const {
	return g_state.reason;
}

bool ZigbeeService::permit_join(uint32_t seconds, const char** error) {
	if (!g_state.available) {
		if (error != nullptr) {
			*error = g_state.reason;
		}
		return false;
	}
	return g_state.adapter->permit_join(seconds, error);
}

zigbee::ZigbeeAdapter* ZigbeeService::adapter() {
	return g_state.available ? g_state.adapter : nullptr;
}

void ZigbeeService::write_json(JsonWriter& json) {
	if (!g_state.available) {
		json.begin_object().member("available", false).member("reason", g_state.reason).end_object();
		return;
	}
	g_state.adapter->write_json(json);
}

uint32_t ZigbeeService::dropped() const {
	return g_state.dropped.load();
}

} // namespace esp
} // namespace hc
