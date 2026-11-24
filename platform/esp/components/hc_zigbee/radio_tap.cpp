/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "radio_tap.h"

#include <cstddef>
#include <cstdint>

#include "esp_attr.h"
#include "esp_ieee802154.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace hc {
namespace esp {

namespace {

/// Short addresses counted at once; the least recently heard one makes room for a new one.
constexpr size_t kEntries = 24;

constexpr uint16_t kNone = 0xffff;

/// The MAC frame types and the command of a poll (IEEE 802.15.4).
constexpr uint8_t kFrameCommand = 3;
constexpr uint8_t kDataRequest = 0x04;

struct Entry {
	uint16_t short_addr;
	uint32_t polls;
	uint32_t pending;
	int64_t last_poll_us;
	int64_t last_heard_us;
	uint32_t longest_gap_ms;
	uint32_t gaps_ms[ZigbeePolls::kGaps];
	uint32_t sent;
	uint32_t no_ack;
	uint32_t coexist;
	uint32_t failed;
};

struct Totals {
	uint32_t sent;
	uint32_t no_ack;
	uint32_t coexist;
	uint32_t failed;
};

// Written in the driver's interrupt, read on the event loop, under one spin lock.
DRAM_ATTR Entry s_entries[kEntries];
DRAM_ATTR Totals s_totals;
DRAM_ATTR portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
bool s_installed = false;

/// The addresses and the payload of a MAC frame as the driver passes it: the length byte (the PHY
/// header, counting the two bytes of the FCS at the end), then the MAC header.
struct Header {
	uint8_t type;
	uint16_t dst;
	uint16_t src;
	const uint8_t* payload;
	const uint8_t* end;
};

IRAM_ATTR bool parse(const uint8_t* frame, Header& header) {
	uint8_t length = frame[0];
	if (length < 5 || length > 127) {
		return false;
	}
	const uint8_t* p = frame + 1;
	const uint8_t* end = frame + 1 + length - 2;
	uint16_t fcf = static_cast<uint16_t>(p[0] | (p[1] << 8));
	p += 3; // frame control and sequence number (Zigbee's 2003/2006 frames always carry it)
	header.type = fcf & 0x7;
	bool pan_compression = (fcf & (1u << 6)) != 0;
	uint8_t dst_mode = (fcf >> 10) & 0x3;
	uint8_t src_mode = (fcf >> 14) & 0x3;
	header.dst = kNone;
	header.src = kNone;
	if (dst_mode != 0) {
		p += 2; // destination PAN
		if (dst_mode == 2) {
			if (p + 2 > end) {
				return false;
			}
			header.dst = static_cast<uint16_t>(p[0] | (p[1] << 8));
			p += 2;
		} else if (dst_mode == 3) {
			p += 8;
		} else {
			return false;
		}
	}
	if (src_mode != 0) {
		if (!pan_compression) {
			p += 2; // source PAN
		}
		if (src_mode == 2) {
			if (p + 2 > end) {
				return false;
			}
			header.src = static_cast<uint16_t>(p[0] | (p[1] << 8));
			p += 2;
		} else if (src_mode == 3) {
			p += 8;
		} else {
			return false;
		}
	}
	header.payload = p;
	header.end = end;
	return p <= end;
}

/// The entry of `short_addr`, made room for when `create`; nullptr without one. Under the lock.
IRAM_ATTR Entry* entry(uint16_t short_addr, bool create) {
	Entry* oldest = &s_entries[0];
	for (Entry& e : s_entries) {
		if (e.short_addr == short_addr && e.last_heard_us != 0) {
			return &e;
		}
		if (e.last_heard_us < oldest->last_heard_us) {
			oldest = &e;
		}
	}
	if (!create) {
		return nullptr;
	}
	*oldest = Entry{};
	oldest->short_addr = short_addr;
	return oldest;
}

IRAM_ATTR void on_receive(uint8_t* frame, esp_ieee802154_frame_info_t* info) {
	Header header;
	if (parse(frame, header) && header.type == kFrameCommand && header.src != kNone && header.payload < header.end
		&& header.payload[0] == kDataRequest) {
		int64_t now = esp_timer_get_time();
		portENTER_CRITICAL_ISR(&s_lock);
		Entry* e = entry(header.src, true);
		if (e->last_poll_us != 0) {
			uint32_t gap = static_cast<uint32_t>((now - e->last_poll_us) / 1000);
			for (size_t i = ZigbeePolls::kGaps - 1; i > 0; --i) {
				e->gaps_ms[i] = e->gaps_ms[i - 1];
			}
			e->gaps_ms[0] = gap;
			if (gap > e->longest_gap_ms) {
				e->longest_gap_ms = gap;
			}
		}
		e->last_poll_us = now;
		e->last_heard_us = now;
		++e->polls;
		if (info != nullptr && info->pending) {
			++e->pending;
		}
		portEXIT_CRITICAL_ISR(&s_lock);
	}
	esp_ieee802154_receive_done(frame, info);
}

IRAM_ATTR void count_sent(const uint8_t* frame, esp_ieee802154_tx_error_t error) {
	Header header;
	uint16_t dst = parse(frame, header) ? header.dst : kNone;
	portENTER_CRITICAL_ISR(&s_lock);
	Entry* e = dst != kNone ? entry(dst, false) : nullptr;
	++s_totals.sent;
	if (e != nullptr) {
		++e->sent;
	}
	switch (error) {
		case ESP_IEEE802154_TX_ERR_NONE:
			break;
		case ESP_IEEE802154_TX_ERR_NO_ACK:
			++s_totals.no_ack;
			if (e != nullptr) {
				++e->no_ack;
			}
			break;
		case ESP_IEEE802154_TX_ERR_COEXIST:
			++s_totals.coexist;
			if (e != nullptr) {
				++e->coexist;
			}
			break;
		default:
			++s_totals.failed;
			if (e != nullptr) {
				++e->failed;
			}
			break;
	}
	portEXIT_CRITICAL_ISR(&s_lock);
}

IRAM_ATTR void on_sent(const uint8_t* frame, const uint8_t* ack, esp_ieee802154_frame_info_t* info) {
	count_sent(frame, ESP_IEEE802154_TX_ERR_NONE);
	esp_ieee802154_transmit_done(frame, ack, info);
}

IRAM_ATTR void on_failed(const uint8_t* frame, esp_ieee802154_tx_error_t error) {
	count_sent(frame, error);
	esp_ieee802154_transmit_failed(frame, error);
}

} // namespace

bool radio_tap_install() {
	esp_ieee802154_event_cb_list_t callbacks = {};
	callbacks.rx_done_cb = &on_receive;
	callbacks.tx_done_cb = &on_sent;
	callbacks.tx_failed_cb = &on_failed;
	s_installed = esp_ieee802154_event_callback_list_register(callbacks) == ESP_OK;
	return s_installed;
}

bool radio_tap_read(uint16_t short_addr, bool reset, ZigbeePolls& out) {
	if (!s_installed) {
		return false;
	}
	out = ZigbeePolls{};
	int64_t now = esp_timer_get_time();
	portENTER_CRITICAL(&s_lock);
	Entry* e = entry(short_addr, false);
	if (e != nullptr) {
		out.polls = e->polls;
		out.pending = e->pending;
		if (e->last_poll_us != 0) {
			out.since_poll_ms = static_cast<uint32_t>((now - e->last_poll_us) / 1000);
		}
		out.longest_gap_ms = e->longest_gap_ms;
		for (size_t i = 0; i < ZigbeePolls::kGaps; ++i) {
			out.gaps_ms[i] = e->gaps_ms[i];
		}
		out.sent = e->sent;
		out.no_ack = e->no_ack;
		out.coexist = e->coexist;
		out.failed = e->failed;
		if (reset) {
			// The time of the last poll stays: the next gap is measured from it.
			int64_t last_poll = e->last_poll_us;
			int64_t last_heard = e->last_heard_us;
			*e = Entry{};
			e->short_addr = short_addr;
			e->last_poll_us = last_poll;
			e->last_heard_us = last_heard;
		}
	}
	out.all_sent = s_totals.sent;
	out.all_no_ack = s_totals.no_ack;
	out.all_coexist = s_totals.coexist;
	out.all_failed = s_totals.failed;
	portEXIT_CRITICAL(&s_lock);
	return true;
}

} // namespace esp
} // namespace hc
