/*
 * WiVRn VR streaming
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Rooted Quest Pro eye-camera stream over adb: pushes and starts the headset
// binaries in headset/ and reads their QPLIVE3 frames through adb forward.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace wivrn::qpro
{
struct camera_frame
{
	std::vector<uint8_t> strip;  // width = camera_ids.size() * 400, height 400, row-major 8-bit gray
	std::vector<int> camera_ids; // e.g. {0, 1}
	uint64_t headset_ns;         // headset CLOCK_MONOTONIC when the source frame was copied
	uint64_t sequence;           // headset source frame counter; gaps are skipped source frames
	uint64_t rejected_torn;      // torn source frames rejected on the headset so far
};

namespace detail
{
inline constexpr int camera_width = 400;
inline constexpr int camera_height = 400;
inline constexpr int stream_port = 27273;
inline constexpr size_t header_size = 64; // struct "<8sIIQQIIIIIIQ"

struct frame_header
{
	uint64_t sequence;
	uint64_t timestamp_ns;
	uint64_t rejected_torn;
	uint32_t width;
	uint32_t payload_size;
	std::vector<int> camera_ids;
};

std::vector<int> camera_ids_from_mask(uint32_t mask);
// Throws std::runtime_error unless raw is a QPLIVE3 v3 header with a supported layout.
frame_header parse_header(std::span<const uint8_t, header_size> raw);

struct adb_device
{
	std::string serial;
	bool usb;       // "usb:" in `adb devices -l`
	bool quest_pro; // product/device "seacliff"
};
// Devices in the "device" state from `adb devices -l` output.
std::vector<adb_device> parse_adb_devices(std::string_view output);
// USB Quest Pro > <headset_ip>:5555 > other wireless Quest Pro > single USB device.
// USB first: it takes no Wi-Fi bandwidth from the WiVRn stream.
std::optional<adb_device> select_device(const std::vector<adb_device> & devices, const std::string & headset_ip);

std::string relay_command(int max_fps); // headset relay, eye cameras only
std::string su_argument(std::string_view command); // one `adb shell` argument: su -c '<command>'

struct process_result
{
	int status; // exit code; -1 when killed (timeout) or not exited normally
	bool timed_out;
	std::string output; // stdout + stderr, bounded
};
// Runs argv (PATH lookup, no shell). Kills the child on timeout.
// Throws std::runtime_error("Stop requested") when cancel_fd becomes readable.
process_result run_process(const std::vector<std::string> & argv, std::chrono::milliseconds timeout, int cancel_fd = -1);

// QPLIVE3 TCP client for 127.0.0.1:port; reconnects while the relay comes up or after a drop.
class frame_stream
{
	int port;
	int cancel_fd;
	int fd = -1;
	bool streamed = false; // a frame arrived since construction
	std::optional<std::chrono::steady_clock::time_point> reconnect_deadline;
	std::array<uint8_t, header_size> header;
	std::optional<frame_header> current;
	std::vector<uint8_t> payload;
	size_t have = 0;

	void disconnect();
	bool connect_once(std::chrono::steady_clock::time_point deadline);

public:
	frame_stream(int port, int cancel_fd);
	frame_stream(const frame_stream &) = delete;
	frame_stream & operator=(const frame_stream &) = delete;
	~frame_stream();

	// nullopt on timeout or when cancel_fd is readable; throws on bad data or
	// when no connection comes up within 20 s (first) / 5 s (after a drop).
	std::optional<camera_frame> next(std::chrono::milliseconds timeout);
};
} // namespace detail

class camera_link
{
	std::string adb;
	std::string headset_ip;
	std::filesystem::path binaries_dir;
	int max_fps;

	std::string serial;
	bool wireless = false;
	bool relay_started = false;
	pid_t relay_session = -1; // wireless: long-lived `adb shell su -c relay`
	int relay_log = -1;       // memfd with the relay session output
	int stop_pipe[2] = {-1, -1};
	std::atomic<const std::string *> status_;
	std::unique_ptr<detail::frame_stream> stream;

	void set_status(const std::string & s);
	detail::process_result adb_call(std::vector<std::string> args, std::chrono::milliseconds timeout, bool cancellable = true);
	detail::process_result root_call(std::string_view command, std::chrono::milliseconds timeout, bool cancellable = true);
	void select_serial();
	void cleanup();

public:
	// adb: adb executable (PATH lookup); headset_ip: IPv4 for wireless adb (may be empty);
	// binaries_dir: folder with the 3 headset binaries; max_fps: 0..120, 0 = uncapped.
	camera_link(std::string adb, std::string headset_ip, std::filesystem::path binaries_dir, int max_fps = 24);
	camera_link(const camera_link &) = delete;
	camera_link & operator=(const camera_link &) = delete;
	~camera_link(); // relay --stop, forward --remove, relay session killed + reaped; ~12 s worst case

	void start(); // throws std::runtime_error with a user-readable message
	// nullopt on timeout or after request_stop(); throws on fatal stream loss.
	std::optional<camera_frame> next_frame(std::chrono::milliseconds timeout);
	// Thread-safe; blocked start()/next_frame() return promptly. Not after destruction began.
	void request_stop();
	const std::string & status() const;
};
} // namespace wivrn::qpro
