/* Copyright (c) 2026 Bernhard Haumacher */
// A tiny element builder: h("div", {class: "card"}, "text", child). Strings become text nodes,
// so nothing from the device or a language pack is ever parsed as HTML.

export function h(tag, attributes, ...children) {
	const element = document.createElement(tag);
	if (attributes) {
		for (const [name, value] of Object.entries(attributes)) {
			if (value === undefined || value === null || value === false) {
				continue;
			}
			if (name.startsWith("on") && typeof value === "function") {
				element.addEventListener(name.slice(2), value);
			} else if (name === "class") {
				element.className = value;
			} else if (value === true) {
				element.setAttribute(name, "");
			} else {
				element.setAttribute(name, String(value));
			}
		}
	}
	append(element, children);
	return element;
}

function append(element, children) {
	for (const child of children) {
		if (child === undefined || child === null || child === false) {
			continue;
		}
		if (Array.isArray(child)) {
			append(element, child);
		} else if (child instanceof Node) {
			element.appendChild(child);
		} else {
			element.appendChild(document.createTextNode(String(child)));
		}
	}
}

const SVG = "http://www.w3.org/2000/svg";

/// An icon from a 24x24 path.
export function icon(path) {
	const svg = document.createElementNS(SVG, "svg");
	svg.setAttribute("viewBox", "0 0 24 24");
	svg.setAttribute("aria-hidden", "true");
	const p = document.createElementNS(SVG, "path");
	p.setAttribute("d", path);
	svg.appendChild(p);
	return svg;
}

export const icons = {
	bulb: "M9 21h6v-1H9zm3-19a7 7 0 0 0-4 12.7V17a1 1 0 0 0 1 1h6a1 1 0 0 0 1-1v-2.3A7 7 0 0 0 12 2z",
	toggle: "M17 7H7a5 5 0 0 0 0 10h10a5 5 0 0 0 0-10zm0 8a3 3 0 1 1 0-6 3 3 0 0 1 0 6z",
	gauge: "M12 4a9 9 0 0 0-9 9h2a7 7 0 0 1 14 0h2a9 9 0 0 0-9-9zm-1 10.7a1.5 1.5 0 1 0 2 0V8h-2z",
	text: "M5 4v3h5.5v12h3V7H19V4z",
	clock: "M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zm1 10.4 3.2 3.2-1.4 1.4L11 13.2V6h2z",
	palette: "M12 3a9 9 0 0 0 0 18c1 0 1.5-.7 1.5-1.5 0-.4-.2-.8-.4-1-.3-.3-.4-.6-.4-1 0-.8.7-1.5 1.5-1.5H16a5 5 0 0 0 5-5c0-4.4-4-8-9-8zM6.5 12a1.5 1.5 0 1 1 0-3 1.5 1.5 0 0 1 0 3zm3-4a1.5 1.5 0 1 1 0-3 1.5 1.5 0 0 1 0 3zm5 0a1.5 1.5 0 1 1 0-3 1.5 1.5 0 0 1 0 3zm3 4a1.5 1.5 0 1 1 0-3 1.5 1.5 0 0 1 0 3z",
	power: "M11 3h2v10h-2zm5.6 2.4 1.4-1.4A9 9 0 1 1 6 4l1.4 1.4a7 7 0 1 0 9.2 0z",
	up: "M7 14l5-5 5 5z",
	down: "M7 10l5 5 5-5z",
	stop: "M8 8h8v8H8z",
	plus: "M11 5h2v6h6v2h-6v6h-2v-6H5v-2h6z",
	minus: "M5 11h14v2H5z",
	play: "M8 5v14l11-7z",
	touch: "M9 11.2V7.5a2.5 2.5 0 0 1 5 0v3.7a4.5 4.5 0 1 0-5 0zM18.8 15.8 14.3 13.6A1 1 0 0 0 14 13.5h-.5V7.5a1.5 1.5 0 0 0-3 0v10.7l-3.4-.7-.8.8 4.6 4.7h6.8l.9-6.1a1.1 1.1 0 0 0-.8-1.1z",
	thermometer: "M15 13V5a3 3 0 0 0-6 0v8a5 5 0 1 0 6 0zm-3-9a1 1 0 0 1 1 1v3h-2V5a1 1 0 0 1 1-1z",
	blind: "M4 3h16v2h-1v12h1v2h-7v1.3a1.5 1.5 0 1 1-2 0V19H4v-2h1V5H4zm3 2v2h10V5zm0 4v2h10V9zm0 4v2h10v-2z",
	rule: "M7 4h10v2H7zm-3 5h16v2H4zm3 5h10v2H7zm-3 5h16v2H4z",
	calendar: "M7 2h2v2h6V2h2v2h3v18H4V4h3zm-1 8v10h12V10zm2 2h3v3H8z",
	close: "M6.4 5 12 10.6 17.6 5 19 6.4 13.4 12l5.6 5.6-1.4 1.4-5.6-5.6L6.4 19 5 17.6 10.6 12 5 6.4z",
	data: "M12 3C7 3 4 4.8 4 7v10c0 2.2 3 4 8 4s8-1.8 8-4V7c0-2.2-3-4-8-4zm0 2c3.9 0 6 1.3 6 2s-2.1 2-6 2-6-1.3-6-2 2.1-2 6-2z",
};

/// Replaces the children of `element`.
export function replace(element, ...children) {
	element.textContent = "";
	append(element, children);
}
