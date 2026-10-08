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

#include "vrchat_osc.h"

#include "osc.h"
#include "oscquery.h"
#include "params.h"
#include "steamlink.h"
#include "unified.h"

#include "driver/wivrn_fb_face2_tracker.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wivrn::vrchat_osc
{
namespace
{
constexpr auto output_period = std::chrono::microseconds(1'000'000 / 60);
constexpr auto scan_period = std::chrono::seconds(3); // VRChat OSCQuery discovery, Resonite / Edrakon process scan

struct shared_state
{
	std::mutex mutex;
	std::optional<nlohmann::json> avatar; // new avatar tree to load, consumed by the output thread
	std::atomic<bool> resonite = false;   // send Steam Link OSC (Resonite runs, Edrakon doesn't)
};

// Cheap identity of an avatar tree: parameter paths only (VALUEs change every frame).
void collect_paths(const nlohmann::json & node, std::string & out, int depth = 0)
{
	if (depth > max_json_depth)
		return;
	if (auto it = node.find("FULL_PATH"); it != node.end() and it->is_string())
		out += it->get_ref<const std::string &>() + '\n';
	if (auto it = node.find("CONTENTS"); it != node.end() and it->is_object())
		for (const auto & child: *it)
			collect_paths(child, out, depth + 1);
}

void scan_resonite(shared_state & state, bool & edrakon_warned)
{
	auto p = scan_processes();
	bool on = p.resonite and not p.edrakon;
	if (on != state.resonite.exchange(on))
	{
		if (on)
			U_LOG_I("Resonite detected: sending face tracking (Steam Link OSC) to 127.0.0.1:%d", steamlink_port);
		else
			U_LOG_I("Resonite output to 127.0.0.1:%d paused", steamlink_port);
	}
	if (p.resonite and p.edrakon and not edrakon_warned)
		U_LOG_W("Edrakon is running, leaving 127.0.0.1:%d to it; close Edrakon for face tracking from WiVRn in Resonite", steamlink_port);
	edrakon_warned = p.resonite and p.edrakon;
}

void run_discovery(std::stop_token stop, std::shared_ptr<shared_state> state)
{
	std::string last_paths;
	bool found = false, edrakon_warned = false;
	while (not stop.stop_requested())
	{
		// ponytail: polls VRChat's OSCQuery tree instead of serving our own OSCQuery node for /avatar/change;
		// avatar switches are picked up within scan_period. Port oscavmgr's oscquery.rs responder if that's too slow.
		try
		{
			if (auto ep = find_vrchat())
			{
				if (not found)
					U_LOG_I("VRChat OSC: found VRChat OSCQuery at %s:%d", ep->address.c_str(), ep->port);
				found = true;
				if (auto avatar = fetch_avatar(*ep))
				{
					std::string paths;
					collect_paths(*avatar, paths);
					if (paths != last_paths)
					{
						last_paths = std::move(paths);
						std::lock_guard lock(state->mutex);
						state->avatar = std::move(*avatar);
					}
				}
			}
			else if (found)
			{
				U_LOG_I("VRChat OSC: VRChat OSCQuery lost");
				found = false;
				last_paths.clear(); // reload (and resend) the avatar when VRChat comes back
			}
			scan_resonite(*state, edrakon_warned);
		}
		catch (std::exception & e)
		{
			U_LOG_W("VRChat OSC: discovery error: %s", e.what());
		}

		auto until = std::chrono::steady_clock::now() + scan_period;
		while (not stop.stop_requested() and std::chrono::steady_clock::now() < until)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

void run_output(std::stop_token stop, wivrn_fb_face2_tracker & face, std::shared_ptr<shared_state> state, std::string host, uint16_t port, qpro::pupil_service * pupils)
{
	std::optional<osc_sender> sender, resonite;
	try
	{
		sender.emplace(host, port);
		resonite.emplace("127.0.0.1", steamlink_port);
	}
	catch (std::exception & e)
	{
		U_LOG_E("VRChat OSC: cannot send to %s:%d: %s", host.c_str(), port, e.what());
		return;
	}

	avatar_params params(param_names());
	params.load_defaults();
	std::vector<float> values(param_names().size());

	auto next = std::chrono::steady_clock::now();
	auto next_resend = next + scan_period;
	while (not stop.stop_requested())
	{
		next += output_period;
		std::this_thread::sleep_until(next);

		{
			std::lock_guard lock(state->mutex);
			if (state->avatar)
			{
				params.load(*state->avatar);
				state->avatar.reset();
				U_LOG_I("VRChat OSC: avatar has %zu face tracking parameters", params.size());
			}
		}

		// Polling also issues the tracking request that keeps the headset sending face data.
		xrt_facial_expression_set set{};
		face.get_face_tracking(XRT_INPUT_FB_FACE_TRACKING2_VISUAL, os_monotonic_get_ns(), &set);
		const auto & fb = set.face_expression_set2_fb;

		auto state_now = fb.is_valid ? from_fb2(std::span<const float, fb2_count>(fb.weights, fb2_count)) : idle_state();
		if (pupils)
			pupils->apply(state_now);

		// VRChat drops parameter values on restart / avatar reload: send everything again now and then
		if (next >= next_resend)
		{
			next_resend = next + scan_period;
			params.resend();
		}
		compute_params(state_now, values);
		params.send(values, *sender);

		// VRChat native eye tracking, only for avatars without their own eye parameters (as VRCFaceTracking does)
		if (not params.has_eye_gaze_params())
		{
			auto pitch_yaw = left_right_pitch_yaw(state_now);
			sender->add("/tracking/eye/LeftRightPitchYaw", std::span<const float>(pitch_yaw));
		}
		if (not params.has_eye_lid_params())
			sender->add("/tracking/eye/EyesClosedAmount", eyes_closed_amount(state_now));

		sender->flush();

		// Resonite (Steam Link OSC), only while it runs and Edrakon doesn't own the port
		if (state->resonite and fb.is_valid)
			send_steamlink(*resonite, std::span<const float, fb2_count>(fb.weights, fb2_count), state_now);
	}
}
} // namespace

emitter::emitter(wivrn_fb_face2_tracker & face, const std::string & host, uint16_t port, std::optional<float> pupil_sensitivity, const std::string & headset_ip)
{
	auto state = std::make_shared<shared_state>();
	U_LOG_I("VRChat OSC: sending face tracking to %s:%d", host.c_str(), port);
	if (pupil_sensitivity)
		pupils.emplace(headset_ip, *pupil_sensitivity);
	discovery = std::jthread(run_discovery, state);
	output = std::jthread(run_output, std::ref(face), state, host, port, pupils ? &*pupils : nullptr);
}

} // namespace wivrn::vrchat_osc
