/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Addresses, cEMI and KNXnet/IP frames byte for byte (layouts of KNX 03.06.03 cEMI, 03.08.02 core
// and 03.08.04 tunnelling), the gateway setting, the group value helpers and the telegram log.

#include <cstring>
#include <string>

#include "doctest.h"
#include "hc/knx/address.h"
#include "hc/knx/cemi.h"
#include "hc/knx/group_value.h"
#include "hc/knx/knx_settings.h"
#include "hc/knx/knxnetip.h"
#include "hc/knx/telegram_log.h"
#include "knx_test_support.h"

using hc::Dpt;
using hc::Value;
using hc::knx::CemiStatus;
using hc::knx::GatewayAddress;
using hc::knx::GroupAddress;
using hc::knx::GroupData;
using hc::knx::GroupService;
using hc::knx::IndividualAddress;
using hc::knx::LData;
using hc::knx::MessageCode;
using hc::knx::Priority;
using hc::knx::TelegramLog;
using hc::knx::TelegramRecord;
using hc_test::Bytes;
using hc_test::bytes;
using hc_test::hex;
namespace ip = hc::knx::ip;

namespace {

std::string format(GroupAddress address) {
	char text[GroupAddress::kMaxText];
	return address.format(text, sizeof(text));
}

std::string format(IndividualAddress address) {
	char text[IndividualAddress::kMaxText];
	return address.format(text, sizeof(text));
}

std::string encode(const LData& frame) {
	uint8_t out[LData::kMaxFrame];
	size_t length = 0;
	hc::knx::CemiResult result = hc::knx::encode(frame, out, sizeof(out), length);
	REQUIRE(result);
	return hex(Bytes(out, out + length));
}

hc::knx::CemiResult decode(const Bytes& data, LData& frame) {
	return hc::knx::decode(data.data(), data.size(), frame);
}

const hc::Ipv4Endpoint kLocal{hc_test::kLocalIp, hc_test::kLocalPort};

} // namespace

// ---- Addresses ----------------------------------------------------------------------------------

TEST_CASE("KnxAddress.GroupThreeAndTwoLevels") {
	CHECK(GroupAddress::parse("1/2/3")->raw == 0x0A03);
	CHECK(GroupAddress::parse("31/7/255")->raw == 0xFFFF);
	CHECK(GroupAddress::parse("0/0/0")->raw == 0x0000);
	CHECK(GroupAddress::parse("1/515")->raw == 0x0A03); // 515 = 2 * 256 + 3
	CHECK(GroupAddress::parse("31/2047")->raw == 0xFFFF);
	CHECK(format(GroupAddress{0x0A03}) == "1/2/3");
	CHECK(format(GroupAddress{0xFFFF}) == "31/7/255");
	CHECK(GroupAddress::of(1, 0, 1).raw == 0x0801);
}

TEST_CASE("KnxAddress.GroupRefusals") {
	const char* error = nullptr;
	CHECK_FALSE(GroupAddress::parse("", &error));
	CHECK(std::string(error) == "address is empty");
	CHECK_FALSE(GroupAddress::parse("1", &error));
	CHECK(std::string(error).find("two or three levels") != std::string::npos);
	CHECK_FALSE(GroupAddress::parse("32/0/0", &error));
	CHECK(std::string(error) == "main group must be 0..31");
	CHECK_FALSE(GroupAddress::parse("1/8/0", &error));
	CHECK(std::string(error) == "middle group must be 0..7");
	CHECK_FALSE(GroupAddress::parse("1/2/256", &error));
	CHECK(std::string(error) == "sub group must be 0..255");
	CHECK_FALSE(GroupAddress::parse("1/2048", &error));
	CHECK_FALSE(GroupAddress::parse("1/2/3/4", &error));
	CHECK(std::string(error) == "address has too many parts");
	CHECK_FALSE(GroupAddress::parse("1//3", &error));
	CHECK_FALSE(GroupAddress::parse("1/2/3 ", &error));
	CHECK_FALSE(GroupAddress::parse("-1/2/3", &error));
	CHECK_FALSE(GroupAddress::parse("1.2.3", &error));
	CHECK_FALSE(GroupAddress::parse(nullptr, &error));
}

TEST_CASE("KnxAddress.Individual") {
	CHECK(IndividualAddress::parse("1.1.250")->raw == 0x11FA);
	CHECK(IndividualAddress::parse("15.15.255")->raw == 0xFFFF);
	CHECK(format(IndividualAddress{0x11FA}) == "1.1.250");
	const char* error = nullptr;
	CHECK_FALSE(IndividualAddress::parse("16.1.1", &error));
	CHECK(std::string(error) == "area must be 0..15");
	CHECK_FALSE(IndividualAddress::parse("1.16.1", &error));
	CHECK(std::string(error) == "line must be 0..15");
	CHECK_FALSE(IndividualAddress::parse("1.1.256", &error));
	CHECK(std::string(error) == "device must be 0..255");
	CHECK_FALSE(IndividualAddress::parse("1.1", &error));
	CHECK_FALSE(IndividualAddress::parse("1/1/1", &error));
}

TEST_CASE("KnxAddress.Ipv4") {
	uint32_t address = 0;
	CHECK(hc::parse_ipv4("192.0.2.1", address));
	CHECK(address == 0xC0000201);
	CHECK_FALSE(hc::parse_ipv4("192.0.2", address));
	CHECK_FALSE(hc::parse_ipv4("192.0.2.256", address));
	CHECK_FALSE(hc::parse_ipv4("192.0.2.1.", address));
	CHECK_FALSE(hc::parse_ipv4("192.0.2.0001", address));
	CHECK_FALSE(hc::parse_ipv4("knx.example", address));
	char text[hc::Ipv4Endpoint::kMaxText];
	CHECK(std::string(hc::Ipv4Endpoint{0xC0000201, 3671}.format(text, sizeof(text))) == "192.0.2.1:3671");
}

// ---- cEMI ---------------------------------------------------------------------------------------

TEST_CASE("KnxCemi.GroupWriteShortForm") {
	// The textbook frame: L_Data.req, standard frame, repeat allowed, broadcast, priority low, group
	// address 1/0/1, hop count 6, length 1, T_Data_Group, A_GroupValue_Write with the value 1.
	LData frame;
	uint8_t on = 1;
	REQUIRE(hc::knx::make_group_request(frame, GroupAddress::of(1, 0, 1), GroupService::write, &on, 1, true));
	CHECK(encode(frame) == "11 00 BC E0 00 00 08 01 01 00 81");
}

TEST_CASE("KnxCemi.GroupWriteLongFormAndRead") {
	LData frame;
	const uint8_t temperature[] = {0x0C, 0x1A};
	REQUIRE(hc::knx::make_group_request(frame, GroupAddress::of(1, 2, 3), GroupService::write, temperature, 2, false));
	CHECK(encode(frame) == "11 00 BC E0 00 00 0A 03 03 00 80 0C 1A");
	REQUIRE(hc::knx::make_group_request(frame, GroupAddress::of(1, 2, 3), GroupService::response, temperature, 2, false));
	CHECK(encode(frame) == "11 00 BC E0 00 00 0A 03 03 00 40 0C 1A");
	hc::knx::make_group_read(frame, GroupAddress::of(1, 2, 3));
	CHECK(encode(frame) == "11 00 BC E0 00 00 0A 03 01 00 00");
}

TEST_CASE("KnxCemi.DecodeIndication") {
	// L_Data.ind from 1.1.10 to 1/0/1, hop count 5, A_GroupValue_Write 0C 1A (21.0 as DPT 9.001).
	LData frame;
	REQUIRE(decode(bytes("29 00 BC D0 11 0A 08 01 03 00 80 0C 1A"), frame));
	CHECK(frame.code == MessageCode::ldata_ind);
	CHECK(frame.priority == Priority::low);
	CHECK_FALSE(frame.repeated);
	CHECK(frame.group);
	CHECK(frame.hop_count == 5);
	CHECK(format(frame.source) == "1.1.10");
	CHECK(format(frame.group_address()) == "1/0/1");
	GroupData data;
	REQUIRE(hc::knx::group_data(frame, data));
	CHECK(data.service == GroupService::write);
	CHECK_FALSE(data.short_form);
	CHECK(data.length == 2);
	CHECK(data.data[0] == 0x0C);
	CHECK(data.data[1] == 0x1A);
	Value value;
	REQUIRE(hc::knx::decode_group_value(Dpt{9, 1}, data, value));
	CHECK(value.as_real() == doctest::Approx(21.0));

	// A repeated short response of "off", priority normal (control field 1 with bit 5 clear).
	REQUIRE(decode(bytes("29 00 94 E0 11 0A 08 01 01 00 40"), frame));
	CHECK(frame.repeated);
	CHECK(frame.priority == Priority::normal);
	REQUIRE(hc::knx::group_data(frame, data));
	CHECK(data.service == GroupService::response);
	CHECK(data.short_form);
	CHECK(data.length == 1);
	CHECK(data.data[0] == 0);

	// A read.
	REQUIRE(decode(bytes("29 00 BC E0 11 0A 08 01 01 00 00"), frame));
	REQUIRE(hc::knx::group_data(frame, data));
	CHECK(data.service == GroupService::read);
	CHECK(data.length == 0);
}

TEST_CASE("KnxCemi.SkipsAdditionalInformation") {
	LData frame;
	REQUIRE(decode(bytes("29 03 01 01 FF BC E0 11 0A 08 01 01 00 81"), frame));
	CHECK(format(frame.group_address()) == "1/0/1");
	CHECK(frame.tpdu_length == 2);
}

TEST_CASE("KnxCemi.ConfirmationAndMatching") {
	LData request;
	uint8_t on = 1;
	hc::knx::make_group_request(request, GroupAddress::of(1, 0, 1), GroupService::write, &on, 1, true);
	LData confirmation;
	REQUIRE(decode(bytes("2E 00 BC E0 11 FA 08 01 01 00 81"), confirmation));
	CHECK(confirmation.code == MessageCode::ldata_con);
	CHECK_FALSE(confirmation.confirm_error);
	CHECK(hc::knx::confirms(confirmation, request));
	REQUIRE(decode(bytes("2E 00 BD E0 11 FA 08 01 01 00 81"), confirmation));
	CHECK(confirmation.confirm_error);
	CHECK(hc::knx::confirms(confirmation, request));
	REQUIRE(decode(bytes("2E 00 BC E0 11 FA 08 01 01 00 80"), confirmation));
	CHECK_FALSE(hc::knx::confirms(confirmation, request)); // other value
	REQUIRE(decode(bytes("2E 00 BC E0 11 FA 08 02 01 00 81"), confirmation));
	CHECK_FALSE(hc::knx::confirms(confirmation, request)); // other address
}

TEST_CASE("KnxCemi.Refusals") {
	LData frame;
	CHECK(decode(bytes("29"), frame).status == CemiStatus::wrong_length);
	CHECK(decode(bytes("FC 00 00 01 01 00"), frame).status == CemiStatus::unsupported); // M_PropRead.con
	CHECK(decode(bytes("29 00 BC E0 11 0A 08"), frame).status == CemiStatus::wrong_length);
	CHECK(decode(bytes("29 00 BC E0 11 0A 08 01 03 00 80 0C"), frame).status == CemiStatus::wrong_length);
	CHECK(decode(bytes("29 00 BC E0 11 0A 08 01 01 00 81 00"), frame).status == CemiStatus::wrong_length);
	CHECK(decode(bytes("29 05 01 BC E0 11 0A 08 01 01 00 81"), frame).status == CemiStatus::wrong_length);
	Bytes long_frame = bytes("29 00 3C E0 11 0A 08 01 22");
	long_frame.resize(long_frame.size() + 35, 0x00);
	CHECK(decode(long_frame, frame).status == CemiStatus::too_long);

	// Individual addresses decode but are no group telegram; their payload is not interpreted.
	REQUIRE(decode(bytes("29 00 B0 60 11 0A 11 05 01 43 00"), frame));
	CHECK_FALSE(frame.group);
	GroupData data;
	hc::knx::CemiResult result = hc::knx::group_data(frame, data);
	CHECK(result.status == CemiStatus::unsupported);
	CHECK(std::string(result.reason) == "telegram to an individual address");
	CHECK(data.service == GroupService::other);

	// Another application service (A_GroupPropValue, APCI 0x3EB) and another transport service.
	REQUIRE(decode(bytes("29 00 BC E0 11 0A 08 01 01 03 EB"), frame));
	CHECK(hc::knx::group_data(frame, data).status == CemiStatus::unsupported);
	REQUIRE(decode(bytes("29 00 BC E0 11 0A 08 01 01 04 80"), frame));
	CHECK(hc::knx::group_data(frame, data).status == CemiStatus::unsupported);
	// A read carrying data.
	REQUIRE(decode(bytes("29 00 BC E0 11 0A 08 01 02 00 00 01"), frame));
	CHECK(hc::knx::group_data(frame, data).status == CemiStatus::invalid);

	// Sending: short data above 6 bits, a read with data, too much data.
	uint8_t big = 0x40;
	CHECK(hc::knx::make_group_request(frame, GroupAddress{1}, GroupService::write, &big, 1, true).status ==
		CemiStatus::invalid);
	CHECK(hc::knx::make_group_request(frame, GroupAddress{1}, GroupService::read, &big, 1, false).status ==
		CemiStatus::invalid);
	uint8_t many[15] = {};
	CHECK(hc::knx::make_group_request(frame, GroupAddress{1}, GroupService::write, many, 15, false).status ==
		CemiStatus::too_long);
	LData empty;
	uint8_t out[LData::kMaxFrame];
	size_t length = 0;
	CHECK(hc::knx::encode(empty, out, sizeof(out), length).status == CemiStatus::invalid);
}

TEST_CASE("KnxCemi.RoundTrip") {
	LData frame;
	const uint8_t text[14] = {'H', 'e', 'l', 'l', 'o', 0, 0, 0, 0, 0, 0, 0, 0, 0};
	REQUIRE(hc::knx::make_group_request(frame, GroupAddress::of(5, 6, 7), GroupService::write, text, 14, false));
	frame.source = IndividualAddress::of(1, 1, 250);
	uint8_t out[LData::kMaxFrame];
	size_t length = 0;
	REQUIRE(hc::knx::encode(frame, out, sizeof(out), length));
	CHECK(length == 25);
	LData back;
	REQUIRE(hc::knx::decode(out, length, back));
	CHECK(back.destination == frame.destination);
	CHECK(back.source == frame.source);
	CHECK(back.tpdu_length == 16);
	CHECK(std::memcmp(back.tpdu, frame.tpdu, 16) == 0);
}

// ---- KNXnet/IP ----------------------------------------------------------------------------------

TEST_CASE("KnxIp.ClientFrames") {
	ip::FrameBuffer frame;
	ip::build_connect_request(frame, kLocal, kLocal);
	CHECK(hex(frame) == "06 10 02 05 00 1A 08 01 C0 00 02 64 C3 B4 08 01 C0 00 02 64 C3 B4 04 04 02 00");
	ip::build_connect_request(frame, hc::Ipv4Endpoint{}, hc::Ipv4Endpoint{});
	CHECK(hex(frame) == "06 10 02 05 00 1A 08 01 00 00 00 00 00 00 08 01 00 00 00 00 00 00 04 04 02 00");
	ip::build_connectionstate_request(frame, 7, kLocal);
	CHECK(hex(frame) == "06 10 02 07 00 10 07 00 08 01 C0 00 02 64 C3 B4");
	ip::build_disconnect_request(frame, 7, kLocal);
	CHECK(hex(frame) == "06 10 02 09 00 10 07 00 08 01 C0 00 02 64 C3 B4");
	ip::build_disconnect_response(frame, 7, 0);
	CHECK(hex(frame) == "06 10 02 0A 00 08 07 00");
	ip::build_tunneling_ack(frame, 7, 3, 0);
	CHECK(hex(frame) == "06 10 04 21 00 0A 04 07 03 00");
	LData data;
	uint8_t on = 1;
	hc::knx::make_group_request(data, GroupAddress::of(1, 0, 1), GroupService::write, &on, 1, true);
	REQUIRE(ip::build_tunneling_request(frame, 7, 0, data));
	CHECK(hex(frame) == "06 10 04 20 00 15 04 07 00 00 11 00 BC E0 00 00 08 01 01 00 81");
	ip::build_search_request(frame, hc::Ipv4Endpoint{hc_test::kLocalIp, 3671});
	CHECK(hex(frame) == "06 10 02 01 00 0E 08 01 C0 00 02 64 0E 57");
}

TEST_CASE("KnxIp.Header") {
	uint16_t service = 0;
	Bytes ok = bytes("06 10 02 08 00 08 07 00");
	CHECK(ip::parse_header(ok.data(), ok.size(), service));
	CHECK(service == 0x0208);
	Bytes version = bytes("06 20 02 08 00 08 07 00");
	CHECK(ip::parse_header(version.data(), version.size(), service).status == ip::ParseStatus::wrong_header);
	Bytes length = bytes("06 10 02 08 00 09 07 00");
	CHECK(ip::parse_header(length.data(), length.size(), service).status == ip::ParseStatus::wrong_length);
	Bytes tiny = bytes("06 10 02");
	CHECK(ip::parse_header(tiny.data(), tiny.size(), service).status == ip::ParseStatus::wrong_length);
	CHECK(std::string(ip::service_name(0x0420)) == "TUNNELING_REQUEST");
	CHECK(std::string(ip::status_name(0x24)) == "E_NO_MORE_CONNECTIONS");
}

TEST_CASE("KnxIp.ConnectResponse") {
	Bytes frame = hc_test::connect_response(7);
	CHECK(hex(frame) == "06 10 02 06 00 14 07 00 08 01 C0 00 02 01 0E 57 04 04 11 FA");
	ip::ConnectResponse response;
	REQUIRE(ip::parse_connect_response(frame.data(), frame.size(), response));
	CHECK(response.channel == 7);
	CHECK(response.status == 0);
	CHECK(response.data == hc_test::kGateway);
	CHECK(format(response.address) == "1.1.250");

	Bytes refused = hc_test::connect_response(0, 0x24);
	REQUIRE(ip::parse_connect_response(refused.data(), refused.size(), response));
	CHECK(response.status == 0x24);

	Bytes bad_crd = bytes("06 10 02 06 00 14 07 00 08 01 C0 00 02 01 0E 57 04 03 11 FA");
	CHECK(ip::parse_connect_response(bad_crd.data(), bad_crd.size(), response).status == ip::ParseStatus::invalid);
	Bytes bad_hpai = bytes("06 10 02 06 00 14 07 00 08 02 C0 00 02 01 0E 57 04 04 11 FA");
	CHECK(ip::parse_connect_response(bad_hpai.data(), bad_hpai.size(), response).status == ip::ParseStatus::invalid);
	Bytes short_crd = bytes("06 10 02 06 00 10 07 00 08 01 C0 00 02 01 0E 57");
	CHECK(ip::parse_connect_response(short_crd.data(), short_crd.size(), response).status ==
		ip::ParseStatus::wrong_length);
	Bytes other = hc_test::connectionstate_response(7);
	CHECK(ip::parse_connect_response(other.data(), other.size(), response).status == ip::ParseStatus::wrong_service);
}

TEST_CASE("KnxIp.TunnelingFrames") {
	ip::TunnelingRequest request;
	Bytes frame = hc_test::tunneling_request(7, 9, bytes("29 00 BC D0 11 0A 08 01 01 00 81"));
	REQUIRE(ip::parse_tunneling_request(frame.data(), frame.size(), request));
	CHECK(request.channel == 7);
	CHECK(request.sequence == 9);
	CHECK(format(request.frame.group_address()) == "1/0/1");

	// A cEMI refusal keeps the connection header, so the request can be acknowledged.
	Bytes bad = hc_test::tunneling_request(7, 10, bytes("FC 00 00 01 01 00"));
	ip::ParseResult result = ip::parse_tunneling_request(bad.data(), bad.size(), request);
	CHECK(result.status == ip::ParseStatus::cemi);
	CHECK(request.sequence == 10);

	Bytes wrong_header = bytes("06 10 04 20 00 0B 05 07 00 00 29");
	CHECK(ip::parse_tunneling_request(wrong_header.data(), wrong_header.size(), request).status ==
		ip::ParseStatus::invalid);

	ip::TunnelingAck ack;
	Bytes ack_frame = hc_test::tunneling_ack(7, 4, 0x29);
	REQUIRE(ip::parse_tunneling_ack(ack_frame.data(), ack_frame.size(), ack));
	CHECK(ack.channel == 7);
	CHECK(ack.sequence == 4);
	CHECK(ack.status == 0x29);

	ip::DisconnectRequest disconnect;
	Bytes d = hc_test::disconnect_request(7);
	REQUIRE(ip::parse_disconnect_request(d.data(), d.size(), disconnect));
	CHECK(disconnect.channel == 7);
	CHECK(disconnect.control == hc_test::kGateway);
}

TEST_CASE("KnxIp.SearchResponse") {
	Bytes frame = bytes("06 10 02 02 00 4C 08 01 C0 00 02 01 0E 57"
		" 36 01 02 01 11 00 00 2A 00 01 02 03 04 05 E0 00 17 0C 00 11 22 33 44 55"
		" 54 65 73 74 20 47 61 74 65 77 61 79 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
		" 08 02 02 01 04 01 05 01");
	REQUIRE(frame.size() == 76);
	ip::SearchResponse response;
	REQUIRE(ip::parse_search_response(frame.data(), frame.size(), response));
	CHECK(response.control == hc_test::kGateway);
	CHECK(response.device.medium == 0x02);
	CHECK(response.device.device_status == 0x01);
	CHECK(format(response.device.address) == "1.1.0");
	CHECK(response.device.installation == 0x002A);
	CHECK(response.device.serial[5] == 0x05);
	CHECK(response.device.multicast == 0xE000170C);
	CHECK(response.device.mac[0] == 0x00);
	CHECK(response.device.mac[5] == 0x55);
	CHECK(std::string(response.device.name) == "Test Gateway");
	CHECK(response.families.count == 3);
	CHECK(response.families.version(ip::family::kTunnelling) == 1);
	CHECK(response.families.version(ip::family::kRouting) == 1);
	CHECK(response.families.version(ip::family::kDeviceManagement) == 0);

	Bytes cut = frame;
	cut.resize(70);
	cut[5] = 70;
	CHECK_FALSE(ip::parse_search_response(cut.data(), cut.size(), response));
	Bytes no_services(frame.begin(), frame.begin() + 68);
	no_services[5] = 68;
	CHECK(ip::parse_search_response(no_services.data(), no_services.size(), response).status ==
		ip::ParseStatus::invalid);
}

// ---- Setting ------------------------------------------------------------------------------------

TEST_CASE("KnxSettings.GatewayParsing") {
	std::optional<GatewayAddress> gateway = GatewayAddress::parse("192.0.2.1");
	REQUIRE(gateway);
	CHECK(std::string(gateway->host) == "192.0.2.1");
	CHECK(gateway->port == 3671);
	gateway = GatewayAddress::parse("knx-gateway.example:3700");
	REQUIRE(gateway);
	CHECK(std::string(gateway->host) == "knx-gateway.example");
	CHECK(gateway->port == 3700);
	char text[GatewayAddress::kMaxText];
	CHECK(std::string(gateway->format(text, sizeof(text))) == "knx-gateway.example:3700");

	const char* error = nullptr;
	CHECK_FALSE(GatewayAddress::parse("", &error));
	CHECK(std::string(error) == "gateway is empty");
	CHECK_FALSE(GatewayAddress::parse(":3671", &error));
	CHECK(std::string(error) == "host name is empty");
	CHECK_FALSE(GatewayAddress::parse("192.0.2.1:", &error));
	CHECK(std::string(error) == "gateway port is not a number");
	CHECK_FALSE(GatewayAddress::parse("192.0.2.1:0", &error));
	CHECK(std::string(error) == "gateway port must be 1..65535");
	CHECK_FALSE(GatewayAddress::parse("192.0.2.1:65536", &error));
	CHECK_FALSE(GatewayAddress::parse("192.0.2.1:36x", &error));
	CHECK_FALSE(GatewayAddress::parse("2001:db8::1", &error));
	CHECK(std::string(error).find("IPv6") != std::string::npos);
	CHECK_FALSE(GatewayAddress::parse("knx_gateway", &error));
	CHECK(std::string(error).find("letters, digits") != std::string::npos);
	std::string long_name(130, 'a');
	CHECK_FALSE(GatewayAddress::parse(long_name.c_str(), &error));
}

TEST_CASE("KnxSettings.ReadAndStore") {
	hc_test::ScopedCapture capture;
	hc_test::FakeSettings settings;
	GatewayAddress gateway;
	bool nat = true;
	hc::knx::read_gateway_settings(settings, gateway, nat);
	CHECK(gateway.empty());
	CHECK_FALSE(nat);
	CHECK(capture.sink.lines.empty()); // not set is not an error

	const char* error = nullptr;
	CHECK_FALSE(hc::knx::store_gateway_setting(settings, "192.0.2.1:99999", &error));
	CHECK(settings.entries.empty());
	REQUIRE(hc::knx::store_gateway_setting(settings, "192.0.2.1:3672", &error));
	settings.set_bool(hc::knx::setting::kNat, true);
	hc::knx::read_gateway_settings(settings, gateway, nat);
	CHECK(std::string(gateway.host) == "192.0.2.1");
	CHECK(gateway.port == 3672);
	CHECK(nat);

	settings.set_string(hc::knx::setting::kGateway, "not a host");
	hc::knx::read_gateway_settings(settings, gateway, nat);
	CHECK(gateway.empty());
	CHECK(capture.sink.contains("E hc.knx: setting knx.gateway is invalid"));
}

// ---- Group values -------------------------------------------------------------------------------

TEST_CASE("KnxGroupValue.EncodeDecode") {
	LData frame;
	const char* error = nullptr;
	REQUIRE(hc::knx::make_group_value(frame, GroupAddress::of(1, 0, 1), GroupService::write, Dpt{1, 1},
		Value::of_bool(true), &error));
	CHECK(encode(frame) == "11 00 BC E0 00 00 08 01 01 00 81");
	REQUIRE(hc::knx::make_group_value(frame, GroupAddress::of(1, 2, 3), GroupService::write, Dpt{9, 1},
		Value::of_real(21.0), &error));
	CHECK(encode(frame) == "11 00 BC E0 00 00 0A 03 03 00 80 0C 1A");
	CHECK_FALSE(hc::knx::make_group_value(frame, GroupAddress::of(1, 2, 3), GroupService::write, Dpt{9, 1},
		Value::of_real(-300.0), &error));
	CHECK_FALSE(hc::knx::make_group_value(frame, GroupAddress::of(1, 2, 3), GroupService::read, Dpt{9, 1},
		Value::of_real(1.0), &error));

	GroupData data;
	data.short_form = true;
	data.length = 1;
	data.data[0] = 1;
	Value value;
	CHECK(hc::knx::decode_group_value(Dpt{1, 1}, data, value));
	CHECK(value.as_bool());
	CHECK(hc::knx::decode_group_value(Dpt{9, 1}, data, value).status == hc::DptStatus::wrong_length);
	data.short_form = false;
	CHECK(hc::knx::decode_group_value(Dpt{1, 1}, data, value).status == hc::DptStatus::wrong_length);
	CHECK(hc::knx::decode_group_value(Dpt{5, 1}, data, value));
}

TEST_CASE("KnxGroupValue.TextForms") {
	Value value;
	const char* error = nullptr;
	REQUIRE(hc::knx::parse_value_text(Dpt{1, 1}, "on", value, &error));
	CHECK(value.as_bool());
	REQUIRE(hc::knx::parse_value_text(Dpt{1, 1}, "0", value, &error));
	CHECK_FALSE(value.as_bool());
	CHECK_FALSE(hc::knx::parse_value_text(Dpt{1, 1}, "maybe", value, &error));
	REQUIRE(hc::knx::parse_value_text(Dpt{9, 1}, "21.5", value, &error));
	CHECK(value.as_real() == doctest::Approx(21.5));
	CHECK_FALSE(hc::knx::parse_value_text(Dpt{9, 1}, "21,5", value, &error));
	REQUIRE(hc::knx::parse_value_text(Dpt{5, 10}, "200", value, &error));
	REQUIRE(hc::knx::parse_value_text(Dpt{16, 0}, "Hello", value, &error));
	CHECK(std::string(value.as_string()) == "Hello");
	REQUIRE(hc::knx::parse_value_text(Dpt{3, 7}, "1,5", value, &error));
	CHECK(value.as_controlled().control);
	CHECK(value.as_controlled().value == 5);
	REQUIRE(hc::knx::parse_value_text(Dpt{10, 1}, "13:45", value, &error));
	CHECK(value.as_time().minute == 45);
	REQUIRE(hc::knx::parse_value_text(Dpt{11, 1}, "2026-10-17", value, &error));
	CHECK(value.as_date().day == 17);
	REQUIRE(hc::knx::parse_value_text(Dpt{19, 1}, "2026-10-17T13:45:30", value, &error));
	CHECK(value.as_date_time().second == 30);
	REQUIRE(hc::knx::parse_value_text(Dpt{232, 600}, "255,128,0", value, &error));
	CHECK(value.as_color().green == 128);
	CHECK_FALSE(hc::knx::parse_value_text(Dpt{232, 600}, "255,128", value, &error));
	CHECK(std::string(error) == "expected R,G,B or R,G,B,W");
	CHECK_FALSE(hc::knx::parse_value_text(Dpt{999, 1}, "1", value, &error));
}

// ---- Telegram log -------------------------------------------------------------------------------

TEST_CASE("KnxTelegramLog.StampsCountsAndHandsOn") {
	hc_test::FakeClock clock;
	TelegramLog log(clock);
	hc_test::TelegramRecorder recorded;
	log.append(TelegramRecord{}); // without a listener: counted only
	log.set_listener(&recorded);
	for (uint16_t i = 0; i < 70; ++i) {
		TelegramRecord record;
		record.destination = GroupAddress{i};
		record.monotonic_ms = 1; // the log stamps its own time
		clock.advance(10);
		log.append(record);
	}
	CHECK(log.total() == 71);
	REQUIRE(recorded.size() == 70);
	CHECK(recorded.at(0)->destination.raw == 0);
	CHECK(recorded.at(0)->monotonic_ms == 1000 + 10);
	CHECK(recorded.at(69)->destination.raw == 69);
	CHECK(recorded.at(69)->monotonic_ms == 1000 + 700);
	log.clear();
	CHECK(log.total() == 0);
}
