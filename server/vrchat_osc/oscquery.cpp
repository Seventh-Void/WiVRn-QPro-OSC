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

#include "oscquery.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "mdns.h"
#pragma GCC diagnostic pop

using namespace std::chrono;

namespace wivrn::vrchat_osc
{

namespace
{
constexpr std::string_view service = "_oscjson._tcp.local.";
constexpr std::string_view vrchat_prefix = "VRChat-Client";
constexpr size_t max_response = 4 << 20; // an avatar tree is a few hundred KB

struct browse_state
{
	std::set<std::string> instances;                                // VRChat-Client-*._oscjson._tcp.local.
	std::map<std::string, std::pair<std::string, uint16_t>> srv;    // instance -> (target host, port)
	std::map<std::string, std::string> a;                           // host -> IPv4
	std::map<std::string, std::string> source;                      // instance -> IPv4 of the responder

	// Prefers a VRChat on this PC (an address in `local`) over others on the LAN
	std::optional<http_endpoint> resolve(const std::set<std::string> & local) const
	{
		std::optional<http_endpoint> any;
		for (const auto & instance: instances)
		{
			auto s = srv.find(instance);
			if (s == srv.end())
				continue;
			std::optional<http_endpoint> ep;
			if (auto ip = a.find(s->second.first); ip != a.end())
				ep = http_endpoint{ip->second, s->second.second};
			else if (auto ip = source.find(instance); ip != source.end())
				ep = http_endpoint{ip->second, s->second.second};
			if (ep and local.contains(ep->address))
				return ep;
			if (not any)
				any = ep;
		}
		return any;
	}
};

std::string to_string(in_addr addr)
{
	char buf[INET_ADDRSTRLEN] = {};
	inet_ntop(AF_INET, &addr, buf, sizeof(buf));
	return buf;
}

int query_callback(int,
                   const sockaddr * from,
                   size_t,
                   mdns_entry_type_t,
                   uint16_t,
                   uint16_t rtype,
                   uint16_t,
                   uint32_t ttl,
                   const void * data,
                   size_t size,
                   size_t name_offset,
                   size_t,
                   size_t record_offset,
                   size_t record_length,
                   void * user_data)
{
	auto & state = *static_cast<browse_state *>(user_data);
	if (ttl == 0) // goodbye packet
		return 0;

	char entrybuffer[256];
	mdns_string_t entry = mdns_string_extract(data, size, &name_offset, entrybuffer, sizeof(entrybuffer));
	std::string name{entry.str, entry.length};

	std::string from_ip;
	if (from and from->sa_family == AF_INET)
		from_ip = to_string(reinterpret_cast<const sockaddr_in *>(from)->sin_addr);

	char namebuffer[256];
	switch (rtype)
	{
		case MDNS_RECORDTYPE_PTR: {
			mdns_string_t ptr = mdns_record_parse_ptr(data, size, record_offset, record_length, namebuffer, sizeof(namebuffer));
			std::string instance{ptr.str, ptr.length};
			if (instance.starts_with(vrchat_prefix))
			{
				state.instances.insert(instance);
				if (not from_ip.empty())
					state.source.emplace(instance, from_ip);
			}
			break;
		}
		case MDNS_RECORDTYPE_SRV: {
			mdns_record_srv_t srv = mdns_record_parse_srv(data, size, record_offset, record_length, namebuffer, sizeof(namebuffer));
			state.srv[name] = {std::string{srv.name.str, srv.name.length}, srv.port};
			if (not from_ip.empty())
				state.source.emplace(name, from_ip);
			break;
		}
		case MDNS_RECORDTYPE_A: {
			sockaddr_in addr{};
			mdns_record_parse_a(data, size, record_offset, record_length, &addr);
			state.a[name] = to_string(addr.sin_addr);
			break;
		}
		default:
			break;
	}
	return 0;
}

// IPv4 addresses of this PC (loopback included)
std::set<std::string> local_addresses()
{
	std::set<std::string> result{"127.0.0.1"};
	ifaddrs * ifs = nullptr;
	if (getifaddrs(&ifs) != 0)
		return result;
	for (ifaddrs * i = ifs; i; i = i->ifa_next)
		if (i->ifa_addr and i->ifa_addr->sa_family == AF_INET)
			result.insert(to_string(reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr));
	freeifaddrs(ifs);
	return result;
}

// One socket per multicast-capable IPv4 interface, plus a passive listener on 5353.
std::vector<int> open_mdns_sockets()
{
	std::vector<int> sockets;

	ifaddrs * ifs = nullptr;
	if (getifaddrs(&ifs) == 0)
	{
		for (ifaddrs * i = ifs; i; i = i->ifa_next)
		{
			if (not i->ifa_addr or i->ifa_addr->sa_family != AF_INET)
				continue;
			if (not(i->ifa_flags & IFF_UP) or not(i->ifa_flags & IFF_MULTICAST))
				continue;
			sockaddr_in addr = *reinterpret_cast<sockaddr_in *>(i->ifa_addr);
			addr.sin_port = 0; // ephemeral port: one-shot query, responders reply unicast
			if (int fd = mdns_socket_open_ipv4(&addr); fd >= 0)
				sockets.push_back(fd);
		}
		freeifaddrs(ifs);
	}

	if (sockets.empty())
	{
		// Default multicast route, INADDR_ANY, ephemeral port
		if (int fd = mdns_socket_open_ipv4(nullptr); fd >= 0)
			sockets.push_back(fd);
	}

	return sockets;
}

// Catch responders that ignore the unicast-response bit and multicast their answer.
// Shares port 5353 with avahi through SO_REUSEADDR/SO_REUSEPORT; failure is fine.
int open_mdns_listener()
{
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(MDNS_PORT);
	return mdns_socket_open_ipv4(&addr);
}

int remaining_ms(steady_clock::time_point deadline)
{
	return std::max<int64_t>(0, duration_cast<milliseconds>(deadline - steady_clock::now()).count());
}

std::string lower(std::string_view s)
{
	std::string r{s};
	for (auto & c: r)
		c = std::tolower((unsigned char)c);
	return r;
}

// Value of header `name` (lowercase, with leading "\r\n" and trailing ':') in lowercased headers
std::optional<std::string_view> header(std::string_view headers, std::string_view name)
{
	size_t pos = headers.find(name);
	if (pos == std::string_view::npos)
		return std::nullopt;
	pos += name.size();
	return headers.substr(pos, headers.find("\r\n", pos) - pos);
}

// Decodes a chunked body. Returns nullopt while incomplete.
std::optional<std::string> dechunk(std::string_view raw)
{
	std::string body;
	size_t pos = 0;
	while (true)
	{
		size_t eol = raw.find("\r\n", pos);
		if (eol == std::string_view::npos)
			return std::nullopt;
		std::string size_line{raw.substr(pos, eol - pos)};
		size_t chunk = 0;
		try
		{
			chunk = std::stoul(size_line, nullptr, 16); // stops at ';' extensions
		}
		catch (...)
		{
			return std::nullopt;
		}
		pos = eol + 2;
		if (chunk == 0)
			return body; // trailers are ignored
		if (chunk > raw.size() - pos or raw.size() - pos - chunk < 2)
			return std::nullopt;
		body.append(raw.substr(pos, chunk));
		pos += chunk + 2;
	}
}

enum class http_status
{
	incomplete,
	done,
	error,
};

// Parses an HTTP response; on done, body is filled.
http_status parse_response(std::string_view raw, bool eof, std::string & body)
{
	size_t header_end = raw.find("\r\n\r\n");
	if (header_end == std::string_view::npos)
		return eof ? http_status::error : http_status::incomplete;

	std::string headers = lower(raw.substr(0, header_end));
	std::string_view rest = raw.substr(header_end + 4);

	// "http/1.x 200 ..."
	size_t sp = headers.find(' ');
	if (not headers.starts_with("http/") or sp == std::string::npos or headers.compare(sp + 1, 3, "200") != 0)
		return http_status::error;

	if (auto te = header(headers, "\r\ntransfer-encoding:"); te and te->find("chunked") != std::string_view::npos)
	{
		if (auto decoded = dechunk(rest))
		{
			body = std::move(*decoded);
			return http_status::done;
		}
		return eof ? http_status::error : http_status::incomplete;
	}

	if (auto cl = header(headers, "\r\ncontent-length:"))
	{
		size_t length = std::strtoull(std::string{*cl}.c_str(), nullptr, 10);
		if (rest.size() >= length)
		{
			body = rest.substr(0, length);
			return http_status::done;
		}
		return eof ? http_status::error : http_status::incomplete;
	}

	if (not eof)
		return http_status::incomplete;
	body = rest;
	return http_status::done;
}
} // namespace

std::optional<http_endpoint> find_vrchat(int timeout_ms)
{
	auto deadline = steady_clock::now() + milliseconds(timeout_ms);

	std::vector<int> senders = open_mdns_sockets();
	std::vector<pollfd> pollfds;
	for (int fd: senders)
		pollfds.push_back({.fd = fd, .events = POLLIN, .revents = 0});
	if (int fd = open_mdns_listener(); fd >= 0)
		pollfds.push_back({.fd = fd, .events = POLLIN, .revents = 0});

	std::array<uint8_t, 4096> buffer;
	for (int fd: senders)
		mdns_query_send(fd, MDNS_RECORDTYPE_PTR, service.data(), service.size(), buffer.data(), buffer.size(), 0);

	browse_state state;
	const auto local = local_addresses();
	std::set<std::string> srv_queried;
	std::optional<http_endpoint> result;

	while (not result and not pollfds.empty())
	{
		int timeout = remaining_ms(deadline);
		if (timeout <= 0 or poll(pollfds.data(), pollfds.size(), timeout) <= 0)
			break;

		for (auto & p: pollfds)
		{
			if (p.revents & POLLIN)
				mdns_query_recv(p.fd, buffer.data(), buffer.size(), query_callback, &state, 0);
		}

		result = state.resolve(local);

		// PTR answered without SRV in the additional section: ask for it explicitly
		for (const auto & instance: state.instances)
		{
			if (state.srv.contains(instance) or not srv_queried.insert(instance).second)
				continue;
			for (int fd: senders)
				mdns_query_send(fd, MDNS_RECORDTYPE_SRV, instance.data(), instance.size(), buffer.data(), buffer.size(), 0);
		}
	}

	for (auto & p: pollfds)
		mdns_socket_close(p.fd);

	return result;
}

std::optional<nlohmann::json> fetch_avatar(const http_endpoint & ep, int timeout_ms)
{
	auto deadline = steady_clock::now() + milliseconds(timeout_ms);

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ep.port);
	if (inet_pton(AF_INET, ep.address.c_str(), &addr.sin_addr) != 1)
		return std::nullopt;

	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return std::nullopt;

	auto fail = [fd]() -> std::optional<nlohmann::json> {
		close(fd);
		return std::nullopt;
	};

	auto wait = [&](short events) {
		pollfd p{.fd = fd, .events = events, .revents = 0};
		int timeout = remaining_ms(deadline);
		return timeout > 0 and poll(&p, 1, timeout) > 0;
	};

	if (connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
	{
		if (errno != EINPROGRESS or not wait(POLLOUT))
			return fail();
		int err = 0;
		socklen_t len = sizeof(err);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 or err != 0)
			return fail();
	}

	// Host must match the HttpListener prefix (http://<HostIP>:<port>/)
	std::string request = "GET /avatar HTTP/1.1\r\nHost: " + ep.address + ":" + std::to_string(ep.port) +
	                      "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
	for (size_t sent = 0; sent < request.size();)
	{
		ssize_t n = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
		if (n > 0)
			sent += n;
		else if (n < 0 and (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR))
		{
			if (not wait(POLLOUT))
				return fail();
		}
		else
			return fail();
	}

	std::string response;
	std::string body;
	std::array<char, 16384> buffer;
	while (true)
	{
		ssize_t n = recv(fd, buffer.data(), buffer.size(), 0);
		bool eof = n == 0;
		if (n > 0)
			response.append(buffer.data(), n);
		if (response.size() > max_response)
			return fail();
		else if (n < 0)
		{
			if (errno != EAGAIN and errno != EWOULDBLOCK and errno != EINTR)
				return fail();
			if (not wait(POLLIN))
				return fail();
			continue;
		}

		auto status = parse_response(response, eof, body);
		if (status == http_status::error)
			return fail();
		if (status == http_status::done)
			break;
	}
	close(fd);

	auto json = nlohmann::json::parse(body, nullptr, false);
	if (json.is_discarded())
		return std::nullopt;
	return json;
}

} // namespace wivrn::vrchat_osc
