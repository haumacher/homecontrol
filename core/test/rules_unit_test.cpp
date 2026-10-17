/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Unit tests of the rule interpreter beyond the conformance corpus: the canonical printer (round
// trip through a text parser for every corpus expression), the load checks and limits of spec
// section 10, the parameter key mapping, the memory accounting and the loop guard.
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <regex>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/rules/engine.h"
#include "hc/rules/parameter_store.h"
#include "hc/rules/printer.h"
#include "hc/rules/rule_set.h"
#include "rules_test_support.h"
#include "test_support.h"

// ------------------------------------------------------------------------------------------
// Allocation counting (for the steady-state check): replaces the global allocation functions of
// the test binary; counts only while enabled.

namespace {
std::atomic<bool> g_counting{false};
std::atomic<size_t> g_allocations{0};
} // namespace

void* operator new(size_t size) {
	if (g_counting) {
		++g_allocations;
	}
	void* p = std::malloc(size ? size : 1);
	if (p == nullptr) {
		std::abort();
	}
	return p;
}
void* operator new[](size_t size) { return operator new(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept {
	if (g_counting) {
		++g_allocations;
	}
	return std::malloc(size ? size : 1);
}
void* operator new[](size_t size, const std::nothrow_t& tag) noexcept { return operator new(size, tag); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

using hc::rules::Engine;
using hc::rules::LoadResult;
using hc::rules::RuleSet;
using hc::rules::Type;
using hc::rules::Val;
using hc_test::Json;
using hc_test::JsonTree;

namespace {

constexpr uint16_t kNone = hc::rules::kNone;

// ------------------------------------------------------------------------------------------
// Helpers

struct Datapoints {
	std::vector<std::string> names;
	std::vector<hc::DatapointSpec> specs;

	void add(const std::string& name, hc::ValueType type, bool writable = true) {
		names.push_back(name);
		specs.push_back(hc::DatapointSpec{nullptr, type,
			static_cast<uint8_t>(hc::DatapointFlags::kReadable |
				(writable ? hc::DatapointFlags::kWritable | hc::DatapointFlags::kTransmit : 0))});
	}

	void load(hc::DatapointModel& model) {
		for (size_t i = 0; i < specs.size(); ++i) {
			specs[i].name = names[i].c_str();
		}
		REQUIRE(model.load(specs.data(), specs.size()) == hc::DatapointStatus::ok);
	}
};

struct NoSchedules : hc::rules::ScheduleDirectory {
	bool has_schedule(const char* id) const override { return std::strcmp(id, "morning") == 0; }
};

/// A model with the datapoints the refusal tests use.
struct Bench {
	hc_test::FakeClock clock;
	hc::DatapointModel model{clock};
	JsonTree tree;
	NoSchedules schedules;

	Bench() {
		Datapoints d;
		d.add("b", hc::ValueType::boolean);
		d.add("u", hc::ValueType::unsigned_int);
		d.add("r", hc::ValueType::real);
		d.add("s", hc::ValueType::string);
		d.add("t", hc::ValueType::time_of_day);
		d.add("ro", hc::ValueType::boolean, false);
		d.load(model);
	}

	LoadResult load(const std::string& text, RuleSet& set) {
		Json json;
		std::string error;
		REQUIRE_MESSAGE(hc_test::parse_json(text, json, error), error << "\n" << text);
		hc_test::ScopedCapture capture(hc::LogLevel::error);
		return set.load(tree.value(json), model, &schedules);
	}

	std::string refusal(const std::string& text) {
		RuleSet set;
		LoadResult result = load(text, set);
		return result.ok() ? "ok" : hc::rules::load_error_name(result.error);
	}
};

/// One rule around `reaction` (and optional extra members of the rule).
std::string rule_json(const std::string& reaction, const std::string& extra = "", const std::string& id = "x") {
	return R"({"id": ")" + id + R"(", "name": "X", )" + (extra.empty() ? "" : extra + ", ") + R"("reactions": [)" +
		reaction + "]}";
}

std::string image(const std::string& rules) { return "[" + rules + "]"; }

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
				for (size_t k = 0; k < a.expr; ++k) {
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

/// Loads one rule with the given condition (against the Bench model) and prints the condition.
std::string print_condition(Bench& bench, const std::string& condition, const std::string& extra = "") {
	RuleSet set;
	std::string rules = image(rule_json(
		R"({"triggers": [{"kind": "datapoint", "datapoint": "b"}], "condition": )" + condition + R"(, "actions": []})",
		extra));
	LoadResult result = bench.load(rules, set);
	REQUIRE_MESSAGE(result.ok(), std::string(result.path) << ": " << std::string(result.text));
	return print(set, bench.model, 0, set.reaction(0).condition);
}

} // namespace

// ------------------------------------------------------------------------------------------
// Printer

TEST_CASE("RulesPrinter round trip of every corpus expression") {
	size_t checked = 0;
	for (const std::string& path : hc_test::rule_corpus_files()) {
		std::string text, error;
		REQUIRE(hc_test::read_text(path, text));
		Json json;
		REQUIRE(hc_test::parse_json(text, json, error));
		if (json["expected"].has("loadRefused")) {
			continue;
		}
		hc_test::FakeClock clock;
		hc::DatapointModel model(clock);
		Datapoints d;
		for (const Json& p : json["datapoints"].items) {
			d.add(p["name"].text, hc::rules::datapoint_type_from_name(p["type"].text.c_str()),
				!p.has("writable") || p["writable"].boolean);
		}
		d.load(model);
		struct Schedules : hc::rules::ScheduleDirectory {
			const Json* ids;
			bool has_schedule(const char* id) const override {
				for (const Json& s : ids->items) {
					if (s.text == id) {
						return true;
					}
				}
				return false;
			}
		} schedules;
		schedules.ids = &json["schedules"];
		std::vector<const Json*> images{&json["rules"]};
		for (const Json& step : json["timeline"].items) {
			if (step.has("image")) {
				images.push_back(&step["image"]["rules"]);
			}
		}
		JsonTree tree;
		for (const Json* rules : images) {
			RuleSet set;
			LoadResult result = set.load(tree.value(*rules), model, &schedules);
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
	Bench bench;
	std::string lux_rule = R"("parameters": [{"id": "darkBelow", "label": "Dark", "type": "real", "default": 50.0}])";
	CHECK(print_condition(bench,
			  bin("and", dp("b"), bin("or", dp("b"), bin("<", dp("r"), R"({"kind": "parameter", "id": "darkBelow"})"))),
			  lux_rule) == "@b and (@b or @r < $darkBelow)");
	CHECK(print_condition(bench,
			  bin(">", R"({"kind": "call", "function": "remaining", "args": [{"kind": "timer", "id": "off"}]})",
				  R"({"kind": "literal", "type": "duration", "value": 90000})"),
			  R"("timers": [{"id": "off", "duration": {"kind": "literal", "type": "duration", "value": 1000}}])") ==
		"remaining(off) > 1min30s");
	// a - (b - c), a - b - c, a or (b or c)
	CHECK(print_condition(bench, bin("==", bin("-", lit_int(1), bin("-", lit_int(2), lit_int(3))), lit_int(0))) ==
		"1 - (2 - 3) == 0");
	CHECK(print_condition(bench, bin("==", bin("-", bin("-", lit_int(1), lit_int(2)), lit_int(3)), lit_int(0))) ==
		"1 - 2 - 3 == 0");
	CHECK(print_condition(bench, bin("or", dp("b"), bin("or", dp("b"), dp("b")))) == "@b or (@b or @b)");
	// a comparison under a comparison; -(-5); -(a + b); not binds looser than comparisons
	CHECK(print_condition(bench, bin("==", bin("<", lit_int(1), lit_int(2)), lit_bool(true))) == "(1 < 2) == true");
	CHECK(print_condition(bench, bin("==", un("-", lit_int(-5)), lit_int(5))) == "-(-5) == 5");
	CHECK(print_condition(bench, bin("==", un("-", bin("+", lit_int(1), lit_int(2))), lit_int(-3))) == "-(1 + 2) == -3");
	CHECK(print_condition(bench, un("not", bin("==", dp("u"), lit_int(1)))) == "not @u == 1");
	CHECK(print_condition(bench, bin("==", un("not", dp("b")), lit_bool(false))) == "(not @b) == false");
	CHECK(print_condition(bench, bin("==", bin("*", bin("+", lit_int(1), lit_int(2)), lit_int(3)), lit_int(9))) ==
		"(1 + 2) * 3 == 9");
	CHECK(print_condition(bench, bin("==", dp("s"), R"({"kind": "literal", "type": "string", "value": "say \"hi\"\n"})")) ==
		R"(@s == "say \"hi\"\n")");
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

// ------------------------------------------------------------------------------------------
// Load checks (spec 10.1)

TEST_CASE("RulesLoad refusal reasons") {
	Bench bench;
	std::string ok_reaction = write_reaction("b", lit_bool(true));
	CHECK(bench.refusal(image(rule_json(ok_reaction))) == "ok");

	// malformed: not an array, unknown member, unknown kind, unknown function, missing reactions
	CHECK(bench.refusal("{}") == "malformed");
	CHECK(bench.refusal(image(rule_json(ok_reaction, R"("colour": 1)"))) == "malformed");
	CHECK(bench.refusal(image(rule_json(write_reaction("b", R"({"kind": "magic"})")))) == "malformed");
	CHECK(bench.refusal(image(rule_json(write_reaction("b", R"({"kind": "call", "function": "nope", "args": []})")))) ==
		"malformed");
	CHECK(bench.refusal(image(R"({"id": "x", "name": "X"})")) == "malformed");
	CHECK(bench.refusal(image(rule_json(write_reaction("b", bin("**", lit_int(1), lit_int(2)))))) == "malformed");

	// duplicateId: two rules, two timers, the reserved parameter id
	CHECK(bench.refusal(image(rule_json(ok_reaction) + "," + rule_json(ok_reaction))) == "duplicateId");
	CHECK(bench.refusal(image(rule_json(ok_reaction,
			  R"("timers": [{"id": "t", "duration": {"kind": "literal", "type": "duration", "value": 1000}}, )"
			  R"({"id": "t", "duration": {"kind": "literal", "type": "duration", "value": 1000}}])"))) == "duplicateId");
	CHECK(bench.refusal(image(rule_json(ok_reaction,
			  R"("parameters": [{"id": "enabled", "label": "E", "type": "boolean", "default": true}])"))) == "duplicateId");

	// unknownReference: datapoint, schedule, parameter, timer, state of another rule
	CHECK(bench.refusal(image(rule_json(write_reaction("nope", lit_bool(true))))) == "unknownReference");
	CHECK(bench.refusal(image(rule_json(R"({"triggers": [{"kind": "schedule", "schedule": "evening"}], "actions": []})"))) ==
		"unknownReference");
	CHECK(bench.refusal(image(rule_json(R"({"triggers": [{"kind": "schedule", "schedule": "morning"}], "actions": []})"))) ==
		"ok");
	CHECK(bench.refusal(image(rule_json(write_reaction("b", R"({"kind": "parameter", "id": "p"})")))) == "unknownReference");
	CHECK(bench.refusal(image(rule_json(R"({"triggers": [{"kind": "timer", "timer": "t"}], "actions": []})"))) ==
		"unknownReference");
	CHECK(bench.refusal(image(rule_json(write_reaction("b",
			  bin("==", R"({"kind": "call", "function": "state", "args": [{"kind": "literal", "type": "string", "value": "zz"}]})",
				  R"({"kind": "literal", "type": "string", "value": "on"})"))))) == "unknownReference");

	// typeError: arithmetic on a boolean, a non-boolean condition, a write that cannot convert, a
	// timer duration that is not a duration
	CHECK(bench.refusal(image(rule_json(write_reaction("u", bin("+", lit_bool(true), lit_int(1)))))) == "typeError");
	CHECK(bench.refusal(image(rule_json(write_reaction("b", lit_bool(true), lit_int(1))))) == "typeError");
	CHECK(bench.refusal(image(rule_json(write_reaction("b", lit_int(1))))) == "typeError");
	CHECK(bench.refusal(image(rule_json(ok_reaction, R"("timers": [{"id": "t", "duration": )" + lit_int(5) + "}]"))) ==
		"typeError");

	// invalidLiteral: 25:00, a level of 256, a string of 29 bytes, an integer beyond 2^53-1
	CHECK(bench.refusal(image(rule_json(write_reaction("t",
			  R"({"kind": "literal", "type": "timeOfDay", "value": {"hour": 25, "minute": 0, "second": 0}})")))) ==
		"invalidLiteral");
	CHECK(bench.refusal(image(rule_json(write_reaction("b",
			  bin("==", R"({"kind": "call", "function": "red", "args": [{"kind": "literal", "type": "color", "value": {"red": 256, "green": 0, "blue": 0}}]})",
				  lit_int(1)))))) == "invalidLiteral");
	CHECK(bench.refusal(image(rule_json(write_reaction("s",
			  R"({"kind": "literal", "type": "string", "value": "12345678901234567890123456789"})")))) == "invalidLiteral");
	CHECK(bench.refusal(image(rule_json(write_reaction("u", lit_int(9007199254740992LL))))) == "invalidLiteral");

	// invalidParameter: default outside min..max, min > max, choices on a boolean
	CHECK(bench.refusal(image(rule_json(ok_reaction,
			  R"("parameters": [{"id": "p", "label": "P", "type": "integer", "default": 11, "min": 0, "max": 10}])"))) ==
		"invalidParameter");
	CHECK(bench.refusal(image(rule_json(ok_reaction,
			  R"("parameters": [{"id": "p", "label": "P", "type": "integer", "default": 5, "min": 10, "max": 0}])"))) ==
		"invalidParameter");
	CHECK(bench.refusal(image(rule_json(ok_reaction,
			  R"("parameters": [{"id": "p", "label": "P", "type": "boolean", "default": true, "choices": [{"value": true, "label": "Y"}]}])"))) ==
		"invalidParameter");

	// notWritable
	CHECK(bench.refusal(image(rule_json(write_reaction("ro", lit_bool(true))))) == "notWritable");

	// forbidden: time function in a match, a reference to enabled, `to` without states,
	// elseActions with `to`, a timer node outside running/remaining, state() with a non-literal
	CHECK(bench.refusal(image(rule_json(
			  R"({"triggers": [{"kind": "match", "condition": )" +
			  bin(">", R"({"kind": "call", "function": "time", "args": []})",
				  R"({"kind": "literal", "type": "timeOfDay", "value": {"hour": 6, "minute": 0, "second": 0}})") +
			  R"(}], "actions": []})"))) == "forbidden");
	CHECK(bench.refusal(image(rule_json(write_reaction("b", R"({"kind": "parameter", "id": "enabled"})")))) == "forbidden");
	CHECK(bench.refusal(image(rule_json(R"({"triggers": [{"kind": "datapoint", "datapoint": "b"}], "to": "on", "actions": []})"))) ==
		"forbidden");
	std::string states = R"("states": [{"id": "on"}, {"id": "off"}], "initialState": "off")";
	CHECK(bench.refusal(image(rule_json(
			  R"({"triggers": [{"kind": "datapoint", "datapoint": "b"}], "condition": )" + dp("b") +
				  R"(, "to": "on", "actions": [], "elseActions": []})",
			  states))) == "forbidden");
	CHECK(bench.refusal(image(rule_json(write_reaction("b", bin("==", R"({"kind": "timer", "id": "t"})", lit_int(1))),
			  R"("timers": [{"id": "t", "duration": {"kind": "literal", "type": "duration", "value": 1000}}])"))) ==
		"forbidden");
	CHECK(bench.refusal(image(rule_json(write_reaction("b",
			  bin("==", R"({"kind": "call", "function": "state", "args": [{"kind": "datapoint", "name": "s"}]})",
				  R"({"kind": "literal", "type": "string", "value": "on"})")),
			  states))) == "forbidden");
}

TEST_CASE("RulesLoad refusal names rule and path") {
	Bench bench;
	RuleSet set;
	LoadResult result = bench.load(image(rule_json(write_reaction("b", lit_bool(true))) + "," +
		rule_json(write_reaction("b", lit_int(1)), "", "second")), set);
	CHECK(result.error == hc::rules::LoadError::type_error);
	CHECK(std::string(result.rule) == "second");
	CHECK(std::string(result.path) == "rules[1].reactions[0].actions[0].value");
	CHECK(std::strlen(result.text) > 0);
	CHECK(set.empty()); // a refusal changes nothing
}

// ------------------------------------------------------------------------------------------
// Limits (spec 10.2)

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
	return image(s);
}

} // namespace

TEST_CASE("RulesLoad limits") {
	Bench bench;
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

	CHECK(bench.refusal(rules_json(128)) == "ok");
	CHECK(bench.refusal(rules_json(129)) == "limitExceeded");
	CHECK(bench.refusal(image(rule_json(many(16, reaction)))) == "ok");
	CHECK(bench.refusal(image(rule_json(many(17, reaction)))) == "limitExceeded");
	CHECK(bench.refusal(image(rule_json(R"({"triggers": [)" + many(8, trig) + R"(], "actions": []})"))) == "ok");
	CHECK(bench.refusal(image(rule_json(R"({"triggers": [)" + many(9, trig) + R"(], "actions": []})"))) == "limitExceeded");
	CHECK(bench.refusal(image(rule_json(R"({"triggers": [)" + trig + R"(], "actions": [)" + many(16, act) + "]}"))) == "ok");
	CHECK(bench.refusal(image(rule_json(R"({"triggers": [)" + trig + R"(], "actions": [)" + many(17, act) + "]}"))) ==
		"limitExceeded");
	CHECK(bench.refusal(image(rule_json(reaction, params(7)))) == "ok"); // with enabled: 8
	CHECK(bench.refusal(image(rule_json(reaction, params(8)))) == "limitExceeded");
	CHECK(bench.refusal(image(rule_json(reaction, timers(8)))) == "ok");
	CHECK(bench.refusal(image(rule_json(reaction, timers(9)))) == "limitExceeded");
	CHECK(bench.refusal(image(rule_json(reaction, states(16)))) == "ok");
	CHECK(bench.refusal(image(rule_json(reaction, states(17)))) == "limitExceeded");
	// nodes per expression: a balanced tree of 63 nodes (depth 6) and one of 127
	CHECK(bench.refusal(image(rule_json(write_reaction("u", balanced(5))))) == "ok");
	CHECK(bench.refusal(image(rule_json(write_reaction("u", balanced(6))))) == "limitExceeded");
	// depth: a chain of 12 literals has depth 12; 13 is refused
	CHECK(bench.refusal(image(rule_json(write_reaction("u", sum_chain(12))))) == "ok");
	CHECK(bench.refusal(image(rule_json(write_reaction("u", sum_chain(13))))) == "limitExceeded");
	// arguments per call
	CHECK(bench.refusal(image(rule_json(write_reaction("u",
			  R"({"kind": "call", "function": "max", "args": [)" + many(8, lit_int(1)) + "]}")))) == "ok");
	CHECK(bench.refusal(image(rule_json(write_reaction("u",
			  R"({"kind": "call", "function": "max", "args": [)" + many(9, lit_int(1)) + "]}")))) == "limitExceeded");
	// parts of a log message
	auto log_action = [](size_t parts) {
		return R"({"triggers": [{"kind": "datapoint", "datapoint": "b"}], "actions": [{"kind": "log", "level": "info", "message": [)" +
			many(parts, R"("x")") + "]}]}";
	};
	CHECK(bench.refusal(image(rule_json(log_action(8)))) == "ok");
	CHECK(bench.refusal(image(rule_json(log_action(9)))) == "limitExceeded");
}

TEST_CASE("RulesLoad node budget of the image") {
	Bench bench;
	// 65 reactions' worth of 63-node values: 5 rules of 13 reactions = 65 * 63 = 4095 nodes plus
	// the 65 write targets need no nodes; one more node is refused.
	std::string ok_rules, over_rules;
	for (size_t i = 0; i < 5; ++i) {
		ok_rules += (i ? "," : "") + rule_json(many(13, write_reaction("u", balanced(5))), "", "n" + std::to_string(i));
	}
	CHECK(bench.refusal(image(ok_rules)) == "ok");
	over_rules = ok_rules + "," + rule_json(write_reaction("u", bin("+", lit_int(1), lit_int(1))), "", "last");
	CHECK(bench.refusal(image(over_rules)) == "limitExceeded");
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
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc::DatapointModel model(clock);
	Datapoints d;
	d.add("b", hc::ValueType::boolean);
	d.load(model);
	hc_test::FakeSettings settings;
	struct NoTime : hc::rules::TimeSource {
		bool local_now(hc::DateTime&) const override { return false; }
		bool sun_time(hc::rules::SunEvent, hc::TimeOfDay&) const override { return false; }
	} time;
	Engine engine(loop, model, clock, settings, time);
	std::string param = R"("parameters": [{"id": "p", "label": "P", "type": "integer", "default": 1}])";
	std::string reaction = R"({"triggers": [{"kind": "datapoint", "datapoint": "b"}], "actions": []})";
	Json json;
	std::string error;
	REQUIRE(hc_test::parse_json(image(rule_json(reaction, param, "r6432d85626ca") + "," +
		rule_json(reaction, param, "re7a81faa983a")), json, error));
	JsonTree tree;
	hc_test::ScopedCapture capture(hc::LogLevel::error);
	LoadResult result = engine.load(tree.value(json), nullptr);
	CHECK(result.error == hc::rules::LoadError::limit_exceeded);
	CHECK(std::string(result.text).find("collide") != std::string::npos);
	CHECK_FALSE(engine.loaded());
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
	std::unique_ptr<Engine> engine;
	JsonTree tree;
	Json json;
	std::vector<std::string> logs;
	size_t commands = 0;

	void rule_logged(const char* rule, hc::rules::LogCode code, hc::LogLevel, const char* text) override {
		logs.push_back(std::string(rule) + " " + hc::rules::log_code_name(code) + ": " + text);
	}
	void rule_commanded(const char*, const hc::Datapoint&, const hc::Value&) override { ++commands; }

	void setup(Datapoints& d, const std::string& rules) {
		d.load(model);
		for (size_t i = 0; i < model.size(); ++i) {
			REQUIRE(model.bind(static_cast<uint16_t>(i), technology) == hc::DatapointStatus::ok);
		}
		engine = std::make_unique<Engine>(loop, model, clock, settings, time);
		engine->set_observer(this);
		std::string error;
		REQUIRE_MESSAGE(hc_test::parse_json(rules, json, error), error);
		LoadResult result = engine->load(tree.value(json), nullptr);
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
	Datapoints d;
	d.add("go", hc::ValueType::boolean);
	d.add("n", hc::ValueType::unsigned_int);
	d.add("other", hc::ValueType::boolean);
	// `count` writes n + 1 whenever n changes: each write is a derived event for itself.
	std::string rules = image(
		rule_json(R"({"triggers": [{"kind": "datapoint", "datapoint": "go"}], "actions": [{"kind": "write", "datapoint": "n", "value": )" +
			lit_int(0) + R"(}]}, {"triggers": [{"kind": "datapoint", "datapoint": "n"}], "actions": [{"kind": "write", "datapoint": "n", "value": )" +
			bin("+", dp("n"), lit_int(1)) + "}]}",
			"", "count") + "," +
		rule_json(R"({"triggers": [{"kind": "datapoint", "datapoint": "other"}], "actions": []})", "", "quiet"));
	hc_test::ScopedCapture capture(hc::LogLevel::error);
	bench.setup(d, rules);
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

namespace {

/// A typical image: `count` rules of a presence light (a parameter, a timer, two reactions).
std::string typical_image(size_t count) {
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
	return image(s);
}

Datapoints typical_datapoints(size_t count) {
	Datapoints d;
	for (size_t i = 0; i < count; ++i) {
		d.add("in" + std::to_string(i), hc::ValueType::boolean);
		d.add("lux" + std::to_string(i), hc::ValueType::real);
		d.add("out" + std::to_string(i), hc::ValueType::boolean);
	}
	return d;
}

} // namespace

TEST_CASE("RulesEngine memory accounting of a 50-rule image") {
	EngineBench bench;
	Datapoints d = typical_datapoints(50);
	bench.setup(d, typical_image(50));
	const RuleSet& set = bench.engine->rules();
	hc::rules::RuleSetMemory m = set.memory();
	hc::rules::EngineMemory e = bench.engine->memory();
	CHECK(sizeof(hc::rules::Node) == 16);
	CHECK(m.nodes == set.node_count() * sizeof(hc::rules::Node));
	size_t sections = m.rules + m.reactions + m.triggers + m.actions + m.log_parts + m.parameters + m.choices + m.timers +
		m.states + m.nodes + m.constants + m.schedules + m.tracked + m.matches + m.strings;
	CHECK(m.block >= sections);
	CHECK(m.block <= sections + 16 * 8); // alignment padding only
	size_t runtime = e.per_rule * set.rule_count() + e.per_parameter * set.parameter_count() +
		e.per_timer * set.timer_count() + e.per_node * set.node_count() + e.per_tracked * set.tracked_count();
	CHECK(e.runtime >= runtime);
	CHECK(e.runtime <= runtime + 64 + bench.model.size() / 8 + 8 * 8 + 2 * set.match_count() + 64);
	MESSAGE("50 rules, " << set.node_count() << " nodes, " << set.parameter_count() << " parameters, "
		<< set.timer_count() << " timers: rule set block " << m.block << " bytes (nodes " << m.nodes << ", strings "
		<< m.strings << ", constants " << m.constants << "), runtime block " << e.runtime << " bytes, engine object "
		<< e.object << " bytes; per rule " << e.per_rule << ", per parameter " << e.per_parameter << ", per timer "
		<< e.per_timer << ", per node " << sizeof(hc::rules::Node) << "+" << e.per_node << ", per tracked datapoint "
		<< e.per_tracked);
	CHECK(m.block + e.runtime < 64 * 1024);
}

TEST_CASE("RulesEngine allocates nothing while processing events") {
	EngineBench bench;
	Datapoints d = typical_datapoints(10);
	bench.setup(d, typical_image(10));
	bench.report("lux3", hc::Value::of_real(10.0));
	g_allocations = 0;
	g_counting = true;
	for (int round = 0; round < 3; ++round) {
		bench.report("in3", hc::Value::of_bool(true));
		bench.report("in3", hc::Value::of_bool(false));
		bench.advance(200000); // the timer expires
	}
	g_counting = false;
	CHECK(g_allocations == 0);
	CHECK(bench.commands >= 6);
}

TEST_CASE("RulesEngine diagnostics texts") {
	EngineBench bench;
	Datapoints d = typical_datapoints(1);
	bench.setup(d, typical_image(1));
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
