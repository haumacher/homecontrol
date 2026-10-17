/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "serial_mux.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#error "hc_improv shares the console's USB Serial/JTAG port: set CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y"
#endif

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.serial";

/// Driver buffers: a frame fits the receive buffer; a scan's result frames (about 1 KB in a burst)
/// fit the transmit buffer.
constexpr uint32_t kRxBuffer = 512;
constexpr uint32_t kTxBuffer = 1024;

/// Console input waiting for the REPL.
constexpr size_t kConsoleBuffer = 256;

/// Reader task: small, its buffers live in the SerialMux object; freed when Improv ends.
constexpr uint32_t kReaderStack = 3072;
constexpr UBaseType_t kReaderPriority = 4;

/// Read timeout of the reader task: the granularity of ImprovReader::poll() and of noticing the end.
constexpr TickType_t kReadTicks = pdMS_TO_TICKS(20);

/// How long the reader waits for the loop to take the previous frame, and how long a frame write or
/// a console push may block.
constexpr TickType_t kMailboxWait = pdMS_TO_TICKS(10);
constexpr int kMailboxTries = 100;
constexpr TickType_t kWriteTicks = pdMS_TO_TICKS(100);
constexpr TickType_t kPushTicks = pdMS_TO_TICKS(100);

// ---- The console input device (REPL task) ------------------------------------------------------

StaticStreamBuffer_t g_stream_control;
uint8_t g_stream_storage[kConsoleBuffer + 1];
StreamBufferHandle_t g_stream = nullptr;
/// Set when the reader task is gone (or never ran): the device reads the driver itself.
std::atomic<bool> g_direct{true};
std::atomic<bool> g_nonblocking{false};

/// CR -> LF, as the IDF console's VFS does for linenoise (which ends a line on LF).
void translate_line_endings(uint8_t* bytes, size_t length) {
	for (size_t i = 0; i < length; ++i) {
		if (bytes[i] == '\r') {
			bytes[i] = '\n';
		}
	}
}

int console_open(const char*, int, int) {
	return 0;
}

int console_close(int) {
	return 0;
}

int console_fstat(int, struct stat* st) {
	std::memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR;
	return 0;
}

int console_fcntl(int, int cmd, int arg) {
	if (cmd == F_GETFL) {
		return O_RDONLY | (g_nonblocking.load() ? O_NONBLOCK : 0);
	}
	if (cmd == F_SETFL) {
		g_nonblocking.store((arg & O_NONBLOCK) != 0);
		return 0;
	}
	errno = EINVAL;
	return -1;
}

ssize_t console_read(int, void* dst, size_t size) {
	if (size == 0) {
		return 0;
	}
	uint8_t* out = static_cast<uint8_t*>(dst);
	for (;;) {
		bool nonblocking = g_nonblocking.load();
		bool direct = g_direct.load();
		// What the reader task released comes first, also after it is gone.
		size_t n = xStreamBufferReceive(g_stream, out, size, (nonblocking || direct) ? 0 : pdMS_TO_TICKS(50));
		if (n > 0) {
			return static_cast<ssize_t>(n);
		}
		if (direct) {
			int r = usb_serial_jtag_read_bytes(out, size, nonblocking ? 0 : portMAX_DELAY);
			if (r > 0) {
				translate_line_endings(out, static_cast<size_t>(r));
				return r;
			}
		}
		if (nonblocking) {
			errno = EAGAIN;
			return -1;
		}
	}
}

} // namespace

bool SerialMux::init() {
	usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
	config.rx_buffer_size = kRxBuffer;
	config.tx_buffer_size = kTxBuffer;
	esp_err_t err = usb_serial_jtag_driver_install(&config);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "USB Serial/JTAG driver not installed: %s; no console input, no Improv", esp_err_to_name(err));
		return false;
	}
	// stdout (logs, console output) through the driver, with CRLF line endings.
	usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
	usb_serial_jtag_vfs_use_driver();
	fcntl(fileno(stdout), F_SETFL, 0);

	g_stream = xStreamBufferCreateStatic(kConsoleBuffer, 1, g_stream_storage, &g_stream_control);
	esp_vfs_t vfs = {};
	vfs.flags = ESP_VFS_FLAG_DEFAULT;
	vfs.open = &console_open;
	vfs.close = &console_close;
	vfs.read = &console_read;
	vfs.fstat = &console_fstat;
	vfs.fcntl = &console_fcntl;
	err = esp_vfs_register(kConsoleInput, &vfs, nullptr);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "console input device %s not registered: %s; no console input", kConsoleInput,
			esp_err_to_name(err));
		return false;
	}
	initialized_ = true;
	return true;
}

bool SerialMux::start_improv(LoopQueue& queue, Improv& improv, const char* device_name) {
	device_name_ = device_name;
	if (!initialized_) {
		ESP_LOGE(kTag, "Improv not started: the serial port is not set up (see the error above)");
		return false;
	}
	if (!improv.active()) {
		ESP_LOGI(kTag, "Improv not active: the console reads the port directly");
		return false;
	}
	queue_ = &queue;
	improv_ = &improv;
	stop_.store(false);
	g_direct.store(false);
	if (xTaskCreate(&reader_task, "hc_serial", kReaderStack, this, kReaderPriority, nullptr) != pdPASS) {
		g_direct.store(true);
		ESP_LOGE(kTag, "Improv reader task not created (stack %u B): Improv frames are not received",
			static_cast<unsigned>(kReaderStack));
		return false;
	}
	return true;
}

// ---- Reader task --------------------------------------------------------------------------------

void SerialMux::reader_task(void* arg) {
	static_cast<SerialMux*>(arg)->read_loop();
	vTaskDelete(nullptr);
}

void SerialMux::read_loop() {
	uint8_t buffer[64];
	while (!stop_.load()) {
		int n = usb_serial_jtag_read_bytes(buffer, sizeof(buffer), kReadTicks);
		uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
		for (int i = 0; i < n; ++i) {
			take(reader_.feed(buffer[i], now));
		}
		if (n <= 0) {
			take(reader_.poll(now));
		}
	}
	// Improv is over: release held text, then let the console read the driver itself.
	take(reader_.flush());
	g_direct.store(true);
	if (dropped_console_ > 0) {
		ESP_LOGW(kTag, "%u console input byte(s) were dropped while the console was busy",
			static_cast<unsigned>(dropped_console_));
	}
	ESP_LOGI(kTag, "Improv reader stopped: the console reads the port directly");
}

void SerialMux::take(ImprovReader::Event event) {
	switch (event) {
	case ImprovReader::Event::none:
		break;
	case ImprovReader::Event::console:
		push_console(reader_.console_data(), reader_.console_length());
		break;
	case ImprovReader::Event::frame: {
		int tries = 0;
		while (mailbox_full_.load() && tries++ < kMailboxTries) {
			vTaskDelay(kMailboxWait);
		}
		if (mailbox_full_.load()) {
			ESP_LOGW(kTag, "Improv frame dropped: the event loop did not take the previous one within 1 s");
			break;
		}
		mailbox_.type = reader_.frame_type();
		mailbox_.length = static_cast<uint8_t>(reader_.frame_length());
		std::memcpy(mailbox_.data, reader_.frame_data(), mailbox_.length);
		mailbox_full_.store(true);
		if (!queue_->post(&handle_frame, this, 0, nullptr, "Improv frame")) {
			mailbox_full_.store(false);
		}
		break;
	}
	case ImprovReader::Event::bad_frame:
		queue_->post(&handle_bad_frame, this, 0, reader_.bad_reason(), "bad Improv frame");
		break;
	}
}

void SerialMux::push_console(const uint8_t* bytes, size_t length) {
	uint8_t copy[improv::kHeaderSize];
	std::memcpy(copy, bytes, length);
	translate_line_endings(copy, length);
	size_t sent = xStreamBufferSend(g_stream, copy, length, kPushTicks);
	if (sent < length) {
		dropped_console_ += static_cast<uint32_t>(length - sent);
	}
}

// ---- Event loop ---------------------------------------------------------------------------------

void SerialMux::handle_frame(void* context, const LoopMessage&) {
	SerialMux* self = static_cast<SerialMux*>(context);
	self->improv_->handle_frame(self->mailbox_.type, self->mailbox_.data, self->mailbox_.length);
	self->mailbox_full_.store(false);
}

void SerialMux::handle_bad_frame(void* context, const LoopMessage& message) {
	static_cast<SerialMux*>(context)->improv_->bad_frame(message.text);
}

void SerialMux::send_frame(const uint8_t* frame, size_t length) {
	if (!initialized_ || length > improv::kMaxFrame) {
		return;
	}
	// One driver call per frame: newline, frame, newline. The clients find the magic only at the
	// start of a line; the trailing newline keeps the next log line off the frame.
	uint8_t framed[improv::kMaxFrame + 2];
	framed[0] = '\n';
	std::memcpy(framed + 1, frame, length);
	framed[length + 1] = '\n';
	if (usb_serial_jtag_write_bytes(framed, length + 2, kWriteTicks) == 0) {
		ESP_LOGW(kTag, "Improv frame not sent: the USB transmit buffer stayed full (nobody reading?)");
	}
}

void SerialMux::improv_ended() {
	stop_.store(true);
}

const char* SerialMux::chip_family() const {
	esp_chip_info_t info;
	esp_chip_info(&info);
	switch (info.model) {
	case CHIP_ESP32:
		return "ESP32";
	case CHIP_ESP32S2:
		return "ESP32-S2";
	case CHIP_ESP32S3:
		return "ESP32-S3";
	case CHIP_ESP32C3:
		return "ESP32-C3";
	case CHIP_ESP32C2:
		return "ESP32-C2";
	case CHIP_ESP32C6:
		return "ESP32-C6";
	case CHIP_ESP32H2:
		return "ESP32-H2";
	default:
		return "ESP32 (unknown model)";
	}
}

} // namespace esp
} // namespace hc
