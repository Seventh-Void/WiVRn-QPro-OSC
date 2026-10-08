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

#include "osc.h"

#include <arpa/inet.h>
#include <bit>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace wivrn::vrchat_osc
{
namespace
{
void put_string(std::vector<std::byte> & out, std::string_view s)
{
	for (char c: s)
		out.push_back(std::byte(c));
	// NUL terminator + padding to 4 bytes
	do
		out.push_back(std::byte(0));
	while (out.size() % 4);
}

void put_u32(std::vector<std::byte> & out, uint32_t v)
{
	for (int shift = 24; shift >= 0; shift -= 8)
		out.push_back(std::byte(v >> shift));
}

std::vector<std::byte> encode(std::string_view address, std::string_view tags, std::span<const float> values)
{
	std::vector<std::byte> msg;
	put_string(msg, address);
	put_string(msg, tags);
	for (float f: values)
		put_u32(msg, std::bit_cast<uint32_t>(f));
	return msg;
}
} // namespace

osc_sender::osc_sender(const std::string & host, uint16_t port)
{
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
		throw std::system_error(std::make_error_code(std::errc::invalid_argument), "invalid OSC host " + host);

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		throw std::system_error(errno, std::system_category(), "socket");

	if (connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
	{
		int err = errno;
		close(fd);
		throw std::system_error(err, std::system_category(), "connect");
	}
}

osc_sender::~osc_sender()
{
	close(fd);
}

void osc_sender::append(const std::vector<std::byte> & msg)
{
	if (messages and packet.size() + 4 + msg.size() > max_packet)
		flush();
	if (packet.empty())
	{
		put_string(packet, "#bundle");
		put_u32(packet, 0);
		put_u32(packet, 1); // timetag: immediately
	}
	put_u32(packet, msg.size());
	packet.insert(packet.end(), msg.begin(), msg.end());
	++messages;
}

void osc_sender::add(std::string_view address, float value)
{
	add(address, std::span<const float>(&value, 1));
}

void osc_sender::add(std::string_view address, bool value)
{
	append(encode(address, value ? ",T" : ",F", {}));
}

void osc_sender::add(std::string_view address, std::span<const float> values)
{
	append(encode_message(address, values));
}

void osc_sender::flush()
{
	if (messages == 0)
		return;
	// Best effort: ECONNREFUSED when VRChat is not running is expected
	(void)::send(fd, packet.data(), packet.size(), MSG_DONTWAIT);
	packet.clear();
	messages = 0;
}

std::vector<std::byte> osc_sender::encode_message(std::string_view address, std::span<const float> values)
{
	return encode(address, "," + std::string(values.size(), 'f'), values);
}

} // namespace wivrn::vrchat_osc
