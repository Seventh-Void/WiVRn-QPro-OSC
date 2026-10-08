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
#include "preview_server.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace wivrn::qpro;
using namespace std::chrono;

// blocking client socket with a 2 s receive timeout so a broken server fails the test instead of hanging it
static int connect_to(uint16_t port, const std::string & request = "")
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(fd >= 0);
	timeval tv{.tv_sec = 2, .tv_usec = 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(connect(fd, (sockaddr *)&addr, sizeof(addr)) == 0);
	if (not request.empty())
		assert(send(fd, request.data(), request.size(), MSG_NOSIGNAL) == (ssize_t)request.size());
	return fd;
}

static std::string read_exact(int fd, size_t size)
{
	std::string s(size, '\0');
	for (size_t got = 0; got < size;)
	{
		ssize_t n = recv(fd, s.data() + got, size - got, 0);
		assert(n > 0);
		got += n;
	}
	return s;
}

static std::string read_until(int fd, const std::string & end)
{
	std::string s;
	while (not s.ends_with(end))
		s += read_exact(fd, 1);
	return s;
}

static std::string read_all(int fd)
{
	std::string s;
	char buf[4096];
	ssize_t n;
	while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
		s.append(buf, n);
	assert(n == 0);
	return s;
}

static std::string get(uint16_t port, const std::string & path)
{
	int fd = connect_to(port, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
	auto s = read_all(fd);
	close(fd);
	return s;
}

static bool wait_for(const std::function<bool()> & cond)
{
	for (auto end = steady_clock::now() + 2s; steady_clock::now() < end; std::this_thread::sleep_for(1ms))
		if (cond())
			return true;
	return false;
}

static std::vector<uint8_t> make_frame(size_t size, uint8_t seed)
{
	std::vector<uint8_t> f(size);
	for (size_t i = 0; i < size; ++i)
		f[i] = uint8_t(seed + i * 7);
	// boundary-looking bytes inside the payload must not matter
	std::string fake = "\r\n--frame\r\n";
	std::copy(fake.begin(), fake.end(), f.begin());
	return f;
}

static std::string part(const std::vector<uint8_t> & f)
{
	return "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(f.size()) + "\r\n\r\n" +
	       std::string(f.begin(), f.end()) + "\r\n";
}

// reads one multipart part, returns its payload
static std::string read_part(int fd)
{
	auto head = read_until(fd, "\r\n\r\n");
	std::string prefix = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ";
	assert(head.starts_with(prefix));
	size_t size = std::stoul(head.substr(prefix.size()));
	auto body = read_exact(fd, size + 2);
	assert(body.ends_with("\r\n"));
	body.resize(size);
	return body;
}

static const std::string stream_header = "HTTP/1.0 200 OK\r\n"
                                         "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                                         "Cache-Control: no-cache\r\n\r\n";

int main()
{
	preview_server server(0);
	assert(server.bound());
	assert(server.port() != 0);

	// a second server on the same port stays disabled
	{
		preview_server busy(server.port());
		assert(not busy.bound());
		busy.publish_jpeg({1, 2, 3});
		busy.publish_status("{}");
		assert(not busy.has_viewers());
	}

	// status
	assert(get(server.port(), "/status.json") == "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}");
	server.publish_status(R"({"tracking":true})");
	assert(get(server.port(), "/status.json?x=1") == "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: 17\r\n\r\n{\"tracking\":true}");

	// 404
	assert(get(server.port(), "/nope").starts_with("HTTP/1.0 404 Not Found\r\n"));
	assert(get(server.port(), "/preview.mjpgx").starts_with("HTTP/1.0 404 "));

	// MJPEG framing, 3 frames
	assert(not server.has_viewers());
	int viewer = connect_to(server.port(), "GET /preview.mjpg HTTP/1.1\r\nHost: localhost\r\n\r\n");
	assert(read_exact(viewer, stream_header.size()) == stream_header);
	assert(wait_for([&] { return server.has_viewers(); }));
	for (int i = 1; i <= 3; ++i)
	{
		auto f = make_frame(1000 * i, i);
		server.publish_jpeg(f);
		assert(read_exact(viewer, part(f).size()) == part(f));
	}

	// a client that never reads: producer stays fast, other viewer keeps getting the latest frame
	int stalled = connect_to(server.port(), "GET /preview.mjpg HTTP/1.0\r\n\r\n");
	std::this_thread::sleep_for(50ms);
	std::vector<std::vector<uint8_t>> big;
	for (int i = 0; i < 50; ++i)
		big.push_back(make_frame(512 * 1024, i));
	auto start = steady_clock::now();
	for (auto & f: big)
		server.publish_jpeg(std::move(f));
	assert(steady_clock::now() - start < 100ms);
	auto last = make_frame(4321, 99);
	server.publish_jpeg(last);
	std::string want(last.begin(), last.end());
	while (read_part(viewer) != want)
		;

	// client limit: 2 connected, 2 more accepted, the 5th is closed straight away
	int c3 = connect_to(server.port());
	int c4 = connect_to(server.port());
	std::this_thread::sleep_for(50ms);
	int c5 = connect_to(server.port());
	char byte;
	assert(recv(c5, &byte, 1, 0) == 0);
	close(c5);
	close(c3);
	close(c4);

	// viewers gone -> has_viewers false
	close(viewer);
	close(stalled);
	assert(wait_for([&] { return not server.has_viewers(); }));

	// destructor with connected (stalled, idle, half-requested) clients returns quickly
	{
		auto s = std::make_unique<preview_server>(0);
		int a = connect_to(s->port(), "GET /preview.mjpg HTTP/1.0\r\n\r\n");
		int b = connect_to(s->port());
		int c = connect_to(s->port(), "GET /sta");
		assert(wait_for([&] { return s->has_viewers(); }));
		for (int i = 0; i < 20; ++i)
			s->publish_jpeg(make_frame(512 * 1024, i));
		std::this_thread::sleep_for(50ms);
		auto t = steady_clock::now();
		s.reset();
		assert(steady_clock::now() - t < 200ms);
		close(a);
		close(b);
		close(c);
	}

	std::printf("test_preview_server: OK\n");
}
