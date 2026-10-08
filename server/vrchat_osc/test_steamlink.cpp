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
#include "steamlink.h"

#include <arpa/inet.h>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace wivrn::vrchat_osc;
using namespace std::string_view_literals;

// Oracle: the /sl/xrfb/facew/ names in Resonite's FrooxEngine.dll
static const std::string_view resonite_names[] = {
        "BrowLowererL", "BrowLowererR", "CheekPuffL", "CheekPuffR", "CheekRaiserL", "CheekRaiserR",
        "CheekSuckL", "CheekSuckR", "ChinRaiserB", "ChinRaiserT", "DimplerL", "DimplerR", "EyesClosedL",
        "EyesClosedR", "InnerBrowRaiserL", "InnerBrowRaiserR", "JawDrop", "JawSidewaysLeft",
        "JawSidewaysRight", "JawThrust", "LidTightenerL", "LidTightenerR", "LipCornerDepressorL",
        "LipCornerDepressorR", "LipCornerPullerL", "LipCornerPullerR", "LipFunnelerLB", "LipFunnelerLT",
        "LipFunnelerRB", "LipFunnelerRT", "LipPressorL", "LipPressorR", "LipPuckerL", "LipPuckerR",
        "LipsToward", "LipStretcherL", "LipStretcherR", "LipSuckLB", "LipSuckLT", "LipSuckRB", "LipSuckRT",
        "LipTightenerL", "LipTightenerR", "LowerLipDepressorL", "LowerLipDepressorR", "MouthLeft",
        "MouthRight", "NoseWrinklerL", "NoseWrinklerR", "OuterBrowRaiserL", "OuterBrowRaiserR", "TongueOut",
        "TongueRetreat", "UpperLidRaiserL", "UpperLidRaiserR", "UpperLipRaiserL", "UpperLipRaiserR",
};

static bool near(float a, float b)
{
	return std::abs(a - b) < 1e-5f;
}

static float length(std::array<float, 3> v)
{
	return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static uint32_t get_u32(const unsigned char * p)
{
	return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// OSC string at p, returns the padded size
static std::size_t get_string(const unsigned char * p, std::string & out)
{
	out = (const char *)p;
	return (out.size() / 4 + 1) * 4;
}

using message = std::pair<std::string, std::vector<float>>;

static void parse_bundle(const unsigned char * p, std::size_t size, std::vector<message> & out)
{
	assert(size >= 16 and std::memcmp(p, "#bundle\0", 8) == 0);
	for (std::size_t at = 16; at < size;)
	{
		std::size_t len = get_u32(p + at), m = at + 4;
		at = m + len;
		assert(at <= size);
		message msg;
		std::string tags;
		m += get_string(p + m, msg.first);
		m += get_string(p + m, tags);
		assert(tags.size() >= 2 and tags[0] == ',' and tags.find_first_not_of('f', 1) == std::string::npos);
		for (std::size_t i = 1; i < tags.size(); ++i, m += 4)
			msg.second.push_back(std::bit_cast<float>(get_u32(p + m)));
		assert(m == at);
		out.push_back(std::move(msg));
	}
}

int main()
{
	// name table: matches Resonite's list exactly, unique, index maps to the same fb name
	static_assert(std::size(resonite_names) == 57);
	std::set<std::string_view> unique;
	for (std::size_t i = 0; i < steamlink_shapes.size(); ++i)
	{
		assert(steamlink_shapes[i].name == resonite_names[i]);
		assert(steamlink_shapes[i].index < fb2_count);
		unique.insert(steamlink_shapes[i].name);
	}
	assert(unique.size() == 57);
	assert(steamlink_shapes[0].index == fb::BrowLowererL && steamlink_shapes[0].index == 0);
	assert(steamlink_shapes[26].name == "LipFunnelerLB" && steamlink_shapes[26].index == 34);
	assert(steamlink_shapes[51].name == "TongueOut" && steamlink_shapes[51].index == 68);
	assert(steamlink_shapes[52].name == "TongueRetreat" && steamlink_shapes[52].index == 69);

	// direction: OpenXR, unit length
	{
		auto ahead = gaze_direction(0, 0);
		assert(ahead[0] == 0 && ahead[1] == 0 && ahead[2] == -1);
		auto right = gaze_direction(0.5f, 0);
		assert(right[0] > 0 && right[1] == 0 && right[2] < 0 && near(length(right), 1));
		assert(near(right[0] / -right[2], 0.5f)); // tangent preserved
		auto up_left = gaze_direction(-0.3f, 0.4f);
		assert(up_left[0] < 0 && up_left[1] > 0 && near(length(up_left), 1));
	}

	// cmdline matcher: NUL-separated, case-insensitive
	{
		auto p = match_cmdline("Z:\\home\\u\\Steam\\steamapps\\common\\Resonite\\RESONITE.EXE\0-Screen\0"sv);
		assert(p.resonite && !p.edrakon);
		p = match_cmdline("dotnet\0/opt/edrakon/edrakon.dll\0"sv);
		assert(!p.resonite && p.edrakon);
		p = match_cmdline("wine\0resonite.exe\0--edrakon\0"sv);
		assert(p.resonite && p.edrakon);
		p = match_cmdline("Resonite\0.exe\0"sv); // split across arguments: no match
		assert(!p.resonite && !p.edrakon);
		p = match_cmdline("");
		assert(!p.resonite && !p.edrakon);
	}

	// round trip over UDP, ephemeral port (never the real 9015)
	{
		int rx = socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		assert(bind(rx, (sockaddr *)&a, sizeof(a)) == 0);
		socklen_t len = sizeof(a);
		assert(getsockname(rx, (sockaddr *)&a, &len) == 0);
		timeval tv{0, 200'000};
		setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		std::array<float, fb2_count> w{};
		w[fb::JawDrop] = 0.4f;
		w[fb::LipFunnelerLB] = 0.25f;
		w[fb::TongueRetreat] = 0.6f;
		w[fb::TongueTipInterdental] = 0.9f; // BoltOn TongueHack: tongue out at 63
		w[fb::TongueOut] = 0.2f;
		face_state f;
		f.left.gaze_x = 0.5f;
		f.right.gaze_x = 0.1f;

		osc_sender tx("127.0.0.1", ntohs(a.sin_port));
		send_steamlink(tx, w, f);

		std::vector<message> got;
		unsigned char buf[4096];
		int packets = 0;
		for (ssize_t n; got.size() < 3 + 57 and (n = recv(rx, buf, sizeof(buf), 0)) > 0; ++packets)
		{
			assert(std::size_t(n) <= osc_sender::max_packet);
			parse_bundle(buf, n, got);
		}
		close(rx);
		assert(packets >= 2); // 60 messages do not fit in one 1400-byte bundle

		const std::string_view head[] = {"/sl/eyeTrackedGazePoint", "/sl/xrfb/facec/LowerFace", "/sl/xrfb/facec/UpperFace"};
		assert(got.size() == std::size(head) + 57);
		for (std::size_t i = 0; i < got.size(); ++i)
		{
			auto expected = i < std::size(head) ? std::string(head[i]) : "/sl/xrfb/facew/" + std::string(resonite_names[i - std::size(head)]);
			assert(got[i].first == expected);
			assert(got[i].second.size() == (i == 0 ? 3u : 1u));
		}
		auto value = [&](std::string_view name) { for (auto & m: got) if (m.first == "/sl/xrfb/facew/" + std::string(name)) return m.second[0]; assert(false); return NAN; };

		auto combined = gaze_direction(0.3f, 0);
		for (int i = 0; i < 3; ++i)
			assert(near(got[0].second[i], combined[i]));
		assert(got[1].second[0] == 1 && got[2].second[0] == 1);
		assert(value("JawDrop") == 0.4f && value("LipFunnelerLB") == 0.25f && value("BrowLowererL") == 0);
		assert(value("TongueOut") == 0.9f && value("TongueRetreat") == 0.6f);
	}

	std::puts("test_steamlink: OK");
}
