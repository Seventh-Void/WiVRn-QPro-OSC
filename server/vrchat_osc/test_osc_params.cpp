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
#include "osc.h"
#include "params.h"

#include <arpa/inet.h>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <unistd.h>

using namespace wivrn::vrchat_osc;

struct value
{
	char tag;
	float f = 0;
};

// Minimal decoder: receive all pending bundles, return address -> (tag, value)
static std::map<std::string, value> receive(int fd, size_t * packets = nullptr)
{
	std::map<std::string, value> out;
	char buf[2048];
	ssize_t n;
	while ((n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0)
	{
		if (packets)
			++*packets;
		assert(n <= ssize_t(osc_sender::max_packet));
		assert(memcmp(buf, "#bundle\0\0\0\0\0\0\0\0\1", 16) == 0);
		auto u32 = [&](size_t pos) { return ntohl(*(uint32_t *)(buf + pos)); };
		auto str_len = [&](size_t pos) { return (strlen(buf + pos) + 4) & ~size_t(3); };
		for (size_t pos = 16; pos < size_t(n);)
		{
			size_t size = u32(pos), msg = pos + 4;
			std::string addr = buf + msg;
			size_t tags = msg + str_len(msg);
			size_t args = tags + str_len(tags);
			assert(strlen(buf + tags) == 2);
			value v{buf[tags + 1]};
			if (v.tag == 'f')
				v.f = std::bit_cast<float>(u32(args));
			out[addr] = v;
			pos = msg + size;
		}
	}
	return out;
}

int main()
{
	// 1. message encoding
	{
		float one = 1;
		auto msg = osc_sender::encode_message("/a", {&one, 1});
		const unsigned char expected[] = {'/', 'a', 0, 0, ',', 'f', 0, 0, 0x3f, 0x80, 0, 0};
		assert(msg.size() == sizeof(expected));
		assert(memcmp(msg.data(), expected, sizeof(expected)) == 0);
	}

	// local receiver
	int rx = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(rx, (sockaddr *)&addr, sizeof(addr)) == 0);
	socklen_t len = sizeof(addr);
	assert(getsockname(rx, (sockaddr *)&addr, &len) == 0);
	osc_sender osc("127.0.0.1", ntohs(addr.sin_port));

	// 2. bundles split at max_packet
	{
		for (int i = 0; i < 100; ++i)
			osc.add("/avatar/parameters/FT/v2/SomeLongParameterName" + std::to_string(i), float(i));
		osc.flush();
		size_t packets = 0;
		auto got = receive(rx, &packets);
		assert(got.size() == 100 and packets > 1);
		assert(got["/avatar/parameters/FT/v2/SomeLongParameterName42"].f == 42);
	}

	// 3. parameter matching
	constexpr std::string_view names[] = {"JawOpen", "MouthX", "EyeLeftX", "TongueOut", "EyeTrackingActive", "EyeLeftY"};
	auto leaf = [](std::string path, const char * type) { return nlohmann::json{{"FULL_PATH", path}, {"TYPE", type}}; };
	const std::string v2 = "/avatar/parameters/FT/v2/";
	nlohmann::json avatar = {
	        {"FULL_PATH", "/avatar"},
	        {"CONTENTS",
	         {{"parameters",
	           {{"FULL_PATH", "/avatar/parameters"},
	            {"CONTENTS",
	             {{"FT",
	               {{"FULL_PATH", "/avatar/parameters/FT"},
	                {"CONTENTS",
	                 {{"v2",
	                   {{"FULL_PATH", "/avatar/parameters/FT/v2"},
	                    {"CONTENTS",
	                     {{"JawOpen", leaf(v2 + "JawOpen", "f")},
	                      {"MouthX1", leaf(v2 + "MouthX1", "T")},
	                      {"MouthX2", leaf(v2 + "MouthX2", "T")},
	                      {"MouthX4", leaf(v2 + "MouthX4", "T")},
	                      {"MouthXNegative", leaf(v2 + "MouthXNegative", "T")},
	                      {"EyeLeftX", leaf(v2 + "EyeLeftX", "f")},
	                      {"TongueOut", leaf(v2 + "TongueOut", "T")}}}}}}}}},
	              {"OSCm", {{"FULL_PATH", "/avatar/parameters/OSCm"}, {"CONTENTS", {{"EyeLeftY", leaf("/avatar/parameters/OSCm/v2/EyeLeftY", "f")}}}}},
	              {"NoV2", leaf("/avatar/parameters/JawOpen", "f")},
	              {"EyeTrackingActive", leaf("/avatar/parameters/EyeTrackingActive", "T")},
	              {"VRCEmote", leaf("/avatar/parameters/VRCEmote", "i")}}}}}}}};

	avatar_params params(names);
	params.load(avatar);
	assert(params.size() == 5); // JawOpen, MouthX, EyeLeftX, TongueOut, EyeTrackingActive
	assert(params.has_eye_gaze_params());
	assert(not params.has_eye_lid_params());

	float values[] = {0.25f, -0.6f, 0.3f, 0.8f, 1.f, 0.f};
	params.send(values, osc);
	osc.flush();
	auto got = receive(rx);
	assert(got.size() == 8);
	assert(got[v2 + "JawOpen"].tag == 'f' and got[v2 + "JawOpen"].f == 0.25f);
	assert(got[v2 + "EyeLeftX"].tag == 'f' and std::abs(got[v2 + "EyeLeftX"].f - 0.3f) < 1e-6);
	// VRCFT: |-0.6| * 2^3 = 4.8 -> 4 = 0b100
	assert(got[v2 + "MouthXNegative"].tag == 'T');
	assert(got[v2 + "MouthX1"].tag == 'F');
	assert(got[v2 + "MouthX2"].tag == 'F');
	assert(got[v2 + "MouthX4"].tag == 'T');
	assert(got[v2 + "TongueOut"].tag == 'F'); // VRCFT EParam bool: value < 0.5
	assert(got["/avatar/parameters/EyeTrackingActive"].tag == 'T');

	// unchanged values: nothing sent
	params.send(values, osc);
	osc.flush();
	assert(receive(rx).empty());

	// resend(): everything again (VRChat restarted / reloaded the avatar)
	params.resend();
	params.send(values, osc);
	osc.flush();
	assert(receive(rx).size() == 8);

	// small float change ignored, bit change sent
	values[0] = 0.255f;
	values[1] = 1.f; // all bits set, negative cleared
	params.send(values, osc);
	osc.flush();
	got = receive(rx);
	assert(got.size() == 3); // Negative, MouthX1, MouthX2 (MouthX4 already true)
	assert(got[v2 + "MouthXNegative"].tag == 'F');
	assert(got[v2 + "MouthX1"].tag == 'T' and got[v2 + "MouthX2"].tag == 'T');

	// defaults: only names present in the list
	params.load_defaults();
	assert(params.size() == 2); // JawOpen, MouthX
	params.send(values, osc);
	osc.flush();
	got = receive(rx);
	assert(got.size() == 2 and got[v2 + "MouthX"].f == 1.f);

	close(rx);
	puts("test_osc_params: OK");
}
