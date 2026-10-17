# DPT conformance corpus

`spec/fixtures/dpt/` holds the encodings of KNX datapoint types (DPTs) that every codec of the
project must reproduce: the C++ core here (`core/src/dpt.cpp`, run by
`core/test/dpt_corpus_test.cpp`) and E-Home's `DatapointCodec`. A change of codec behavior lands
with new or changed corpus entries.

## Files

One file per main type, named `dpt-<main, three digits>.json` (`dpt-009.json`,
`dpt-232.json`), UTF-8:

```json
{
	"main": 9,
	"title": "2-octet float (F16): (0.01 * M) * 2^E",
	"entries": [
		{"dpt": "9.001", "bytes": "0C 1A", "value": 21.0},
		{"dpt": "9.001", "value": 21.55, "bytes": "0C 36", "direction": "encode", "comment": "1077.5 rounds up"},
		{"dpt": "9.001", "bytes": "7F FF", "error": "invalid", "comment": "0x7FFF is the invalid value"},
		{"dpt": "9.001", "value": -273.01, "error": "out_of_range"}
	]
}
```

Present: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19, 20, 232, 251.

## Entries

| Member | Meaning |
|---|---|
| `dpt` | The type, `main.sub` with the sub number in at least three digits; every entry belongs to the file's main type. |
| `bytes` | The data octets in hexadecimal, separated by single spaces, as they follow the APCI of a group write. For the embedded types (main 1, 2, 3, value in the low 6 bits of the APCI octet) it is the one octet holding those bits. |
| `value` | The value (below). |
| `error` | The refusal: `wrong_length`, `out_of_range` or `invalid`. |
| `direction` | Optional: `decode` or `encode` restricts a value entry to one direction. |
| `comment` | Optional, for people. |

Four kinds of entries:

- **`bytes` + `value`**: decoding the bytes gives the value, encoding the value gives exactly the
  bytes; both round trips (decode then encode, encode then decode) are the identity.
- **`direction: "decode"`**: only decoding is checked — the bytes are not canonical (reserved bits
  set, data after the terminating NUL of a string).
- **`direction: "encode"`**: only encoding is checked — the value is not exactly representable
  and rounds to these bytes (5.001 50 % is 127.5 and gives 0x80).
- **`bytes` + `error`**: decoding is refused with this status. **`value` + `error`**: encoding is
  refused with this status. A codec must refuse; it must not clamp, replace or truncate.

Statuses: `wrong_length` — the number of octets does not match the type; `out_of_range` — the value
lies outside the type's range (also a field that does not fit at all, like a color level of 256);
`invalid` — the data is no value of the type (a reserved or invalid code, a field out of range).

## Values

| Main types | JSON value |
|---|---|
| 1 | `true` / `false` |
| 2, 3, 18 | `{"control": bool, "value": n}`: 2 control bit and value bit (0/1); 3 control bit (3.007: 1 increase; 3.008: 1 down) and step code 0..7 (0 stops); 18 learn bit and scene number 0..63 |
| 4, 16 | a JSON string (Unicode; the codec maps it to ASCII or ISO 8859-1) |
| 5, 6, 7, 8, 12, 13, 17 | a number: unscaled except 5.001 (percent), 5.003 (degrees), 7.003/7.004/8.003/8.004 (milliseconds), 8.010 (percent) |
| 9, 14 | a number |
| 10 | `{"dayOfWeek": 0..7, "hour", "minute", "second"}` (day 0: no day, 1 Monday ... 7 Sunday) |
| 11 | `{"year", "month", "day"}` |
| 19 | `{"year", "month", "day", "dayOfWeek", "hour", "minute", "second"}` plus the flags as booleans, `false` when absent: `fault`, `workingDay`, `noWorkingDay`, `noYear`, `noDate`, `noDayOfWeek`, `noTime`, `summerTime`, `clockQuality` (CLQ), `syncReliable` (SRC) |
| 20 | the element number |
| 232 | `{"red", "green", "blue"}`, levels 0..255 |
| 251 | `{"red", "green", "blue", "white"}`, levels 0..255 (255 is 100 %), plus `"valid": ["red", ...]`, the channels whose validity bit is set (all four when absent) |

Real numbers compare with a relative tolerance of 1e-9 (the corpus writes decimal approximations,
e.g. 5.001 0xBA is `72.94117647058823`); bytes compare exactly.

## Rules the corpus pins

- 5.001 and 5.003 scale to 0..255 and round a half up: `round(v * 255 / 100)`, decode
  `b * 100 / 255`.
- 9.x: `value = 0.01 * M * 2^E`, M a 12-bit two's complement mantissa (sign bit 7 of the first
  octet), E 0..15. Encoding takes the smallest E for which the hundredfold value, halved E times,
  lies within -2048..2047 before rounding, then rounds a half up. 0x7FFF is invalid. The range is
  -671088.64..670433.28, narrowed by the subtype (9.001 from -273, 9.004..9.008 and 9.028..9.030
  from 0, 9.027 from -459.6).
- 10: hour 0..23; 11: year 0..99 with 90..99 as 1990..1999 and 0..89 as 2000..2089, octets day,
  month, year; 19: year 1900..2155, hour 24 only as 24:00:00, fields flagged absent are not checked.
- 16.000 is ASCII, 16.001 ISO 8859-1: 14 octets, NUL padded; decoding stops at the first NUL.
- 20.102 HVAC mode 0..4, 20.105 HVAC control mode 0..17 and 20.
- Reserved bits are ignored when decoding and written as 0.

## Known differences in E-Home

E-Home's `DatapointCodec` (Calimero 2.6) was run against this corpus; these entries fail there. The
corpus states the intended behavior; the E-Home side needs the fix:

- **16.000 is decoded and encoded as ISO 8859-1.** `DatapointCodec.Dpt` uses sub number 0 for "main
  type only", so "16.000" selects the translator of 16.001.
- **Silent replacement and clamping.** Characters outside the character set become `?` (16.000,
  16.001); 13.x clamps values beyond 32 bits to the limits; 5.x (unscaled) and 13.x truncate
  fractions. The corpus refuses all of these.
- **9.x: 0x7FFF decodes as 670760.96** instead of being refused as invalid.
- **9.x: a negative value that rounds to the mantissa 0** (-0.004) is encoded as 0x8000, which is
  -20.48; the sign must follow the rounded mantissa (0x0000).
- **14.x: NaN and infinities decode** as values.
- **5.006: the tariff 255 decodes** (the type's range is 0..254).
- **19.x: absent fields are range-checked when encoding** (month 0 under `noDate`), although
  decoding accepts them; the corpus encodes what it decodes. Calimero's API also cannot set the
  `syncReliable` bit.
- **4.x** has no translator (shown as raw bytes).
- **The main type alone** ("5") uses the first subtype (5.001, scaled); the corpus has no entry for
  it; the core decodes it unscaled.
- **251.600** levels are percentages in Calimero; the bytes agree, the corpus writes levels 0..255.
