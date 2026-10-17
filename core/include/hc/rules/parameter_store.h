/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_RULES_PARAMETER_STORE_H
#define HC_RULES_PARAMETER_STORE_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/settings.h"
#include "hc/rules/value.h"

namespace hc::rules {

/// The persisted parameter overrides of spec/rules.md 8.4, stored in the platform's Settings.
///
/// Key mapping (persisted format, doc/rules.md): the logical key `<ruleId>/<parameterId>` (up to
/// 57 bytes) does not fit NVS's 15-character keys, so each override is stored under
/// `rp.` + 12 lowercase hex digits of the 64-bit FNV-1a hash of the logical key folded to 48 bits
/// (`rp.1a2b3c4d5e6f`), and the stored string carries the full logical key with the value:
/// `<ruleId>/<parameterId>=<code>:<text>`. Codes: `b` boolean (0/1), `i` integer, `r` real
/// (%.17g), `s` string (hex bytes), `d` duration (ms), `t` time of day (HH:MM:SS/dow), `e`
/// enumeration, `c` color (rrggbbww/valid, hex). At most 57 + 3 + 56 = 116 bytes, within
/// Settings::kMaxStringLength. A read whose stored logical key differs (a hash collision with
/// another, undeclared parameter) counts as no override; the loader refuses an image in which two
/// declared parameters share a key. The mapping depends only on the ids, so it survives any
/// reordering or renumbering of rules and parameters in E-Home.
class ParameterStore {
public:
	/// Longest logical key: a 32-byte rule id, '/', a 24-byte parameter id.
	static constexpr size_t kMaxLogicalKey = 57;

	explicit ParameterStore(Settings& settings) : settings_(settings) {}

	/// The settings key of a logical key (`out` holds at least 16 bytes).
	static void key_for(const char* rule, const char* parameter, char* out);
	/// The 48-bit hash behind key_for().
	static uint64_t hash(const char* rule, const char* parameter);

	/// Stores an override. False (and logged) if the platform refused it.
	bool write(const char* rule, const char* parameter, const Val& value);
	/// Reads the override of `rule/parameter`; false if there is none (or the slot holds another
	/// logical key, or the text does not parse).
	bool read(const char* rule, const char* parameter, Val& out) const;
	/// Removes the override (no-op if absent or the slot holds another logical key).
	void erase(const char* rule, const char* parameter);

	/// Receives the stored overrides.
	class Visitor {
	public:
		/// `logical` is `<ruleId>/<parameterId>`; return false to delete the entry (prune()).
		virtual bool entry(const char* logical, const Val& value) = 0;

	protected:
		~Visitor() = default;
	};

	/// Calls `visitor` for every stored override (order unspecified, each entry twice: a counting
	/// and a collecting pass, so the answer must depend on the entry only); entries for which it
	/// returns false are erased afterwards. Entries under the `rp.` prefix that do not parse are erased too.
	/// Allocates a temporary list of the keys to erase (load time only).
	void prune(Visitor& visitor);

	/// Encodes/decodes the stored text (exposed for the tests).
	static bool encode(const char* rule, const char* parameter, const Val& value, char* out, size_t size);
	static bool decode(const char* text, char* logical, size_t logical_size, Val& out);

private:
	Settings& settings_;
};

} // namespace hc::rules

#endif // HC_RULES_PARAMETER_STORE_H
