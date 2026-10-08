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

#include "unified.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

// Resonite reads Quest Pro face and eye tracking as "Steam Link OSC" on 127.0.0.1:9015 (what Edrakon emulates).
// Message format: LinkFT's Steam Link OSC notes (https://github.com/ykeara/LinkFT, docs/SteamLinkSpec.md).

namespace wivrn::vrchat_osc
{
class osc_sender;

constexpr uint16_t steamlink_port = 9015;

struct steamlink_shape
{
	std::string_view name; // sent as /sl/xrfb/facew/<name>
	std::size_t index;     // XrFaceExpression2FB
};

// The /sl/xrfb/facew/ names Resonite's Steam Link driver listens for (the strings in its FrooxEngine.dll)
#define WIVRN_SL(n) steamlink_shape{#n, fb::n}
inline constexpr std::array steamlink_shapes = {
        WIVRN_SL(BrowLowererL), WIVRN_SL(BrowLowererR), WIVRN_SL(CheekPuffL), WIVRN_SL(CheekPuffR), WIVRN_SL(CheekRaiserL), WIVRN_SL(CheekRaiserR),
        WIVRN_SL(CheekSuckL), WIVRN_SL(CheekSuckR), WIVRN_SL(ChinRaiserB), WIVRN_SL(ChinRaiserT), WIVRN_SL(DimplerL), WIVRN_SL(DimplerR), WIVRN_SL(EyesClosedL),
        WIVRN_SL(EyesClosedR), WIVRN_SL(InnerBrowRaiserL), WIVRN_SL(InnerBrowRaiserR), WIVRN_SL(JawDrop), WIVRN_SL(JawSidewaysLeft),
        WIVRN_SL(JawSidewaysRight), WIVRN_SL(JawThrust), WIVRN_SL(LidTightenerL), WIVRN_SL(LidTightenerR), WIVRN_SL(LipCornerDepressorL),
        WIVRN_SL(LipCornerDepressorR), WIVRN_SL(LipCornerPullerL), WIVRN_SL(LipCornerPullerR), WIVRN_SL(LipFunnelerLB), WIVRN_SL(LipFunnelerLT),
        WIVRN_SL(LipFunnelerRB), WIVRN_SL(LipFunnelerRT), WIVRN_SL(LipPressorL), WIVRN_SL(LipPressorR), WIVRN_SL(LipPuckerL), WIVRN_SL(LipPuckerR),
        WIVRN_SL(LipsToward), WIVRN_SL(LipStretcherL), WIVRN_SL(LipStretcherR), WIVRN_SL(LipSuckLB), WIVRN_SL(LipSuckLT), WIVRN_SL(LipSuckRB), WIVRN_SL(LipSuckRT),
        WIVRN_SL(LipTightenerL), WIVRN_SL(LipTightenerR), WIVRN_SL(LowerLipDepressorL), WIVRN_SL(LowerLipDepressorR), WIVRN_SL(MouthLeft),
        WIVRN_SL(MouthRight), WIVRN_SL(NoseWrinklerL), WIVRN_SL(NoseWrinklerR), WIVRN_SL(OuterBrowRaiserL), WIVRN_SL(OuterBrowRaiserR), WIVRN_SL(TongueOut),
        WIVRN_SL(TongueRetreat), WIVRN_SL(UpperLidRaiserL), WIVRN_SL(UpperLidRaiserR), WIVRN_SL(UpperLipRaiserL), WIVRN_SL(UpperLipRaiserR),
};
#undef WIVRN_SL
static_assert(steamlink_shapes.size() == 57);

// OpenXR gaze direction (+X right, +Y up, -Z forward, unit length) from eye_state gaze_x / gaze_y
std::array<float, 3> gaze_direction(float gaze_x, float gaze_y);

struct process_match
{
	bool resonite = false; // "Resonite.exe" (Proton)
	bool edrakon = false;  // owns port 9015 when running
};
// cmdline as read from /proc/<pid>/cmdline (NUL-separated arguments), case-insensitive
process_match match_cmdline(std::string_view cmdline);
// Every /proc/<pid>/cmdline, OR-ed
process_match scan_processes();

// One frame: combined gaze, face confidences, the 57 weights; then flush.
void send_steamlink(osc_sender &, std::span<const float, fb2_count> weights, const face_state &);

} // namespace wivrn::vrchat_osc
