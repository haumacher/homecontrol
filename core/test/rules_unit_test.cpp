/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Unit tests of the rule interpreter beyond the conformance corpus, on rules compiled into the binary
// image (issue #87): the canonical printer (round trip through a text parser for every corpus
// expression), the compiled form of spec/runtime-image.md 9.13 and 9.14, the device's checks and
// limits of the rules (spec/runtime-image.md 10.6, 10.8), the parameter key mapping, the memory
// accounting and the loop guard. The load checks of the JSON rules (spec/rules.md 10.1) are E-Home's.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <new>
#include <regex>
#include <string>
#include <vector>

#include "allocation_counter.h"
#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/history.h"
#include "hc/image/binary.h"
#include "hc/image/loader.h"
#include "hc/image/rules_clock.h"
#include "hc/image/validator.h"
#include "hc/json.h"
#include "hc/knx/adapter.h"
#include "hc/rules/engine.h"
#include "hc/rules/parameter_store.h"
#include "hc/rules/printer.h"
#include "hc/rules/rule_set.h"
#include "image_compiler.h"
#include "image_support.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"

using hc::rules::Engine;
using hc::rules::LoadResult;
using hc::rules::RuleSet;
using hc::rules::Type;
using hc::rules::Val;
using hc_test::Json;

namespace {

constexpr uint16_t kNone = hc::rules::kNone;

// ------------------------------------------------------------------------------------------
// Helpers

/// The datapoints the rules of the printer and limit tests use.
const std::string kBenchDatapoints = hc_test::datapoint_source("b", "boolean") + "," +
	hc_test::datapoint_source("u", "unsigned") + "," + hc_test::datapoint_source("r", "real") + "," +
	hc_test::datapoint_source("s", "string") + "," + hc_test::datapoint_source("t", "timeOfDay") + "," +
	hc_test::datapoint_source("ro", "boolean", false) + "," + hc_test::datapoint_source("e", "enumeration") + "," +
	hc_test::datapoint_source("c", "controlled");

/// The source image of the bench datapoints and `rules` (the items of the member).
std::string bench_image(const std::string& rules, const std::string& extra = "") {
	return hc_test::rules_source(kBenchDatapoints, rules, extra);
}

/// Rules compiled over the bench datapoints, the model of the image's datapoints and the bound set.
struct Bench {
	hc_test::FakeClock clock;
	hc::DatapointModel model{clock};
	hc_test::RuleImage image;
	RuleSet set;

	explicit Bench(const std::string& rules, const std::string& extra = "", const char* language = "en") {
		REQUIRE(image.compile(bench_image(rules, extra)));
		image.load_model(model);
		set = image.bind(language);
	}
};

/// The first refusal of a report as "<code> <path>: <reason>".
std::string first_refusal(const hc::image::Report& report) {
	if (report.count == 0) {
		return std::string(hc::image::report_kind_code(report.kind));
	}
	return std::string(hc::image::report_kind_code(report.kind)) + " " + report.refusals[0].path + ": " +
		report.refusals[0].reason;
}

/// What the device says of the binary image `bytes`: "ok", or its first refusal as "<code> <path>:
/// <reason>". The checks of a receipt (spec/runtime-image.md 10.1 to 10.7, the counted limits of
/// 10.8), then those of the loader that need this firmware (10.8: the persistence keys, ...).
std::string device_verdict(const std::vector<uint8_t>& bytes) {
	hc::image::Report report;
	if (!hc::image::check_header(bytes.data(), bytes.size(), bytes.size(), report) ||
		!hc::image::check_crc(bytes.data(), report) || !hc::image::check_structure(bytes.data(), report)) {
		return first_refusal(report);
	}
	hc::image::bin::View view;
	view.open(bytes.data());
	hc_test::FakeClock clock;
	hc::image::Loader loader(clock, hc::HistoryLimits().cap_bytes);
	hc::image::Plan plan;
	hc_test::ScopedCapture capture(hc::LogLevel::error);
	if (!loader.build(view, plan, report)) {
		return first_refusal(report);
	}
	return "ok";
}

/// The device's verdict on the bench datapoints with `rules`, compiled.
std::string verdict(const std::string& rules, const std::string& extra = "") {
	return device_verdict(hc_test::compiled(bench_image(rules, extra)));
}

/// Whether a verdict is the refusal of a limit (spec/runtime-image.md 10.6, 10.8).
bool limit_refused(const std::string& v) { return v.find("image.invalid") == 0 && v.find("limitExceeded") != std::string::npos; }

/// Rewrites the records of section `id` of the compiled `bytes` with `patch` (record index, its
/// bytes) and writes the header's CRC anew (spec 9.2): an image E-Home's compiler would not write.
void patch_records(std::vector<uint8_t>& bytes, uint16_t id, const std::function<void(size_t, uint8_t*)>& patch) {
	hc::image::bin::View view;
	view.open(bytes.data());
	const hc::image::bin::Table& table = view.table(id);
	size_t offset = static_cast<size_t>(table.base - bytes.data());
	for (size_t i = 0; i < table.count; ++i) {
		patch(i, bytes.data() + offset + i * table.stride);
	}
	const auto& header = *reinterpret_cast<const hc::image::bin::Header*>(bytes.data());
	size_t end = (header.flags & hc::image::bin::kFlagSigned) != 0 ? header.length - 64 : header.length;
	uint32_t crc = hc::image::bin::image_crc(bytes.data(), end);
	std::memcpy(bytes.data() + 12, &crc, sizeof(crc));
}

/// One rule around `reaction` (and optional extra members of the rule).
std::string rule_json(const std::string& reaction, const std::string& extra = "", const std::string& id = "x") {
	return R"({"id": ")" + id + R"(", "name": "X", )" + (extra.empty() ? "" : extra + ", ") + R"("reactions": [)" +
		reaction + "]}";
}

std::string lit_int(long long v) { return R"({"kind": "literal", "type": "integer", "value": )" + std::to_string(v) + "}"; }
std::string lit_bool(bool v) { return std::string(R"({"kind": "literal", "type": "boolean", "value": )") + (v ? "true" : "false") + "}"; }
std::string dp(const std::string& name) { return R"({"kind": "datapoint", "name": ")" + name + R"("})"; }
std::string bin(const std::string& op, const std::string& l, const std::string& r) {
	return R"({"kind": "binary", "op": ")" + op + R"(", "left": )" + l + R"(, "right": )" + r + "}";
}
std::string un(const std::string& op, const std::string& x) {
	return R"({"kind": "unary", "op": ")" + op + R"(", "operand": )" + x + "}";
}

/// A reaction on `b` writing `value` to `target`.
std::string write_reaction(const std::string& target, const std::string& value, const std::string& condition = "") {
	return R"({"triggers": [{"kind": "datapoint", "datapoint": "b"}], )" +
		(condition.empty() ? "" : R"("condition": )" + condition + ", ") +
		R"("actions": [{"kind": "write", "datapoint": ")" + target + R"(", "value": )" + value + "}]}";
}

/// The rule corpus files (spec/fixtures/rules/NN-*.json), sorted.
std::vector<std::string> rule_cases() {
	std::vector<std::string> files;
	for (const auto& entry : std::filesystem::directory_iterator(HC_SPEC_DIR "/fixtures/rules")) {
		std::string name = entry.path().filename().string();
		if (entry.path().extension() == ".json" && name.size() > 3 && std::isdigit(static_cast<unsigned char>(name[0]))) {
			files.push_back(entry.path().string());
		}
	}
	std::sort(files.begin(), files.end());
	return files;
}

// ------------------------------------------------------------------------------------------
// The text parser of the round trip (spec/rules-grammar.ebnf): produces a structural S-expression
// that is compared with the one taken from the compiled nodes.

class TextParser {
public:
	explicit TextParser(const std::string& text) : text_(text) {}

	bool parse(std::string& out, std::string& error) {
		next();
		out = or_expr();
		if (error_.empty() && kind_ != Tok::end) {
			fail("trailing input");
		}
		error = error_;
		return error_.empty();
	}

private:
	enum class Tok { end, literal, number, datapoint, parameter, ident, punct };

	void fail(const std::string& what) {
		if (error_.empty()) {
			error_ = what + " at " + std::to_string(pos_) + " in: " + text_;
		}
	}

	bool match_at(const std::regex& re, size_t& length) {
		std::smatch m;
		std::string rest = text_.substr(pos_);
		if (std::regex_search(rest, m, re, std::regex_constants::match_continuous)) {
			length = static_cast<size_t>(m.length(0));
			return true;
		}
		return false;
	}

	void next() {
		while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
			++pos_;
		}
		start_ = pos_;
		if (pos_ >= text_.size()) {
			kind_ = Tok::end;
			tok_.clear();
			return;
		}
		char c = text_[pos_];
		size_t length = 0;
		if (std::isdigit(static_cast<unsigned char>(c))) {
			static const std::regex kDateTime(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(:\d{2})?)");
			static const std::regex kDate(R"(\d{4}-\d{2}-\d{2})");
			static const std::regex kTime(R"(\d\d?:\d\d(:\d\d)?)");
			static const std::regex kDuration(R"((\d+(d|h|min|ms|s))+)");
			static const std::regex kReal(R"(\d+\.\d+([eE][+-]?\d+)?)");
			static const std::regex kInteger(R"(\d+)");
			const std::regex* order[] = {&kDateTime, &kDate, &kTime, &kDuration, &kReal, &kInteger};
			size_t best = 0;
			int which = -1;
			for (int i = 0; i < 6; ++i) {
				size_t n = 0;
				if (match_at(*order[i], n) && n > best) {
					best = n;
					which = i;
				}
			}
			tok_ = text_.substr(pos_, best);
			pos_ += best;
			kind_ = which >= 3 ? Tok::number : Tok::literal; // duration, real, integer fold a minus
			return;
		}
		if (c == '"') {
			string_token();
			kind_ = Tok::literal;
			return;
		}
		if (c == 'x' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '"') {
			static const std::regex kRaw(R"(x"([0-9A-Fa-f]{2})*")");
			if (match_at(kRaw, length)) {
				tok_ = text_.substr(pos_, length);
				pos_ += length;
				kind_ = Tok::literal;
				return;
			}
		}
		if (c == '#') {
			static const std::regex kColor(R"(#[0-9A-Fa-f]{6}([0-9A-Fa-f]{2})?)");
			if (match_at(kColor, length)) {
				tok_ = text_.substr(pos_, length);
				pos_ += length;
				kind_ = Tok::literal;
				return;
			}
		}
		if (c == '@') {
			++pos_;
			if (pos_ < text_.size() && text_[pos_] == '"') {
				string_token();
				tok_ = unquote(tok_);
			} else {
				static const std::regex kName(R"([A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)*)");
				if (!match_at(kName, length)) {
					fail("datapoint name");
				}
				tok_ = text_.substr(pos_, length);
				pos_ += length;
			}
			kind_ = Tok::datapoint;
			return;
		}
		if (c == '$' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '"') {
			// A parameter id that is not an identifier: $"max-helligkeit" (spec/rules.md 4.3).
			++pos_;
			string_token();
			tok_ = unquote(tok_);
			kind_ = Tok::parameter;
			return;
		}
		if (c == '$' || std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
			size_t s = c == '$' ? pos_ + 1 : pos_;
			size_t e = s;
			while (e < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[e])) || text_[e] == '_')) {
				++e;
			}
			tok_ = text_.substr(s, e - s);
			pos_ = e;
			kind_ = c == '$' ? Tok::parameter : Tok::ident;
			if (kind_ == Tok::ident && (tok_ == "true" || tok_ == "false")) {
				kind_ = Tok::literal;
			}
			if (kind_ == Tok::ident) {
				// A time of day with a day of the week: "mon 07:30" (grammar "time literal").
				static const std::regex kDayTime(R"((mon|tue|wed|thu|fri|sat|sun) [0-9]{1,2}:[0-9]{2}(:[0-9]{2})?)");
				size_t length = 0;
				pos_ = s;
				if (match_at(kDayTime, length)) {
					tok_ = text_.substr(pos_, length);
					pos_ += length;
					kind_ = Tok::literal;
				} else {
					pos_ = e;
				}
			}
			return;
		}
		static const char* const kPuncts[] = {"==", "!=", "<=", ">=", "<", ">", "+", "-", "*", "/", "%", "(", ")", ","};
		for (const char* p : kPuncts) {
			if (text_.compare(pos_, std::strlen(p), p) == 0) {
				tok_ = p;
				pos_ += std::strlen(p);
				kind_ = Tok::punct;
				return;
			}
		}
		fail("unexpected character");
		kind_ = Tok::end;
	}

	void string_token() {
		size_t e = pos_ + 1;
		while (e < text_.size() && text_[e] != '"') {
			e += text_[e] == '\\' ? 2 : 1;
		}
		tok_ = text_.substr(pos_, e + 1 - pos_);
		pos_ = e + 1;
	}

	static std::string unquote(const std::string& quoted) {
		std::string out;
		for (size_t i = 1; i + 1 < quoted.size(); ++i) {
			char c = quoted[i];
			if (c == '\\') {
				char e = quoted[++i];
				out += e == 'n' ? '\n' : (e == 't' ? '\t' : e);
			} else {
				out += c;
			}
		}
		return out;
	}

	bool is(const char* punct) const { return kind_ == Tok::punct && tok_ == punct; }
	bool is_word(const char* word) const { return kind_ == Tok::ident && tok_ == word; }

	std::string or_expr() {
		std::string left = and_expr();
		while (is_word("or")) {
			next();
			left = "(or " + left + " " + and_expr() + ")";
		}
		return left;
	}

	std::string and_expr() {
		std::string left = not_expr();
		while (is_word("and")) {
			next();
			left = "(and " + left + " " + not_expr() + ")";
		}
		return left;
	}

	std::string not_expr() {
		if (is_word("not")) {
			next();
			return "(not " + not_expr() + ")";
		}
		std::string left = additive();
		for (const char* op : {"==", "!=", "<", "<=", ">", ">="}) {
			if (is(op)) {
				next();
				return "(" + std::string(op) + " " + left + " " + additive() + ")";
			}
		}
		return left;
	}

	std::string additive() {
		std::string left = multiplicative();
		while (is("+") || is("-")) {
			std::string op = tok_;
			next();
			left = "(" + op + " " + left + " " + multiplicative() + ")";
		}
		return left;
	}

	std::string multiplicative() {
		std::string left = unary();
		while (is("*") || is("/") || is("%")) {
			std::string op = tok_;
			next();
			left = "(" + op + " " + left + " " + unary() + ")";
		}
		return left;
	}

	std::string unary() {
		if (is("-")) {
			next();
			if (kind_ == Tok::number && start_ > 0 && text_[start_ - 1] == '-') {
				std::string folded = "L:-" + tok_; // a minus directly before the literal folds into it
				next();
				return folded;
			}
			return "(neg " + unary() + ")";
		}
		return primary();
	}

	std::string primary() {
		std::string result;
		switch (kind_) {
			case Tok::literal:
			case Tok::number: result = "L:" + tok_; next(); return result;
			case Tok::datapoint: result = "@" + tok_; next(); return result;
			case Tok::parameter: result = "$" + tok_; next(); return result;
			case Tok::ident: {
				std::string name = tok_;
				next();
				if (!is("(")) {
					fail("call expected");
					return "?";
				}
				next();
				result = "(" + name;
				if (name == "running" || name == "remaining") {
					result += " T:" + tok_;
					next();
				} else {
					while (!is(")") && kind_ != Tok::end) {
						result += " " + or_expr();
						if (is(",")) {
							next();
						}
					}
				}
				if (!is(")")) {
					fail("')' expected");
				}
				next();
				return result + ")";
			}
			case Tok::punct:
				if (is("(")) {
					next();
					result = or_expr();
					if (!is(")")) {
						fail("')' expected");
					}
					next();
					return result;
				}
				break;
			case Tok::end: break;
		}
		fail("primary expected");
		next();
		return "?";
	}

	const std::string& text_;
	size_t pos_ = 0;
	size_t start_ = 0;
	Tok kind_ = Tok::end;
	std::string tok_;
	std::string error_;
};


/// The S-expression of a compiled expression.
std::string tree_of(const RuleSet& set, const hc::DatapointModel& model, size_t rule, uint16_t index) {
	const hc::rules::Node& n = set.node(index);
	const hc::rules::Rule& r = set.rule(rule);
	char text[128];
	switch (n.kind) {
		case hc::rules::NodeKind::literal: {
			Val v;
			switch (n.type) {
				case Type::boolean: v = Val::of_bool(n.lit.b); break;
				case Type::integer: v = Val::of_int(n.lit.i); break;
				case Type::duration: v = Val::of_duration(n.lit.i); break;
				case Type::real: v = Val::of_real(n.lit.r); break;
				case Type::enumeration: v = Val::of_enum(n.lit.e); break;
				case Type::time_of_day: v = Val::of_time(n.lit.tod); break;
				case Type::date: v = Val::of_date(n.lit.date); break;
				case Type::controlled: v = Val::of_controlled(n.lit.ctl.control, n.lit.ctl.value); break;
				case Type::color: v = Val::of_color(n.lit.color); break;
				default: v = set.constant(n.a); break;
			}
			hc::rules::literal_text(v, text, sizeof(text));
			return std::string("L:") + text;
		}
		case hc::rules::NodeKind::datapoint: return std::string("@") + model.get(n.a)->name();
		case hc::rules::NodeKind::parameter: return std::string("$") + set.str(set.parameter(r.first_parameter + n.a).id);
		case hc::rules::NodeKind::timer: return std::string("T:") + set.str(set.timer(r.first_timer + n.a).id);
		case hc::rules::NodeKind::unary:
			return std::string(static_cast<hc::rules::Op>(n.op) == hc::rules::Op::neg ? "(neg " : "(not ") +
				tree_of(set, model, rule, n.a) + ")";
		case hc::rules::NodeKind::binary:
			return std::string("(") + hc::rules::operator_text(static_cast<hc::rules::Op>(n.op)) + " " +
				tree_of(set, model, rule, n.a) + " " + tree_of(set, model, rule, static_cast<uint16_t>(n.a + 1)) + ")";
		case hc::rules::NodeKind::call: {
			std::string s = std::string("(") + hc::rules::function_name(static_cast<hc::rules::Fn>(n.op));
			for (uint8_t i = 0; i < n.argc; ++i) {
				s += " " + tree_of(set, model, rule, static_cast<uint16_t>(n.a + i));
			}
			return s + ")";
		}
	}
	return "?";
}

/// Every expression root of a rule set, with its rule.
std::vector<std::pair<size_t, uint16_t>> expression_roots(const RuleSet& set) {
	std::vector<std::pair<size_t, uint16_t>> roots;
	auto actions = [&](size_t rule, const hc::rules::ActionList& list) {
		for (size_t i = 0; i < list.count; ++i) {
			const hc::rules::Action& a = set.action(list.first + i);
			if (a.kind == hc::rules::ActionKind::log) {
				for (size_t k = 0; k < a.count; ++k) {
					if (set.log_part(a.target + k).expr != kNone) {
						roots.emplace_back(rule, set.log_part(a.target + k).expr);
					}
				}
			} else if (a.expr != kNone) {
				roots.emplace_back(rule, a.expr);
			}
		}
	};
	for (size_t r = 0; r < set.rule_count(); ++r) {
		const hc::rules::Rule& rule = set.rule(r);
		for (size_t t = 0; t < rule.timer_count; ++t) {
			roots.emplace_back(r, set.timer(rule.first_timer + t).duration);
		}
		for (size_t s = 0; s < rule.state_count; ++s) {
			actions(r, set.state(rule.first_state + s).entry);
			actions(r, set.state(rule.first_state + s).exit);
		}
		for (size_t i = 0; i < rule.reaction_count; ++i) {
			const hc::rules::Reaction& reaction = set.reaction(rule.first_reaction + i);
			for (size_t k = 0; k < reaction.trigger_count; ++k) {
				const hc::rules::Trigger& t = set.trigger(reaction.first_trigger + k);
				if (t.kind == hc::rules::TriggerKind::match) {
					roots.emplace_back(r, t.ref);
				} else if (t.kind == hc::rules::TriggerKind::silence) {
					roots.emplace_back(r, t.ref2);
				}
			}
			if (reaction.condition != kNone) {
				roots.emplace_back(r, reaction.condition);
			}
			actions(r, reaction.actions);
			actions(r, reaction.else_actions);
		}
	}
	return roots;
}

std::string print(const RuleSet& set, const hc::DatapointModel& model, size_t rule, uint16_t root) {
	char text[512];
	size_t length = hc::rules::print_expression(set, model, rule, root, text, sizeof(text));
	REQUIRE(length < sizeof(text));
	return text;
}

/// A reaction on `b` with `condition` and no actions.
std::string condition_reaction(const std::string& condition) {
	return R"({"triggers": [{"kind": "datapoint", "datapoint": "b"}], "condition": )" + condition + R"(, "actions": []})";
}

/// Compiles one rule with the given condition (over the bench datapoints) and prints the condition.
std::string print_condition(const std::string& condition, const std::string& extra = "") {
	Bench bench(rule_json(condition_reaction(condition), extra));
	return print(bench.set, bench.model, 0, bench.set.reaction(0).condition);
}

} // namespace

// ------------------------------------------------------------------------------------------
// Printer

TEST_CASE("RulesPrinter round trip of every corpus expression") {
	size_t checked = 0;
	for (const std::string& path : rule_cases()) {
		std::string text, error;
		REQUIRE(hc_test::read_text(path, text));
		Json json;
		REQUIRE(hc_test::parse_json(text, json, error));
		if (json["expected"].has("loadRefused")) {
			continue;
		}
		// The case's own image and the image of every `image` step (spec/runtime-image.md 13.1).
		std::vector<int> steps{-1};
		for (size_t k = 0; k < json["timeline"].items.size(); ++k) {
			if (json["timeline"].items[k].has("image")) {
				steps.push_back(static_cast<int>(k));
			}
		}
		for (int step : steps) {
			CAPTURE(path);
			CAPTURE(step);
			hc_test::CompiledImage image;
			std::string compile_error;
			REQUIRE_MESSAGE(hc::test::compile_case(text.data(), text.size(), step, image.bytes, &compile_error),
				compile_error);
			REQUIRE(image.open());
			hc_test::FakeClock clock;
			hc::DatapointModel model(clock);
			std::vector<hc::DatapointSpec> specs = image.specs();
			REQUIRE(model.load(specs.data(), specs.size()) == hc::DatapointStatus::ok);
			RuleSet set;
			LoadResult result = image.bind(set);
			REQUIRE_MESSAGE(result.ok(), path << ": " << std::string(result.text));
			for (const auto& root : expression_roots(set)) {
				std::string printed = print(set, model, root.first, root.second);
				std::string parsed, parse_error;
				TextParser parser(printed);
				CHECK_MESSAGE(parser.parse(parsed, parse_error), parse_error);
				std::string expected = tree_of(set, model, root.first, root.second);
				CHECK_MESSAGE(parsed == expected, path << "\n  text:     " << printed << "\n  parsed:   " << parsed
					<< "\n  compiled: " << expected);
				++checked;
			}
		}
	}
	MESSAGE("corpus expressions printed and parsed back: " << checked);
	CHECK(checked > 200);
}

TEST_CASE("RulesPrinter spec examples and parentheses") {
	std::string lux_rule = R"("parameters": [{"id": "darkBelow", "label": "Dark", "type": "real", "default": 50.0}])";
	CHECK(print_condition(
			  bin("and", dp("b"), bin("or", dp("b"), bin("<", dp("r"), R"({"kind": "parameter", "id": "darkBelow"})"))),
			  lux_rule) == "@b and (@b or @r < $darkBelow)");
	CHECK(print_condition(
			  bin(">", R"({"kind": "call", "function": "remaining", "args": [{"kind": "timer", "id": "off"}]})",
				  R"({"kind": "literal", "type": "duration", "value": 90000})"),
			  R"("timers": [{"id": "off", "duration": {"kind": "literal", "type": "duration", "value": 1000}}])") ==
		"remaining(off) > 1min30s");
	// a - (b - c), a - b - c, a or (b or c)
	CHECK(print_condition(bin("==", bin("-", lit_int(1), bin("-", lit_int(2), lit_int(3))), lit_int(0))) ==
		"1 - (2 - 3) == 0");
	CHECK(print_condition(bin("==", bin("-", bin("-", lit_int(1), lit_int(2)), lit_int(3)), lit_int(0))) ==
		"1 - 2 - 3 == 0");
	CHECK(print_condition(bin("or", dp("b"), bin("or", dp("b"), dp("b")))) == "@b or (@b or @b)");
	// a comparison under a comparison; -(-5); -(a + b); not binds looser than comparisons
	CHECK(print_condition(bin("==", bin("<", lit_int(1), lit_int(2)), lit_bool(true))) == "(1 < 2) == true");
	// A minus applied to a number literal is folded into it by the compiler (4.8): -(-5) is the literal 5.
	CHECK(print_condition(bin("==", un("-", lit_int(-5)), lit_int(5))) == "5 == 5");
	CHECK(print_condition(bin("==", un("-", dp("r")), lit_int(5))) == "-@r == 5");
	CHECK(print_condition(bin("==", un("-", un("-", dp("r"))), lit_int(5))) == "-(-@r) == 5");
	CHECK(print_condition(bin("==", un("-", bin("+", lit_int(1), lit_int(2))), lit_int(-3))) == "-(1 + 2) == -3");
	CHECK(print_condition(un("not", bin("==", dp("u"), lit_int(1)))) == "not @u == 1");
	CHECK(print_condition(bin("==", un("not", dp("b")), lit_bool(false))) == "(not @b) == false");
	CHECK(print_condition(bin("==", bin("*", bin("+", lit_int(1), lit_int(2)), lit_int(3)), lit_int(9))) ==
		"(1 + 2) * 3 == 9");
	CHECK(print_condition(bin("==", dp("s"), R"({"kind": "literal", "type": "string", "value": "say \"hi\"\n"})")) ==
		R"(@s == "say \"hi\"\n")");
}

TEST_CASE("RulesPrinter normalized trees (spec 4.9)") {
	// Compiles a condition; returns the printed text and the compiled right operand of its root.
	auto load = [&](const std::string& condition, hc::rules::Node& right) {
		Bench bench(rule_json(condition_reaction(condition)));
		uint16_t root = bench.set.reaction(0).condition;
		right = bench.set.node(static_cast<uint16_t>(bench.set.node(root).a + 1));
		return print(bench.set, bench.model, 0, root);
	};
	hc::rules::Node right;
	// 1. A unary minus on a number literal is a negative literal, an odd chain negated.
	CHECK(load(bin("==", dp("r"), un("-", R"({"kind": "literal", "type": "real", "value": 1.5})")), right) == "@r == -1.5");
	CHECK(right.kind == hc::rules::NodeKind::literal);
	CHECK(right.type == Type::real);
	CHECK(load(bin("==", dp("u"), un("-", un("-", un("-", lit_int(5))))), right) == "@u == -5");
	CHECK(right.kind == hc::rules::NodeKind::literal);
	CHECK(right.lit.i == -5);
	CHECK(load(bin("<", dp("t"), bin("+", dp("t"), un("-", R"({"kind": "literal", "type": "duration", "value": 90000})"))), right) ==
		"@t < @t + -1min30s");
	// 2. An integer literal compared with an enumeration is an enumeration literal; its text stays
	// the number. An enumeration literal compared with an integer is an integer literal.
	CHECK(load(bin("==", dp("e"), lit_int(3)), right) == "@e == 3");
	CHECK(right.type == Type::enumeration);
	CHECK(right.lit.e == 3u);
	CHECK(load(bin("!=", dp("u"), R"({"kind": "literal", "type": "enumeration", "value": 3})"), right) == "@u != 3");
	CHECK(right.type == Type::integer);
	CHECK(load(bin("==", dp("e"), R"({"kind": "call", "function": "coalesce", "args": [{"kind": "datapoint", "name": "e"}, )" +
		lit_int(2) + "]}"), right) == "@e == coalesce(@e, 2)");
	CHECK(right.type == Type::enumeration);
	// 3. A controlled literal is the call controlled(b, n).
	CHECK(load(bin("==", dp("c"), R"({"kind": "literal", "type": "controlled", "value": {"control": true, "value": 3}})"), right) ==
		"@c == controlled(true, 3)");
	CHECK(right.kind == hc::rules::NodeKind::call);
	CHECK(right.type == Type::controlled);
	// 4. A time of day with a day of the week: "mon 07:30".
	CHECK(load(bin("==", dp("t"), R"({"kind": "literal", "type": "timeOfDay", "value": {"hour": 7, "minute": 30, "second": 0, "dayOfWeek": 1}})"), right) ==
		"@t == mon 07:30");
	CHECK(load(bin("==", dp("t"), R"({"kind": "literal", "type": "timeOfDay", "value": {"hour": 23, "minute": 5, "second": 9, "dayOfWeek": 7}})"), right) ==
		"@t == sun 23:05:09");
}

TEST_CASE("RulesPrinter bare datapoint names") {
	CHECK(hc::rules::bare_datapoint_name("hall.light"));
	CHECK(hc::rules::bare_datapoint_name("a.1b"));
	CHECK_FALSE(hc::rules::bare_datapoint_name("Living room light 1"));
	CHECK_FALSE(hc::rules::bare_datapoint_name("and.x"));
	CHECK_FALSE(hc::rules::bare_datapoint_name("a..b"));
	CHECK_FALSE(hc::rules::bare_datapoint_name("a-b"));
	CHECK_FALSE(hc::rules::bare_datapoint_name("1a"));
}

TEST_CASE("RulesPrinter parameter references: $id for an identifier, else $\"id\" (issue #81)") {
	CHECK(hc::rules::bare_parameter_id("x"));
	CHECK(hc::rules::bare_parameter_id("max_helligkeit"));
	CHECK(hc::rules::bare_parameter_id("mon"));
	CHECK_FALSE(hc::rules::bare_parameter_id("x-y"));
	CHECK_FALSE(hc::rules::bare_parameter_id("2nd"));
	CHECK_FALSE(hc::rules::bare_parameter_id("a.b"));
	CHECK_FALSE(hc::rules::bare_parameter_id("and"));
	std::string parameters = R"("parameters": [
		{"id": "x", "label": "X", "type": "real", "default": 1.0},
		{"id": "x-y", "label": "X-Y", "type": "real", "default": 2.0},
		{"id": "x_y", "label": "X_Y", "type": "real", "default": 3.0},
		{"id": "2nd", "label": "2nd", "type": "real", "default": 4.0},
		{"id": "-", "label": "Minus", "type": "real", "default": 5.0}])";
	auto param = [](const char* id) { return std::string(R"({"kind": "parameter", "id": ")") + id + R"("})"; };
	CHECK(print_condition(bin("<", dp("r"), param("x")), parameters) == "@r < $x");
	CHECK(print_condition(bin("<", dp("r"), param("x-y")), parameters) == R"(@r < $"x-y")");
	CHECK(print_condition(bin("<", param("x_y"), param("x-y")), parameters) == R"($x_y < $"x-y")");
	CHECK(print_condition(bin("==", un("-", param("2nd")), param("-")), parameters) == R"(-$"2nd" == $"-")");
	// The printed text parses back to the same tree.
	std::string printed = print_condition(bin("<", bin("-", dp("r"), param("x-y")), param("-")), parameters);
	CHECK(printed == R"(@r - $"x-y" < $"-")");
	std::string parsed, error;
	TextParser parser(printed);
	REQUIRE_MESSAGE(parser.parse(parsed, error), error);
	CHECK(parsed == R"((< (- @r $x-y) $-))");
}

// ------------------------------------------------------------------------------------------
// The compiled form (spec/runtime-image.md 9.13, 9.14)

TEST_CASE("RulesImage the compiled form: records, node layout, constants (spec/runtime-image.md 9.13, 9.14)") {
	std::string rule = R"({"id": "x", "name": "X", "enabledAdjustable": false,
		"parameters": [{"id": "level", "label": "Level", "type": "real", "default": 50.0, "min": 0.0, "max": 100.0, "step": 0.5,
			"choices": [{"value": 10.0, "label": "Low"}, {"value": 90.0, "label": "High"}]},
			{"id": "timeout", "label": "Timeout", "type": "duration", "default": 180000, "adjustable": false}],
		"timers": [{"id": "off", "duration": {"kind": "parameter", "id": "timeout"}}],
		"states": [{"id": "idle"}, {"id": "on", "label": "On", "entry": [{"kind": "restartTimer", "timer": "off"}],
			"exit": [{"kind": "log", "level": "warning", "message": ["left on ", {"kind": "datapoint", "name": "r"}, "!"]}]}],
		"initialState": "idle",
		"reactions": [
			{"triggers": [{"kind": "start"}, {"kind": "start", "reasons": ["image"]}], "actions": [], "to": "idle"},
			{"triggers": [{"kind": "silence", "datapoint": "r", "after": {"kind": "literal", "type": "duration", "value": 60000}}],
				"condition": {"kind": "binary", "op": ">", "left": {"kind": "call", "function": "remaining", "args": [{"kind": "timer", "id": "off"}]},
					"right": {"kind": "literal", "type": "duration", "value": 90000}},
				"from": ["on"], "to": "idle", "actions": [{"kind": "write", "datapoint": "b", "value": {"kind": "literal", "type": "boolean", "value": true}, "onlyOnChange": true}]},
			{"triggers": [{"kind": "datapoint", "datapoint": "b", "on": "update", "origin": "command"}, {"kind": "timer", "timer": "off"},
				{"kind": "parameter", "parameter": "level"}],
				"actions": [{"kind": "setParameter", "parameter": "level", "value": {"kind": "literal", "type": "real", "value": 1.0}},
					{"kind": "read", "datapoint": "u"}, {"kind": "cancelTimer", "timer": "off"}]}]})";
	Bench bench(rule);
	const RuleSet& set = bench.set;
	REQUIRE(set.rule_count() == 1);
	const hc::rules::Rule& r = set.rule(0);
	CHECK(std::string(set.str(r.id)) == "x");
	CHECK(std::string(set.label(r.name)) == "X");
	// Parameters: the enable switch first (boolean, its default the rule's `enabled`, adjustable as
	// `enabledAdjustable`, no label), then the declared ones, each mirrored by rule/<id>/<parameter>.
	REQUIRE(r.parameter_count == 3);
	const hc::rules::Parameter& enabled = set.parameter(r.first_parameter);
	CHECK(std::string(set.str(enabled.id)) == "enabled");
	CHECK(enabled.type == Type::boolean);
	CHECK_FALSE(enabled.adjustable);
	CHECK(enabled.label == 0u);
	CHECK(set.constant(enabled.default_value).type == Type::boolean);
	CHECK(set.constant(enabled.default_value).u.b);
	CHECK(enabled.min == kNone);
	CHECK(enabled.step == kNone);
	CHECK(std::string(bench.model.get(enabled.datapoint)->name()) == "rule/x/enabled");
	const hc::rules::Parameter& level = set.parameter(r.first_parameter + 1);
	CHECK(std::string(set.str(level.id)) == "level");
	CHECK(std::string(set.label(level.label)) == "Level");
	CHECK(level.type == Type::real);
	CHECK(level.adjustable);
	CHECK(set.constant(level.default_value).u.r == 50.0);
	CHECK(set.constant(level.min).u.r == 0.0);
	CHECK(set.constant(level.max).u.r == 100.0);
	CHECK(set.constant(level.step).type == Type::real);
	CHECK(set.constant(level.step).u.r == 0.5);
	REQUIRE(level.choice_count == 2);
	CHECK(set.constant(set.choice(level.first_choice + 1).value).u.r == 90.0);
	CHECK(std::string(set.label(set.choice(level.first_choice + 1).label)) == "High");
	CHECK(std::string(bench.model.get(level.datapoint)->name()) == "rule/x/level");
	CHECK(bench.model.get(level.datapoint)->type() == hc::ValueType::real);
	const hc::rules::Parameter& timeout = set.parameter(r.first_parameter + 2);
	CHECK(timeout.type == Type::duration);
	CHECK_FALSE(timeout.adjustable);
	CHECK(bench.model.get(timeout.datapoint)->type() == hc::ValueType::unsigned_int);
	// The constants in the order of 9.18: per parameter its default, min, max, step, then the choices'
	// values (the old JSON loader's order was its own; the image fixes it).
	CHECK(enabled.default_value < level.default_value);
	CHECK(level.default_value < level.min);
	CHECK(level.min < level.max);
	CHECK(level.max < level.step);
	CHECK(level.step < set.choice(level.first_choice).value);
	CHECK(set.choice(level.first_choice).value < set.choice(level.first_choice + 1).value);
	CHECK(set.choice(level.first_choice + 1).value < timeout.default_value);
	// Timers: the duration is an expression root.
	REQUIRE(r.timer_count == 1);
	const hc::rules::Node& duration = set.node(set.timer(r.first_timer).duration);
	CHECK(duration.kind == hc::rules::NodeKind::parameter);
	CHECK(duration.a == 2);
	// States, their action runs and the state datapoint.
	REQUIRE(r.state_count == 2);
	CHECK(r.initial_state == 0);
	CHECK(std::string(set.str(set.state(r.first_state + 1).id)) == "on");
	CHECK(std::string(set.label(set.state(r.first_state + 1).label)) == "On");
	CHECK(set.state(r.first_state).label == 0u);
	CHECK(set.state(r.first_state).entry.count == 0);
	REQUIRE(set.state(r.first_state + 1).entry.count == 1);
	CHECK(set.action(set.state(r.first_state + 1).entry.first).kind == hc::rules::ActionKind::restart_timer);
	CHECK(set.action(set.state(r.first_state + 1).entry.first).expr == kNone);
	REQUIRE(r.state_datapoint != kNone);
	CHECK(std::string(bench.model.get(r.state_datapoint)->name()) == "rule/x/state");
	CHECK(bench.model.get(r.state_datapoint)->type() == hc::ValueType::enumeration);
	CHECK(r.timeline == kNone);
	// A log action: `target` its first part, `count` its parts; a text part a NUL-terminated string.
	const hc::rules::Action& log = set.action(set.state(r.first_state + 1).exit.first);
	CHECK(log.kind == hc::rules::ActionKind::log);
	CHECK(log.flags == static_cast<uint8_t>(hc::rules::LogActionLevel::warning));
	CHECK(log.expr == kNone);
	REQUIRE(log.count == 3);
	CHECK(std::string(set.str(set.log_part(log.target).text)) == "left on ");
	CHECK(set.log_part(log.target).expr == kNone);
	CHECK(set.log_part(log.target + 1).text == 0u);
	CHECK(set.node(set.log_part(log.target + 1).expr).kind == hc::rules::NodeKind::datapoint);
	CHECK(std::string(set.str(set.log_part(log.target + 2).text)) == "!");
	// Reactions and triggers.
	REQUIRE(r.reaction_count == 3);
	const hc::rules::Reaction& start = set.reaction(r.first_reaction);
	REQUIRE(start.trigger_count == 2);
	// The start trigger: the reasons as flags (all three by default), `ref` and `ref2` kNone (the old
	// JSON loader left `ref` 0).
	const hc::rules::Trigger& any_start = set.trigger(start.first_trigger);
	CHECK(any_start.kind == hc::rules::TriggerKind::start);
	CHECK(any_start.flags == (hc::rules::TriggerFlags::kBoot | hc::rules::TriggerFlags::kImage |
		hc::rules::TriggerFlags::kEnable));
	CHECK(any_start.ref == kNone);
	CHECK(any_start.ref2 == kNone);
	CHECK(set.trigger(start.first_trigger + 1).flags == hc::rules::TriggerFlags::kImage);
	CHECK(start.to == 0);
	CHECK(start.condition == kNone);
	CHECK(start.from_mask == 0xFFFF);
	CHECK(start.actions.count == 0);
	CHECK(start.else_actions.count == 0);
	const hc::rules::Reaction& silent = set.reaction(r.first_reaction + 1);
	// The silence trigger: `ref` its datapoint, `ref2` the root of `after` (the old loader's `ref` was
	// the index of its silence slot); the engine derives the slots (silence()).
	const hc::rules::Trigger& silence = set.trigger(silent.first_trigger);
	CHECK(silence.kind == hc::rules::TriggerKind::silence);
	CHECK(std::string(bench.model.get(silence.ref)->name()) == "r");
	CHECK(set.node(silence.ref2).kind == hc::rules::NodeKind::literal);
	CHECK(set.node(silence.ref2).lit.i == 60000);
	REQUIRE(set.silence_count() == 1);
	CHECK(set.silence(0).datapoint == silence.ref);
	CHECK(set.silence(0).after == silence.ref2);
	CHECK(set.silence_of(silent.first_trigger) == 0);
	CHECK(silent.from_mask == 0x0002);
	CHECK(silent.to == 0);
	const hc::rules::Action& write = set.action(silent.actions.first);
	CHECK(write.kind == hc::rules::ActionKind::write);
	CHECK(write.flags == 1);
	CHECK(std::string(bench.model.get(write.target)->name()) == "b");
	// remaining(off) > 1min30s exactly as 9.14 shows it: the root, its children as one block, then the
	// children's subtrees.
	uint16_t n0 = silent.condition;
	const hc::rules::Node& root = set.node(n0);
	CHECK(root.kind == hc::rules::NodeKind::binary);
	CHECK(root.op == static_cast<uint8_t>(hc::rules::Op::gt));
	CHECK(root.type == Type::boolean);
	CHECK(root.argc == 2);
	CHECK(root.a == n0 + 1);
	CHECK(root.size == 4);
	const hc::rules::Node& call = set.node(n0 + 1);
	CHECK(call.kind == hc::rules::NodeKind::call);
	CHECK(call.op == static_cast<uint8_t>(hc::rules::Fn::remaining));
	CHECK(call.type == Type::duration);
	CHECK(call.argc == 1);
	CHECK(call.a == n0 + 3);
	CHECK(call.size == 2);
	const hc::rules::Node& literal = set.node(n0 + 2);
	CHECK(literal.kind == hc::rules::NodeKind::literal);
	CHECK(literal.type == Type::duration);
	CHECK(literal.lit.i == 90000);
	CHECK(literal.a == kNone);
	CHECK(literal.size == 1);
	const hc::rules::Node& timer = set.node(n0 + 3);
	CHECK(timer.kind == hc::rules::NodeKind::timer);
	CHECK(timer.a == 0);
	CHECK(timer.size == 1);
	// A timer node has no expression type: the compiler writes code 0 (the old JSON loader had
	// Type::invalid, which is no code of 9.14).
	CHECK(static_cast<uint8_t>(timer.type) == 0);
	const hc::rules::Reaction& third = set.reaction(r.first_reaction + 2);
	REQUIRE(third.trigger_count == 3);
	const hc::rules::Trigger& command = set.trigger(third.first_trigger);
	CHECK(command.kind == hc::rules::TriggerKind::datapoint);
	CHECK(command.flags == (hc::rules::TriggerFlags::kOnUpdate | hc::rules::TriggerFlags::kOriginCommand));
	CHECK(command.ref2 == kNone);
	CHECK(set.trigger(third.first_trigger + 1).kind == hc::rules::TriggerKind::timer);
	CHECK(set.trigger(third.first_trigger + 1).ref == 0);
	CHECK(set.trigger(third.first_trigger + 2).kind == hc::rules::TriggerKind::parameter);
	CHECK(set.trigger(third.first_trigger + 2).ref == 1);
	REQUIRE(third.actions.count == 3);
	CHECK(set.action(third.actions.first).kind == hc::rules::ActionKind::set_parameter);
	CHECK(set.action(third.actions.first).target == 1);
	CHECK(set.action(third.actions.first + 1).kind == hc::rules::ActionKind::read);
	CHECK(std::string(bench.model.get(set.action(third.actions.first + 1).target)->name()) == "u");
	CHECK(set.action(third.actions.first + 1).expr == kNone);
	CHECK(set.action(third.actions.first + 2).kind == hc::rules::ActionKind::cancel_timer);
	CHECK(set.action(third.actions.first + 2).expr == kNone);
	CHECK(set.find_rule("x") == 0);
	CHECK(set.find_parameter(0, "timeout") == 2);
	CHECK(set.find_timer(0, "off") == 0);
	CHECK(set.find_state(0, "on") == 1);
}

// ------------------------------------------------------------------------------------------
// The device's checks of the rules (spec/runtime-image.md 10.6, 10.8)

namespace {

std::string many(size_t count, const std::string& item) {
	std::string s;
	for (size_t i = 0; i < count; ++i) {
		s += (i ? "," : "") + item;
	}
	return s;
}

/// A chain 1 + 1 + ... with `count` literals (2*count - 1 nodes, depth count).
std::string sum_chain(size_t count) {
	std::string e = lit_int(1);
	for (size_t i = 1; i < count; ++i) {
		e = bin("+", e, lit_int(1));
	}
	return e;
}

/// A balanced sum of 2^levels literals (2^(levels+1) - 1 nodes, depth levels + 1).
std::string balanced(size_t levels) {
	return levels == 0 ? lit_int(1) : bin("+", balanced(levels - 1), balanced(levels - 1));
}

std::string rules_json(size_t count) {
	std::string s;
	for (size_t i = 0; i < count; ++i) {
		s += (i ? "," : "") + rule_json(write_reaction("b", lit_bool(true)), "", "r" + std::to_string(i));
	}
	return s;
}

} // namespace

TEST_CASE("RulesImage limits of the rules: refused by the device (spec/runtime-image.md 10.6, 10.8)") {
	std::string trig = R"({"kind": "datapoint", "datapoint": "b"})";
	std::string act = R"({"kind": "write", "datapoint": "b", "value": )" + lit_bool(true) + "}";
	std::string reaction = write_reaction("b", lit_bool(true));
	auto param = [](size_t i) {
		return R"({"id": "p)" + std::to_string(i) + R"(", "label": "P", "type": "integer", "default": 1})";
	};
	auto params = [&](size_t n) {
		std::string s;
		for (size_t i = 0; i < n; ++i) {
			s += (i ? "," : "") + param(i);
		}
		return R"("parameters": [)" + s + "]";
	};
	auto timers = [](size_t n) {
		std::string s;
		for (size_t i = 0; i < n; ++i) {
			s += std::string(i ? "," : "") + R"({"id": "t)" + std::to_string(i) +
				R"(", "duration": {"kind": "literal", "type": "duration", "value": 1000}})";
		}
		return R"("timers": [)" + s + "]";
	};
	auto states = [](size_t n) {
		std::string s;
		for (size_t i = 0; i < n; ++i) {
			s += std::string(i ? "," : "") + R"({"id": "s)" + std::to_string(i) + R"("})";
		}
		return R"("states": [)" + s + R"(], "initialState": "s0")";
	};
	auto log_action = [](size_t parts) {
		return R"({"triggers": [{"kind": "datapoint", "datapoint": "b"}], "actions": [{"kind": "log", "level": "info", "message": [)" +
			many(parts, R"("x")") + "]}]}";
	};
	auto choices = [](size_t n) {
		std::string s;
		for (size_t i = 0; i < n; ++i) {
			s += std::string(i ? "," : "") + R"({"value": )" + std::to_string(i) + R"(, "label": "C"})";
		}
		return R"("parameters": [{"id": "m", "label": "M", "type": "integer", "default": 0, "choices": [)" + s + "]}]";
	};
	struct Row {
		std::string at_limit;
		std::string beyond;
		const char* path; ///< The refusal's path in the image (a prefix).
	};
	const Row rows[] = {
		{rules_json(128), rules_json(129), "rules"},
		{rule_json(many(16, reaction)), rule_json(many(17, reaction)), "rules[0]"},
		{rule_json(R"({"triggers": [)" + many(8, trig) + R"(], "actions": []})"),
			rule_json(R"({"triggers": [)" + many(9, trig) + R"(], "actions": []})"), "rules.reactions[0]"},
		{rule_json(R"({"triggers": [)" + trig + R"(], "actions": [)" + many(16, act) + "]}"),
			rule_json(R"({"triggers": [)" + trig + R"(], "actions": [)" + many(17, act) + "]}"), "rules.reactions[0]"},
		{rule_json(reaction, params(7)), rule_json(reaction, params(8)), "rules[0]"}, // with enabled: 8
		{rule_json(reaction, timers(8)), rule_json(reaction, timers(9)), "rules[0]"},
		{rule_json(reaction, states(16)), rule_json(reaction, states(17)), "rules[0]"},
		// nodes per expression: a balanced tree of 63 nodes (depth 6) and one of 127
		{rule_json(write_reaction("u", balanced(5))), rule_json(write_reaction("u", balanced(6))), "nodes["},
		// depth: a chain of 12 literals has depth 12; 13 is refused
		{rule_json(write_reaction("u", sum_chain(12))), rule_json(write_reaction("u", sum_chain(13))), "nodes["},
		// parts of a log message
		{rule_json(log_action(8)), rule_json(log_action(9)), "rules.actions[0]"},
		// choices of a parameter
		{rule_json(reaction, choices(16)), rule_json(reaction, choices(17)), "rules.parameters[1]"},
	};
	for (const Row& row : rows) {
		CHECK(verdict(row.at_limit) == "ok");
		std::string v = verdict(row.beyond);
		CHECK_MESSAGE(limit_refused(v), v);
		CHECK_MESSAGE(v.find(std::string("image.invalid ") + row.path) == 0, v);
	}
	// The arguments of a call (at most 8) the compiler cannot write beyond: a call node's `argc` is
	// checked against its function's count (10.6).
	CHECK(verdict(rule_json(write_reaction("u",
		R"({"kind": "call", "function": "max", "args": [)" + many(8, lit_int(1)) + "]}"))) == "ok");
	// A log text of 200 bytes is fine; the bytes of a text part are E-Home's to check (strings of the
	// image are up to 511 bytes).
	CHECK(verdict(rule_json(R"({"triggers": [{"kind": "datapoint", "datapoint": "b"}], "actions": [{"kind": "log", "level": "info", "message": [")" +
		std::string(200, 'x') + R"("]}]})")) == "ok");
}

TEST_CASE("RulesImage the node budget of the image (spec/runtime-image.md 10.6)") {
	// 65 reactions' worth of 63-node values: 5 rules of 13 reactions = 65 * 63 = 4095 nodes plus
	// the 65 write targets need no nodes; one more expression is refused.
	std::string ok_rules;
	for (size_t i = 0; i < 5; ++i) {
		ok_rules += (i ? "," : "") + rule_json(many(13, write_reaction("u", balanced(5))), "", "n" + std::to_string(i));
	}
	CHECK(verdict(ok_rules) == "ok");
	std::string over = verdict(ok_rules + "," + rule_json(write_reaction("u", bin("+", lit_int(1), lit_int(1))), "", "last"));
	CHECK_MESSAGE(limit_refused(over), over);
	CHECK_MESSAGE(over.find("image.invalid nodes") == 0, over);
}

TEST_CASE("RulesImage literals the device checks: a date within 1900..2155, a timer node only in running() and remaining()") {
	auto date_literal = [](int year) {
		return R"({"kind": "literal", "type": "date", "value": {"year": )" + std::to_string(year) + R"(, "month": 1, "day": 1}})";
	};
	std::string today = R"({"kind": "call", "function": "today", "args": []})";
	CHECK(verdict(rule_json(condition_reaction(bin("<", today, date_literal(1900))))) == "ok");
	CHECK(verdict(rule_json(condition_reaction(bin("<", today, date_literal(2155))))) == "ok");
	// The compiler writes only valid literals (E-Home refuses 1899 as invalidLiteral); an image
	// written otherwise is refused by the device.
	for (int year : {1899, 2156}) {
		std::vector<uint8_t> bytes = hc_test::compiled(bench_image(rule_json(condition_reaction(bin("<", today, date_literal(1900))))));
		patch_records(bytes, hc::image::bin::kNodes, [year](size_t, uint8_t* node) {
			if (node[0] == static_cast<uint8_t>(hc::rules::NodeKind::literal) && node[2] == static_cast<uint8_t>(Type::date)) {
				node[8] = static_cast<uint8_t>(year);
				node[9] = static_cast<uint8_t>(year >> 8);
			}
		});
		std::string v = device_verdict(bytes);
		CAPTURE(year);
		CHECK_MESSAGE(v.find("image.invalid nodes[") == 0, v);
		CHECK_MESSAGE(v.find("malformed") != std::string::npos, v);
	}
	// A timer node is only the argument of running() or remaining(): `running(off)` patched into a
	// `not` over the timer node.
	std::string timers = R"("timers": [{"id": "off", "duration": {"kind": "literal", "type": "duration", "value": 1000}}])";
	std::string running = R"({"kind": "call", "function": "running", "args": [{"kind": "timer", "id": "off"}]})";
	CHECK(verdict(rule_json(condition_reaction(running), timers)) == "ok");
	std::vector<uint8_t> bytes = hc_test::compiled(bench_image(rule_json(condition_reaction(running), timers)));
	patch_records(bytes, hc::image::bin::kNodes, [](size_t, uint8_t* node) {
		if (node[0] == static_cast<uint8_t>(hc::rules::NodeKind::call)) {
			node[0] = static_cast<uint8_t>(hc::rules::NodeKind::unary);
			node[1] = static_cast<uint8_t>(hc::rules::Op::not_);
		}
	});
	std::string v = device_verdict(bytes);
	CHECK_MESSAGE(v.find("image.invalid nodes[") == 0, v);
	CHECK_MESSAGE(v.find("a timer node outside running() and remaining()") != std::string::npos, v);
}

// ------------------------------------------------------------------------------------------
// Parameter key mapping

TEST_CASE("RulesParameterStore key mapping") {
	char key[16];
	hc::rules::ParameterStore::key_for("hall", "timeout", key);
	CHECK(std::strlen(key) == 15);
	CHECK(std::string(key, 3) == "rp.");
	for (size_t i = 3; i < 15; ++i) {
		CHECK(std::isxdigit(static_cast<unsigned char>(key[i])));
		CHECK_FALSE(std::isupper(static_cast<unsigned char>(key[i])));
	}
	// FNV-1a 64 of "hall/timeout", folded to 48 bits: an independent computation pins the format.
	uint64_t h = 14695981039346656037ull;
	for (const char* p = "hall/timeout"; *p; ++p) {
		h ^= static_cast<uint8_t>(*p);
		h *= 1099511628211ull;
	}
	char want[16];
	std::snprintf(want, sizeof(want), "rp.%012" PRIx64, static_cast<uint64_t>((h ^ (h >> 48)) & 0xFFFFFFFFFFFFull));
	CHECK(std::string(key) == want);
	// Depends on the ids only; the longest ids still give a 15-character key.
	char other[16];
	hc::rules::ParameterStore::key_for("hall", "timeOut", other);
	CHECK(std::string(other) != key);
	hc::rules::ParameterStore::key_for("abcdefghijklmnopqrstuvwxyz012345", "abcdefghijklmnopqrstuvwx", other);
	CHECK(std::strlen(other) == 15);
}

TEST_CASE("RulesParameterStore encode and decode every parameter type") {
	hc::TimeOfDay tod;
	tod.hour = 6;
	tod.minute = 5;
	tod.second = 30;
	tod.day_of_week = 3;
	hc::Color color;
	color.red = 1;
	color.green = 2;
	color.blue = 3;
	color.white = 4;
	color.valid = hc::Color::kAll;
	std::vector<Val> values = {Val::of_bool(true), Val::of_int(-9007199254740991LL), Val::of_real(0.1),
		Val::of_real(-1.5e300), Val::of_string("a=b/c \"\xc3\xa9", 9), Val::of_duration(90061001), Val::of_time(tod),
		Val::of_enum(4000000000u), Val::of_color(color), Val::of_controlled(true, 7)};
	for (const Val& v : values) {
		char text[160];
		if (v.type == Type::controlled) {
			continue; // not a parameter type
		}
		REQUIRE(hc::rules::ParameterStore::encode("abcdefghijklmnopqrstuvwxyz012345", "abcdefghijklmnopqrstuvwx", v, text,
			sizeof(text)));
		CHECK(std::strlen(text) <= 116);
		char logical[64];
		Val back;
		REQUIRE(hc::rules::ParameterStore::decode(text, logical, sizeof(logical), back));
		CHECK(std::string(logical) == "abcdefghijklmnopqrstuvwxyz012345/abcdefghijklmnopqrstuvwx");
		CHECK_MESSAGE(hc::rules::val_identical(back, v), text);
	}
	char logical[64];
	Val back;
	CHECK_FALSE(hc::rules::ParameterStore::decode("no-equals", logical, sizeof(logical), back));
	CHECK_FALSE(hc::rules::ParameterStore::decode("a/b=q:1", logical, sizeof(logical), back));
	CHECK_FALSE(hc::rules::ParameterStore::decode("a/b=i:1x", logical, sizeof(logical), back));
}

TEST_CASE("RulesParameterStore write, read, collision slot and prune") {
	hc_test::FakeSettings settings;
	hc::rules::ParameterStore store(settings);
	REQUIRE(store.write("hall", "timeout", Val::of_duration(180000)));
	Val v;
	REQUIRE(store.read("hall", "timeout", v));
	CHECK(v.u.i == 180000);
	// A slot holding another logical key (a collision with an undeclared parameter) is no override.
	char key[16];
	hc::rules::ParameterStore::key_for("hall", "level", key);
	settings.entries[key] = hc_test::FakeSettings::Entry{hc::SettingType::string, "other/x=i:5", 0};
	CHECK_FALSE(store.read("hall", "level", v));
	store.erase("hall", "level"); // not ours: stays
	CHECK(settings.entries.count(key) == 1);
	// prune: keep hall/timeout, drop the rest; foreign keys stay.
	settings.entries["wifi.ssid"] = hc_test::FakeSettings::Entry{hc::SettingType::string, "home", 0};
	settings.entries["rp.zzzzzzzzzzzz"] = hc_test::FakeSettings::Entry{hc::SettingType::string, "garbage", 0};
	struct Keep : hc::rules::ParameterStore::Visitor {
		bool entry(const char* logical, const Val&) override { return std::strcmp(logical, "hall/timeout") == 0; }
	} keep;
	store.prune(keep);
	CHECK(settings.entries.size() == 2);
	CHECK(settings.entries.count("wifi.ssid") == 1);
	CHECK(store.read("hall", "timeout", v));
}


TEST_CASE("RulesParameterStore an image whose parameter keys collide is refused") {
	// r6432d85626ca/p and re7a81faa983a/p share the key rp.4728dda3ea0f (found by a cycle search).
	CHECK(hc::rules::ParameterStore::hash("r6432d85626ca", "p") == hc::rules::ParameterStore::hash("re7a81faa983a", "p"));
	std::string param = R"("parameters": [{"id": "p", "label": "P", "type": "integer", "default": 1}])";
	std::string reaction = R"({"triggers": [{"kind": "datapoint", "datapoint": "b"}], "actions": []})";
	hc_test::RuleImage image;
	REQUIRE(image.compile(bench_image(rule_json(reaction, param, "r6432d85626ca") + "," +
		rule_json(reaction, param, "re7a81faa983a"))));
	// The check belongs to the binding of the rules (spec/runtime-image.md 10.8): the loader and every
	// other binder see it; a refused bind leaves the set empty.
	RuleSet set;
	hc_test::ScopedCapture capture(hc::LogLevel::error);
	LoadResult result = image.compiled.bind(set);
	CHECK(result.error == hc::rules::LoadError::limit_exceeded);
	CHECK(std::string(result.text).find("collide") != std::string::npos);
	CHECK(std::string(result.path) == "rules[1]");
	CHECK(set.rule_count() == 0);
	// The device refuses the image with it.
	std::string v = device_verdict(image.compiled.bytes);
	CHECK_MESSAGE(v.find("image.invalid rules[1]") == 0, v);
	CHECK_MESSAGE(v.find("collide") != std::string::npos, v);
}

// ------------------------------------------------------------------------------------------
// Labels (spec/rules.md 2.2, spec/runtime-image.md 9.5)

TEST_CASE("RulesImage labels: a string or a language map, resolved in the device's language (spec 2.2)") {
	std::string reaction = R"({"triggers": [{"kind": "datapoint", "datapoint": "b"}], "actions": []})";
	auto name_in = [&](const std::string& name, const char* language, std::string* json = nullptr) {
		Bench bench(R"({"id": "x", "name": )" + name + R"(, "reactions": [)" + reaction + "]}", "", language);
		if (json != nullptr) {
			char buffer[256];
			hc::JsonWriter writer(buffer, sizeof(buffer));
			hc::image::bin::write_label(writer, bench.set.view(), bench.set.rule(0).name);
			*json = std::string(writer.data(), writer.length());
		}
		return std::string(bench.set.label(bench.set.rule(0).name));
	};
	std::string map = R"({"fr": "Escalier", "en": "Stairs", "de": "Treppe", "de-AT": "Stiege"})";
	CHECK(name_in(map, "de-AT") == "Stiege"); // the exact tag
	CHECK(name_in(map, "de-CH") == "Treppe"); // the base language
	CHECK(name_in(map, "de") == "Treppe");
	CHECK(name_in(map, "it") == "Stairs"); // English
	CHECK(name_in(R"({"fr": "Escalier", "it": "Scala"})", "de") == "Escalier"); // the first entry
	CHECK(name_in(R"({"DE": "Treppe", "en": "Stairs"})", "de") == "Treppe"); // tags compare without case
	std::string json;
	CHECK(name_in(R"({"en": "A \"q\"", "de": "B"})", "en", &json) == "A \"q\"");
	// Written back as the image gave it: a map in the source's order (the old JSON loader's test tree
	// ordered the members by name), a plain label as a string.
	CHECK(json == R"({"en":"A \"q\"","de":"B"})");
	CHECK(name_in(R"("Plain")", "de", &json) == "Plain");
	CHECK(json == R"("Plain")");
	// More than 16 languages: the device refuses the label reference (10.4).
	std::string many = "{";
	for (int i = 0; i < 17; ++i) {
		many += std::string(i ? "," : "") + "\"l" + static_cast<char>('a' + i) + "\": \"x\"";
	}
	many += "}";
	std::string v = verdict(R"({"id": "x", "name": )" + many + R"(, "reactions": [)" + reaction + "]}");
	CHECK_MESSAGE(v.find("image.invalid rules[0].name") == 0, v);
	CHECK_MESSAGE(v.find("limitExceeded: a label of 17 languages, at most 16") != std::string::npos, v);
}

// ------------------------------------------------------------------------------------------
// The engine: loop guard, memory, no allocation in steady state

namespace {

struct EngineBench : hc::rules::EngineObserver {
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc::DatapointModel model{clock};
	hc_test::FakeSettings settings;
	hc_test::TestTechnology technology;
	struct NoTime : hc::rules::TimeSource {
		bool local_now(hc::DateTime&) const override { return false; }
		bool sun_time(hc::rules::SunEvent, hc::TimeOfDay&) const override { return false; }
	} time;
	hc_test::RuleImage image;
	std::unique_ptr<Engine> engine;
	std::vector<std::string> logs;
	size_t commands = 0;

	void rule_logged(const char* rule, hc::rules::LogCode code, hc::LogLevel, const char* text) override {
		logs.push_back(std::string(rule) + " " + hc::rules::log_code_name(code) + ": " + text);
	}
	void rule_commanded(const char*, const hc::Datapoint&, const hc::Value&) override { ++commands; }

	/// Compiles `datapoints` and `rules` (the items of the members), loads the image's datapoints (the
	/// declared ones bound to the test technology, the parameter datapoints the engine's), adopts the
	/// rules and starts them.
	void setup(const std::string& datapoints, const std::string& rules) {
		REQUIRE(image.compile(hc_test::rules_source(datapoints, rules)));
		image.load_model(model);
		for (size_t i = 0; i < image.datapoint_count(); ++i) {
			if (image.declared(i)) {
				REQUIRE(model.bind(static_cast<uint16_t>(i), technology) == hc::DatapointStatus::ok);
			}
		}
		engine = std::make_unique<Engine>(loop, model, clock, settings, time);
		engine->set_observer(this);
		LoadResult result = engine->adopt(image.bind());
		REQUIRE_MESSAGE(result.ok(), std::string(result.path) << ": " << std::string(result.text));
		engine->start(hc::rules::StartReason::boot);
		settle();
	}

	void settle() {
		for (;;) {
			uint64_t next = loop.next_deadline();
			if (next > clock.now_ms) {
				return;
			}
			loop.run_once(clock.now_ms);
		}
	}

	void advance(uint64_t ms) {
		uint64_t target = clock.now_ms + ms;
		for (;;) {
			uint64_t next = loop.next_deadline();
			if (next > target) {
				break;
			}
			clock.now_ms = std::max(clock.now_ms, next);
			loop.run_once(clock.now_ms);
		}
		clock.now_ms = target;
	}

	void report(const char* name, const hc::Value& value) {
		const hc::Datapoint* dp = model.find(name);
		REQUIRE(dp != nullptr);
		REQUIRE(model.report(technology, dp->id(), value) == hc::DatapointStatus::ok);
		settle();
	}
};

} // namespace

TEST_CASE("RulesEngine a self-triggering rule ends with loopGuard naming it") {
	EngineBench bench;
	std::string datapoints = hc_test::datapoint_source("go", "boolean") + "," + hc_test::datapoint_source("n", "unsigned") +
		"," + hc_test::datapoint_source("other", "boolean");
	// `count` writes n + 1 whenever n changes: each write is a derived event for itself.
	std::string rules =
		rule_json(R"({"triggers": [{"kind": "datapoint", "datapoint": "go"}], "actions": [{"kind": "write", "datapoint": "n", "value": )" +
			lit_int(0) + R"(}]}, {"triggers": [{"kind": "datapoint", "datapoint": "n"}], "actions": [{"kind": "write", "datapoint": "n", "value": )" +
			bin("+", dp("n"), lit_int(1)) + "}]}",
			"", "count") + "," +
		rule_json(R"({"triggers": [{"kind": "datapoint", "datapoint": "other"}], "actions": []})", "", "quiet");
	hc_test::ScopedCapture capture(hc::LogLevel::error);
	bench.setup(datapoints, rules);
	bench.report("go", hc::Value::of_bool(true));
	REQUIRE(bench.logs.size() == 1);
	CHECK(bench.logs[0].find("count loopGuard") == 0);
	CHECK(bench.logs[0].find("most: count with 32") != std::string::npos);
	CHECK(bench.commands == 33); // the external firing's write plus 32 derived firings' writes
	CHECK(bench.model.find("n")->value().as_unsigned() == 32);
	hc::rules::RuleDiagnostics diagnostics;
	REQUIRE(bench.engine->diagnostics(0, diagnostics));
	CHECK(diagnostics.loop_guards == 1);
	CHECK(diagnostics.peak_cascade_firings == 32);
	CHECK(diagnostics.firings == 33);
	CHECK(diagnostics.errors == 1);
	CHECK(diagnostics.last_error_code == hc::rules::LogCode::loop_guard);
	REQUIRE(bench.engine->diagnostics(1, diagnostics));
	CHECK(diagnostics.loop_guards == 0);
	CHECK(capture.sink.contains("count loopGuard"));
	// The next external event is processed normally (and loops again).
	bench.report("go", hc::Value::of_bool(false));
	CHECK(bench.logs.size() == 2);
}

TEST_CASE("RulesEngine lastError names the expression and its first datapoint without a value (#97)") {
	EngineBench bench;
	std::string datapoints = hc_test::datapoint_source("go", "boolean") + "," +
		hc_test::datapoint_source("presence", "boolean") + "," + hc_test::datapoint_source("lux", "unsigned") + "," +
		hc_test::datapoint_source("a", "unsigned") + "," + hc_test::datapoint_source("z", "unsigned") + "," +
		hc_test::datapoint_source("light", "boolean") + "," + hc_test::datapoint_source("level", "unsigned") + "," +
		hc_test::datapoint_source("ratio", "unsigned");
	std::string on_go = R"({"triggers": [{"kind": "datapoint", "datapoint": "go", "on": "update"}], )";
	std::string condition = on_go + R"("condition": )" + bin("and", dp("presence"), bin("<", dp("lux"), lit_int(50))) +
		R"(, "actions": [{"kind": "write", "datapoint": "light", "value": )" + lit_bool(true) + "}]}";
	std::string value = on_go + R"("actions": [{"kind": "write", "datapoint": "level", "value": )" +
		bin("+", dp("lux"), lit_int(1)) + "}]}";
	std::string error = on_go + R"("actions": [{"kind": "write", "datapoint": "ratio", "value": {"kind": "call", "function": "coalesce", "args": [)" +
		dp("a") + ", " + bin("/", lit_int(10), dp("z")) + "]}}]}";
	std::string rules = rule_json(condition, "", "cond") + "," + rule_json(value, "", "value") + "," +
		rule_json(error, "", "eval");
	hc_test::ScopedCapture capture(hc::LogLevel::warn);
	bench.setup(datapoints, rules);
	char text[256];
	CHECK(bench.engine->last_error_text(0, text, sizeof(text)) == 0);
	CHECK(std::string(text).empty());

	bench.report("presence", hc::Value::of_bool(true));
	bench.report("z", hc::Value::of_unsigned(0));
	bench.report("go", hc::Value::of_bool(true));
	hc::rules::RuleDiagnostics d;
	REQUIRE(bench.engine->diagnostics(0, d));
	CHECK(d.last_error_code == hc::rules::LogCode::condition_no_value);
	bench.engine->last_error_text(0, text, sizeof(text));
	CHECK(std::string(text) == "@presence and @lux < 50 – lux has no value");
	REQUIRE(bench.engine->diagnostics(1, d));
	CHECK(d.last_error_code == hc::rules::LogCode::value_no_value);
	bench.engine->last_error_text(1, text, sizeof(text));
	CHECK(std::string(text) == "@lux + 1 – lux has no value");
	REQUIRE(bench.engine->diagnostics(2, d));
	CHECK(d.last_error_code == hc::rules::LogCode::eval_error);
	bench.engine->last_error_text(2, text, sizeof(text));
	CHECK(std::string(text) == "division by zero in coalesce(@a, 10 / @z) – a has no value");
	// The log lines name the datapoint too.
	CHECK(capture.sink.contains("cond conditionNoValue: condition has no value: @presence and @lux < 50 – lux has no value"));
	CHECK(capture.sink.contains("eval evalError: division by zero in coalesce(@a, 10 / @z) – a has no value"));

	// The first datapoint without a value, in the order of the text: presence before lux.
	bench.report("lux", hc::Value::of_unsigned(10));
	bench.report("go", hc::Value::of_bool(true)); // the condition had a value: logged again when it has none
	bench.report("presence", hc::Value::of_bool(true));
	REQUIRE(bench.model.report_stale(bench.technology, bench.model.find("presence")->id()) == hc::DatapointStatus::ok);
	bench.settle();
	bench.report("go", hc::Value::of_bool(true));
	bench.engine->last_error_text(0, text, sizeof(text));
	CHECK(std::string(text) == "@presence and @lux < 50 – presence has no value");

	// An evaluation error with every datapoint known names only the expression.
	bench.report("a", hc::Value::of_unsigned(1));
	bench.report("go", hc::Value::of_bool(true)); // evaluates without error: logged again on the next error
	bench.engine->last_error_text(2, text, sizeof(text));
	CHECK(std::string(text) == "division by zero in coalesce(@a, 10 / @z) – a has no value"); // unchanged: no new error
	REQUIRE(bench.model.report_stale(bench.technology, bench.model.find("a")->id()) == hc::DatapointStatus::ok);
	bench.settle();
	bench.report("go", hc::Value::of_bool(true));
	bench.engine->last_error_text(2, text, sizeof(text));
	CHECK(std::string(text) == "division by zero in coalesce(@a, 10 / @z) – a has no value");

	// A truncated buffer keeps the snprintf convention.
	char small[8];
	size_t full = bench.engine->last_error_text(0, small, sizeof(small));
	CHECK(full == std::strlen("@presence and @lux < 50 – presence has no value"));
	CHECK(std::string(small) == "@presen");

	// A swap: the expression was a node of the old set and is forgotten; the datapoint is followed.
	hc_test::RuleImage next;
	REQUIRE(next.compile(hc_test::rules_source(datapoints, rule_json(value, "", "value") + "," + rule_json(condition, "", "cond"))));
	LoadResult result = bench.engine->adopt(next.bind());
	REQUIRE(result.ok());
	bench.settle();
	bench.engine->last_error_text(1, text, sizeof(text));
	CHECK(std::string(text) == "condition has no value – presence has no value");
}

TEST_CASE("RulesEngine readRefused is logged once per action until a read is accepted") {
	EngineBench bench;
	std::string datapoints = hc_test::datapoint_source("go", "boolean") + "," + hc_test::datapoint_source("t", "real");
	std::string rules = rule_json(
		R"({"triggers": [{"kind": "datapoint", "datapoint": "go", "on": "update"}], "actions": [{"kind": "read", "datapoint": "t"}]})");
	hc_test::ScopedCapture capture(hc::LogLevel::error);
	bench.setup(datapoints, rules);
	bench.technology.refuse_reads = true;
	bench.report("go", hc::Value::of_bool(true));
	bench.report("go", hc::Value::of_bool(true));
	REQUIRE(bench.logs.size() == 1);
	CHECK(bench.logs[0].find("x readRefused") == 0);
	bench.technology.refuse_reads = false;
	bench.report("go", hc::Value::of_bool(true));
	CHECK(bench.logs.size() == 1);
	hc::rules::RuleDiagnostics diagnostics;
	REQUIRE(bench.engine->diagnostics(0, diagnostics));
	CHECK(std::string(diagnostics.last_action) == "read t");
	bench.technology.refuse_reads = true;
	bench.report("go", hc::Value::of_bool(true));
	CHECK(bench.logs.size() == 2);
}

TEST_CASE("RulesEngine a silence period outside the timer limits is timerRefused once") {
	EngineBench bench;
	std::string rules = rule_json(
		R"({"triggers": [{"kind": "silence", "datapoint": "t", "after": {"kind": "literal", "type": "duration", "value": 50}}], "actions": []})");
	hc_test::ScopedCapture capture(hc::LogLevel::error);
	bench.setup(hc_test::datapoint_source("t", "real"), rules);
	REQUIRE(bench.logs.size() == 1);
	CHECK(bench.logs[0].find("x timerRefused") == 0);
	bench.report("t", hc::Value::of_real(1));
	bench.advance(1000);
	CHECK(bench.logs.size() == 1);
	hc::rules::RuleDiagnostics diagnostics;
	REQUIRE(bench.engine->diagnostics(0, diagnostics));
	CHECK(diagnostics.firings == 0);
}

TEST_CASE("RulesEngine the silence trigger in the diagnostics") {
	EngineBench bench;
	std::string rules = rule_json(
		R"({"triggers": [{"kind": "silence", "datapoint": "t", "after": {"kind": "literal", "type": "duration", "value": 1000}}], "actions": []})");
	bench.setup(hc_test::datapoint_source("t", "real"), rules);
	bench.advance(999);
	hc::rules::RuleDiagnostics diagnostics;
	REQUIRE(bench.engine->diagnostics(0, diagnostics));
	CHECK(diagnostics.firings == 0);
	bench.advance(1);
	REQUIRE(bench.engine->diagnostics(0, diagnostics));
	CHECK(diagnostics.firings == 1);
	CHECK(std::string(diagnostics.last_trigger) == "silence t");
	bench.advance(10000);
	REQUIRE(bench.engine->diagnostics(0, diagnostics));
	CHECK(diagnostics.firings == 1);
}

namespace {

/// A typical image: `count` rules of a presence light (a parameter, a timer, two reactions).
std::string typical_rules(size_t count) {
	std::string s;
	for (size_t i = 0; i < count; ++i) {
		std::string n = std::to_string(i);
		s += (i ? "," : "") +
			rule_json(
				R"({"triggers": [{"kind": "datapoint", "datapoint": "in)" + n + R"("}], "condition": )" +
					bin("and", dp("in" + n), bin("<", dp("lux" + n), R"({"kind": "parameter", "id": "dark"})")) +
					R"(, "actions": [{"kind": "write", "datapoint": "out)" + n + R"(", "value": )" + lit_bool(true) +
					R"(, "onlyOnChange": true}, {"kind": "restartTimer", "timer": "off"}]},)"
					R"({"triggers": [{"kind": "timer", "timer": "off"}], "actions": [{"kind": "write", "datapoint": "out)" + n +
					R"(", "value": )" + lit_bool(false) + "}]}",
				R"("parameters": [{"id": "dark", "label": "Dark below", "type": "real", "default": 50.0, "min": 0.0, "max": 1000.0},)"
				R"({"id": "timeout", "label": "Timeout", "type": "duration", "default": 180000}],)"
				R"("timers": [{"id": "off", "duration": {"kind": "parameter", "id": "timeout"}}])",
				"light" + n);
	}
	return s;
}

std::string typical_datapoints(size_t count) {
	std::string s;
	for (size_t i = 0; i < count; ++i) {
		std::string n = std::to_string(i);
		s += (i ? "," : "") + hc_test::datapoint_source("in" + n, "boolean") + "," +
			hc_test::datapoint_source("lux" + n, "real") + "," + hc_test::datapoint_source("out" + n, "boolean");
	}
	return s;
}

} // namespace

TEST_CASE("RulesEngine memory accounting of a 50-rule image") {
	EngineBench bench;
	bench.setup(typical_datapoints(50), typical_rules(50));
	const RuleSet& set = bench.engine->rules();
	hc::rules::RuleSetMemory m = set.memory();
	hc::rules::EngineMemory e = bench.engine->memory();
	CHECK(sizeof(hc::rules::Node) == 16);
	CHECK(m.nodes == set.node_count() * sizeof(hc::rules::Node));
	// The records are the image's, read in place from the slot; RAM holds only the derived tables
	// (tracked datapoints, match and silence triggers) and the timelines.
	size_t slot = m.rules + m.reactions + m.triggers + m.actions + m.log_parts + m.parameters + m.choices + m.timers +
		m.states + m.nodes + m.constants;
	size_t derived = m.tracked + m.matches + m.silences;
	CHECK(m.block >= derived + m.timelines);
	CHECK(m.block <= derived + m.timelines + 16 * 8); // alignment padding only
	size_t runtime = e.per_rule * set.rule_count() + e.per_parameter * set.parameter_count() +
		e.per_timer * set.timer_count() + e.per_node * set.node_count() + e.per_tracked * set.tracked_count() +
		e.per_silence * set.silence_count() + e.per_action * set.action_count();
	CHECK(e.runtime >= runtime);
	// Beside the tables: the runtime object with its list of parts, the chunk lists of the per-rule
	// and per-parameter tables (one pointer per 16, homecontrol #70), the interest bitmap.
	size_t chunk_lists = sizeof(void*) * ((set.rule_count() + 15) / 16 + (set.parameter_count() + 15) / 16);
	CHECK(e.runtime <= runtime + 256 + chunk_lists + bench.model.size() / 8 + 8 * 8 + 2 * set.match_count() + 64);
	MESSAGE("50 rules, " << set.node_count() << " nodes, " << set.parameter_count() << " parameters, "
		<< set.timer_count() << " timers: rule records " << slot << " bytes in the slot (nodes " << m.nodes
		<< ", constants " << m.constants << "), derived tables " << m.block << " bytes, runtime block " << e.runtime
		<< " bytes, engine object " << e.object << " bytes; per rule " << e.per_rule << ", per parameter "
		<< e.per_parameter << ", per timer " << e.per_timer << ", per node " << sizeof(hc::rules::Node) << "+"
		<< e.per_node << ", per tracked datapoint " << e.per_tracked);
	CHECK(m.block + e.runtime < 64 * 1024);
}

TEST_CASE("RulesEngine allocates nothing while processing events") {
	EngineBench bench;
	bench.setup(typical_datapoints(10), typical_rules(10));
	bench.report("lux3", hc::Value::of_real(10.0));
	size_t allocations = 0;
	{
		hc_test::AllocationScope counting;
		for (int round = 0; round < 3; ++round) {
			bench.report("in3", hc::Value::of_bool(true));
			bench.report("in3", hc::Value::of_bool(false));
			bench.advance(200000); // the timer expires
		}
		allocations = counting.count();
	}
	CHECK(allocations == 0);
	CHECK(bench.commands >= 6);
}

TEST_CASE("RulesEngine state changes allocate nothing; their write is delayed and coalesced (spec 8.5)") {
	EngineBench bench;
	std::string datapoints = hc_test::datapoint_source("button", "boolean") + "," + hc_test::datapoint_source("light", "boolean");
	std::string stairs = R"({"id": "stairs", "name": "Stairs", )"
		R"("timers": [{"id": "t", "duration": {"kind": "literal", "type": "duration", "value": 2000}}], )"
		R"("states": [{"id": "off", "entry": [{"kind": "write", "datapoint": "light", "value": )" + lit_bool(false) + R"(}]}, )"
		R"({"id": "on", "entry": [{"kind": "write", "datapoint": "light", "value": )" + lit_bool(true) +
		R"(}, {"kind": "restartTimer", "timer": "t"}]}], "initialState": "off", )"
		R"("reactions": [{"triggers": [{"kind": "datapoint", "datapoint": "button", "on": "update"}], "actions": [], "to": "on"}, )"
		R"({"triggers": [{"kind": "timer", "timer": "t"}], "actions": [], "from": ["on"], "to": "off"}]})";
	bench.setup(datapoints, stairs);
	char key[16];
	hc::rules::ParameterStore::key_for("stairs", hc::rules::ParameterStore::kStateKey, key);
	CHECK(bench.settings.entries.count(key) == 0); // the initial state is written 10 s later
	bench.advance(hc::rules::Engine::kStatePersistDelayMs);
	REQUIRE(bench.settings.entries.count(key) == 1);
	CHECK(bench.settings.entries[key].text == "stairs/.state=s:6f6666");
	size_t allocations = 0;
	{
		hc_test::AllocationScope counting;
		for (int round = 0; round < 3; ++round) {
			bench.report("button", hc::Value::of_bool(true));
			bench.advance(3000); // on, then off when the timer expires: within the delay, no write
		}
		allocations = counting.count();
	}
	CHECK(allocations == 0);
	CHECK(bench.settings.entries[key].text == "stairs/.state=s:6f6666");
	bench.report("button", hc::Value::of_bool(true));
	bench.engine->persist_now();
	CHECK(bench.settings.entries[key].text == "stairs/.state=s:6f6e");
}

TEST_CASE("RulesClock date-time instants across the end of summer time (spec 4.5)") {
	// The test zone evaluates a POSIX TZ string itself: no dependency on the host's TZ.
	hc_test::PosixRuleZone zone;
	REQUIRE(zone.parse("CET-1CEST,M3.5.0,M10.5.0/3"));
	int64_t change = hc::days_from_civil(2026, 10, 25) * 86400 + 3600; // 01:00 UTC, 03:00 CEST -> 02:00 CET
	hc::LocalTime l{};
	REQUIRE(zone.to_local(change - 1, l));
	CHECK((l.hour == 2 && l.minute == 59 && l.dst));
	REQUIRE(zone.to_local(change, l));
	CHECK((l.hour == 2 && l.minute == 0 && !l.dst));
	REQUIRE(zone.to_local(hc::days_from_civil(2026, 3, 29) * 86400 + 3600, l)); // 01:00 UTC, 02:00 CET -> 03:00 CEST
	CHECK((l.hour == 3 && l.dst));
	REQUIRE(zone.to_local(hc::days_from_civil(2026, 7, 1) * 86400, l));
	CHECK((l.hour == 2 && l.dst));

	struct Wall : hc::TimeSource {
		uint64_t monotonic_ms() const override { return 0; }
		hc::WallTime now_utc() const override { return hc::WallTime{utc, true}; }
		bool to_local(int64_t seconds, hc::LocalTime& out) const override { return zone->to_local(seconds, out); }
		bool sun_times(const hc::Date&, hc::SunTimes&) const override { return false; }
		bool sun_window(const hc::Date&, hc::SunWindow&) const override { return false; }
		const hc_test::PosixRuleZone* zone = nullptr;
		int64_t utc = 0;
	} wall;
	wall.zone = &zone;
	hc::image::RulesClock clock(wall);
	hc::DateTime t;
	t.year = 2026;
	t.month = 10;
	t.day = 25;
	t.hour = 2;
	t.minute = 30;
	// The ambiguous 02:30: the summer-time flag selects the first occurrence, no flag the second.
	t.flags = hc::DateTime::kSummerTime;
	CHECK(clock.local_to_utc(t) == change - 1800);
	t.flags = 0;
	CHECK(clock.local_to_utc(t) == change + 1800);
	// now() carries the flag; utc_to_local sets it from the zone.
	wall.utc = change - 1800;
	REQUIRE(clock.local_now(t));
	CHECK((t.hour == 2 && t.minute == 30 && t.flags == hc::DateTime::kSummerTime));
	REQUIRE(clock.utc_to_local(change + 1800, t));
	CHECK((t.hour == 2 && t.minute == 30 && t.flags == 0));
	// A time in the spring gap is its end, 03:00 CEST.
	t = hc::DateTime();
	t.year = 2026;
	t.month = 3;
	t.day = 29;
	t.hour = 2;
	t.minute = 30;
	CHECK(clock.local_to_utc(t) == hc::days_from_civil(2026, 3, 29) * 86400 + 3600);
}

TEST_CASE("RulesSwap facts of spec 7.6: values kept by name and type, read-on-start for the new datapoints") {
	// The image service swaps with DatapointModel::load(..., keep_values = true) and configures the
	// KNX adapter again (Image.SwapKeepsValuesAndFollowsIds covers the values through the service).
	hc_test::TunnelFixture f;
	hc::DatapointModel model{f.clock};
	hc::knx::KnxAdapter adapter{f.loop, f.clock, model, f.tunnel, nullptr};
	using hc::knx::GroupAddress;
	GroupAddress temperature[1] = {GroupAddress::of(1, 2, 3)};
	GroupAddress humidity[1] = {GroupAddress::of(1, 2, 4)};
	auto mapping = [](uint16_t id, const GroupAddress* listen) {
		hc::knx::KnxMapping m;
		m.datapoint = id;
		m.dpt = hc::Dpt{9, 1};
		m.listen = listen;
		m.listen_count = 1;
		return m;
	};
	hc::DatapointSpec first[] = {{"temperature", hc::ValueType::real, hc::DatapointFlags::kReadOnStart}};
	REQUIRE(model.load(first, 1) == hc::DatapointStatus::ok);
	adapter.start();
	hc::knx::KnxMapping m1[] = {mapping(0, temperature)};
	REQUIRE(adapter.configure(m1, 1));
	uint8_t gateway_sequence = 0;
	auto confirm_last = [&]() { // acknowledges the last TUNNELING_REQUEST and confirms it
		const hc_test::Bytes& request = f.socket.last().data;
		REQUIRE(request[3] == 0x20);
		f.socket.inject(hc_test::tunneling_ack(7, request[8]));
		hc_test::Bytes cemi(request.begin() + 10, request.end());
		cemi[0] = 0x2E;
		f.socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
	};
	f.connect(7);
	f.run_for(150);
	CHECK(adapter.reads_pending() == 0);
	confirm_last();
	// The answer: 21.0 °C from 1.1.10.
	f.socket.inject(hc_test::tunneling_request(7, gateway_sequence++, hc_test::bytes("29 00 BC D0 11 0A 0A 03 03 00 40 0C 1A")));
	REQUIRE(model.get(0)->quality() == hc::Quality::good);
	uint64_t updated = model.get(0)->updated_ms();
	// The swap adds the humidity before the temperature: the temperature keeps value, quality and
	// update time under its new id; the new datapoint is read (the adapter reads every
	// read-on-start datapoint after a reconfiguration, the new ones among them).
	f.run_for(2000);
	hc::DatapointSpec second[] = {{"humidity", hc::ValueType::real, hc::DatapointFlags::kReadOnStart},
		{"temperature", hc::ValueType::real, hc::DatapointFlags::kReadOnStart}};
	REQUIRE(model.load(second, 2, true) == hc::DatapointStatus::ok);
	hc::knx::KnxMapping m2[] = {mapping(0, humidity), mapping(1, temperature)};
	REQUIRE(adapter.configure(m2, 2));
	const hc::Datapoint* t = model.find("temperature");
	REQUIRE(t != nullptr);
	CHECK(t->quality() == hc::Quality::good);
	CHECK(t->value().as_real() == doctest::Approx(21.0));
	CHECK(t->updated_ms() == updated);
	CHECK(model.find("humidity")->quality() == hc::Quality::unknown);
	CHECK(adapter.reads_pending() == 2);
	size_t before = f.socket.count(hc::knx::ip::Service::tunneling_request);
	f.run_for(150);
	REQUIRE(f.socket.count(hc::knx::ip::Service::tunneling_request) == before + 1);
	const hc_test::Bytes& request = f.socket.last().data;
	CHECK(hc_test::hex(hc_test::Bytes(request.begin() + 16, request.end())) == "0A 04 01 00 00"); // humidity: read
}

TEST_CASE("RulesEngine diagnostics texts") {
	EngineBench bench;
	bench.setup(typical_datapoints(1), typical_rules(1));
	bench.report("lux0", hc::Value::of_real(10.0));
	bench.report("in0", hc::Value::of_bool(true));
	hc::rules::RuleDiagnostics diagnostics;
	REQUIRE(bench.engine->diagnostics(0, diagnostics));
	CHECK(std::string(diagnostics.id) == "light0");
	CHECK(diagnostics.enabled);
	CHECK(diagnostics.firings == 1);
	CHECK(std::string(diagnostics.last_trigger) == "datapoint in0");
	CHECK(std::string(diagnostics.last_action) == "restartTimer off 3min");
	int64_t remaining = 0;
	CHECK(bench.engine->timer_remaining(0, 0, remaining));
	CHECK(remaining == 180000);
	bench.advance(180000);
	REQUIRE(bench.engine->diagnostics(0, diagnostics));
	CHECK(std::string(diagnostics.last_trigger) == "timer off");
	CHECK(std::string(diagnostics.last_action) == "write out0 = false");
	CHECK(diagnostics.firings == 2);
	CHECK_FALSE(bench.engine->timer_remaining(0, 0, remaining));
}
