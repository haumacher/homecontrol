/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/rules/printer.h"

#include <cmath>
#include <cstring>

namespace hc::rules {

namespace {

/// Binding levels of spec 4.4 (primary: literal, reference, call).
enum Level : int { kOr = 1, kAnd = 2, kNot = 3, kCompare = 4, kAdd = 5, kMul = 6, kNeg = 7, kPrimary = 8 };

int level_of(const Node& node) {
	switch (node.kind) {
		case NodeKind::unary: return static_cast<Op>(node.op) == Op::neg ? kNeg : kNot;
		case NodeKind::binary:
			switch (static_cast<Op>(node.op)) {
				case Op::or_: return kOr;
				case Op::and_: return kAnd;
				case Op::eq: case Op::ne: case Op::lt: case Op::le: case Op::gt: case Op::ge: return kCompare;
				case Op::add: case Op::sub: return kAdd;
				default: return kMul;
			}
		default: return kPrimary;
	}
}

struct Printer {
	const RuleSet& set;
	const DatapointModel& model;
	const Rule& rule;
	char* buffer;
	size_t size;
	size_t length = 0;

	void put(char c) {
		if (length + 1 < size) {
			buffer[length] = c;
			buffer[length + 1] = '\0';
		}
		++length;
	}

	void put(const char* text) {
		while (*text) {
			put(*text++);
		}
	}

	void put_quoted(const char* text) {
		put('"');
		for (; *text; ++text) {
			switch (*text) {
				case '"': put("\\\""); break;
				case '\\': put("\\\\"); break;
				case '\n': put("\\n"); break;
				case '\t': put("\\t"); break;
				default: put(*text); break;
			}
		}
		put('"');
	}

	Val literal(const Node& node) const {
		switch (node.type) {
			case Type::boolean: return Val::of_bool(node.lit.b);
			case Type::integer: return Val::of_int(node.lit.i);
			case Type::duration: return Val::of_duration(node.lit.i);
			case Type::real: return Val::of_real(node.lit.r);
			case Type::enumeration: return Val::of_enum(node.lit.e);
			case Type::time_of_day: return Val::of_time(node.lit.tod);
			case Type::date: return Val::of_date(node.lit.date);
			case Type::controlled: return Val::of_controlled(node.lit.ctl.control, node.lit.ctl.value);
			case Type::color: return Val::of_color(node.lit.color);
			default: return set.constant(node.a);
		}
	}

	void operand(uint16_t index, bool parens) {
		if (parens) {
			put('(');
		}
		print(index);
		if (parens) {
			put(')');
		}
	}

	void print(uint16_t index) {
		const Node& node = set.node(index);
		switch (node.kind) {
			case NodeKind::literal: {
				char text[96];
				literal_text(literal(node), text, sizeof(text));
				put(text);
				break;
			}
			case NodeKind::datapoint: {
				const Datapoint* dp = model.get(node.a);
				const char* name = dp ? dp->name() : "?";
				put('@');
				if (bare_datapoint_name(name)) {
					put(name);
				} else {
					put_quoted(name);
				}
				break;
			}
			case NodeKind::parameter:
				put('$');
				put(set.str(set.parameter(rule.first_parameter + node.a).id));
				break;
			case NodeKind::timer: put(set.str(set.timer(rule.first_timer + node.a).id)); break;
			case NodeKind::unary: {
				const Node& child = set.node(node.a);
				if (static_cast<Op>(node.op) == Op::not_) {
					put("not ");
					operand(node.a, level_of(child) < kNot);
				} else {
					bool starts_with_minus = child.kind == NodeKind::literal &&
						((child.type == Type::integer && child.lit.i < 0) ||
							(child.type == Type::duration && child.lit.i < 0) ||
							(child.type == Type::real && std::signbit(child.lit.r)));
					bool simple = child.kind == NodeKind::literal || child.kind == NodeKind::datapoint ||
						child.kind == NodeKind::parameter || child.kind == NodeKind::call;
					put('-');
					operand(node.a, !simple || starts_with_minus);
				}
				break;
			}
			case NodeKind::binary: {
				int level = level_of(node);
				const Node& left = set.node(node.a);
				const Node& right = set.node(node.a + 1);
				int left_level = level_of(left);
				int right_level = level_of(right);
				operand(node.a, left_level < level || (level == kCompare && left_level == kCompare));
				put(' ');
				put(operator_text(static_cast<Op>(node.op)));
				put(' ');
				operand(static_cast<uint16_t>(node.a + 1), right_level <= level);
				break;
			}
			case NodeKind::call:
				put(function_name(static_cast<Fn>(node.op)));
				put('(');
				for (uint8_t i = 0; i < node.argc; ++i) {
					if (i > 0) {
						put(", ");
					}
					print(static_cast<uint16_t>(node.a + i));
				}
				put(')');
				break;
		}
	}
};

bool letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }

} // namespace

bool bare_datapoint_name(const char* name) {
	// identifier { "." (letter | digit | "_")+ }, the identifier not a keyword.
	const char* p = name;
	if (!(letter(*p) || *p == '_')) {
		return false;
	}
	const char* start = p;
	while (letter(*p) || digit(*p) || *p == '_') {
		++p;
	}
	size_t n = static_cast<size_t>(p - start);
	static const char* const kKeywords[] = {"and", "or", "not", "true", "false"};
	for (const char* keyword : kKeywords) {
		if (std::strlen(keyword) == n && std::strncmp(start, keyword, n) == 0) {
			return false;
		}
	}
	while (*p == '.') {
		++p;
		if (!(letter(*p) || digit(*p) || *p == '_')) {
			return false;
		}
		while (letter(*p) || digit(*p) || *p == '_') {
			++p;
		}
	}
	return *p == '\0';
}

size_t print_expression(const RuleSet& set, const DatapointModel& model, size_t rule, uint16_t root, char* out,
	size_t size) {
	if (size > 0) {
		out[0] = '\0';
	}
	Printer printer{set, model, set.rule(rule), out, size};
	printer.print(root);
	return printer.length;
}

} // namespace hc::rules
