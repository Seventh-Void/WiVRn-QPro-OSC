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

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace wivrn::vrchat_osc
{
class osc_sender;

// OSCQuery trees nested deeper than this are ignored (stack safety against hostile replies)
constexpr int max_json_depth = 64;

// Maps the current avatar's OSC parameters onto VRCFT v2 parameter indices
// (indices into the `names` list given at construction, i.e. param_names()).
// Handles float, bool, binary-encoded (Name1/2/4/8…) and NameNegative params, like VRCFaceTracking.
class avatar_params
{
public:
	explicit avatar_params(std::span<const std::string_view> names);

	// Default set used before any avatar info is known: float params "/avatar/parameters/FT/v2/<Name>"
	// for a small list of common names (no binary).
	void load_defaults();

	// Parse the JSON returned by VRChat's OSCQuery server for the "/avatar" node
	// (nodes with "FULL_PATH", "TYPE", "CONTENTS"). Matches addresses ending in "v2/<Name>[suffix]".
	void load(const nlohmann::json & avatar_node);

	// Queue messages for values that changed since the last call (all of them after a load).
	// values is indexed like `names`.
	void send(std::span<const float> values, osc_sender &);

	// Send every parameter again on the next send()
	void resend()
	{
		for (auto & e: entries)
			e.sent = false;
	}

	bool has_eye_gaze_params() const // avatar drives eyes itself (Eye…X / Eye…Y)
	{
		return gaze;
	}
	bool has_eye_lid_params() const // avatar has Eye…Open / Eye…Lid
	{
		return lid;
	}
	std::size_t size() const // number of matched v2 parameters
	{
		return entries.size();
	}

private:
	struct entry
	{
		std::size_t index;
		bool tracking_active; // VRCFT ConditionalBoolParameter (…TrackingActive): bool = value
		std::string main{};   // float or bool parameter with the exact name
		bool main_is_bool = false;
		std::string neg{};              // <Name>Negative
		std::array<std::string, 8> bits{}; // <Name>1, <Name>2, … <Name>128, indexed by bit
		int num_bits = 0;               // number of bit parameters present

		bool sent = false; // false: send everything on next send()
		float last_value = 0;
		bool last_main = false;
		bool last_neg = false;
		std::array<bool, 8> last_bits{};
	};
	std::span<const std::string_view> names;
	std::vector<entry> entries;
	bool gaze = false;
	bool lid = false;
};

} // namespace wivrn::vrchat_osc
