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

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace wivrn::vrchat_osc
{
struct face_state;
}

namespace wivrn::qpro
{

// Camera pupil tracking on a rooted Quest Pro for one headset session: starts the eye cameras over adb,
// tracks both pupils, and serves status + camera view to the dashboard on 127.0.0.1:8081.
// Failures (no adb device, no root…) are shown on the dashboard and retried.
class pupil_service
{
	std::mutex mutex;
	std::optional<float> mm[2];
	std::chrono::steady_clock::time_point updated;
	std::jthread thread;

	void run(std::stop_token, std::string headset_ip, float sensitivity);

public:
	pupil_service(std::string headset_ip, float sensitivity);

	// Writes left/right pupil_mm: the camera values while fresh, else 5 mm.
	void apply(vrchat_osc::face_state &);
};

} // namespace wivrn::qpro
