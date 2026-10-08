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

#include "preview_server.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono;

namespace wivrn::qpro
{
namespace
{
constexpr size_t max_clients = 4;
constexpr size_t max_request = 4096;
constexpr auto request_timeout = 2s;

struct client
{
	int fd;
	steady_clock::time_point deadline;
	std::string request = {};
	bool reading = true;
	bool streaming = false;
	bool close_when_sent = false;
	std::string out = {}; // pending bytes, at most one response or one frame
	size_t out_pos = 0;
	uint64_t seq = 0; // last frame queued to this client

	bool pending() const
	{
		return out_pos < out.size();
	}
	void queue(std::string data)
	{
		out = std::move(data);
		out_pos = 0;
	}
};

std::string reply(std::string_view code, std::string_view type, std::string_view body)
{
	return "HTTP/1.0 " + std::string(code) + "\r\nContent-Type: " + std::string(type) +
	       "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

// "GET /path?query HTTP/1.x" -> "/path"; empty when not a GET
std::string_view request_path(std::string_view request)
{
	if (not request.starts_with("GET "))
		return {};
	request.remove_prefix(4);
	return request.substr(0, request.find_first_of(" ?\r\n"));
}
} // namespace

preview_server::preview_server(uint16_t port)
{
	listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t len = sizeof(addr);
	int one = 1;
	if (listen_fd < 0 or wake_fd < 0 or
	    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 or
	    bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) < 0 or
	    listen(listen_fd, max_clients) < 0 or
	    getsockname(listen_fd, (sockaddr *)&addr, &len) < 0)
		return;
	port_ = ntohs(addr.sin_port);

	try
	{
		thread = std::thread(&preview_server::run, this);
	}
	catch (...)
	{
	}
}

preview_server::~preview_server()
{
	quit = true;
	wake();
	if (thread.joinable())
		thread.join();
	if (listen_fd >= 0)
		close(listen_fd);
	if (wake_fd >= 0)
		close(wake_fd);
}

void preview_server::wake()
{
	if (wake_fd >= 0)
		eventfd_write(wake_fd, 1);
}

void preview_server::publish_jpeg(std::vector<uint8_t> data)
{
	auto frame = std::make_shared<const std::vector<uint8_t>>(std::move(data));
	{
		std::lock_guard lock(mutex);
		jpeg.swap(frame);
		++jpeg_seq;
	}
	// old frame (now in `frame`) is freed outside the lock
	if (viewers > 0)
		wake();
}

void preview_server::publish_status(std::string json)
{
	std::lock_guard lock(mutex);
	status = std::move(json);
}

void preview_server::run()
{
	std::vector<client> clients;
	try
	{
		std::vector<pollfd> fds;
		char buffer[max_request];
		while (not quit)
		{
			auto now = steady_clock::now();
			int timeout = -1;
			fds.assign({{wake_fd, POLLIN, 0}, {listen_fd, POLLIN, 0}});
			for (auto & c: clients)
			{
				fds.push_back({c.fd, short(POLLIN | (c.pending() ? POLLOUT : 0)), 0});
				if (c.reading)
				{
					int ms = std::max<int>(0, ceil<milliseconds>(c.deadline - now).count());
					timeout = timeout < 0 ? ms : std::min(timeout, ms);
				}
			}

			if (poll(fds.data(), fds.size(), timeout) < 0 and errno != EINTR)
				break;
			now = steady_clock::now();

			if (fds[0].revents & POLLIN)
			{
				eventfd_t v;
				eventfd_read(wake_fd, &v);
			}

			std::shared_ptr<const std::vector<uint8_t>> frame;
			uint64_t seq;
			{
				std::lock_guard lock(mutex);
				frame = jpeg;
				seq = jpeg_seq;
			}

			for (size_t i = 0; i < clients.size(); ++i)
			{
				auto & c = clients[i];
				short revents = fds[i + 2].revents;
				bool drop = revents & (POLLERR | POLLNVAL);

				if (not drop and (revents & (POLLIN | POLLHUP)))
				{
					size_t room = c.reading ? max_request - c.request.size() : sizeof(buffer);
					ssize_t n = recv(c.fd, buffer, room, MSG_DONTWAIT);
					if (n == 0 or (n < 0 and errno != EAGAIN and errno != EINTR))
						drop = true;
					else if (n > 0 and c.reading)
						c.request.append(buffer, n);
				}

				if (not drop and c.reading)
				{
					bool complete = c.request.find("\r\n\r\n") != std::string::npos or
					                c.request.find("\n\n") != std::string::npos or
					                c.request.size() >= max_request;
					if (complete)
					{
						c.reading = false;
						auto path = request_path(c.request);
						if (path == "/preview.mjpg")
						{
							c.streaming = true;
							c.queue("HTTP/1.0 200 OK\r\n"
							        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
							        "Cache-Control: no-cache\r\n\r\n");
						}
						else
						{
							c.close_when_sent = true;
							if (path == "/status.json")
							{
								std::lock_guard lock(mutex);
								c.queue(reply("200 OK", "application/json", status.empty() ? "{}" : status));
							}
							else
								c.queue(reply("404 Not Found", "text/plain", "not found\n"));
						}
						std::string().swap(c.request);
					}
					else if (now >= c.deadline)
						drop = true;
				}

				while (not drop)
				{
					// only the latest frame matters: a client still sending an older one skips the ones in between
					if (c.streaming and not c.pending() and frame and c.seq != seq)
					{
						std::string part = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
						                   std::to_string(frame->size()) + "\r\n\r\n";
						part.append(frame->begin(), frame->end());
						part += "\r\n";
						c.queue(std::move(part));
						c.seq = seq;
					}
					if (not c.pending())
						break;
					ssize_t n = send(c.fd, c.out.data() + c.out_pos, c.out.size() - c.out_pos, MSG_NOSIGNAL | MSG_DONTWAIT);
					if (n < 0)
					{
						if (errno == EAGAIN)
							break;
						if (errno != EINTR)
							drop = true;
					}
					else
						c.out_pos += n;
				}
				if (not drop and c.close_when_sent and not c.pending())
					drop = true;

				if (drop)
				{
					close(c.fd);
					c.fd = -1;
				}
			}
			std::erase_if(clients, [](const client & c) { return c.fd < 0; });

			if (fds[1].revents & POLLIN)
			{
				int fd;
				while ((fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0)
				{
					if (clients.size() >= max_clients)
						close(fd);
					else
						clients.push_back({.fd = fd, .deadline = now + request_timeout});
				}
			}

			viewers = std::ranges::count_if(clients, [](const client & c) { return c.streaming; });
		}
	}
	catch (...)
	{
	}
	for (auto & c: clients)
		if (c.fd >= 0)
			close(c.fd);
	viewers = 0;
}

} // namespace wivrn::qpro
