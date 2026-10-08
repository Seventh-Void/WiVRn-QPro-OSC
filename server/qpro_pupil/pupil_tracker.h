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

// Relative pupil-size estimates from the Quest Pro eye cameras.
//
// The eye cameras have no millimetre scale. Each eye learns the range of pupil
// sizes it sees during the session (bright scene = small, dark scene = large) and
// maps it onto a 2..8 mm output range. The values drive avatar animation; they
// are not clinical measurements.

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace wivrn::qpro
{

inline constexpr int eye_camera_size = 400; // each camera is 400x400, 8 bit gray

struct pupil_detection
{
	// Full 400x400 eye-frame coordinates.
	float cx, cy, diameter_px, confidence;
	// Fitted ellipse (cv::fitEllipse axes and angle), centred on cx, cy.
	float axis_a, axis_b, angle;
};

// Finds a dark pupil despite small tracking-LED reflections.
// eye: 400x400 CV_8UC1 (may be a sub-matrix); throws std::invalid_argument otherwise.
std::optional<pupil_detection> detect_pupil(const cv::Mat & eye, std::optional<cv::Point2f> previous_center);

// Pupil size of one eye mapped onto the 2..8 mm dilation range.
class relative_pupil_eye
{
	static constexpr size_t baseline_frames = 12;
	static constexpr size_t recent_frames = 5;
	static constexpr size_t history_frames = 24 * 60 * 20;

	double sensitivity;
	std::deque<double> baseline_samples;
	std::deque<double> recent_sizes;
	std::deque<float> history; // stored as float32; the percentiles depend on it
	std::optional<double> baseline;
	std::optional<double> center;
	double half_span;
	std::optional<double> smoothed;
	int invalid_frames = 0;
	uint64_t updates = 0;
	std::string status_ = "warming";

	std::optional<float> invalid(const std::string & reason);
	// ponytail: no saved white/black calibration yet; a saved range would slide here instead of being relearned.
	void learn_range();

public:
	// throws std::invalid_argument outside 1.0..3.0
	explicit relative_pupil_eye(float sensitivity = 1.4f);
	std::optional<float> update(const std::optional<pupil_detection> & detection);
	const std::string & status() const
	{
		return status_;
	}
};

class pupil_tracker
{
	relative_pupil_eye eyes[2];
	std::optional<cv::Point2f> previous_centers[2];

public:
	// throws std::invalid_argument outside 1.0..3.0
	explicit pupil_tracker(float sensitivity = 1.4f);

	struct eye_result
	{
		std::optional<float> mm;
		std::optional<pupil_detection> detection;
		std::string status;
	};
	struct result
	{
		eye_result left, right; // left = camera id 0, right = camera id 1
	};

	// strip: camera_ids.size()*400 wide, 400 high, gray, row-major.
	// Throws std::invalid_argument when the strip size does not match camera_ids.
	result update(std::span<const uint8_t> strip, std::span<const int> camera_ids);

	// JPEG of the eye cameras (left then right, those present) with the detection overlay.
	// Empty when no eye camera is in the strip.
	std::vector<uint8_t> preview_jpeg(std::span<const uint8_t> strip, std::span<const int> camera_ids, const result &, int quality = 70) const;
};

} // namespace wivrn::qpro
