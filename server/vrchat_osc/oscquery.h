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

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace wivrn::vrchat_osc
{

struct http_endpoint
{
	std::string address; // numeric IPv4
	uint16_t port;
};

// Blocking mDNS browse for "_oscjson._tcp.local." services whose instance name starts with
// "VRChat-Client"; waits at most timeout_ms. Returns the first match.
std::optional<http_endpoint> find_vrchat(int timeout_ms = 1000);

// Blocking HTTP/1.1 GET http://<ep>/avatar (timeout_ms for connect + read). Returns parsed JSON or nullopt.
std::optional<nlohmann::json> fetch_avatar(const http_endpoint &, int timeout_ms = 1000);

} // namespace wivrn::vrchat_osc
