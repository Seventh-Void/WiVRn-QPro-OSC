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

#include "pupil_service.h"

#include "camera_link.h"
#include "preview_server.h"
#include "pupil_tracker.h"

#include "vrchat_osc/unified.h"

#include "util/u_logging.h"

#include <nlohmann/json.hpp>

namespace wivrn::qpro
{
namespace
{
using namespace std::chrono_literals;

constexpr auto fresh = 350ms;       // older camera values fall back to 5 mm
constexpr auto retry_delay = 10s;   // after a failed start or a lost stream
constexpr auto status_period = 1s;

std::string status_json(std::string_view state, std::string_view message, const pupil_tracker::result * r = nullptr, float fps = 0)
{
	nlohmann::json j{{"state", state}, {"message", message}};
	if (r)
	{
		auto mm = [](const std::optional<float> & v) { return v ? nlohmann::json(*v) : nlohmann::json(nullptr); };
		j["left_mm"] = mm(r->left.mm);
		j["right_mm"] = mm(r->right.mm);
		j["left_status"] = r->left.status;
		j["right_status"] = r->right.status;
		j["fps"] = fps;
	}
	return j.dump();
}

void sleep_for(std::stop_token stop, std::chrono::steady_clock::duration d)
{
	auto until = std::chrono::steady_clock::now() + d;
	while (not stop.stop_requested() and std::chrono::steady_clock::now() < until)
		std::this_thread::sleep_for(100ms);
}
} // namespace

pupil_service::pupil_service(std::string headset_ip, float sensitivity) :
        thread([this, headset_ip = std::move(headset_ip), sensitivity](std::stop_token stop) { run(stop, headset_ip, sensitivity); })
{
}

void pupil_service::apply(vrchat_osc::face_state & f)
{
	std::lock_guard lock(mutex);
	bool ok = std::chrono::steady_clock::now() - updated <= fresh;
	f.left.pupil_mm = ok and mm[0] ? *mm[0] : 5;
	f.right.pupil_mm = ok and mm[1] ? *mm[1] : 5;
}

void pupil_service::run(std::stop_token stop, std::string headset_ip, float sensitivity)
{
	preview_server preview;
	if (not preview.bound())
		U_LOG_W("Pupil tracking: 127.0.0.1:8081 is in use, the dashboard cannot show its status");

	while (not stop.stop_requested())
	{
		try
		{
			preview.publish_status(status_json("starting", "Starting the eye cameras"));
			camera_link link("adb", headset_ip, WIVRN_QPRO_HEADSET_DIR);
			std::stop_callback on_stop(stop, [&link] { link.request_stop(); });
			link.start();
			U_LOG_I("Pupil tracking: eye cameras started");

			pupil_tracker tracker(sensitivity);
			auto next_status = std::chrono::steady_clock::now();
			auto last_frame = next_status;
			int frames = 0;
			while (not stop.stop_requested())
			{
				auto frame = link.next_frame(500ms);
				if (not frame)
				{
					if (std::chrono::steady_clock::now() - last_frame > 2s)
						preview.publish_status(status_json("starting", "Waiting for the eye cameras"));
					continue;
				}
				last_frame = std::chrono::steady_clock::now();
				auto r = tracker.update(frame->strip, frame->camera_ids);
				{
					std::lock_guard lock(mutex);
					mm[0] = r.left.mm;
					mm[1] = r.right.mm;
					updated = std::chrono::steady_clock::now();
				}
				++frames;
				if (preview.has_viewers())
					preview.publish_jpeg(tracker.preview_jpeg(frame->strip, frame->camera_ids, r));
				if (auto now = std::chrono::steady_clock::now(); now >= next_status)
				{
					float fps = frames / std::chrono::duration<float>(now - next_status + status_period).count();
					preview.publish_status(status_json("streaming", "", &r, fps));
					next_status = now + status_period;
					frames = 0;
				}
			}
		}
		catch (std::exception & e)
		{
			if (stop.stop_requested())
				break;
			U_LOG_W("Pupil tracking: %s", e.what());
			preview.publish_status(status_json("error", e.what()));
			{
				std::lock_guard lock(mutex);
				mm[0] = mm[1] = std::nullopt;
			}
			sleep_for(stop, retry_delay);
		}
	}
}

} // namespace wivrn::qpro
