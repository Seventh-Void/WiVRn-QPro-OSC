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

#include "steamlink.h"

#include "osc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace wivrn::vrchat_osc
{
namespace
{
bool contains_nocase(std::string_view haystack, std::string_view needle)
{
	return std::ranges::search(haystack, needle, [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }).begin() != haystack.end();
}
} // namespace

// gaze_x/gaze_y are tangents (as in left_right_pitch_yaw for VRChat), so the direction is (x, y, -1) normalized;
// a unit vector is also a valid "point the user looks at" one metre away.
std::array<float, 3> gaze_direction(float gaze_x, float gaze_y)
{
	float n = std::sqrt(gaze_x * gaze_x + gaze_y * gaze_y + 1);
	return {gaze_x / n, gaze_y / n, -1 / n};
}

process_match match_cmdline(std::string_view cmdline)
{
	return {contains_nocase(cmdline, "Resonite.exe"), contains_nocase(cmdline, "Edrakon")};
}

process_match scan_processes()
{
	process_match m;
	std::error_code ec;
	for (std::filesystem::directory_iterator it("/proc", ec), end; not ec and it != end; it.increment(ec))
	{
		auto pid = it->path().filename().string();
		if (not std::isdigit((unsigned char)pid[0]))
			continue;
		std::ifstream f(it->path() / "cmdline"); // gone or unreadable: empty
		std::string cmdline{std::istreambuf_iterator<char>(f), {}};
		auto p = match_cmdline(cmdline);
		m.resonite |= p.resonite;
		m.edrakon |= p.edrakon;
	}
	return m;
}

void send_steamlink(osc_sender & out, std::span<const float, fb2_count> w, const face_state & f)
{
	auto combined = gaze_direction((f.left.gaze_x + f.right.gaze_x) / 2, (f.left.gaze_y + f.right.gaze_y) / 2);
	out.add("/sl/eyeTrackedGazePoint", std::span<const float>(combined));
	// Meta's face confidences are not passed through; report them as valid
	out.add("/sl/xrfb/facec/LowerFace", 1.0f);
	out.add("/sl/xrfb/facec/UpperFace", 1.0f);
	for (const auto & s: steamlink_shapes)
	{
		float v = w[s.index];
		// BoltOn's TongueHack layout puts tongue out at index 63, same as the VRChat output (unified.cpp)
		if (s.index == fb::TongueOut)
			v = std::max(w[fb::TongueTipInterdental], v);
		out.add("/sl/xrfb/facew/" + std::string(s.name), v);
	}
	out.flush();
}

} // namespace wivrn::vrchat_osc
