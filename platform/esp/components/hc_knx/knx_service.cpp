/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "knx_service.h"

#include "hc/knx/group_value.h"
#include "hc/knx/knx_settings.h"
#include "hc/log.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.knx";

} // namespace

KnxService::KnxService(Controller& controller, LoopQueue& queue, DatapointModel& model)
	: controller_(controller),
	  model_(model),
	  socket_(queue),
	  telegrams_(controller.clock()),
	  tunnel_(controller.loop(), controller.clock(), controller.connectivity(), socket_),
	  adapter_(controller.loop(), controller.clock(), model, tunnel_, &telegrams_) {}

void KnxService::init() {
	socket_.init();
	adapter_.start();
	tunnel_.add_listener(this);
	knx::GatewayAddress gateway;
	bool nat = false;
	knx::read_gateway_settings(controller_.settings(), gateway, nat);
	if (gateway.empty()) {
		Log::info(kTag, "no KNX gateway set: knx gateway set <host[:port]>");
	}
	tunnel_.set_gateway(&gateway, nat);
	tunnel_.start();
}

bool KnxService::set_gateway(const char* text, const char** error) {
	if (!knx::store_gateway_setting(controller_.settings(), text, error)) {
		return false;
	}
	std::optional<knx::GatewayAddress> gateway = knx::GatewayAddress::parse(text, error);
	tunnel_.set_gateway(&*gateway, tunnel_.nat());
	return true;
}

bool KnxService::clear_gateway(const char** error) {
	SettingsStatus status = controller_.settings().erase(knx::setting::kGateway);
	if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
		*error = settings_status_name(status);
		return false;
	}
	tunnel_.set_gateway(nullptr, tunnel_.nat());
	return true;
}

bool KnxService::set_nat(bool nat, const char** error) {
	SettingsStatus status = controller_.settings().set_bool(knx::setting::kNat, nat);
	if (status != SettingsStatus::ok) {
		*error = settings_status_name(status);
		return false;
	}
	knx::GatewayAddress gateway = tunnel_.gateway();
	tunnel_.set_gateway(&gateway, nat);
	return true;
}

bool KnxService::write(const char* address_text, const char* dpt_text, const char* value_text, const char** error) {
	std::optional<knx::GroupAddress> address = knx::GroupAddress::parse(address_text, error);
	if (!address) {
		return false;
	}
	std::optional<Dpt> dpt = Dpt::parse(dpt_text);
	if (!dpt) {
		*error = "not a datapoint type (expected e.g. 1.001, 9.001)";
		return false;
	}
	Value value;
	if (!knx::parse_value_text(*dpt, value_text, value, error)) {
		return false;
	}
	knx::LData frame;
	if (!knx::make_group_value(frame, *address, knx::GroupService::write, *dpt, value, error)) {
		return false;
	}
	uint32_t token = tunnel_.send(frame);
	if (token == 0) {
		*error = tunnel_.is_up() ? "the tunnel's queue is full" : "the KNX tunnel is not up";
		return false;
	}
	write_token_ = token;
	return true;
}

bool KnxService::read(const char* address_text, const char* dpt_text, const char** error) {
	std::optional<knx::GroupAddress> address = knx::GroupAddress::parse(address_text, error);
	if (!address) {
		return false;
	}
	std::optional<Dpt> dpt = Dpt::parse(dpt_text);
	if (!dpt || !dpt->supported()) {
		*error = !dpt ? "not a datapoint type (expected e.g. 1.001, 9.001)" : dpt->unsupported_reason();
		return false;
	}
	knx::LData frame;
	knx::make_group_read(frame, *address);
	uint32_t token = tunnel_.send(frame);
	if (token == 0) {
		*error = tunnel_.is_up() ? "the tunnel's queue is full" : "the KNX tunnel is not up";
		return false;
	}
	read_token_ = token;
	reading_ = true;
	read_address_ = *address;
	read_dpt_ = *dpt;
	controller_.loop().cancel(read_timer_);
	read_timer_ = controller_.loop().schedule_at(controller_.clock().monotonic_ms() + kReadTimeoutMs,
		Callback::member<KnxService, &KnxService::read_timeout>(this));
	return true;
}

void KnxService::read_timeout() {
	read_timer_ = TimerHandle();
	if (!reading_) {
		return;
	}
	reading_ = false;
	char text[knx::GroupAddress::kMaxText];
	Log::warn(kTag, "knx read %s: no response within 5 s", read_address_.format(text, sizeof(text)));
}

void KnxService::tunnel_frame(const knx::KnxTunnel&, const knx::LData& frame) {
	if (!reading_ || frame.group_address() != read_address_) {
		return;
	}
	knx::GroupData data;
	if (!knx::group_data(frame, data) || data.service != knx::GroupService::response) {
		return;
	}
	reading_ = false;
	controller_.loop().cancel(read_timer_);
	read_timer_ = TimerHandle();
	char address[knx::GroupAddress::kMaxText];
	char source[knx::IndividualAddress::kMaxText];
	char dpt[Dpt::kMaxText];
	char hex[knx::GroupData::kMaxData * 3 + 1];
	read_address_.format(address, sizeof(address));
	frame.source.format(source, sizeof(source));
	read_dpt_.format(dpt, sizeof(dpt));
	knx::format_hex(data.data, data.length, hex, sizeof(hex));
	Value value;
	DptResult result = knx::decode_group_value(read_dpt_, data, value);
	if (!result) {
		Log::warn(kTag, "knx read %s: response %s from %s does not decode as %s (%s: %s)", address, hex, source, dpt,
			dpt_status_name(result.status), result.reason);
		return;
	}
	char text[64];
	Log::info(kTag, "knx read %s: %s (%s, %s from %s)", address, value.format(text, sizeof(text)), dpt, hex, source);
}

void KnxService::tunnel_sent(const knx::KnxTunnel&, uint32_t token, const knx::LData& frame, knx::SendOutcome outcome) {
	if (token == 0 || (token != write_token_ && token != read_token_)) {
		return;
	}
	char address[knx::GroupAddress::kMaxText];
	frame.group_address().format(address, sizeof(address));
	bool is_read = token == read_token_;
	if (outcome == knx::SendOutcome::confirmed) {
		Log::info(kTag, "knx %s %s: confirmed by the gateway", is_read ? "read" : "write", address);
	} else {
		Log::warn(kTag, "knx %s %s: %s", is_read ? "read" : "write", address, knx::send_outcome_name(outcome));
		if (is_read) {
			reading_ = false;
			controller_.loop().cancel(read_timer_);
			read_timer_ = TimerHandle();
		}
	}
	if (is_read) {
		read_token_ = 0;
	} else {
		write_token_ = 0;
	}
}

} // namespace esp
} // namespace hc
