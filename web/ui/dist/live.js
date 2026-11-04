/* Copyright (c) 2026 Bernhard Haumacher */
// The live connection (/ws, spec/api.md "WebSocket"): subscribes after the greeting, keeps the
// datapoints and the status current, sends commands and matches their results. Reconnects with a
// growing pause; a refusal is shown, not hidden. A ping every 20 s keeps the connection off the
// device's idle purge; a connection silent for 45 s (status heartbeats come every 10 s) is dead.

const PING_MS = 20000;
const SILENCE_MS = 45000;

export class Live extends EventTarget {
	constructor() {
		super();
		this.datapoints = new Map();
		this.status = null;
		this.state = "connecting";
		this.reason = "";
		this.snapshotDone = false;
		this.socket = null;
		this.wsPort = 0;
		this.retry = 1000;
		this.timer = null;
		this.nextRef = 1;
		this.pending = new Map();
		this.stopped = false;
		this.pinger = null;
		this.lastReceived = 0;
	}

	/// Opens the connection; `wsPort` 0 means the page's own port.
	start(wsPort) {
		this.wsPort = wsPort || 0;
		this.stopped = false;
		this.open();
	}

	stop() {
		this.stopped = true;
		clearTimeout(this.timer);
		clearInterval(this.pinger);
		if (this.socket) {
			this.socket.close(1000);
			this.socket = null;
		}
	}

	url() {
		const secure = location.protocol === "https:";
		const host = this.wsPort ? `${location.hostname.includes(":") ? `[${location.hostname}]` : location.hostname}:${this.wsPort}` : location.host;
		return `${secure ? "wss" : "ws"}://${host}/ws`;
	}

	setState(state, reason = "") {
		this.state = state;
		this.reason = reason;
		this.dispatchEvent(new CustomEvent("state", { detail: { state, reason } }));
	}

	open() {
		clearTimeout(this.timer);
		this.setState("connecting");
		let socket;
		try {
			socket = new WebSocket(this.url());
		} catch (e) {
			this.schedule();
			return;
		}
		this.socket = socket;
		this.lastReceived = Date.now();
		clearInterval(this.pinger);
		this.pinger = setInterval(() => this.keepAlive(socket), PING_MS);
		socket.onmessage = (event) => {
			this.lastReceived = Date.now();
			this.receive(event.data);
		};
		socket.onclose = (event) => {
			if (this.socket !== socket) {
				return;
			}
			this.socket = null;
			clearInterval(this.pinger);
			this.failPending();
			if (this.stopped) {
				return;
			}
			if (event.code === 1008) {
				this.dispatchEvent(new CustomEvent("unauthorized", { detail: this.reason }));
				this.setState("refused", this.reason);
				return;
			}
			if (this.state !== "refused") {
				this.setState("connecting");
			}
			this.schedule(event.code === 1013 ? 30000 : undefined);
		};
	}

	failPending() {
		for (const [, entry] of this.pending) {
			entry.reject({ code: "error.network" });
		}
		this.pending.clear();
	}

	keepAlive(socket) {
		if (this.socket !== socket) {
			return;
		}
		if (Date.now() - this.lastReceived > SILENCE_MS) {
			// Do not wait for the closing handshake of a dead connection: reconnect now.
			socket.onclose = null;
			socket.onmessage = null;
			socket.close(4000);
			this.socket = null;
			clearInterval(this.pinger);
			this.failPending();
			this.setState("connecting");
			this.schedule();
			return;
		}
		this.send({ type: "ping" });
	}

	schedule(delay) {
		const wait = delay || this.retry;
		this.retry = Math.min(this.retry * 2, 30000);
		this.timer = setTimeout(() => this.open(), wait);
	}

	send(message) {
		if (this.socket && this.socket.readyState === WebSocket.OPEN) {
			this.socket.send(JSON.stringify(message));
			return true;
		}
		return false;
	}

	/// Commands a datapoint; resolves on success, rejects with {code, error} on a refusal.
	command(id, value) {
		return new Promise((resolve, reject) => {
			const ref = this.nextRef++;
			if (!this.send({ type: "command", id, value, ref })) {
				reject({ code: "error.network" });
				return;
			}
			this.pending.set(ref, { resolve, reject });
		});
	}

	receive(text) {
		let message;
		try {
			message = JSON.parse(text);
		} catch (e) {
			return;
		}
		switch (message.type) {
			case "hello":
				if (message.authenticated) {
					this.send({ type: "subscribe" });
				} else {
					this.dispatchEvent(new CustomEvent("unauthorized", { detail: "" }));
				}
				break;
			case "status":
				this.retry = 1000;
				this.status = message;
				if (this.state !== "connected") {
					this.setState("connected");
				}
				this.dispatchEvent(new CustomEvent("status", { detail: message }));
				break;
			case "datapoints":
				this.merge(message);
				break;
			case "zigbee":
				this.dispatchEvent(new CustomEvent("zigbee", { detail: message }));
				break;
			case "result": {
				const entry = this.pending.get(message.ref);
				if (entry) {
					this.pending.delete(message.ref);
					if (message.ok) {
						entry.resolve(message);
					} else {
						entry.reject({ code: message.code, reason: message.error });
					}
				}
				break;
			}
			case "error":
				this.reason = message.code || "";
				this.lastError = message;
				if (message.code === "ws.too_many_clients" || message.code === "ws.foreign_origin") {
					this.setState("refused", message.code);
				}
				this.dispatchEvent(new CustomEvent("error", { detail: message }));
				break;
		}
	}

	/// How long ago the datapoint was updated, in ms (by the device's clock); null if never.
	ageMs(dp) {
		if (!dp || !dp.updated_ms || this.offset === undefined) {
			return null;
		}
		return Math.max(0, Date.now() - this.offset - dp.updated_ms);
	}

	merge(message) {
		if (message.now_ms !== undefined) {
			this.offset = Date.now() - message.now_ms;
		}
		if (message.snapshot) {
			if (message.from === 0) {
				this.seen = new Set();
			}
			for (const dp of message.updates) {
				this.datapoints.set(dp.id, dp);
				this.seen && this.seen.add(dp.id);
			}
			if (message.done) {
				for (const id of [...this.datapoints.keys()]) {
					if (this.seen && !this.seen.has(id)) {
						this.datapoints.delete(id);
					}
				}
				this.snapshotDone = true;
			}
			this.dispatchEvent(new CustomEvent("datapoints", { detail: { all: true } }));
			return;
		}
		const ids = [];
		for (const update of message.updates) {
			const dp = this.datapoints.get(update.id);
			if (dp) {
				dp.value = update.value;
				dp.quality = update.quality;
				dp.updated_ms = update.updated_ms;
				dp.pending = update.pending === true;
				ids.push(update.id);
			}
		}
		this.dispatchEvent(new CustomEvent("datapoints", { detail: { ids } }));
	}
}
