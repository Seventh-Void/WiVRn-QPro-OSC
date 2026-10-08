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

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wivrn::vrchat_osc
{

// Encodes OSC 1.0 messages into bundles and sends them over UDP.
// Bundles are flushed automatically before they would exceed max_packet bytes.
class osc_sender
{
	int fd = -1;
	std::vector<std::byte> packet; // current bundle, "#bundle\0" + timetag already written when non-empty
	std::size_t messages = 0;

	void append(const std::vector<std::byte> & msg);

public:
	static constexpr std::size_t max_packet = 1400;

	// host is a numeric IPv4 address (e.g. "127.0.0.1"); throws std::system_error on failure
	osc_sender(const std::string & host, uint16_t port);
	~osc_sender();
	osc_sender(const osc_sender &) = delete;
	osc_sender & operator=(const osc_sender &) = delete;

	void add(std::string_view address, float value);
	void add(std::string_view address, bool value); // OSC type tag T / F, no payload
	void add(std::string_view address, std::span<const float> values);

	// Send pending messages (no-op when empty). Send errors are swallowed (UDP, best effort).
	void flush();

	// Test hook: encode one message without sending.
	static std::vector<std::byte> encode_message(std::string_view address, std::span<const float> values);
};

} // namespace wivrn::vrchat_osc
