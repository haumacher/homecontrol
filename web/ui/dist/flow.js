/* Copyright (c) 2026 Bernhard Haumacher */
// A grid of tiles in which every tile keeps its own height (haumacher/homecontrol#90): the grid has a
// fine row unit (.tiles.flow in style.css), and each tile spans as many units as it is high, measured
// in the browser and followed when it changes (a sparkline appearing, an error line, a font size).
// The grid places the tiles in the order of the page, each at the next free place (no dense
// packing), so small tiles flow next to a tall one. Without ResizeObserver the grid stays as it was:
// rows as high as their highest tile.

/// The row unit of a flowing grid in pixels; the same as grid-auto-rows of .tiles.flow.
const ROW = 4;

/// Makes the grid flow and returns the function that ends it. Tiles added later are followed too.
export function flowTiles(grid) {
	if (typeof ResizeObserver !== "function" || typeof MutationObserver !== "function") {
		return () => {};
	}
	grid.classList.add("flow");
	const span = (tile) => {
		// The vertical gap is part of the span: the grid has no row gap between its fine rows.
		const gap = parseFloat(getComputedStyle(grid).columnGap) || 0;
		const height = tile.getBoundingClientRect().height;
		const rows = Math.max(1, Math.ceil((height + gap) / ROW));
		const value = `span ${rows}`;
		if (tile.style.gridRowEnd !== value) {
			tile.style.gridRowEnd = value;
		}
	};
	const sizes = new ResizeObserver((entries) => entries.forEach((entry) => span(entry.target)));
	const watch = (node) => {
		if (node.nodeType === Node.ELEMENT_NODE) {
			sizes.observe(node);
		}
	};
	Array.from(grid.children).forEach(watch);
	const children = new MutationObserver((records) => {
		for (const record of records) {
			record.addedNodes.forEach(watch);
			record.removedNodes.forEach((node) => node.nodeType === Node.ELEMENT_NODE && sizes.unobserve(node));
		}
	});
	children.observe(grid, { childList: true });
	return () => {
		sizes.disconnect();
		children.disconnect();
		grid.classList.remove("flow");
	};
}
