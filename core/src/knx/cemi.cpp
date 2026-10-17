/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/knx/cemi.h"

#include <cstring>

namespace hc {
namespace knx {

namespace {

constexpr uint16_t kApciRead = 0x000;
constexpr uint16_t kApciResponse = 0x040;
constexpr uint16_t kApciWrite = 0x080;

CemiResult fail(CemiStatus status, const char* reason) {
	return CemiResult{status, reason};
}

bool known_code(uint8_t code) {
	return code == static_cast<uint8_t>(MessageCode::ldata_req) || code == static_cast<uint8_t>(MessageCode::ldata_con) ||
		code == static_cast<uint8_t>(MessageCode::ldata_ind);
}

} // namespace

const char* message_code_name(MessageCode code) {
	switch (code) {
	case MessageCode::ldata_req:
		return "L_Data.req";
	case MessageCode::ldata_con:
		return "L_Data.con";
	case MessageCode::ldata_ind:
		return "L_Data.ind";
	}
	return "?";
}

const char* priority_name(Priority priority) {
	switch (priority) {
	case Priority::system:
		return "system";
	case Priority::normal:
		return "normal";
	case Priority::urgent:
		return "urgent";
	case Priority::low:
		return "low";
	}
	return "?";
}

const char* group_service_name(GroupService service) {
	switch (service) {
	case GroupService::read:
		return "read";
	case GroupService::response:
		return "response";
	case GroupService::write:
		return "write";
	case GroupService::other:
		return "other";
	}
	return "?";
}

CemiResult encode(const LData& frame, uint8_t* out, size_t size, size_t& length) {
	length = 0;
	if (frame.tpdu_length == 0) {
		return fail(CemiStatus::invalid, "frame has no TPDU");
	}
	if (frame.tpdu_length > LData::kMaxTpdu) {
		return fail(CemiStatus::too_long, "TPDU longer than 32 octets");
	}
	if (frame.hop_count > 7) {
		return fail(CemiStatus::invalid, "hop count above 7");
	}
	size_t total = 9u + frame.tpdu_length;
	if (size < total) {
		return fail(CemiStatus::too_long, "output buffer too small");
	}
	// A standard frame carries at most 15 octets after the TPCI; longer ones are extended frames.
	bool standard = frame.tpdu_length <= 16;
	uint8_t ctrl1 = static_cast<uint8_t>((standard ? 0x80 : 0x00) | (frame.repeated ? 0x00 : 0x20) |
		(frame.broadcast ? 0x10 : 0x00) | (static_cast<uint8_t>(frame.priority) << 2) |
		(frame.ack_request ? 0x02 : 0x00) | (frame.confirm_error ? 0x01 : 0x00));
	uint8_t ctrl2 = static_cast<uint8_t>((frame.group ? 0x80 : 0x00) | (frame.hop_count << 4));
	out[0] = static_cast<uint8_t>(frame.code);
	out[1] = 0; // no additional information
	out[2] = ctrl1;
	out[3] = ctrl2;
	out[4] = static_cast<uint8_t>(frame.source.raw >> 8);
	out[5] = static_cast<uint8_t>(frame.source.raw);
	out[6] = static_cast<uint8_t>(frame.destination >> 8);
	out[7] = static_cast<uint8_t>(frame.destination);
	out[8] = static_cast<uint8_t>(frame.tpdu_length - 1); // octets after the TPCI
	std::memcpy(out + 9, frame.tpdu, frame.tpdu_length);
	length = total;
	return CemiResult{};
}

CemiResult decode(const uint8_t* data, size_t length, LData& frame) {
	if (length < 2) {
		return fail(CemiStatus::wrong_length, "cEMI frame shorter than 2 octets");
	}
	if (!known_code(data[0])) {
		return fail(CemiStatus::unsupported, "cEMI message code is not L_Data.req/con/ind");
	}
	size_t info = data[1];
	size_t base = 2 + info;
	if (length < base + 7) {
		return fail(CemiStatus::wrong_length, "cEMI L_Data frame shorter than its fields");
	}
	size_t tpdu_length = static_cast<size_t>(data[base + 6]) + 1;
	if (length != base + 7 + tpdu_length) {
		return fail(CemiStatus::wrong_length, "cEMI length field disagrees with the frame length");
	}
	if (tpdu_length > LData::kMaxTpdu) {
		return fail(CemiStatus::too_long, "TPDU longer than 32 octets");
	}
	uint8_t ctrl1 = data[base];
	uint8_t ctrl2 = data[base + 1];
	frame.code = static_cast<MessageCode>(data[0]);
	frame.repeated = (ctrl1 & 0x20) == 0;
	frame.broadcast = (ctrl1 & 0x10) != 0;
	frame.priority = static_cast<Priority>((ctrl1 >> 2) & 0x03);
	frame.ack_request = (ctrl1 & 0x02) != 0;
	frame.confirm_error = (ctrl1 & 0x01) != 0;
	frame.group = (ctrl2 & 0x80) != 0;
	frame.hop_count = static_cast<uint8_t>((ctrl2 >> 4) & 0x07);
	frame.source.raw = static_cast<uint16_t>((data[base + 2] << 8) | data[base + 3]);
	frame.destination = static_cast<uint16_t>((data[base + 4] << 8) | data[base + 5]);
	frame.tpdu_length = static_cast<uint8_t>(tpdu_length);
	std::memcpy(frame.tpdu, data + base + 7, tpdu_length);
	return CemiResult{};
}

CemiResult group_data(const LData& frame, GroupData& out) {
	out = GroupData{};
	if (!frame.group) {
		return fail(CemiStatus::unsupported, "telegram to an individual address");
	}
	if (frame.tpdu_length < 2) {
		return fail(CemiStatus::unsupported, "TPDU without an application layer");
	}
	if ((frame.tpdu[0] & 0xFC) != 0x00) {
		return fail(CemiStatus::unsupported, "transport service other than T_Data_Group");
	}
	uint16_t apci = static_cast<uint16_t>(((frame.tpdu[0] & 0x03) << 8) | (frame.tpdu[1] & 0xC0));
	if (apci == kApciRead) {
		out.service = GroupService::read;
	} else if (apci == kApciResponse) {
		out.service = GroupService::response;
	} else if (apci == kApciWrite) {
		out.service = GroupService::write;
	} else {
		return fail(CemiStatus::unsupported, "application service other than A_GroupValue_Read/Response/Write");
	}
	if (out.service == GroupService::read) {
		// The low 6 bits of a read carry nothing; data after the APCI would be a malformed read.
		if (frame.tpdu_length != 2) {
			out.service = GroupService::other;
			return fail(CemiStatus::invalid, "A_GroupValue_Read with data");
		}
		return CemiResult{};
	}
	if (frame.tpdu_length == 2) {
		out.short_form = true;
		out.data[0] = static_cast<uint8_t>(frame.tpdu[1] & 0x3F);
		out.length = 1;
		return CemiResult{};
	}
	size_t length = frame.tpdu_length - 2u;
	if (length > GroupData::kMaxData) {
		out.service = GroupService::other;
		return fail(CemiStatus::too_long, "group data longer than 14 octets");
	}
	std::memcpy(out.data, frame.tpdu + 2, length);
	out.length = static_cast<uint8_t>(length);
	return CemiResult{};
}

CemiResult set_group_data(LData& frame, GroupService service, const uint8_t* data, size_t length, bool short_form) {
	uint16_t apci = 0;
	switch (service) {
	case GroupService::read:
		apci = kApciRead;
		if (length != 0) {
			return fail(CemiStatus::invalid, "A_GroupValue_Read takes no data");
		}
		break;
	case GroupService::response:
		apci = kApciResponse;
		break;
	case GroupService::write:
		apci = kApciWrite;
		break;
	case GroupService::other:
		return fail(CemiStatus::unsupported, "not a group value service");
	}
	if (service != GroupService::read) {
		if (short_form) {
			if (length != 1) {
				return fail(CemiStatus::invalid, "short group data is one octet");
			}
			if ((data[0] & 0xC0) != 0) {
				return fail(CemiStatus::invalid, "short group data has more than 6 bits");
			}
		} else {
			if (length == 0) {
				return fail(CemiStatus::invalid, "group data is empty");
			}
			if (length > GroupData::kMaxData) {
				return fail(CemiStatus::too_long, "group data longer than 14 octets");
			}
		}
	}
	frame.tpdu[0] = static_cast<uint8_t>(apci >> 8); // T_Data_Group: TPCI 000000
	frame.tpdu[1] = static_cast<uint8_t>(apci & 0xFF);
	if (service == GroupService::read) {
		frame.tpdu_length = 2;
	} else if (short_form) {
		frame.tpdu[1] = static_cast<uint8_t>(frame.tpdu[1] | data[0]);
		frame.tpdu_length = 2;
	} else {
		std::memcpy(frame.tpdu + 2, data, length);
		frame.tpdu_length = static_cast<uint8_t>(2 + length);
	}
	return CemiResult{};
}

CemiResult make_group_request(LData& frame, GroupAddress destination, GroupService service, const uint8_t* data,
	size_t length, bool short_form) {
	LData result;
	result.code = MessageCode::ldata_req;
	result.destination = destination.raw;
	result.group = true;
	CemiResult status = set_group_data(result, service, data, length, short_form);
	if (status) {
		frame = result;
	}
	return status;
}

bool confirms(const LData& confirmation, const LData& request) {
	return confirmation.code == MessageCode::ldata_con && confirmation.group == request.group &&
		confirmation.destination == request.destination && confirmation.tpdu_length == request.tpdu_length &&
		std::memcmp(confirmation.tpdu, request.tpdu, request.tpdu_length) == 0;
}

} // namespace knx
} // namespace hc
