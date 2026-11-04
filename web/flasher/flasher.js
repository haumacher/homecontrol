/*
 * Copyright (c) 2026 Bernhard Haumacher
 *
 * The web flasher's pages: shows the hint for browsers without Web Serial, the version the install
 * button offers, and (on the published site, which has releases.json) a choice of the releases on
 * the site. The page's <main> carries data-base, the site root relative to the page.
 */

const main = document.querySelector("main");
const base = main.dataset.base || "";
const button = document.querySelector("esp-web-install-button");
const versionLine = document.getElementById("install-version");
const picker = document.getElementById("install-picker");
const select = picker ? picker.querySelector("select") : null;

if (!("serial" in navigator)) {
	const box = document.getElementById("install-unsupported");
	if (box) {
		box.hidden = false;
	}
} else {
	fixSerialForEspressifUsb();
}

/**
 * Adapts ESP Web Tools 10.4.0 to the ESP32-C6 on its own USB port (USB Serial/JTAG), where it fails
 * without help (measured on the bench, doc/ota.md "The web flasher"):
 *
 * - The browser's port dialog lists every serial device (a laptop's mobile modem, USB adapters)
 *   under names a user cannot judge: requestPort gets the filter of Espressif's vendor id, the
 *   chip's own USB port, over which the installation, the WiFi setup (Improv) and the console speak.
 * - After the installation ESP Web Tools resets the chip with the RTS line, which on this port starts
 *   the ROM's download mode again instead of the firmware: HomeControl does not start, and the WiFi
 *   setup never comes. The page resets the chip with its watchdog instead (what esptool does on the
 *   ESP32-C3; esptool leaves it out on the C6 because the USB port disappears with the reset), waits
 *   for the board to come back on USB and hands ESP Web Tools the port again, which then finds
 *   HomeControl's Improv and offers the WiFi setup. If the board does not come back, the page asks
 *   for RESET and continues as soon as it is there.
 */
function fixSerialForEspressifUsb() {
	const ESPRESSIF = 0x303a;
	const notice = document.getElementById("install-notice");
	const isEspressif = (port) => port.getInfo().usbVendorId === ESPRESSIF;

	const requestPort = navigator.serial.requestPort.bind(navigator.serial);
	navigator.serial.requestPort = (options = {}) =>
		requestPort({ ...options, filters: options.filters && options.filters.length ? options.filters : [{ usbVendorId: ESPRESSIF }] });

	// ESP Web Tools opens the port again 100 ms after the installation has finished, to look for
	// Improv: that open first starts the firmware.
	const done = new WeakSet();
	const open = SerialPort.prototype.open;
	SerialPort.prototype.open = async function (options) {
		const dialog = document.querySelector("ewt-install-dialog");
		const install = dialog && dialog.port === this ? dialog._installState : null;
		if (!install || install.state !== "finished" || done.has(install)) {
			return open.call(this, options);
		}
		done.add(install);
		await open.call(this, options);
		const back = nextEspressifPort();
		if (!(await watchdogReset(this))) {
			// No download mode: the firmware runs already.
			back.cancel();
			return undefined;
		}
		const late = setTimeout(() => {
			if (notice) {
				notice.hidden = false;
			}
		}, 8000);
		const port = await back.port;
		clearTimeout(late);
		if (notice) {
			notice.hidden = true;
		}
		dialog.port = port;
		return open.call(port, options);
	};

	// The next Espressif board that appears on USB.
	function nextEspressifPort() {
		let onConnect;
		const port = new Promise((resolve) => {
			onConnect = (event) => {
				if (isEspressif(event.target)) {
					navigator.serial.removeEventListener("connect", onConnect);
					resolve(event.target);
				}
			};
			navigator.serial.addEventListener("connect", onConnect);
		});
		return { port, cancel: () => navigator.serial.removeEventListener("connect", onConnect) };
	}
}

/**
 * Resets the chip with its watchdog, if its ROM's download mode answers on the open port (the serial
 * protocol of esptool: SLIP frames, SYNC, WRITE_REG on the LP watchdog of the ESP32-C6). Closes the
 * port after a reset and leaves it open otherwise.
 *
 * @return whether the download mode answered, so that the chip resets now
 */
async function watchdogReset(port) {
	const SYNC = 0x08;
	const WRITE_REG = 0x09;
	const LP_WDT = 0x600b1c00;
	const WDT_KEY = 0x50d83aa1;
	const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

	const writer = port.writable.getWriter();
	const reader = port.readable.getReader();
	const answers = [];
	let frame = null;
	let escape = false;
	const reading = (async () => {
		try {
			for (;;) {
				const { value, done } = await reader.read();
				if (done) {
					return;
				}
				for (const b of value) {
					if (b === 0xc0) {
						if (frame && frame.length >= 2 && frame[0] === 0x01) {
							answers.push(frame[1]);
						}
						frame = [];
					} else if (frame) {
						if (escape) {
							frame.push(b === 0xdc ? 0xc0 : b === 0xdd ? 0xdb : b);
							escape = false;
						} else if (b === 0xdb) {
							escape = true;
						} else {
							frame.push(b);
						}
					}
				}
			}
		} catch (e) {
			// the port went away with the reset
		}
	})();

	const u32 = (v) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
	const send = async (op, data) => {
		const packet = [0x00, op, data.length & 0xff, data.length >> 8, 0, 0, 0, 0, ...data];
		const slip = [0xc0];
		for (const b of packet) {
			slip.push(...(b === 0xc0 ? [0xdb, 0xdc] : b === 0xdb ? [0xdb, 0xdd] : [b]));
		}
		slip.push(0xc0);
		await writer.write(new Uint8Array(slip));
	};
	const answered = async (op, ms) => {
		for (const deadline = Date.now() + ms; Date.now() < deadline; await sleep(20)) {
			if (answers.includes(op)) {
				return true;
			}
		}
		return false;
	};
	const writeReg = (address, value) => send(WRITE_REG, [...u32(address), ...u32(value), ...u32(0xffffffff), ...u32(0)]);

	let rom = false;
	try {
		for (let i = 0; i < 3 && !rom; i++) {
			await send(SYNC, [0x07, 0x07, 0x12, 0x20, ...new Array(32).fill(0x55)]);
			rom = await answered(SYNC, 300);
		}
		if (rom) {
			await writeReg(LP_WDT + 0x18, WDT_KEY); // unlock
			await writeReg(LP_WDT + 0x04, 2000); // timeout
			await writeReg(LP_WDT + 0x00, (1 << 31) | (5 << 28) | (1 << 8) | 2); // enable: reset the system
			await writeReg(LP_WDT + 0x18, 0); // lock
			await answered(WRITE_REG, 300);
		}
	} catch (e) {
		// the port went away with the reset
	}
	try {
		await reader.cancel();
	} catch (e) {
		// already gone
	}
	await reading;
	reader.releaseLock();
	writer.releaseLock();
	if (rom) {
		try {
			await port.close();
		} catch (e) {
			// already gone
		}
	}
	return rom;
}

function showVersion(manifestUrl) {
	fetch(manifestUrl, { cache: "no-cache" })
		.then((response) => (response.ok ? response.json() : Promise.reject(new Error(response.status))))
		.then((manifest) => {
			versionLine.textContent = versionLine.dataset.label.replace("{version}", manifest.version);
		})
		.catch(() => {
			versionLine.textContent = versionLine.dataset.missing;
		});
}

function setManifest(manifestUrl) {
	button.setAttribute("manifest", manifestUrl);
	showVersion(manifestUrl);
}

showVersion(button.getAttribute("manifest"));

fetch(base + "releases.json", { cache: "no-cache" })
	.then((response) => (response.ok ? response.json() : null))
	.then((site) => {
		if (!site || !select || site.releases.length < 2) {
			return;
		}
		for (const release of site.releases) {
			const option = document.createElement("option");
			option.value = release.version;
			option.textContent = release.version
				+ (release.version === site.stable ? " (" + select.dataset.stable + ")" : "")
				+ (release.prerelease ? " (" + select.dataset.prerelease + ")" : "");
			option.selected = release.version === site.stable;
			select.append(option);
		}
		select.addEventListener("change", () => {
			setManifest(select.value === site.stable
				? base + "manifest.json"
				: base + "firmware/" + select.value + "/install.json");
		});
		picker.hidden = false;
	})
	.catch(() => {});
