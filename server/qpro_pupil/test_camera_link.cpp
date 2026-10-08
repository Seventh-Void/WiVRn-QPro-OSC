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

#undef NDEBUG
#include "qpro_pupil/camera_link.h"

#include <algorithm>
#include <arpa/inet.h>
#include <bit>
#include <cassert>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace wivrn::qpro;
using namespace std::chrono_literals;

namespace
{
template <typename T>
void put(std::vector<uint8_t> & b, size_t offset, T value)
{
	std::memcpy(b.data() + offset, &value, sizeof(value));
}

// A frame as relay.c send_frame() writes it.
std::vector<uint8_t> make_frame(uint32_t mask, uint64_t sequence, uint8_t fill)
{
	uint32_t count = std::popcount(mask & 0x1fu);
	uint32_t width = count * 400;
	std::vector<uint8_t> b(64 + width * 400, fill);
	std::fill(b.begin(), b.begin() + 64, 0);
	std::memcpy(b.data(), "QPLIVE3", 7);
	put<uint32_t>(b, 8, 3);
	put<uint32_t>(b, 12, 64);
	put<uint64_t>(b, 16, sequence);
	put<uint64_t>(b, 24, 1'000'000'000ull + sequence);
	put<uint32_t>(b, 32, width);
	put<uint32_t>(b, 36, 400);
	put<uint32_t>(b, 40, width);
	put<uint32_t>(b, 44, 1);
	put<uint32_t>(b, 48, width * 400);
	put<uint32_t>(b, 52, mask);
	put<uint64_t>(b, 56, 7);
	return b;
}

bool header_throws(std::vector<uint8_t> b)
{
	try
	{
		detail::parse_header(std::span<const uint8_t, 64>(b.data(), 64));
		return false;
	}
	catch (const std::runtime_error &)
	{
		return true;
	}
}

void test_header()
{
	assert((detail::camera_ids_from_mask(0x03) == std::vector<int>{0, 1}));
	assert((detail::camera_ids_from_mask(0x1f) == std::vector<int>{0, 1, 2, 3, 4}));
	assert((detail::camera_ids_from_mask(0x0c) == std::vector<int>{2, 3}));
	assert(detail::camera_ids_from_mask(0x20).empty());

	auto b = make_frame(0x03, 42, 0);
	auto h = detail::parse_header(std::span<const uint8_t, 64>(b.data(), 64));
	assert(h.sequence == 42 and h.timestamp_ns == 1'000'000'042ull and h.rejected_torn == 7);
	assert(h.width == 800 and h.payload_size == 800 * 400);
	assert((h.camera_ids == std::vector<int>{0, 1}));

	assert(not header_throws(make_frame(0x1f, 1, 0)));
	auto bad = b;
	bad[0] = 'X';
	assert(header_throws(bad)); // magic
	bad = b;
	put<uint32_t>(bad, 8, 2);
	assert(header_throws(bad)); // version
	bad = b;
	put<uint32_t>(bad, 12, 80);
	assert(header_throws(bad)); // header size
	bad = b;
	put<uint32_t>(bad, 32, 1200);
	assert(header_throws(bad)); // width vs mask
	bad = b;
	put<uint32_t>(bad, 40, 1024);
	assert(header_throws(bad)); // stride
	bad = b;
	put<uint32_t>(bad, 44, 3);
	assert(header_throws(bad)); // pixel format
	bad = b;
	put<uint32_t>(bad, 48, 0xffffffffu);
	assert(header_throws(bad)); // payload size, no huge allocation
	bad = b;
	put<uint32_t>(bad, 52, 0);
	put<uint32_t>(bad, 32, 0);
	put<uint32_t>(bad, 40, 0);
	put<uint32_t>(bad, 48, 0);
	assert(header_throws(bad)); // no cameras
}

void test_devices()
{
	const std::string out =
	        "* daemon not running; starting now at tcp:5037\r\n"
	        "* daemon started successfully\n"
	        "List of devices attached\n"
	        "230YC01D9W02VN         device usb:6-3.1 product:seacliff model:Quest_Pro device:seacliff transport_id:4\n"
	        "192.168.1.50:5555      device product:seacliff model:Quest_Pro device:seacliff transport_id:5\n"
	        "R58M123               unauthorized usb:1-2 transport_id:6\n"
	        "192.168.1.60:5555      offline transport_id:7\n"
	        "\n";
	auto d = detail::parse_adb_devices(out);
	assert(d.size() == 2);
	assert(d[0].serial == "230YC01D9W02VN" and d[0].usb and d[0].quest_pro);
	assert(d[1].serial == "192.168.1.50:5555" and not d[1].usb and d[1].quest_pro);

	// USB Quest Pro first, even with the WiVRn IP given.
	assert(detail::select_device(d, "192.168.1.50")->serial == "230YC01D9W02VN");
	// Without USB: the headset IP, then any wireless Quest Pro.
	std::vector<detail::adb_device> wifi{d[1]};
	assert(detail::select_device(wifi, "192.168.1.50")->serial == "192.168.1.50:5555");
	assert(detail::select_device(wifi, "")->serial == "192.168.1.50:5555");
	// Offline transport is not usable: caller runs adb connect.
	assert(not detail::select_device(detail::parse_adb_devices("List of devices attached\n192.168.1.60:5555 offline\n"), "192.168.1.60"));
	// Unknown single USB device is accepted; two are ambiguous.
	auto phones = detail::parse_adb_devices("List of devices attached\nAAA device usb:1-1 product:x\nBBB device usb:1-2 product:y\n");
	assert(not detail::select_device(phones, ""));
	phones.pop_back();
	assert(detail::select_device(phones, "")->serial == "AAA");
	assert(not detail::select_device({}, "10.0.0.2"));
}

void test_commands()
{
	assert(detail::relay_command(24) == "/data/local/tmp/questpro-camera-relay-v8 --mode eyes --max-fps 24");
	assert(detail::su_argument("id") == "su -c 'id'");
	bool threw = false;
	try
	{
		detail::relay_command(121);
	}
	catch (const std::invalid_argument &)
	{
		threw = true;
	}
	assert(threw);
	threw = false;
	try
	{
		detail::su_argument("echo 'x'");
	}
	catch (const std::logic_error &)
	{
		threw = true;
	}
	assert(threw);
	threw = false;
	try
	{
		camera_link("adb", "not-an-ip", "/nonexistent");
	}
	catch (const std::invalid_argument &)
	{
		threw = true;
	}
	assert(threw);
}

void test_process()
{
	auto r = detail::run_process({"sh", "-c", "echo out; echo err >&2; exit 3"}, 5s);
	assert(r.status == 3 and not r.timed_out);
	assert(r.output.find("out") != std::string::npos and r.output.find("err") != std::string::npos);

	auto t0 = std::chrono::steady_clock::now();
	r = detail::run_process({"sleep", "10"}, 200ms);
	assert(r.timed_out and r.status == -1);
	assert(std::chrono::steady_clock::now() - t0 < 2s);

	// A child that leaves a background holder of the pipe must not block.
	t0 = std::chrono::steady_clock::now();
	r = detail::run_process({"sh", "-c", "sleep 3 & echo forked"}, 5s);
	assert(r.status == 0 and r.output.find("forked") != std::string::npos);
	assert(std::chrono::steady_clock::now() - t0 < 1s);

	int cancel[2];
	assert(pipe(cancel) == 0);
	std::thread stopper([&] {
		std::this_thread::sleep_for(100ms);
		assert(write(cancel[1], "x", 1) == 1);
	});
	t0 = std::chrono::steady_clock::now();
	bool stopped = false;
	try
	{
		detail::run_process({"sleep", "10"}, 10s, cancel[0]);
	}
	catch (const std::runtime_error & e)
	{
		stopped = std::string(e.what()) == "Stop requested";
	}
	stopper.join();
	assert(stopped and std::chrono::steady_clock::now() - t0 < 2s);
	close(cancel[0]);
	close(cancel[1]);

	bool threw = false;
	try
	{
		detail::run_process({"/nonexistent/adb", "devices"}, 1s);
	}
	catch (const std::runtime_error &)
	{
		threw = true;
	}
	assert(threw);
}

void send_chunked(int fd, const std::vector<uint8_t> & b, size_t chunk)
{
	for (size_t i = 0; i < b.size(); i += chunk)
	{
		size_t n = std::min(chunk, b.size() - i);
		assert(send(fd, b.data() + i, n, MSG_NOSIGNAL) == (ssize_t)n);
		if (chunk < b.size())
			std::this_thread::sleep_for(1ms);
	}
}

void test_fake_relay()
{
	int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	assert(server >= 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(server, (sockaddr *)&address, sizeof(address)) == 0);
	assert(listen(server, 4) == 0);
	socklen_t len = sizeof(address);
	assert(getsockname(server, (sockaddr *)&address, &len) == 0);
	int port = ntohs(address.sin_port);

	std::thread relay([server] {
		// Connection 1: adb forward with nothing listening on the headset (accept, close).
		int c = accept(server, nullptr, nullptr);
		close(c);
		// Connection 2: two frames (one in small chunks), a silent gap, then a malformed header.
		c = accept(server, nullptr, nullptr);
		send_chunked(c, make_frame(0x03, 1, 11), 1000);
		send_chunked(c, make_frame(0x03, 2, 22), 1 << 20);
		std::this_thread::sleep_for(300ms);
		auto bad = make_frame(0x03, 3, 33);
		bad[0] = 'Z';
		send_chunked(c, bad, 1 << 20);
		std::this_thread::sleep_for(200ms);
		close(c);
	});

	detail::frame_stream stream(port, -1);
	auto f1 = stream.next(5s);
	assert(f1 and f1->sequence == 1 and f1->headset_ns == 1'000'000'001ull and f1->rejected_torn == 7);
	assert((f1->camera_ids == std::vector<int>{0, 1}));
	assert(f1->strip.size() == 800 * 400 and f1->strip.front() == 11 and f1->strip.back() == 11);
	auto f2 = stream.next(5s);
	assert(f2 and f2->sequence == 2 and f2->strip[400 * 400] == 22);
	assert(not stream.next(50ms)); // timeout while the relay is silent
	bool threw = false;
	try
	{
		stream.next(5s);
	}
	catch (const std::runtime_error & e)
	{
		threw = std::string(e.what()).find("header") != std::string::npos;
	}
	assert(threw);
	relay.join();

	// request_stop path: a readable cancel fd ends a blocked next() promptly.
	int cancel[2];
	assert(pipe(cancel) == 0);
	std::thread holder([server] {
		int c = accept(server, nullptr, nullptr);
		std::this_thread::sleep_for(500ms);
		close(c);
	});
	detail::frame_stream stopped(port, cancel[0]);
	std::thread stopper([&] {
		std::this_thread::sleep_for(100ms);
		assert(write(cancel[1], "x", 1) == 1);
	});
	auto t0 = std::chrono::steady_clock::now();
	assert(not stopped.next(10s));
	assert(std::chrono::steady_clock::now() - t0 < 1s);
	stopper.join();
	holder.join();
	close(cancel[0]);
	close(cancel[1]);
	close(server);
}
} // namespace

int main()
{
	test_header();
	test_devices();
	test_commands();
	test_process();
	test_fake_relay();
	std::cout << "test_camera_link: all passed" << std::endl;
}
