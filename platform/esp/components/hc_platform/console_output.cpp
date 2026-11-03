/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "console_output.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/reent.h>
#include <sys/stat.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_log.h"
#include "esp_rom_caps.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/output_ring.h"
#include "sdkconfig.h"

#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#error "the console output path writes the USB Serial/JTAG port: set CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y"
#endif

#if ESP_ROM_NEEDS_SWSETUP_WORKAROUND
extern "C" int __swsetup_r(struct _reent*, FILE*);
#endif

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.console";

/// Driver buffers. Receive: an Improv frame fits. Transmit: unused (the writer polls the FIFO
/// itself, see kPacket), the smallest the driver accepts.
constexpr uint32_t kRxBuffer = 512;
constexpr uint32_t kTxBuffer = 64;

/// The writer: below the serial reader (4) and the event loop (5), so input is read first and the
/// loop is never preempted by output. Its stack holds only the driver call: 304 B used on the C6
/// (2026-11-03, 45 min of logging with a USB host attached). Kept at 2048 B: with 1536 B (issue #45)
/// it overflowed in the marker's snprintf (since formatted without printf); a printf of the ROM reserves 1.25 KB at once (_svfprintf_r 1072 + snprintf 176 B), mostly never written, so the high-water mark does not show it (#48).
constexpr uint32_t kWriterStack = 2048;
constexpr UBaseType_t kWriterPriority = 3;

/// The writer transmits by polling the port's FIFO, one USB packet at a time, and never uses the
/// driver's interrupt-driven transmit path: the driver's handler leaves the "IN empty" interrupt
/// asserted while the host has not taken the previous packet (the FIFO is empty but not writable),
/// so with a host that polls slowly the interrupt fires back to back and starves every task until
/// the host's next poll (issue #46, measured on the board: the loop missed its timers for most of
/// the time output flowed, and the task watchdog ended it). Polling costs the writer one tick of
/// sleep per packet it cannot place; a host that reads keeps up with the logs.
constexpr size_t kPacket = 64;

/// Queued bytes the writer could not hand over for this long: nobody reads the port.
constexpr TickType_t kUnreadTicks = pdMS_TO_TICKS(1000);

/// The stdio buffer newlib gives stdout (st_blksize): a log line usually reaches write() whole.
constexpr int kStdioBuffer = 256;

uint8_t g_storage[ConsoleOutput::kCapacity];
OutputRing g_ring(g_storage, sizeof(g_storage), true); // CRLF, as the IDF's console wrote it
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
uint64_t g_dropped_unread = 0;
/// When the writer last made progress, or output started after the queue had run empty (ticks):
/// a second without either while bytes are queued means nobody reads the port.
TickType_t g_progress = 0;

StaticTask_t g_writer_tcb;
StackType_t g_writer_stack[kWriterStack];
TaskHandle_t g_writer = nullptr;
bool g_driver = false;

void wake_writer() {
	if (g_writer == nullptr) {
		return;
	}
	if (xPortInIsrContext()) {
		BaseType_t woken = pdFALSE;
		vTaskNotifyGiveFromISR(g_writer, &woken);
		portYIELD_FROM_ISR(woken);
	} else {
		xTaskNotifyGive(g_writer);
	}
}

/// Whether anybody reads the port, judged before an append (inside the lock).
bool unread(bool host, TickType_t now) {
	return !host || (g_ring.queued() > 0 && now - g_progress > kUnreadTicks);
}

/// Bookkeeping after an append (inside the lock): output starting on an empty queue restarts the
/// progress clock; drops count as unread when nobody read.
void appended(size_t queued_before, uint32_t dropped, bool was_unread, TickType_t now) {
	if (queued_before == 0) {
		g_progress = now;
	}
	if (dropped > 0 && was_unread) {
		g_dropped_unread += dropped;
	}
}

void writer_task(void*) {
	for (;;) {
		const uint8_t* data = nullptr;
		portENTER_CRITICAL(&g_lock);
		size_t n = g_ring.peek(&data);
		portEXIT_CRITICAL(&g_lock);
		if (n == 0) {
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			continue;
		}
		// Kept off every round: anything that enables it (the driver's own write path) brings the
		// handler back between the writer's packets.
		usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
		if (!usb_serial_jtag_ll_txfifo_writable()) {
			vTaskDelay(1); // the host has not taken the previous packet yet
			continue;
		}
		if (n > kPacket) {
			n = kPacket;
		}
		// The producers only write behind the queued bytes: the run stays valid outside the lock.
		int written = usb_serial_jtag_ll_write_txfifo(data, static_cast<uint32_t>(n));
		usb_serial_jtag_ll_txfifo_flush();
		if (written <= 0) {
			vTaskDelay(1);
			continue;
		}
		TickType_t now = xTaskGetTickCount();
		portENTER_CRITICAL(&g_lock);
		g_ring.consume(static_cast<size_t>(written));
		size_t left = g_ring.queued();
		g_progress = now;
		portEXIT_CRITICAL(&g_lock);
		if (static_cast<size_t>(written) == kPacket && left == 0) {
			// A full packet ends a transfer only with a short one after it: the host holds the data
			// until then. Send the empty packet once the full one is taken.
			for (int tries = 0; tries < 50 && !usb_serial_jtag_ll_txfifo_writable(); ++tries) {
				vTaskDelay(1);
			}
			if (usb_serial_jtag_ll_txfifo_writable()) {
				usb_serial_jtag_ll_txfifo_flush();
			}
		}
	}
}

// ---- The output device (stdout, stderr) --------------------------------------------------------

int out_open(const char*, int, int) {
	return 0;
}

int out_close(int) {
	return 0;
}

int out_fstat(int, struct stat* st) {
	std::memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR; // a terminal: newlib buffers stdout by line
	st->st_blksize = kStdioBuffer;
	return 0;
}

int out_fsync(int) {
	return 0; // the bytes are queued; the writer task sends them
}

int out_fcntl(int, int cmd, int) {
	if (cmd == F_GETFL) {
		return O_WRONLY;
	}
	if (cmd == F_SETFL) {
		return 0;
	}
	errno = EINVAL;
	return -1;
}

ssize_t out_write(int, const void* data, size_t size) {
	bool host = usb_serial_jtag_is_connected();
	TickType_t now = xTaskGetTickCount();
	portENTER_CRITICAL(&g_lock);
	size_t before = g_ring.queued();
	bool was_unread = unread(host, now);
	appended(before, g_ring.append_text(static_cast<const char*>(data), size), was_unread, now);
	portEXIT_CRITICAL(&g_lock);
	wake_writer();
	return static_cast<ssize_t>(size); // dropped lines are counted, never retried by stdio
}

/// Points an open standard stream at the output device (the FILE object stays the same, so every
/// task's stdout follows: the tasks share the global streams).
bool redirect(FILE* stream, const char* name) {
	std::fflush(stream);
	if (std::freopen(ConsoleOutput::kOutputDevice, "w", stream) == nullptr) {
		// freopen closed the old stream: bring it back on the IDF console, then say so.
		std::freopen("/dev/console", "w", stream);
		ESP_LOGE(kTag, "%s not redirected to %s (errno %d): it writes the port directly and may block",
			name, ConsoleOutput::kOutputDevice, errno);
		return false;
	}
#if ESP_ROM_NEEDS_SWSETUP_WORKAROUND
	// As the IDF does at start-up: set the stream up with the toolchain's code before a ROM printf
	// function would (esp_newlib_init_global_stdio()).
	__swsetup_r(_GLOBAL_REENT, stream);
#endif
	return true;
}

} // namespace

bool ConsoleOutput::start() {
	usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
	config.rx_buffer_size = kRxBuffer;
	config.tx_buffer_size = kTxBuffer;
	esp_err_t err = usb_serial_jtag_driver_install(&config);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "USB Serial/JTAG driver not installed: %s; console output stays synchronous, no console "
			"input, no Improv", esp_err_to_name(err));
		return false;
	}
	g_driver = true;
	// What still reads or writes the IDF console's device goes through the driver as well.
	usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
	usb_serial_jtag_vfs_use_driver();
	// The driver enables the "IN empty" interrupt at install; with nothing queued its handler answers
	// every one with an empty packet and keeps it enabled, which also keeps the FIFO busy whenever the
	// writer looks (and storms while the host is slow, see kPacket). The writer transmits by polling
	// and keeps that interrupt off.
	usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);

	g_writer = xTaskCreateStatic(&writer_task, "hc_out", kWriterStack, nullptr, kWriterPriority, g_writer_stack,
		&g_writer_tcb);
	esp_vfs_t vfs = {};
	vfs.flags = ESP_VFS_FLAG_DEFAULT;
	vfs.open = &out_open;
	vfs.close = &out_close;
	vfs.write = &out_write;
	vfs.fstat = &out_fstat;
	vfs.fsync = &out_fsync;
	vfs.fcntl = &out_fcntl;
	err = esp_vfs_register(kOutputDevice, &vfs, nullptr);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "console output device %s not registered: %s; stdout writes the port directly and may block",
			kOutputDevice, esp_err_to_name(err));
		return true;
	}
	if (redirect(stdout, "stdout") && redirect(stderr, "stderr")) {
		ESP_LOGI(kTag, "console output through a %u B queue and the writer task (driver buffer %u B)",
			static_cast<unsigned>(kCapacity), static_cast<unsigned>(kTxBuffer));
	}
	return true;
}

bool ConsoleOutput::driver_installed() const {
	return g_driver;
}

bool ConsoleOutput::append_frame(const uint8_t* frame, size_t length) {
	if (g_writer == nullptr) {
		return false;
	}
	bool host = usb_serial_jtag_is_connected();
	TickType_t now = xTaskGetTickCount();
	portENTER_CRITICAL(&g_lock);
	size_t before = g_ring.queued();
	bool was_unread = unread(host, now);
	uint32_t dropped = g_ring.append_record(frame, length);
	appended(before, dropped, was_unread, now);
	portEXIT_CRITICAL(&g_lock);
	wake_writer();
	return dropped == 0;
}

ConsoleOutputInfo ConsoleOutput::info() const {
	ConsoleOutputInfo out;
	portENTER_CRITICAL(&g_lock);
	out.dropped = g_ring.dropped();
	out.dropped_unread = g_dropped_unread;
	out.queued = static_cast<uint32_t>(g_ring.queued());
	out.capacity = static_cast<uint32_t>(g_ring.capacity());
	portEXIT_CRITICAL(&g_lock);
	out.host = g_driver && usb_serial_jtag_is_connected();
	return out;
}

void ConsoleOutput::drain(uint32_t timeout_ms) {
	if (g_writer == nullptr) {
		return;
	}
	std::fflush(stdout);
	TickType_t start = xTaskGetTickCount();
	while (usb_serial_jtag_is_connected() && pdTICKS_TO_MS(xTaskGetTickCount() - start) < timeout_ms) {
		portENTER_CRITICAL(&g_lock);
		size_t queued = g_ring.queued();
		portEXIT_CRITICAL(&g_lock);
		if (queued == 0) {
			vTaskDelay(pdMS_TO_TICKS(20)); // the last packet goes out in a USB transfer or two
			return;
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

ConsoleOutput& console_output() {
	static ConsoleOutput instance;
	return instance;
}

} // namespace esp
} // namespace hc
