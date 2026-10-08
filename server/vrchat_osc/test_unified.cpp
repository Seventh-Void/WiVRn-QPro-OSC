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
#include "unified.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <vector>

using namespace wivrn::vrchat_osc;

static std::vector<float> params(const face_state & f)
{
	std::vector<float> v(param_names().size());
	compute_params(f, v);
	return v;
}

static float get(const std::vector<float> & v, std::string_view name)
{
	auto names = param_names();
	for (std::size_t i = 0; i < names.size(); ++i)
		if (names[i] == name)
			return v[i];
	std::fprintf(stderr, "missing param %.*s\n", int(name.size()), name.data());
	assert(false);
	return NAN;
}

static bool near(float a, float b)
{
	return std::abs(a - b) < 1e-5f;
}

static face_state with(std::size_t index, float value)
{
	std::array<float, fb2_count> w{};
	w[index] = value;
	return from_fb2(w);
}

int main()
{
	auto names = param_names();

	// names unique, no prefix, size matches
	std::set<std::string_view> unique(names.begin(), names.end());
	assert(unique.size() == names.size());
	for (auto n: names)
		assert(!n.starts_with("v2/") && !n.empty());
	assert(names.size() == 179); // 88 unified + 8 simple + 83 combined
	assert(names[0] == "EyeSquintRight" && names[unified_count - 1] == "NeckFlexLeft");

	// all-zero input: eyes open, neutral
	{
		auto v = params(with(0, 0));
		assert(get(v, "EyeOpenLeft") == 1 && get(v, "EyeOpenRight") == 1);
		assert(get(v, "EyeClosedLeft") == 0);
		assert(get(v, "EyeLeftX") == 0 && get(v, "EyeY") == 0);
		assert(near(get(v, "PupilDilation"), 0.5f));
		face_state wide = from_fb2(std::array<float, fb2_count>{});
		wide.left.pupil_mm = wide.right.pupil_mm = 8;
		assert(near(get(params(wide), "PupilDilation"), 1) && near(get(params(wide), "PupilDiameterLeft"), 0.8f));
		assert(near(get(v, "PupilDiameterLeft"), 0.5f) && near(get(v, "PupilDiameter"), 0.5f));
		assert(near(get(v, "EyeLid"), 0.75f));
		assert(get(v, "EyeTrackingActive") == 1 && get(v, "ExpressionTrackingActive") == 1 && get(v, "LipTrackingActive") == 1);
		assert(get(v, "JawOpen") == 0);
	}

	// idle: eyes closed
	{
		auto f = idle_state();
		auto v = params(f);
		assert(get(v, "EyeClosedLeft") == 1 && get(v, "EyeClosedRight") == 1 && get(v, "EyeClosed") == 1);
		assert(get(v, "EyeOpen") == 0);
		assert(eyes_closed_amount(f) == 1);
	}

	// independent gaze: EyesLookRightL (18)
	{
		auto f = with(18, 1);
		auto v = params(f);
		assert(get(v, "EyeLeftX") > 0 && near(get(v, "EyeLeftX"), 0.5f));
		assert(get(v, "EyeRightX") == 0);
		assert(near(get(v, "EyeX"), 0.25f));
		auto py = left_right_pitch_yaw(f);
		assert(near(py[1], float(std::atan(0.5) * 180 / M_PI)) && py[1] > 0);
		assert(py[0] == 0 && py[2] == 0 && py[3] == 0);
	}
	// EyesLookUpR (21) -> right eye up, pitch negative (VRCFT ToPitch)
	{
		auto f = with(21, 1);
		auto v = params(f);
		assert(near(get(v, "EyeRightY"), 0.5f) && get(v, "EyeLeftY") == 0);
		assert(left_right_pitch_yaw(f)[2] < 0);
	}

	// eye closed with lid tightener (LinearLidTightening)
	{
		std::array<float, fb2_count> w{};
		w[12] = 0.5f; // EyesClosedL
		w[28] = 1;    // LidTightenerL
		auto f = from_fb2(w);
		assert(f.left.openness == 0 && f.right.openness == 1);
		auto v = params(f);
		assert(get(v, "EyeSquintLeft") == 1);
		assert(near(eyes_closed_amount(f), 0.5f));
	}

	// tongue
	{
		auto v = params(with(68, 0.7f)); // stock FB2 TongueOut
		assert(near(get(v, "TongueOut"), 0.7f));
		// BoltOn sample captured from a Quest Pro: tongue out, pointing right (63 out, 65 right, 67 down)
		std::array<float, fb2_count> w{};
		w[63] = 1.0f, w[65] = 0.96f, w[67] = 0.13f;
		v = params(from_fb2(w));
		assert(near(get(v, "TongueOut"), 1) && near(get(v, "TongueRight"), 0.96f) && get(v, "TongueLeft") == 0);
		assert(near(get(v, "TongueX"), 0.96f) && near(get(v, "TongueY"), -0.13f));
	}

	// jaw
	{
		auto v = params(with(24, 1)); // JawDrop
		assert(get(v, "JawOpen") == 1);
		v = params(with(26, 0.6f)); // JawSidewaysRight
		assert(near(get(v, "JawX"), 0.6f));
	}

	// smile: LipCornerPullerL (32) -> MouthSmileLeft = 0.8 * pull + 0.2 * slant
	{
		auto v = params(with(32, 1));
		assert(near(get(v, "MouthSmileLeft"), 0.8f + 0.2f));
		assert(near(get(v, "SmileSadLeft"), 1));
		assert(get(v, "SmileSadRight") == 0);
		assert(near(get(v, "SmileSad"), 0.5f));
		assert(near(get(v, "MouthCornerYLeft"), 1));
	}
	// sad: LipStretcherR (43)
	{
		auto v = params(with(43, 0.8f));
		assert(near(get(v, "SmileSadRight"), -0.8f) && near(get(v, "MouthTightenerStretchRight"), -0.8f));
	}

	// brows: BrowLowererL (0)
	{
		auto v = params(with(0, 1));
		assert(get(v, "BrowPinchLeft") == 1 && near(get(v, "BrowDownLeft"), 1));
		assert(near(get(v, "BrowExpressionLeft"), -1) && near(get(v, "BrowExpression"), -0.5f));
	}

	// lip suck upper suppressed by upper lip raiser
	{
		std::array<float, fb2_count> w{};
		w[45] = 1; // LipSuckLT
		assert(get(params(from_fb2(w)), "LipSuckUpperLeft") == 1);
		w[61] = 1; // UpperLipRaiserL
		auto v = params(from_fb2(w));
		assert(get(v, "LipSuckUpperLeft") == 0);
		assert(near(get(v, "MouthUpperUpLeft"), 1) && near(get(v, "MouthUpperDeepenLeft"), 1));
	}

	std::printf("test_unified: OK (%zu params)\n", names.size());
	return 0;
}
