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
	data: "M12 3C7 3 4 4.8 4 7v10c0 2.2 3 4 8 4s8-1.8 8-4V7c0-2.2-3-4-8-4zm0 2c3.9 0 6 1.3 6 2s-2.1 2-6 2-6-1.3-6-2 2.1-2 6-2z",
};

/// Replaces the children of `element`.
export function replace(element, ...children) {
	element.textContent = "";
	append(element, children);
}
