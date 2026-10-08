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

#include "unified.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <cassert>

namespace wivrn::vrchat_osc
{

namespace
{
// VRCFaceTracking UnifiedExpressions, same order (Max excluded)
#define WIVRN_UNIFIED_SHAPES(X)                                                                                                           \
	X(EyeSquintRight) X(EyeSquintLeft) X(EyeWideRight) X(EyeWideLeft)                                                                 \
	X(BrowPinchRight) X(BrowPinchLeft) X(BrowLowererRight) X(BrowLowererLeft)                                                         \
	X(BrowInnerUpRight) X(BrowInnerUpLeft) X(BrowOuterUpRight) X(BrowOuterUpLeft)                                                     \
	X(NasalDilationRight) X(NasalDilationLeft) X(NasalConstrictRight) X(NasalConstrictLeft)                                           \
	X(CheekSquintRight) X(CheekSquintLeft) X(CheekPuffRight) X(CheekPuffLeft) X(CheekSuckRight) X(CheekSuckLeft)                      \
	X(JawOpen) X(JawRight) X(JawLeft) X(JawForward) X(JawBackward) X(JawClench) X(JawMandibleRaise) X(MouthClosed)                    \
	X(LipSuckUpperRight) X(LipSuckUpperLeft) X(LipSuckLowerRight) X(LipSuckLowerLeft) X(LipSuckCornerRight) X(LipSuckCornerLeft)      \
	X(LipFunnelUpperRight) X(LipFunnelUpperLeft) X(LipFunnelLowerRight) X(LipFunnelLowerLeft)                                         \
	X(LipPuckerUpperRight) X(LipPuckerUpperLeft) X(LipPuckerLowerRight) X(LipPuckerLowerLeft)                                         \
	X(MouthUpperUpRight) X(MouthUpperUpLeft) X(MouthUpperDeepenRight) X(MouthUpperDeepenLeft) X(NoseSneerRight) X(NoseSneerLeft)      \
	X(MouthLowerDownRight) X(MouthLowerDownLeft) X(MouthUpperRight) X(MouthUpperLeft) X(MouthLowerRight) X(MouthLowerLeft)            \
	X(MouthCornerPullRight) X(MouthCornerPullLeft) X(MouthCornerSlantRight) X(MouthCornerSlantLeft)                                   \
	X(MouthFrownRight) X(MouthFrownLeft) X(MouthStretchRight) X(MouthStretchLeft) X(MouthDimpleRight) X(MouthDimpleLeft)              \
	X(MouthRaiserUpper) X(MouthRaiserLower) X(MouthPressRight) X(MouthPressLeft) X(MouthTightenerRight) X(MouthTightenerLeft)         \
	X(TongueOut) X(TongueUp) X(TongueDown) X(TongueRight) X(TongueLeft) X(TongueRoll) X(TongueBendDown) X(TongueCurlUp)               \
	X(TongueSquish) X(TongueFlat) X(TongueTwistRight) X(TongueTwistLeft)                                                              \
	X(SoftPalateClose) X(ThroatSwallow) X(NeckFlexRight) X(NeckFlexLeft)

#define X(n) n,
enum shape : std::size_t
{
	WIVRN_UNIFIED_SHAPES(X) shape_count
};
#undef X
static_assert(shape_count == unified_count);

#define X(n) #n,
constexpr std::string_view shape_names[] = {WIVRN_UNIFIED_SHAPES(X)};
#undef X

// VRCFaceTracking UnifiedSimpleExpressions, same order
enum simple : std::size_t
{
	BrowUpRight,
	BrowUpLeft,
	BrowDownRight,
	BrowDownLeft,
	MouthSmileRight,
	MouthSmileLeft,
	MouthSadRight,
	MouthSadLeft,
	simple_count
};
constexpr std::string_view simple_names[] = {
        "BrowUpRight",
        "BrowUpLeft",
        "BrowDownRight",
        "BrowDownLeft",
        "MouthSmileRight",
        "MouthSmileLeft",
        "MouthSadRight",
        "MouthSadLeft",
};
static_assert(std::size(simple_names) == simple_count);

// Inputs of the combined parameter formulas
struct ctx
{
	const face_state & f;
	const float * s; // simple expressions

	float operator[](shape i) const
	{
		return f.shapes[i];
	}
	float operator[](simple i) const
	{
		return s[i];
	}
};

struct combined_param
{
	std::string_view name;
	float (*get)(const ctx &);
};

// VRCFaceTracking UnifiedExpressionsParameters.UnifiedCombinedShapes, same order
constexpr combined_param combined[] = {
        // Eye gaze
        {"EyeX", [](const ctx & c) { return (c.f.left.gaze_x + c.f.right.gaze_x) / 2.0f; }},
        {"EyeY", [](const ctx & c) { return (c.f.left.gaze_y + c.f.right.gaze_y) / 2.0f; }},
        {"EyeLeftX", [](const ctx & c) { return c.f.left.gaze_x; }},
        {"EyeLeftY", [](const ctx & c) { return c.f.left.gaze_y; }},
        {"EyeRightX", [](const ctx & c) { return c.f.right.gaze_x; }},
        {"EyeRightY", [](const ctx & c) { return c.f.right.gaze_y; }},

        // Pupils: Combined() normalizes over _minDilation = 2, _maxDilation = 8, the range of the camera pupil estimate.
        // ponytail: clamped instead of VRCFT's range widening on outliers above 8 mm.
        {"PupilDilation", [](const ctx & c) { return std::clamp(((c.f.left.pupil_mm + c.f.right.pupil_mm) / 2.0f - 2.0f) / (8.0f - 2.0f), 0.0f, 1.0f); }},
        {"PupilDiameterLeft", [](const ctx & c) { return c.f.left.pupil_mm * 0.1f; }},
        {"PupilDiameterRight", [](const ctx & c) { return c.f.right.pupil_mm * 0.1f; }},
        {"PupilDiameter", [](const ctx & c) { return (c.f.left.pupil_mm + c.f.right.pupil_mm) * .05f; }},

        // Openness
        {"EyeOpenLeft", [](const ctx & c) { return c.f.left.openness; }},
        {"EyeOpenRight", [](const ctx & c) { return c.f.right.openness; }},
        {"EyeOpen", [](const ctx & c) { return (c.f.left.openness + c.f.right.openness) / 2.0f; }},
        {"EyeClosedLeft", [](const ctx & c) { return 1 - c.f.left.openness; }},
        {"EyeClosedRight", [](const ctx & c) { return 1 - c.f.right.openness; }},
        {"EyeClosed", [](const ctx & c) { return 1 - (c.f.left.openness + c.f.right.openness) / 2.0f; }},

        {"EyeWide", [](const ctx & c) { return std::max(c[EyeWideLeft], c[EyeWideRight]); }},

        {"EyeLidLeft", [](const ctx & c) { return c.f.left.openness * .75f + c[EyeWideLeft] * .25f; }},
        {"EyeLidRight", [](const ctx & c) { return c.f.right.openness * .75f + c[EyeWideRight] * .25f; }},
        {"EyeLid", [](const ctx & c) { return ((c.f.left.openness + c.f.right.openness) / 2.0f) * .75f + ((c[EyeWideRight] + c[EyeWideLeft]) / 2.0f) * .25f; }},

        {"EyeSquint", [](const ctx & c) { return std::max(c[EyeSquintLeft], c[EyeSquintRight]); }},
        {"EyesSquint", [](const ctx & c) { return std::max(c[EyeSquintLeft], c[EyeSquintRight]); }},

        // Brows
        {"BrowUp", [](const ctx & c) { return (c[BrowUpRight] + c[BrowUpLeft]) * .5f; }},
        {"BrowDown", [](const ctx & c) { return (c[BrowDownRight] + c[BrowDownLeft]) * .5f; }},
        {"BrowInnerUp", [](const ctx & c) { return (c[BrowInnerUpLeft] + c[BrowInnerUpRight]) / 2.0f; }},
        {"BrowOuterUp", [](const ctx & c) { return (c[BrowOuterUpLeft] + c[BrowOuterUpRight]) / 2.0f; }},
        {"BrowExpressionRight", [](const ctx & c) { return std::min(1.0f, c[BrowInnerUpRight] * .5f + c[BrowOuterUpRight] * .5f) - c[BrowDownRight]; }},
        {"BrowExpressionLeft", [](const ctx & c) { return std::min(1.0f, c[BrowInnerUpLeft] * .5f + c[BrowOuterUpLeft] * .5f) - c[BrowDownLeft]; }},
        {"BrowExpression", [](const ctx & c) {
	         return (std::min(1.0f, (c[BrowInnerUpRight] + c[BrowOuterUpRight]) * .5f) - c[BrowDownRight] +
	                 std::min(1.0f, (c[BrowInnerUpLeft] + c[BrowOuterUpLeft]) * .5f) - c[BrowDownLeft]) *
	                .5f;
         }},

        // Jaw
        {"JawX", [](const ctx & c) { return c[JawRight] - c[JawLeft]; }},
        {"JawZ", [](const ctx & c) { return c[JawForward] - c[JawBackward]; }},

        // Cheeks
        {"CheekSquint", [](const ctx & c) { return (c[CheekSquintLeft] + c[CheekSquintRight]) / 2.0f; }},
        {"CheekPuffSuckLeft", [](const ctx & c) { return c[CheekPuffLeft] - c[CheekSuckLeft]; }},
        {"CheekPuffSuckRight", [](const ctx & c) { return c[CheekPuffRight] - c[CheekSuckRight]; }},
        {"CheekPuffSuck", [](const ctx & c) { return (c[CheekPuffRight] + c[CheekPuffLeft]) / 2.0f - (c[CheekSuckRight] + c[CheekSuckLeft]) / 2.0f; }},
        {"CheekSuck", [](const ctx & c) { return (c[CheekSuckLeft] + c[CheekSuckRight]) / 2.0f; }},

        // Mouth direction
        {"MouthUpperX", [](const ctx & c) { return c[MouthUpperRight] - c[MouthUpperLeft]; }},
        {"MouthLowerX", [](const ctx & c) { return c[MouthLowerRight] - c[MouthLowerLeft]; }},
        {"MouthX", [](const ctx & c) { return (c[MouthUpperRight] + c[MouthLowerRight]) / 2.0f - (c[MouthUpperLeft] + c[MouthLowerLeft]) / 2.0f; }},

        // Lips
        {"LipSuckUpper", [](const ctx & c) { return (c[LipSuckUpperRight] + c[LipSuckUpperLeft]) / 2.0f; }},
        {"LipSuckLower", [](const ctx & c) { return (c[LipSuckLowerRight] + c[LipSuckLowerLeft]) / 2.0f; }},
        {"LipSuck", [](const ctx & c) { return (c[LipSuckUpperRight] + c[LipSuckUpperLeft] + c[LipSuckLowerRight] + c[LipSuckLowerLeft]) / 4.0f; }},
        {"LipFunnelUpper", [](const ctx & c) { return (c[LipFunnelUpperRight] + c[LipFunnelUpperLeft]) / 2.0f; }},
        {"LipFunnelLower", [](const ctx & c) { return (c[LipFunnelLowerRight] + c[LipFunnelLowerLeft]) / 2.0f; }},
        {"LipFunnel", [](const ctx & c) { return (c[LipFunnelUpperRight] + c[LipFunnelUpperLeft] + c[LipFunnelLowerRight] + c[LipFunnelLowerLeft]) / 4.0f; }},
        {"LipPuckerUpper", [](const ctx & c) { return (c[LipPuckerUpperRight] + c[LipPuckerUpperLeft]) / 2.0f; }},
        {"LipPuckerLower", [](const ctx & c) { return (c[LipPuckerLowerRight] + c[LipPuckerLowerLeft]) / 2.0f; }},
        {"LipPuckerRight", [](const ctx & c) { return (c[LipPuckerUpperRight] + c[LipPuckerLowerRight]) / 2.0f; }},
        {"LipPuckerLeft", [](const ctx & c) { return (c[LipPuckerUpperLeft] + c[LipPuckerLowerLeft]) / 2.0f; }},
        {"LipPucker", [](const ctx & c) { return (c[LipPuckerUpperRight] + c[LipPuckerUpperLeft] + c[LipPuckerLowerRight] + c[LipPuckerLowerLeft]) / 4.0f; }},
        {"LipSuckFunnelUpper", [](const ctx & c) { return (c[LipSuckUpperRight] + c[LipSuckUpperLeft]) / 2.0f - (c[LipFunnelUpperRight] + c[LipFunnelUpperLeft]) / 2.0f; }},
        {"LipSuckFunnelLower", [](const ctx & c) { return (c[LipSuckLowerRight] + c[LipSuckLowerLeft]) / 2.0f - (c[LipFunnelLowerRight] + c[LipFunnelLowerLeft]) / 2.0f; }},
        {"LipSuckFunnelLowerLeft", [](const ctx & c) { return c[LipSuckLowerLeft] - c[LipFunnelLowerLeft]; }},
        {"LipSuckFunnelLowerRight", [](const ctx & c) { return c[LipSuckLowerRight] - c[LipFunnelLowerRight]; }},
        {"LipSuckFunnelUpperLeft", [](const ctx & c) { return c[LipSuckUpperLeft] - c[LipFunnelUpperLeft]; }},
        {"LipSuckFunnelUpperRight", [](const ctx & c) { return c[LipSuckUpperRight] - c[LipFunnelUpperRight]; }},

        // Mouth
        {"MouthUpperUp", [](const ctx & c) { return c[MouthUpperUpRight] * .5f + c[MouthUpperUpLeft] * .5f; }},
        {"MouthLowerDown", [](const ctx & c) { return c[MouthLowerDownRight] * .5f + c[MouthLowerDownLeft] * .5f; }},
        {"MouthOpen", [](const ctx & c) { return c[MouthUpperUpRight] * .25f + c[MouthUpperUpLeft] * .25f + c[MouthLowerDownRight] * .25f + c[MouthLowerDownLeft] * .25f; }},
        {"MouthStretch", [](const ctx & c) { return (c[MouthStretchRight] + c[MouthStretchLeft]) / 2.0f; }},
        {"MouthTightener", [](const ctx & c) { return (c[MouthTightenerRight] + c[MouthTightenerLeft]) / 2.0f; }},
        {"MouthPress", [](const ctx & c) { return (c[MouthPressRight] + c[MouthPressLeft]) / 2.0f; }},
        {"MouthDimple", [](const ctx & c) { return (c[MouthDimpleRight] + c[MouthDimpleLeft]) / 2.0f; }},
        {"NoseSneer", [](const ctx & c) { return (c[NoseSneerRight] + c[NoseSneerLeft]) / 2.0f; }},
        {"MouthTightenerStretch", [](const ctx & c) { return (c[MouthTightenerRight] + c[MouthTightenerLeft]) / 2.0f - (c[MouthStretchRight] + c[MouthStretchLeft]) / 2.0f; }},
        {"MouthTightenerStretchLeft", [](const ctx & c) { return c[MouthTightenerLeft] - c[MouthStretchLeft]; }},
        {"MouthTightenerStretchRight", [](const ctx & c) { return c[MouthTightenerRight] - c[MouthStretchRight]; }},

        // Lip corners
        {"MouthCornerYLeft", [](const ctx & c) { return c[MouthCornerSlantLeft] - c[MouthFrownLeft]; }},
        {"MouthCornerYRight", [](const ctx & c) { return c[MouthCornerSlantRight] - c[MouthFrownRight]; }},
        {"MouthCornerY", [](const ctx & c) { return (c[MouthCornerSlantLeft] - c[MouthFrownLeft] + c[MouthCornerSlantRight] - c[MouthFrownRight]) * .5f; }},
        {"SmileFrownRight", [](const ctx & c) { return c[MouthSmileRight] - c[MouthFrownRight]; }},
        {"SmileFrownLeft", [](const ctx & c) { return c[MouthSmileLeft] - c[MouthFrownLeft]; }},
        {"SmileFrown", [](const ctx & c) { return c[MouthSmileRight] * .5f + c[MouthSmileLeft] * .5f - c[MouthFrownRight] * .5f - c[MouthFrownLeft] * .5f; }},
        {"SmileSadRight", [](const ctx & c) { return c[MouthSmileRight] - c[MouthSadRight]; }},
        {"SmileSadLeft", [](const ctx & c) { return c[MouthSmileLeft] - c[MouthSadLeft]; }},
        {"SmileSad", [](const ctx & c) { return (c[MouthSmileLeft] + c[MouthSmileRight]) / 2.0f - (c[MouthSadLeft] + c[MouthSadRight]) / 2.0f; }},

        // Tongue
        {"TongueX", [](const ctx & c) { return c[TongueRight] - c[TongueLeft]; }},
        {"TongueY", [](const ctx & c) { return c[TongueUp] - c[TongueDown]; }},
        {"TongueArchY", [](const ctx & c) { return c[TongueCurlUp] - c[TongueBendDown]; }},
        {"TongueShape", [](const ctx & c) { return c[TongueFlat] - c[TongueSquish]; }},

        // ConditionalBoolParameter: VRCFT sends these WITHOUT the "v2/" prefix
        {"EyeTrackingActive", [](const ctx &) { return 1.0f; }},
        {"ExpressionTrackingActive", [](const ctx &) { return 1.0f; }},
        {"LipTrackingActive", [](const ctx &) { return 1.0f; }},
};

constexpr std::size_t param_count = unified_count + std::size(simple_names) + std::size(combined);

constexpr auto all_names = [] {
	std::array<std::string_view, param_count> names;
	std::size_t i = 0;
	for (auto n: shape_names)
		names[i++] = n;
	for (auto n: simple_names)
		names[i++] = n;
	for (auto & p: combined)
		names[i++] = p.name;
	return names;
}();

// ALXR MakeEye
void make_eye(eye_state & e, float left, float right, float up, float down)
{
	e.gaze_x = (right - left) * 0.5f;
	e.gaze_y = (up - down) * 0.5f;
}

float to_degrees_atan(float v)
{
	return float(std::atan(double(v)) * (180 / std::numbers::pi));
}
} // namespace

// Port of VRCFT-ALXR-Modules ModuleUtils FB V2 functions, all TrackingSensitivity factors = 1,
// FBEyeOpennessMode::LinearLidTightening, UseEyeExpressionForGazePose, FBFaceTrackingV2TongueHack.
face_state from_fb2(std::span<const float, fb2_count> w)
{
	face_state f;
	auto & s = f.shapes;
	auto m = [](float v) { return std::min(1.0f, v); };

	// UpdateEyeDataFBV2
	make_eye(f.left, w[fb::EyesLookLeftL], w[fb::EyesLookRightL], w[fb::EyesLookUpL], w[fb::EyesLookDownL]);
	make_eye(f.right, w[fb::EyesLookLeftR], w[fb::EyesLookRightR], w[fb::EyesLookUpR], w[fb::EyesLookDownR]);

	// UpdateEyeOpenessFBV2 (LinearLidTightening)
	f.left.openness = 1 - std::clamp(w[fb::EyesClosedL] + w[fb::EyesClosedL] * w[fb::LidTightenerL], 0.0f, 1.0f);
	f.right.openness = 1 - std::clamp(w[fb::EyesClosedR] + w[fb::EyesClosedR] * w[fb::LidTightenerR], 0.0f, 1.0f);

	// UpdateEyeExpressionsFBV2
	s[EyeWideLeft] = m(w[fb::UpperLidRaiserL]);
	s[EyeWideRight] = m(w[fb::UpperLidRaiserR]);
	s[EyeSquintLeft] = m(w[fb::LidTightenerL]);
	s[EyeSquintRight] = m(w[fb::LidTightenerR]);

	s[BrowInnerUpLeft] = m(w[fb::InnerBrowRaiserL]);
	s[BrowInnerUpRight] = m(w[fb::InnerBrowRaiserR]);
	s[BrowOuterUpLeft] = m(w[fb::OuterBrowRaiserL]);
	s[BrowOuterUpRight] = m(w[fb::OuterBrowRaiserR]);
	s[BrowLowererLeft] = m(w[fb::BrowLowererL]);
	s[BrowPinchLeft] = m(w[fb::BrowLowererL]);
	s[BrowLowererRight] = m(w[fb::BrowLowererR]);
	s[BrowPinchRight] = m(w[fb::BrowLowererR]);

	// UpdateMouthExpressionsFBV2
	s[JawOpen] = m(w[fb::JawDrop]);
	s[JawLeft] = m(w[fb::JawSidewaysLeft]);
	s[JawRight] = m(w[fb::JawSidewaysRight]);
	s[JawForward] = m(w[fb::JawThrust]);

	s[MouthClosed] = w[fb::LipsToward];

	s[MouthUpperLeft] = m(w[fb::MouthLeft]);
	s[MouthLowerLeft] = m(w[fb::MouthLeft]);
	s[MouthUpperRight] = m(w[fb::MouthRight]);
	s[MouthLowerRight] = m(w[fb::MouthRight]);

	s[MouthCornerPullLeft] = m(w[fb::LipCornerPullerL]);
	s[MouthCornerSlantLeft] = m(w[fb::LipCornerPullerL]);
	s[MouthCornerPullRight] = m(w[fb::LipCornerPullerR]);
	s[MouthCornerSlantRight] = m(w[fb::LipCornerPullerR]);
	s[MouthFrownLeft] = m(w[fb::LipCornerDepressorL]);
	s[MouthFrownRight] = m(w[fb::LipCornerDepressorR]);

	s[MouthLowerDownLeft] = m(w[fb::LowerLipDepressorL]);
	s[MouthLowerDownRight] = m(w[fb::LowerLipDepressorR]);
	s[MouthUpperUpLeft] = m(std::max(w[fb::UpperLipRaiserL], w[fb::NoseWrinklerL]));
	s[MouthUpperDeepenLeft] = m(std::max(w[fb::UpperLipRaiserL], w[fb::NoseWrinklerL]));
	s[MouthUpperUpRight] = m(std::max(w[fb::UpperLipRaiserR], w[fb::NoseWrinklerR]));
	s[MouthUpperDeepenRight] = m(std::max(w[fb::UpperLipRaiserR], w[fb::NoseWrinklerR]));

	s[MouthRaiserUpper] = m(w[fb::ChinRaiserT]);
	s[MouthRaiserLower] = m(w[fb::ChinRaiserB]);
	s[MouthDimpleLeft] = m(w[fb::DimplerL]);
	s[MouthDimpleRight] = m(w[fb::DimplerR]);
	s[MouthTightenerLeft] = m(w[fb::LipTightenerL]);
	s[MouthTightenerRight] = m(w[fb::LipTightenerR]);
	s[MouthPressLeft] = m(w[fb::LipPressorL]);
	s[MouthPressRight] = m(w[fb::LipPressorR]);
	s[MouthStretchLeft] = m(w[fb::LipStretcherL]);
	s[MouthStretchRight] = m(w[fb::LipStretcherR]);

	s[LipPuckerUpperRight] = m(w[fb::LipPuckerR]);
	s[LipPuckerLowerRight] = m(w[fb::LipPuckerR]);
	s[LipPuckerUpperLeft] = m(w[fb::LipPuckerL]);
	s[LipPuckerLowerLeft] = m(w[fb::LipPuckerL]);

	s[LipFunnelUpperLeft] = m(w[fb::LipFunnelerLT]);
	s[LipFunnelUpperRight] = m(w[fb::LipFunnelerRT]);
	s[LipFunnelLowerLeft] = m(w[fb::LipFunnelerLB]);
	s[LipFunnelLowerRight] = m(w[fb::LipFunnelerRB]);

	// max(0, …) only guards pow() against out-of-spec negative weights
	s[LipSuckUpperLeft] = m(std::min(1 - std::pow(std::max(0.0f, w[fb::UpperLipRaiserL]), 1.0f / 6.0f), w[fb::LipSuckLT]));
	s[LipSuckUpperRight] = m(std::min(1 - std::pow(std::max(0.0f, w[fb::UpperLipRaiserR]), 1.0f / 6.0f), w[fb::LipSuckRT]));
	s[LipSuckLowerLeft] = m(w[fb::LipSuckLB]);
	s[LipSuckLowerRight] = m(w[fb::LipSuckRB]);

	s[CheekPuffLeft] = m(w[fb::CheekPuffL]);
	s[CheekPuffRight] = m(w[fb::CheekPuffR]);
	s[CheekSuckLeft] = m(w[fb::CheekSuckL]);
	s[CheekSuckRight] = m(w[fb::CheekSuckR]);
	s[CheekSquintLeft] = m(w[fb::CheekRaiserL]);
	s[CheekSquintRight] = m(w[fb::CheekRaiserR]);

	s[NoseSneerLeft] = m(w[fb::NoseWrinklerL]);
	s[NoseSneerRight] = m(w[fb::NoseWrinklerR]);

	// BoltOn writes tongue in ALXR's FBFaceTrackingV2TongueHack layout: indices 63..67 hold
	// Out/Left/Right/Up/Down (XrFaceParameterIndicesANDROID), FB2 TongueOut stays 0.
	// ponytail: always on; max() keeps stock Quest Pro tongue-out, add a config switch if a non-BoltOn
	// headset ever fills 63..67 with real FB2 tongue shapes.
	s[TongueOut] = std::max(w[63], w[fb::TongueOut]);
	s[TongueLeft] = w[64];
	s[TongueRight] = w[65];
	s[TongueUp] = w[66];
	s[TongueDown] = w[67];

	return f;
}

face_state idle_state()
{
	face_state f;
	f.left.openness = 0;
	f.right.openness = 0;
	return f;
}

std::span<const std::string_view> param_names()
{
	return all_names;
}

void compute_params(const face_state & f, std::span<float> values)
{
	assert(values.size() == param_count);

	// UnifiedSimplifier.ExpressionMap
	const auto & e = f.shapes;
	const float s[simple_count] = {
	        e[BrowOuterUpRight] * .60f + e[BrowInnerUpRight] * .40f,
	        e[BrowOuterUpLeft] * .60f + e[BrowInnerUpLeft] * .40f,
	        e[BrowLowererRight] * .75f + e[BrowPinchRight] * .25f,
	        e[BrowLowererLeft] * .75f + e[BrowPinchLeft] * .25f,
	        e[MouthCornerPullRight] * .8f + e[MouthCornerSlantRight] * .2f,
	        e[MouthCornerPullLeft] * .8f + e[MouthCornerSlantLeft] * .2f,
	        std::max(e[MouthFrownRight], e[MouthStretchRight]),
	        std::max(e[MouthFrownLeft], e[MouthStretchLeft]),
	};

	std::size_t i = 0;
	for (float v: e)
		values[i++] = v;
	for (float v: s)
		values[i++] = v;
	const ctx c{f, s};
	for (const auto & p: combined)
		values[i++] = p.get(c);
}

std::array<float, 4> left_right_pitch_yaw(const face_state & f)
{
	// VRCFT Vector2.ToPitch / ToYaw
	return {
	        -to_degrees_atan(f.left.gaze_y),
	        to_degrees_atan(f.left.gaze_x),
	        -to_degrees_atan(f.right.gaze_y),
	        to_degrees_atan(f.right.gaze_x),
	};
}

float eyes_closed_amount(const face_state & f)
{
	return 1 - (f.left.openness + f.right.openness) / 2.0f;
}

} // namespace wivrn::vrchat_osc
