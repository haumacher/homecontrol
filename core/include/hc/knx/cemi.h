/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_CEMI_H
#define HC_KNX_CEMI_H

#include <cstddef>
#include <cstdint>

#include "hc/knx/address.h"

namespace hc {
namespace knx {

/// The cEMI message codes of the link layer data service (KNX 03.06.03, EMI/IMI).
enum class MessageCode : uint8_t {
	ldata_req = 0x11, ///< L_Data.req: a frame to send.
	ldata_con = 0x2E, ///< L_Data.con: the interface's confirmation of a sent frame.
	ldata_ind = 0x29, ///< L_Data.ind: a frame received from the bus.
};

/// "L_Data.req", "L_Data.con", "L_Data.ind".
const char* message_code_name(MessageCode code);

/// The priority of a frame, as on the bus (control field bits 3..2).
enum class Priority : uint8_t {
	system = 0,
	normal = 1,
	urgent = 2,
	low = 3, ///< Normal process communication: what group telegrams are sent with.
};

/// "system", "normal", "urgent", "low" (E-Home's TelegramPriority classifiers).
const char* priority_name(Priority priority);

/// The application layer services of group communication. Everything else (management services,
/// telegrams to individual addresses, other transport services) is `other`.
enum class GroupService : uint8_t {
	read,     ///< A_GroupValue_Read (APCI 0x000).
	response, ///< A_GroupValue_Response (APCI 0x040).
	write,    ///< A_GroupValue_Write (APCI 0x080).
	other,
};

/// "read", "response", "write", "other" (E-Home's TelegramService classifiers).
const char* group_service_name(GroupService service);

/// Outcome of a cEMI codec call. Everything except `ok` is a refusal with a reason text.
enum class CemiStatus : uint8_t {
	ok,
	wrong_length,  ///< The frame is shorter than its fields, or its length field disagrees with the data.
	unsupported,   ///< A message code other than L_Data.req/con/ind, or no group value service.
	too_long,      ///< The TPDU is longer than LData::kMaxTpdu, or the data longer than GroupData::kMaxData.
	invalid,       ///< A field value the frame cannot carry (short data above 6 bits, a read with data).
};

struct CemiResult {
	CemiStatus status = CemiStatus::ok;
	const char* reason = "";

	explicit operator bool() const { return status == CemiStatus::ok; }
};

/// One cEMI L_Data frame: the control fields, the addresses and the TPDU (transport layer PDU: the
/// TPCI octet and what follows; for group communication the TPCI/APCI octets and the data). The
/// additional information of a received frame is skipped; sent frames carry none.
struct LData {
	/// Longest TPDU kept. A group telegram with the longest DPT (14 octets) takes 16; longer
	/// frames (management, individual addresses) are refused as too long.
	static constexpr size_t kMaxTpdu = 32;

	/// Longest encoded frame: message code, additional info length, two control fields, two
	/// addresses, the length field and the TPDU.
	static constexpr size_t kMaxFrame = 9 + kMaxTpdu;

	/// The hop count a frame is sent with (routing counter).
	static constexpr uint8_t kDefaultHopCount = 6;

	MessageCode code = MessageCode::ldata_req;
	Priority priority = Priority::low;
	/// Control field 1 bit 5 clear. In an indication: the frame is a repetition. In a request: the
	/// medium must not repeat it on an error (false, the default, lets it repeat).
	bool repeated = false;
	/// Control field 1 bit 4: true for a normal (domain) broadcast, false for a system broadcast.
	bool broadcast = true;
	/// Control field 1 bit 1: a layer 2 acknowledge is requested.
	bool ack_request = false;
	/// Control field 1 bit 0: in a confirmation, the frame could not be sent (negative confirmation).
	bool confirm_error = false;
	IndividualAddress source;
	/// A group address if `group`, an individual address otherwise.
	uint16_t destination = 0;
	/// Control field 2 bit 7: the destination is a group address.
	bool group = true;
	uint8_t hop_count = kDefaultHopCount; ///< 0..7
	uint8_t tpdu[kMaxTpdu] = {};
	uint8_t tpdu_length = 0;

	GroupAddress group_address() const { return GroupAddress{destination}; }
};

/// Encodes `frame` into `out` (capacity `size`, at least LData::kMaxFrame suffices); `length` gets
/// the number of octets. Refused (invalid) for an empty TPDU or a hop count above 7.
CemiResult encode(const LData& frame, uint8_t* out, size_t size, size_t& length);

/// Decodes a cEMI frame. Refused: a message code other than L_Data.req/con/ind (unsupported), a
/// frame shorter than its fields or with a length field that disagrees with the octets
/// (wrong_length), a TPDU longer than LData::kMaxTpdu (too_long). The TPDU is kept as received;
/// group_data() interprets it.
CemiResult decode(const uint8_t* data, size_t length, LData& frame);

/// The application data of a group telegram.
struct GroupData {
	static constexpr size_t kMaxData = 14;

	GroupService service = GroupService::other;
	/// The data as hc::decode() takes it: for a short telegram (value in the APCI octet) one
	/// octet holding the low 6 bits, otherwise the octets after the APCI; empty for a read.
	uint8_t data[kMaxData] = {};
	uint8_t length = 0;
	/// The value travelled in the low 6 bits of the APCI octet (the DPTs whose Dpt::embedded()).
	bool short_form = false;
};

/// Interprets the TPDU of a group telegram: A_GroupValue_Read/Response/Write with the data in the
/// APCI octet (TPDU of 2 octets) or after it. Refused (unsupported, `out.service` other) for a frame
/// to an individual address, another transport service or another application service; refused
/// (too_long) for data beyond GroupData::kMaxData.
CemiResult group_data(const LData& frame, GroupData& out);

/// Sets the TPDU of `frame` to a group service with `data`: for `short_form` one octet with the 6
/// bits (merged into the APCI octet), otherwise up to GroupData::kMaxData octets after the APCI. A
/// read takes no data. Refused (invalid, too_long) with the TPDU unchanged.
CemiResult set_group_data(LData& frame, GroupService service, const uint8_t* data, size_t length, bool short_form);

/// A group telegram to send: L_Data.req, priority low, hop count 6, source 0.0.0 (the gateway
/// fills in the tunnel's address), the given service and data (see set_group_data()).
CemiResult make_group_request(LData& frame, GroupAddress destination, GroupService service, const uint8_t* data,
	size_t length, bool short_form);

/// Whether `confirmation` (an L_Data.con) confirms `request`: same destination and address type,
/// same TPDU.
bool confirms(const LData& confirmation, const LData& request);

/// The fields of a cEMI L_Data frame read in place, without the TPDU limit of decode(): what the bus
/// session (spec/api.md "Bus session") needs to check, route and match frames it forwards as they
/// are, extended frames included.
struct RawView {
	MessageCode code = MessageCode::ldata_req;
	/// Control field 2 bit 7: the destination is a group address (0 with it: a broadcast).
	bool group = false;
	uint16_t destination = 0;
	/// Control field 1 bit 0 (meaningful in an L_Data.con: not sent).
	bool confirm_error = false;
	/// Where the TPDU starts in the frame, and its length (the length field plus one).
	size_t tpdu_offset = 0;
	size_t tpdu_length = 0;

	/// A broadcast: the group flag with destination 0.
	bool broadcast() const { return group && destination == 0; }
};

/// Reads a cEMI L_Data frame of any length in place. Refused: a message code other than
/// L_Data.req/con/ind (unsupported), a frame shorter than its fields or with a length field that
/// disagrees with the octets (wrong_length).
CemiResult view(const uint8_t* data, size_t length, RawView& out);

/// Whether the L_Data.con `confirmation` confirms the frame `request` (both read with view()): same
/// address type and destination, same TPDU.
bool confirms(const uint8_t* confirmation, const RawView& confirmation_view, const uint8_t* request,
	const RawView& request_view);

} // namespace knx
} // namespace hc

#endif // HC_KNX_CEMI_H
