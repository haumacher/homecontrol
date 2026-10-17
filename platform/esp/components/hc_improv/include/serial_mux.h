/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_SERIAL_MUX_H
#define HC_ESP_SERIAL_MUX_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "hc/improv.h"
#include "hc/improv_protocol.h"
#include "hc/platform/improv_port.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The USB Serial/JTAG port, shared by the console and Improv Wi-Fi serial (the phoneblock
/// dongle's split, extended by the console: the dongle has none).
///
/// The IDF REPL reads stdin itself, so it cannot share the port: this class installs the USB
/// Serial/JTAG driver, and while Improv is active a reader task reads the raw bytes (no line-ending
/// translation, Improv frames are binary) and splits them with hc::ImprovReader: complete frames go
/// through a one-frame mailbox and the LoopQueue to hc::Improv on the event loop, everything else
/// (with CR turned into LF, as the IDF console expects) into a stream buffer. The console's REPL
/// reads that buffer through the VFS device kConsoleInput. When Improv ends (improv_ended()), the
/// reader task releases what it holds and exits; the device then reads the driver directly.
///
/// Output: frames are written with one driver call each, framed by newlines (the clients look for
/// the magic at the start of a line); log lines and console output keep going through stdout and
/// the same driver, between frames.
class SerialMux : public ImprovPort {
public:
	/// The console's input device (open it as the REPL task's stdin).
	static constexpr const char* kConsoleInput = "/dev/hcin";

	/// Installs the driver, routes stdout through it and registers kConsoleInput. Call once at boot,
	/// before the console starts. Logs and returns false on failure.
	bool init();

	/// Starts the reader task that hands Improv frames to `improv` through `queue`. `device_name`
	/// must outlive the mux. Without this call (or if it fails, logged) the console reads the port
	/// directly and Improv receives nothing.
	bool start_improv(LoopQueue& queue, Improv& improv, const char* device_name);

	// ImprovPort (event loop)
	void send_frame(const uint8_t* frame, size_t length) override;
	void improv_ended() override;
	const char* chip_family() const override;
	const char* device_name() const override { return device_name_; }

private:
	static void reader_task(void* arg);
	static void handle_frame(void* context, const LoopMessage& message);
	static void handle_bad_frame(void* context, const LoopMessage& message);

	void read_loop();
	void take(ImprovReader::Event event);
	void push_console(const uint8_t* bytes, size_t length);

	/// A frame handed from the reader task to the loop. Written only while `mailbox_full_` is
	/// false, read only while it is true.
	struct Mailbox {
		uint8_t type;
		uint8_t length;
		uint8_t data[255];
	};

	LoopQueue* queue_ = nullptr;
	Improv* improv_ = nullptr;
	const char* device_name_ = "homecontrol";
	ImprovReader reader_;
	Mailbox mailbox_ = {};
	std::atomic<bool> mailbox_full_{false};
	std::atomic<bool> stop_{false};
	uint32_t dropped_console_ = 0;
	bool initialized_ = false;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_SERIAL_MUX_H
