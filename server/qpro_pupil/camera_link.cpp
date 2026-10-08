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

#include "camera_link.h"

#include <algorithm>
#include <arpa/inet.h>
#include <bit>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

extern char ** environ;

namespace wivrn::qpro
{
using namespace std::chrono_literals;
using steady = std::chrono::steady_clock;

namespace
{
static_assert(std::endian::native == std::endian::little, "QPLIVE3 is little-endian");

// Headset paths.
const std::string remote_tmp = "/data/local/tmp";
const std::string streamer_name = "libquestpro-camera-streamer-v8.so";
const std::string relay_name = "questpro-camera-relay-v8";
const std::string injector_name = "questpro-camera-injector";
const std::string remote_streamer = remote_tmp + "/" + streamer_name;
const std::string remote_relay = remote_tmp + "/" + relay_name;
const std::string remote_injector = remote_tmp + "/" + injector_name;
const std::string remote_relay_log = remote_tmp + "/questpro-relay-v8.log";
const std::string forward_spec = "tcp:" + std::to_string(detail::stream_port);

const std::string status_idle = "idle";
const std::string status_adb = "connecting adb";
const std::string status_root = "checking root";
const std::string status_push = "pushing binaries";
const std::string status_relay = "starting relay";
const std::string status_inject = "injecting";
const std::string status_connect = "connecting stream";
const std::string status_streaming = "streaming";
const std::string status_stopping = "stopping";
const std::string status_stopped = "stopped";
const std::string status_failed = "failed";

constexpr size_t output_limit = 64 * 1024;

template <typename T>
T read_le(const uint8_t * p)
{
	T value;
	std::memcpy(&value, p, sizeof(value));
	return value;
}

std::string tail(const std::string & text, size_t n = 600)
{
	std::string s = text.size() > n ? "..." + text.substr(text.size() - n) : text;
	while (not s.empty() and (s.back() == '\n' or s.back() == '\r' or s.back() == ' '))
		s.pop_back();
	return s;
}

int remaining_ms(steady::time_point deadline, std::chrono::milliseconds cap)
{
	auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - steady::now());
	return std::clamp<int64_t>(std::min(left, cap).count(), 0, INT_MAX);
}

steady::time_point deadline_after(std::chrono::milliseconds timeout)
{
	if (timeout >= std::chrono::hours(24))
		return steady::time_point::max();
	return steady::now() + std::max(timeout, 0ms);
}

bool readable(int fd)
{
	if (fd < 0)
		return false;
	pollfd p{.fd = fd, .events = POLLIN, .revents = 0};
	return poll(&p, 1, 0) > 0;
}

// Sleeps up to duration; returns false when cancel_fd became readable.
bool wait_cancellable(int cancel_fd, std::chrono::milliseconds duration)
{
	auto deadline = deadline_after(duration);
	for (;;)
	{
		int ms = remaining_ms(deadline, 1h);
		pollfd p{.fd = cancel_fd, .events = POLLIN, .revents = 0};
		int r = poll(&p, cancel_fd >= 0 ? 1 : 0, ms);
		if (r > 0)
			return false;
		if (r == 0 or errno != EINTR)
			return true;
	}
}

pid_t spawn(const std::vector<std::string> & argv, int out_fd)
{
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, out_fd, 1);
	posix_spawn_file_actions_adddup2(&actions, out_fd, 2);
#ifdef __GLIBC__
#if __GLIBC_PREREQ(2, 34)
	// adb may fork its server daemon from this child: keep our sockets out of it.
	posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
#endif

	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	sigset_t mask;
	sigemptyset(&mask);
	posix_spawnattr_setsigmask(&attr, &mask);
	sigset_t defaults;
	sigemptyset(&defaults);
	for (int s: {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGCHLD})
		sigaddset(&defaults, s);
	posix_spawnattr_setsigdefault(&attr, &defaults);
	// Own process group: a terminal Ctrl+C must not kill the relay session
	// before its clean --stop.
	posix_spawnattr_setpgroup(&attr, 0);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);

	std::vector<char *> args;
	for (const auto & a: argv)
		args.push_back(const_cast<char *>(a.c_str()));
	args.push_back(nullptr);

	pid_t pid = -1;
	int err = posix_spawnp(&pid, args[0], &actions, &attr, args.data(), environ);
	posix_spawnattr_destroy(&attr);
	posix_spawn_file_actions_destroy(&actions);
	if (err)
		throw std::runtime_error("Cannot run " + argv[0] + ": " + std::strerror(err));
	return pid;
}

void reap(pid_t pid)
{
	while (waitpid(pid, nullptr, 0) < 0 and errno == EINTR)
		;
}

// Waits up to grace for pid to exit, then SIGTERM, then SIGKILL; always reaps.
void kill_and_reap(pid_t pid, std::chrono::milliseconds grace)
{
	auto try_reap = [pid](std::chrono::milliseconds within) {
		auto deadline = steady::now() + within;
		for (;;)
		{
			int status;
			pid_t r = waitpid(pid, &status, WNOHANG);
			if (r == pid or (r < 0 and errno != EINTR))
				return true;
			if (steady::now() >= deadline)
				return false;
			usleep(20'000);
		}
	};
	if (try_reap(grace))
		return;
	kill(pid, SIGTERM);
	if (try_reap(500ms))
		return;
	kill(pid, SIGKILL);
	reap(pid);
}

} // namespace

namespace detail
{
std::vector<int> camera_ids_from_mask(uint32_t mask)
{
	std::vector<int> ids;
	for (int id = 0; id < 5; ++id)
		if (mask & (1u << id))
			ids.push_back(id);
	return ids;
}

frame_header parse_header(std::span<const uint8_t, header_size> raw)
{
	const uint8_t * p = raw.data();
	if (std::memcmp(p, "QPLIVE3\0", 8) != 0 or read_le<uint32_t>(p + 8) != 3 or read_le<uint32_t>(p + 12) != header_size)
		throw std::runtime_error("Unexpected live-stream header from the headset relay");

	frame_header h{
	        .sequence = read_le<uint64_t>(p + 16),
	        .timestamp_ns = read_le<uint64_t>(p + 24),
	        .rejected_torn = read_le<uint64_t>(p + 56),
	        .width = read_le<uint32_t>(p + 32),
	        .payload_size = read_le<uint32_t>(p + 48),
	        .camera_ids = camera_ids_from_mask(read_le<uint32_t>(p + 52)),
	};
	uint32_t height = read_le<uint32_t>(p + 36);
	uint32_t stride = read_le<uint32_t>(p + 40);
	uint32_t pixel_format = read_le<uint32_t>(p + 44);
	if (h.camera_ids.empty() or h.width != h.camera_ids.size() * camera_width or height != camera_height or stride != h.width or pixel_format != 1)
		throw std::runtime_error("Unsupported frame layout " + std::to_string(h.width) + "x" + std::to_string(height) + ", format " + std::to_string(pixel_format));
	if (h.payload_size != h.width * height)
		throw std::runtime_error("Invalid payload size from the headset relay");
	return h;
}

std::vector<adb_device> parse_adb_devices(std::string_view output)
{
	std::vector<adb_device> devices;
	while (not output.empty())
	{
		auto eol = output.find('\n');
		std::string_view line = output.substr(0, eol);
		output = eol == output.npos ? std::string_view{} : output.substr(eol + 1);

		std::vector<std::string_view> tokens;
		for (size_t i = 0; i < line.size();)
		{
			if (line[i] == ' ' or line[i] == '\t' or line[i] == '\r')
			{
				++i;
				continue;
			}
			size_t end = line.find_first_of(" \t\r", i);
			if (end == line.npos)
				end = line.size();
			tokens.push_back(line.substr(i, end - i));
			i = end;
		}
		if (tokens.size() < 2 or tokens[1] != "device")
			continue;
		adb_device d{.serial = std::string(tokens[0]), .usb = false, .quest_pro = false};
		for (auto t: tokens)
		{
			d.usb = d.usb or t.starts_with("usb:");
			d.quest_pro = d.quest_pro or t == "product:seacliff" or t == "device:seacliff" or t == "model:Quest_Pro";
		}
		devices.push_back(std::move(d));
	}
	return devices;
}

std::optional<adb_device> select_device(const std::vector<adb_device> & devices, const std::string & headset_ip)
{
	auto find = [&](auto pred) -> std::optional<adb_device> {
		auto it = std::ranges::find_if(devices, pred);
		if (it == devices.end())
			return std::nullopt;
		return *it;
	};
	if (auto d = find([](const adb_device & d) { return d.usb and d.quest_pro; }))
		return d;
	if (not headset_ip.empty())
		if (auto d = find([&](const adb_device & d) { return d.serial == headset_ip + ":5555"; }))
			return d;
	if (auto d = find([](const adb_device & d) { return not d.usb and d.quest_pro; }))
		return d;
	if (std::ranges::count_if(devices, [](const adb_device & d) { return d.usb; }) == 1)
		return find([](const adb_device & d) { return d.usb; });
	return std::nullopt;
}

std::string relay_command(int max_fps)
{
	if (max_fps < 0 or max_fps > 120)
		throw std::invalid_argument("max_fps must be 0..120");
	return remote_relay + " --mode eyes --max-fps " + std::to_string(max_fps);
}

std::string su_argument(std::string_view command)
{
	// Single-quoted for the headset shell: su runs the whole command as root.
	if (command.find('\'') != command.npos)
		throw std::logic_error("quote in headset command");
	return "su -c '" + std::string(command) + "'";
}

process_result run_process(const std::vector<std::string> & argv, std::chrono::milliseconds timeout, int cancel_fd)
{
	int pipe_fds[2];
	if (pipe2(pipe_fds, O_CLOEXEC) != 0)
		throw std::system_error(errno, std::system_category(), "pipe2");
	pid_t pid;
	try
	{
		pid = spawn(argv, pipe_fds[1]);
	}
	catch (...)
	{
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		throw;
	}
	close(pipe_fds[1]);
	int out = pipe_fds[0];
	fcntl(out, F_SETFL, fcntl(out, F_GETFL) | O_NONBLOCK);

	process_result result{.status = -1, .timed_out = false, .output = {}};
	auto drain = [&] {
		char buffer[4096];
		ssize_t n;
		while ((n = read(out, buffer, sizeof(buffer))) > 0)
			result.output.append(buffer, std::min<size_t>(n, output_limit - std::min(output_limit, result.output.size())));
	};
	auto deadline = deadline_after(timeout);
	for (;;)
	{
		// The child's exit, not pipe EOF, ends the wait: a daemon it forks may hold the pipe.
		pollfd fds[2] = {{.fd = out, .events = POLLIN, .revents = 0}, {.fd = cancel_fd, .events = POLLIN, .revents = 0}};
		poll(fds, cancel_fd >= 0 ? 2 : 1, remaining_ms(deadline, 20ms));
		drain();
		if (readable(cancel_fd))
		{
			kill(pid, SIGKILL);
			reap(pid);
			close(out);
			throw std::runtime_error("Stop requested");
		}
		int status;
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid or (r < 0 and errno == ECHILD))
		{
			if (r == pid and WIFEXITED(status))
				result.status = WEXITSTATUS(status);
			drain();
			break;
		}
		if (steady::now() >= deadline)
		{
			kill(pid, SIGKILL);
			reap(pid);
			drain();
			result.timed_out = true;
			break;
		}
	}
	close(out);
	return result;
}

frame_stream::frame_stream(int port, int cancel_fd) :
        port(port), cancel_fd(cancel_fd) {}

frame_stream::~frame_stream()
{
	disconnect();
}

void frame_stream::disconnect()
{
	if (fd >= 0)
		close(fd);
	fd = -1;
	current.reset();
	have = 0;
}

bool frame_stream::connect_once(steady::time_point deadline)
{
	int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (s < 0)
		throw std::system_error(errno, std::system_category(), "socket");
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bool ok = ::connect(s, (sockaddr *)&address, sizeof(address)) == 0;
	if (not ok and errno == EINPROGRESS)
	{
		pollfd fds[2] = {{.fd = s, .events = POLLOUT, .revents = 0}, {.fd = cancel_fd, .events = POLLIN, .revents = 0}};
		if (poll(fds, cancel_fd >= 0 ? 2 : 1, remaining_ms(deadline, 2s)) > 0 and (fds[0].revents & POLLOUT))
		{
			int error = 0;
			socklen_t len = sizeof(error);
			ok = getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &len) == 0 and error == 0;
		}
	}
	if (not ok)
	{
		close(s);
		return false;
	}
	int enabled = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
	fd = s;
	return true;
}

std::optional<camera_frame> frame_stream::next(std::chrono::milliseconds timeout)
{
	auto deadline = deadline_after(timeout);
	for (;;)
	{
		if (readable(cancel_fd))
			return std::nullopt;
		if (fd < 0)
		{
			// Allow 20 s for the relay to come up. adb forward accepts and
			// closes at once while nothing listens on the headset, so EOF retries too.
			if (not reconnect_deadline)
				reconnect_deadline = steady::now() + (streamed ? 5s : 20s);
			if (steady::now() >= *reconnect_deadline)
				throw std::runtime_error(streamed ? "The headset camera stream was lost and did not come back within 5 s"
				                                  : "The headset camera relay did not begin streaming within 20 s");
			if (not connect_once(std::min(deadline, *reconnect_deadline)))
			{
				if (steady::now() >= deadline)
					return std::nullopt;
				wait_cancellable(cancel_fd, std::chrono::milliseconds(remaining_ms(deadline, 250ms)));
				continue;
			}
		}

		pollfd fds[2] = {{.fd = fd, .events = POLLIN, .revents = 0}, {.fd = cancel_fd, .events = POLLIN, .revents = 0}};
		int r = poll(fds, cancel_fd >= 0 ? 2 : 1, remaining_ms(deadline, 1h));
		if (r < 0 and errno == EINTR)
			continue;
		if (r < 0)
			throw std::system_error(errno, std::system_category(), "poll");
		if (cancel_fd >= 0 and fds[1].revents)
			return std::nullopt;
		if (r == 0)
			return std::nullopt;

		uint8_t * dst = current ? payload.data() + have : header.data() + have;
		size_t want = current ? payload.size() - have : header_size - have;
		ssize_t n = recv(fd, dst, want, 0);
		if (n < 0 and (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR))
			continue;
		if (n <= 0)
		{
			disconnect();
			wait_cancellable(cancel_fd, std::chrono::milliseconds(remaining_ms(deadline, 250ms)));
			continue;
		}
		have += n;

		if (not current and have == header_size)
		{
			try
			{
				current = parse_header(header);
			}
			catch (...)
			{
				disconnect();
				throw;
			}
			payload.resize(current->payload_size); // bounded: at most 5 * 400 * 400
			have = 0;
		}
		else if (current and have == payload.size())
		{
			camera_frame frame{
			        .strip = std::move(payload),
			        .camera_ids = std::move(current->camera_ids),
			        .headset_ns = current->timestamp_ns,
			        .sequence = current->sequence,
			        .rejected_torn = current->rejected_torn,
			};
			payload = {};
			current.reset();
			have = 0;
			streamed = true;
			reconnect_deadline.reset();
			return frame;
		}
	}
}
} // namespace detail

camera_link::camera_link(std::string adb, std::string headset_ip, std::filesystem::path binaries_dir, int max_fps) :
        adb(std::move(adb)), headset_ip(std::move(headset_ip)), binaries_dir(std::move(binaries_dir)), max_fps(max_fps), status_(&status_idle)
{
	detail::relay_command(max_fps); // validates max_fps
	in_addr unused;
	if (not this->headset_ip.empty() and inet_pton(AF_INET, this->headset_ip.c_str(), &unused) != 1)
		throw std::invalid_argument("Invalid headset IPv4 address: " + this->headset_ip);
	if (pipe2(stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
		throw std::system_error(errno, std::system_category(), "pipe2");
}

camera_link::~camera_link()
{
	try
	{
		cleanup();
	}
	catch (...)
	{
	}
	close(stop_pipe[0]);
	close(stop_pipe[1]);
}

void camera_link::set_status(const std::string & s)
{
	status_.store(&s);
}

const std::string & camera_link::status() const
{
	return *status_.load();
}

void camera_link::request_stop()
{
	set_status(status_stopping);
	[[maybe_unused]] auto n = write(stop_pipe[1], "x", 1);
}

detail::process_result camera_link::adb_call(std::vector<std::string> args, std::chrono::milliseconds timeout, bool cancellable)
{
	if (not serial.empty())
		args.insert(args.begin(), {"-s", serial});
	args.insert(args.begin(), adb);
	return detail::run_process(args, timeout, cancellable ? stop_pipe[0] : -1);
}

detail::process_result camera_link::root_call(std::string_view command, std::chrono::milliseconds timeout, bool cancellable)
{
	return adb_call({"shell", detail::su_argument(command)}, timeout, cancellable);
}

void camera_link::select_serial()
{
	auto devices = adb_call({"devices", "-l"}, 10s);
	if (devices.status != 0)
		throw std::runtime_error("adb is not working (adb devices failed): " + tail(devices.output));
	auto device = detail::select_device(detail::parse_adb_devices(devices.output), headset_ip);

	if (not device and not headset_ip.empty())
	{
		// adb can keep a stale offline transport, so disconnect and retry once.
		std::string target = headset_ip + ":5555";
		auto state = [&] {
			auto r = detail::run_process({adb, "-s", target, "get-state"}, 10s, stop_pipe[0]);
			return r.status == 0 and r.output.starts_with("device");
		};
		auto connected = adb_call({"connect", target}, 15s);
		if (not state())
		{
			adb_call({"disconnect", target}, 10s);
			connected = adb_call({"connect", target}, 15s);
		}
		if (not state())
			throw std::runtime_error("Could not reach the headset over wireless adb at " + target +
			                         ". Enable wireless adb once over USB with `adb tcpip 5555` (repeat after each headset reboot), "
			                         "wake the headset and approve its debugging prompt. adb said: " +
			                         tail(connected.output));
		device = detail::adb_device{.serial = target, .usb = false, .quest_pro = true};
	}
	if (not device)
		throw std::runtime_error("No authorized Quest Pro found over adb. Connect it by USB and accept the debugging prompt, "
		                         "or set the headset IP for wireless adb (enable it once over USB with `adb tcpip 5555`).");
	serial = device->serial;
	wireless = not device->usb;
}

void camera_link::start()
{
	try
	{
		for (const auto & name: {streamer_name, relay_name, injector_name})
			if (not std::filesystem::exists(binaries_dir / name))
				throw std::runtime_error("Quest Pro headset binary missing: " + (binaries_dir / name).string());

		set_status(status_adb);
		select_serial();

		set_status(status_root);
		auto id = root_call("id", 20s);
		if (id.output.find("uid=0(root)") == std::string::npos)
			throw std::runtime_error("Magisk root is not granted to Android Shell. On the headset open Magisk > Superuser and enable Shell (or ADB Shell), then retry. su said: " + tail(id.output));

		set_status(status_push);
		for (const auto & name: {streamer_name, relay_name, injector_name})
		{
			auto r = adb_call({"push", (binaries_dir / name).string(), remote_tmp + "/" + name}, 60s);
			if (r.status != 0)
				throw std::runtime_error("Pushing " + name + " to the headset failed: " + tail(r.output));
		}
		// adb push creates these as Android Shell; Magisk's uid 0 lacks CAP_FOWNER,
		// so set modes as their owner before entering su.
		if (adb_call({"shell", "chmod", "755", remote_relay, remote_injector}, 15s).status != 0)
			throw std::runtime_error("Marking the headset executables runnable failed");
		if (adb_call({"shell", "chmod", "644", remote_streamer}, 15s).status != 0)
			throw std::runtime_error("Setting the streamer library permissions failed");
		const std::string logs = remote_tmp + "/questpro-live-v3.log " + remote_tmp + "/questpro-live-v8.log";
		if (adb_call({"shell", "rm -f " + logs + "; touch " + logs + "; chmod 666 " + logs}, 15s).status != 0)
			throw std::runtime_error("Preparing the headset logs failed");

		adb_call({"forward", "--remove", forward_spec}, 10s);
		const std::string inject = remote_injector + " " + remote_streamer;
		auto run_inject = [&] {
			set_status(status_inject);
			auto r = root_call(inject, 60s);
			if (r.status != 0)
				throw std::runtime_error("Injecting the camera streamer failed" + std::string(r.timed_out ? " (timed out)" : "") + ": " + tail(r.output));
		};

		if (wireless)
		{
			// Wireless: inject first (a second su session sees incomplete provider
			// mappings while the relay session is open), then keep the relay's
			// adb/su session alive for its lifetime.
			run_inject();
			set_status(status_relay);
			relay_log = memfd_create("qpro-relay-log", MFD_CLOEXEC);
			if (relay_log < 0)
				throw std::system_error(errno, std::system_category(), "memfd_create");
			relay_session = spawn({adb, "-s", serial, "shell", detail::su_argument(detail::relay_command(max_fps))}, relay_log);
			relay_started = true;
			if (not wait_cancellable(stop_pipe[0], 800ms))
				throw std::runtime_error("Stop requested");
			if (waitpid(relay_session, nullptr, WNOHANG) == relay_session)
			{
				relay_session = -1;
				std::string log(4096, '\0');
				log.resize(std::max<ssize_t>(0, pread(relay_log, log.data(), log.size(), 0)));
				throw std::runtime_error("The wireless root relay exited during startup: " + tail(log));
			}
		}
		else
		{
			set_status(status_relay);
			relay_started = true; // a timed-out start may still leave it running: --stop in cleanup
			auto r = root_call("chmod 755 " + remote_relay + "; : > " + remote_relay_log + "; nohup " + detail::relay_command(max_fps) + " > " + remote_relay_log + " 2>&1 < /dev/null &", 20s);
			if (r.status != 0)
				throw std::runtime_error("Starting the root relay failed: " + tail(r.output));
			// First check after 0.8 s; retry briefly to tolerate a slow headset.
			for (int attempt = 0;; ++attempt)
			{
				if (not wait_cancellable(stop_pipe[0], attempt == 0 ? 800ms : 250ms))
					throw std::runtime_error("Stop requested");
				auto log = root_call("cat " + remote_relay_log, 10s).output;
				if (log.find("RELAY_LISTENING") != std::string::npos)
					break;
				if (attempt == 8)
					throw std::runtime_error("The root relay did not begin listening: " + tail(log));
			}
			run_inject();
		}

		auto r = adb_call({"forward", forward_spec, forward_spec}, 10s);
		if (r.status != 0)
			throw std::runtime_error("ADB port forwarding failed: " + tail(r.output));
		stream = std::make_unique<detail::frame_stream>(detail::stream_port, stop_pipe[0]);
		set_status(status_connect);
	}
	catch (...)
	{
		bool stopping = readable(stop_pipe[0]);
		cleanup();
		set_status(stopping ? status_stopped : status_failed);
		throw;
	}
}

std::optional<camera_frame> camera_link::next_frame(std::chrono::milliseconds timeout)
{
	if (not stream)
		throw std::logic_error("camera_link::start() has not succeeded");
	try
	{
		auto frame = stream->next(timeout);
		if (frame)
			set_status(status_streaming);
		return frame;
	}
	catch (...)
	{
		set_status(status_failed);
		throw;
	}
}

void camera_link::cleanup()
{
	// relay --stop, let the session end, drop the forward.
	stream.reset();
	if (relay_started)
	{
		root_call(remote_relay + " --stop", 5s, false);
		relay_started = false;
	}
	if (relay_session > 0)
	{
		kill_and_reap(relay_session, 2s);
		relay_session = -1;
	}
	if (relay_log >= 0)
	{
		close(relay_log);
		relay_log = -1;
	}
	if (not serial.empty())
		adb_call({"forward", "--remove", forward_spec}, 5s, false);
	if (status_.load() != &status_failed)
		set_status(status_stopped);
}
} // namespace wivrn::qpro
