/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_VALIDATOR_H
#define HC_IMAGE_VALIDATOR_H

#include <cstddef>
#include <cstdint>

#include "hc/image/binary.h"
#include "hc/image/image.h"

namespace hc::image {

/// The device's checks of a binary image (spec/runtime-image.md section 10, issue #87): structure
/// only, every refusal with its path into the image and its reason (section 4). The checks of 10.8
/// that need a component's catalogue (DPTs, the Zigbee function table, presets, block types, the
/// history caps, the settings' validators, the persistence keys) are the Loader's.

/// 10.1 on receipt: the header in the body's first `available` bytes (at least min(32, length))
/// against the announced body `length`. Refusals: image.format, image.version.
bool check_header(const uint8_t* head, size_t available, size_t length, Report& report);

/// 10.1 at boot on the slot's bytes: `erased` (the first four bytes FF: no image, no refusal) or the
/// header against the slot's size.
bool check_slot_header(const uint8_t* slot, size_t slot_size, bool& erased, Report& report);

/// 10.2: the CRC-32 over [0, signedEnd) (image.damaged).
bool check_crc(const uint8_t* bytes, Report& report);

/// 10.3 to 10.7 and the counted limits of 10.8 on an image whose header passed (image.invalid).
/// 10.3 and 10.4 stop at their first refusal; 10.5 to 10.8 go on with the next record.
bool check_structure(const uint8_t* bytes, Report& report);

/// " (the image's format 1.1 is newer than this firmware reads, up to 1.0: update the firmware)"
/// when the image's minor version is newer than this firmware's, "" otherwise.
const char* newer_hint(const uint8_t* bytes);

} // namespace hc::image

#endif // HC_IMAGE_VALIDATOR_H
