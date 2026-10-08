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
#include <thread>

#include "qpro_pupil/pupil_service.h"

namespace wivrn
{
class wivrn_fb_face2_tracker;
}

namespace wivrn::vrchat_osc
{

// Streams FB face tracking 2 data to VRChat as VRCFaceTracking v2 OSC parameters,
// replacing VRCFaceTracking / oscavmgr on the PC ("Direct" mode).
class emitter
{
	std::optional<qpro::pupil_service> pupils; // outlives the threads below, which read it
	std::jthread discovery;                     // finds VRChat via OSCQuery, fetches avatar parameters
	std::jthread output;                        // polls the face tracker and sends OSC

public:
	// pupil_sensitivity: camera pupil tracking on a rooted Quest Pro at headset_ip, off when empty
	emitter(wivrn_fb_face2_tracker & face, const std::string & host, uint16_t port, std::optional<float> pupil_sensitivity, const std::string & headset_ip);
};

} // namespace wivrn::vrchat_osc
