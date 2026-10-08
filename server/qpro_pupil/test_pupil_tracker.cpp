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

// Synthetic checks for the pupil tracker; real headset footage still needs a manual check.

#undef NDEBUG
#include "pupil_tracker.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include <opencv2/imgproc.hpp>

using namespace wivrn::qpro;

namespace
{
cv::Mat eye(int radius, cv::Point center = {200, 200})
{
	cv::Mat image(400, 400, CV_8UC1, cv::Scalar(155));
	cv::circle(image, center, 95, 105, -1);
	cv::circle(image, center, radius, 18, -1);
	cv::circle(image, center - cv::Point(6, 6), 3, 245, -1);
	return image;
}

cv::Mat zeros()
{
	return cv::Mat::zeros(400, 400, CV_8UC1);
}

cv::Mat strip(std::vector<cv::Mat> frames)
{
	cv::Mat out;
	cv::hconcat(frames, out);
	return out;
}

pupil_tracker::result update(pupil_tracker & tracker, const cv::Mat & s, std::vector<int> ids)
{
	return tracker.update({s.data, s.total()}, ids);
}

std::optional<pupil_detection> sample(float diameter, float cx = 200)
{
	return pupil_detection{cx, 200, diameter, 0.9f, diameter, diameter, 0};
}

bool near(std::optional<float> value, double expected, double delta)
{
	return value and std::abs(*value - expected) <= delta;
}

bool contains(const std::string & s, const char * part)
{
	return s.find(part) != std::string::npos;
}

void dark_pupil_changes_size_and_position()
{
	for (auto [radius, center]: {std::pair{24, cv::Point(200, 200)}, {35, cv::Point(245, 175)}})
	{
		auto d = detect_pupil(eye(radius, center), std::nullopt);
		assert(d);
		assert(std::abs(d->diameter_px - 2 * radius) <= 10);
		assert(std::abs(d->cx - center.x) <= 6 and std::abs(d->cy - center.y) <= 6);
	}
}

void invalid_eye_frames_do_not_produce_dilation()
{
	assert(not detect_pupil(zeros(), std::nullopt));
	assert(not detect_pupil(cv::Mat(400, 400, CV_8UC1, cv::Scalar(140)), std::nullopt));
	bool threw = false;
	try
	{
		detect_pupil(cv::Mat::zeros(300, 400, CV_8UC1), std::nullopt);
	}
	catch (std::invalid_argument &)
	{
		threw = true;
	}
	assert(threw);
}

void reflections_inside_off_center_pupil()
{
	cv::Point center(300, 220);
	cv::Mat image = eye(30, center);
	for (cv::Point offset: {cv::Point(-9, -7), cv::Point(8, 3), cv::Point(4, 12)})
		cv::circle(image, center + offset, 4, 255, -1);
	auto d = detect_pupil(image, std::nullopt);
	assert(d);
	assert(std::abs(d->cx - center.x) <= 6 and std::abs(d->cy - center.y) <= 6);
	assert(std::abs(d->diameter_px - 60) <= 10);
}

void gaze_direction_does_not_reject_samples()
{
	relative_pupil_eye tracker;
	for (int i = 0; i < 11; ++i)
	{
		assert(not tracker.update(sample(38)));
		assert(tracker.status() == "warming " + std::to_string(i + 1) + "/12");
	}
	assert(near(tracker.update(sample(38)), 5.0, 0.01));
	// Looking elsewhere moves the pupil, not its size.
	assert(near(tracker.update(sample(38, 320)), 5.0, 0.01));
	assert(tracker.status().starts_with("tracking"));
	tracker.update(sample(38 * 4));
	assert(contains(tracker.status(), "size outside plausible range"));
}

void learned_range_maps_dark_to_dilated_and_bright_to_constricted()
{
	// Live Quest Pro recording: ~19 px pupils in bright scenes, ~47 px in dark.
	relative_pupil_eye tracker;
	std::optional<float> bright, dark;
	for (int round = 0; round < 4; ++round)
	{
		for (int i = 0; i < 72; ++i)
			bright = tracker.update(sample(19));
		for (int i = 0; i < 72; ++i)
			dark = tracker.update(sample(47));
	}
	assert(tracker.status() == "tracking");
	assert(*dark > 7.0);
	for (int i = 0; i < 72; ++i)
		bright = tracker.update(sample(19));
	assert(*bright < 3.0);
	// A long stay in a dark world keeps the pupils dilated.
	for (int i = 0; i < 24 * 60; ++i)
		dark = tracker.update(sample(47));
	assert(*dark > 7.0);
}

void session_starting_bright_still_dilates_in_the_dark()
{
	relative_pupil_eye tracker;
	std::optional<float> value;
	for (int i = 0; i < 12; ++i)
		value = tracker.update(sample(19));
	assert(near(value, 5.0, 0.01));
	for (int i = 0; i < 30; ++i)
		value = tracker.update(sample(47));
	assert(*value > 7.0);
}

void relative_dilation_is_independent_per_eye()
{
	pupil_tracker tracker;
	pupil_tracker::result r;
	for (int i = 0; i < 12; ++i)
		r = update(tracker, strip({eye(24), eye(24)}), {0, 1});
	assert(near(r.left.mm, 5.0, 0.3) and near(r.right.mm, 5.0, 0.3));
	for (int i = 0; i < 8; ++i)
		r = update(tracker, strip({eye(35), eye(24)}), {0, 1});
	assert(*r.left.mm > 5.5);
	assert(near(r.right.mm, 5.0, 0.3));
	// Pupils move together: an eye that loses its pupil follows the other.
	r = update(tracker, strip({zeros(), eye(24)}), {0, 1});
	assert(contains(r.left.status, "holding last estimate"));
	assert(not r.left.detection and r.right.detection);
	assert(r.left.mm == r.right.mm);
	r = update(tracker, strip({eye(35), zeros()}), {0, 1});
	assert(r.right.mm == r.left.mm);
}

void camera_order_selects_the_eye()
{
	// Camera id 0 is the left eye wherever it sits in the strip.
	pupil_tracker tracker;
	auto r = update(tracker, strip({eye(20, {230, 210}), eye(35, {270, 220})}), {1, 0});
	assert(r.left.detection and r.right.detection);
	assert(std::abs(r.left.detection->cx - 270) <= 6 and std::abs(r.right.detection->cx - 230) <= 6);
	assert(r.left.detection->diameter_px > r.right.detection->diameter_px);
}

void response_gain_makes_both_directions_more_visible()
{
	relative_pupil_eye normal(1.0f), balanced(1.4f);
	std::optional<float> normal_value, balanced_value;
	for (int i = 0; i < 12; ++i)
		assert(normal.update(sample(40)) == balanced.update(sample(40)));
	for (int i = 0; i < 10; ++i)
	{
		normal_value = normal.update(sample(44));
		balanced_value = balanced.update(sample(44));
	}
	assert(*balanced_value > *normal_value);
	for (int i = 0; i < 20; ++i)
	{
		normal_value = normal.update(sample(36));
		balanced_value = balanced.update(sample(36));
	}
	assert(*balanced_value < *normal_value);
}

void high_response_eases_change_and_ignores_one_bad_size()
{
	relative_pupil_eye tracker(3.0f);
	std::optional<float> value;
	for (int i = 0; i < 12; ++i)
		value = tracker.update(sample(40));
	assert(near(value, 5.0, 0.01));
	assert(near(tracker.update(sample(60)), 5.0, 0.01));
	assert(near(tracker.update(sample(40)), 5.0, 0.01));
	float previous = *value;
	for (int i = 0; i < 12; ++i)
	{
		value = tracker.update(sample(44));
		assert(std::abs(*value - previous) <= 0.28);
		previous = *value;
	}
	assert(*value > 6.1 and *value <= 8.5);
	for (float bad: {3.1f, 0.9f})
	{
		bool threw = false;
		try
		{
			relative_pupil_eye{bad};
		}
		catch (std::invalid_argument &)
		{
			threw = true;
		}
		assert(threw);
	}
}

void high_response_rejects_still_gaze_jitter_and_short_occlusion()
{
	relative_pupil_eye tracker(3.0f);
	for (int i = 0; i < 12; ++i)
		tracker.update(sample(40));
	float lowest = 10, highest = 0;
	for (int i = 0; i < 12; ++i)
		for (float size: {39.f, 41.f, 40.f})
		{
			float v = *tracker.update(sample(size));
			lowest = std::min(lowest, v);
			highest = std::max(highest, v);
		}
	assert(highest - lowest < 0.3);
	std::optional<float> enlarged;
	for (int i = 0; i < 20; ++i)
		enlarged = tracker.update(sample(52));
	assert(*enlarged > 7.0);
	assert(near(tracker.update(std::nullopt), *enlarged, 0.01));
	assert(tracker.status() == "pupil not visible; holding last estimate");
	std::optional<float> eased;
	for (int i = 0; i < 35; ++i)
		eased = tracker.update(std::nullopt);
	assert(tracker.status() == "pupil not visible; easing to neutral");
	assert(not eased or std::abs(*eased - 5.0) < 0.1);
	// A low-confidence detection counts as not visible.
	tracker.update(pupil_detection{200, 200, 40, 0.1f, 40, 40, 0});
	assert(tracker.status() == "pupil not visible; easing to neutral");
}

void high_response_keeps_range_near_maximum()
{
	relative_pupil_eye tracker(3.0f);
	for (int i = 0; i < 12; ++i)
		tracker.update(sample(40));
	std::optional<float> first, second;
	for (int i = 0; i < 20; ++i)
		first = tracker.update(sample(52));
	for (int i = 0; i < 20; ++i)
		second = tracker.update(sample(56));
	assert(*first > 7.5);
	assert(*second > *first + 0.10);
	assert(*second < 8.0);
}

void warmup_rejects_pupil_size_trend()
{
	relative_pupil_eye tracker;
	std::optional<float> result;
	for (float diameter: {40, 40, 41, 42, 43, 44, 45, 46, 47, 48, 48, 48})
		result = tracker.update(sample(diameter));
	assert(not result);
	assert(tracker.status() == "warming: hold gaze steady");
	for (int i = 0; i < 12; ++i)
		result = tracker.update(sample(48));
	// Settles near neutral; the baseline can trail the last size by ~1 %.
	assert(near(result, 5.0, 0.2));
}

void combined_eye_and_face_stream()
{
	cv::Mat s = strip({eye(24), eye(24), cv::Mat(400, 400, CV_8UC1, cv::Scalar(42)), cv::Mat(400, 400, CV_8UC1, cv::Scalar(83)), cv::Mat(400, 400, CV_8UC1, cv::Scalar(120))});
	pupil_tracker tracker;
	pupil_tracker::result r;
	for (int i = 0; i < 12; ++i)
		r = update(tracker, s, {0, 1, 2, 3, 4});
	assert(near(r.left.mm, 5.0, 0.3) and near(r.right.mm, 5.0, 0.3));
	auto jpeg = tracker.preview_jpeg({s.data, s.total()}, std::vector<int>{0, 1, 2, 3, 4}, r);
	assert(jpeg.size() > 2 and jpeg[0] == 0xFF and jpeg[1] == 0xD8);
	cv::Mat mouth = strip({cv::Mat(400, 400, CV_8UC1, cv::Scalar(42))});
	assert(tracker.preview_jpeg({mouth.data, mouth.total()}, std::vector<int>{2}, r).empty());
	bool threw = false;
	try
	{
		update(tracker, s, {0, 1});
	}
	catch (std::invalid_argument &)
	{
		threw = true;
	}
	assert(threw);
}

void missing_eye_invalidates_only_that_eye()
{
	pupil_tracker tracker;
	pupil_tracker::result before, r;
	for (int i = 0; i < 12; ++i)
		before = update(tracker, strip({eye(24), eye(24)}), {0, 1});
	r = update(tracker, eye(24), {1});
	assert(r.left.mm == before.left.mm and r.right.mm == before.right.mm);
	assert(contains(r.left.status, "holding last estimate"));
	assert(r.right.status.starts_with("tracking"));
}
} // namespace

int main()
{
	dark_pupil_changes_size_and_position();
	invalid_eye_frames_do_not_produce_dilation();
	reflections_inside_off_center_pupil();
	gaze_direction_does_not_reject_samples();
	learned_range_maps_dark_to_dilated_and_bright_to_constricted();
	session_starting_bright_still_dilates_in_the_dark();
	relative_dilation_is_independent_per_eye();
	camera_order_selects_the_eye();
	response_gain_makes_both_directions_more_visible();
	high_response_eases_change_and_ignores_one_bad_size();
	high_response_rejects_still_gaze_jitter_and_short_occlusion();
	high_response_keeps_range_near_maximum();
	warmup_rejects_pupil_size_trend();
	combined_eye_and_face_stream();
	missing_eye_invalidates_only_that_eye();
	std::puts("pupil tracker tests passed");
}
