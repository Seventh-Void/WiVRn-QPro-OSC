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
#include <string_view>

// FB_face_tracking2 weights -> VRCFaceTracking "Unified Expressions" v2 parameters.
// Mapping ported from VRCFT-ALXR-Modules (MIT), parameter formulas from VRCFaceTracking (Apache-2.0).

namespace wivrn::vrchat_osc
{

constexpr std::size_t fb2_count = 70; // XR_FACE_EXPRESSION2_COUNT_FB

// XrFaceExpression2FB
namespace fb
{
enum : std::size_t
{
	BrowLowererL,
	BrowLowererR,
	CheekPuffL,
	CheekPuffR,
	CheekRaiserL,
	CheekRaiserR,
	CheekSuckL,
	CheekSuckR,
	ChinRaiserB,
	ChinRaiserT,
	DimplerL,
	DimplerR,
	EyesClosedL,
	EyesClosedR,
	EyesLookDownL,
	EyesLookDownR,
	EyesLookLeftL,
	EyesLookLeftR,
	EyesLookRightL,
	EyesLookRightR,
	EyesLookUpL,
	EyesLookUpR,
	InnerBrowRaiserL,
	InnerBrowRaiserR,
	JawDrop,
	JawSidewaysLeft,
	JawSidewaysRight,
	JawThrust,
	LidTightenerL,
	LidTightenerR,
	LipCornerDepressorL,
	LipCornerDepressorR,
	LipCornerPullerL,
	LipCornerPullerR,
	LipFunnelerLB,
	LipFunnelerLT,
	LipFunnelerRB,
	LipFunnelerRT,
	LipPressorL,
	LipPressorR,
	LipPuckerL,
	LipPuckerR,
	LipStretcherL,
	LipStretcherR,
	LipSuckLB,
	LipSuckLT,
	LipSuckRB,
	LipSuckRT,
	LipTightenerL,
	LipTightenerR,
	LipsToward,
	LowerLipDepressorL,
	LowerLipDepressorR,
	MouthLeft,
	MouthRight,
	NoseWrinklerL,
	NoseWrinklerR,
	OuterBrowRaiserL,
	OuterBrowRaiserR,
	UpperLidRaiserL,
	UpperLidRaiserR,
	UpperLipRaiserL,
	UpperLipRaiserR,
	TongueTipInterdental,
	TongueTipAlveolar,
	TongueFrontDorsalPalate,
	TongueMidDorsalPalate,
	TongueBackDorsalVelar,
	TongueOut,
	TongueRetreat,
	count
};
static_assert(count == fb2_count);
} // namespace fb

struct eye_state
{
	float gaze_x = 0; // VRCFT Gaze vector, right positive
	float gaze_y = 0; // up positive
	float openness = 1;
	float pupil_mm = 5;
};

// Number of VRCFT UnifiedExpressions base shapes (EyeSquintRight ... NeckFlexLeft)
constexpr std::size_t unified_count = 88;

struct face_state
{
	std::array<float, unified_count> shapes{}; // indexed like VRCFT UnifiedExpressions enum
	eye_state left, right;
};

// Convert one FB2 sample. Gaze comes from the EyesLook* blendshapes (independent per eye).
face_state from_fb2(std::span<const float, fb2_count> weights);

// Eyes closed, neutral face (used when the HMD is asleep / sample invalid).
face_state idle_state();

// Every v2 parameter name WITHOUT the "v2/" prefix, e.g. "JawOpen", "EyeLeftX", "SmileSadLeft",
// "EyeTrackingActive". Order is stable; compute_params() writes values at the same indices.
std::span<const std::string_view> param_names();

// values.size() must equal param_names().size(). Bool-only params (…TrackingActive) get 1.0.
void compute_params(const face_state &, std::span<float> values);

// VRChat native eye tracking, degrees: {left pitch, left yaw, right pitch, right yaw}
std::array<float, 4> left_right_pitch_yaw(const face_state &);
// VRChat native: 1 - combined openness
float eyes_closed_amount(const face_state &);

} // namespace wivrn::vrchat_osc
