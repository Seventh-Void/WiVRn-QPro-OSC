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

// Pupil detector based on https://github.com/Fwooffy/Qpro-Enhanced-FT-Wireless
// (src/pupil_dilation.py, MIT licence, QproFaceTracking contributors).
// The arithmetic order and the NumPy percentile/median definitions are kept
// exact on purpose, so results stay reproducible against that reference.

#include "pupil_tracker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include <opencv2/geometry.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace wivrn::qpro
{
namespace
{
constexpr int roi_x = 55, roi_y = 75, roi_width = 390 - 55, roi_height = 355 - 75;
constexpr double pi = std::numbers::pi;
constexpr double min_span = 2.0;
constexpr double plausible_ratio = 3.3;

// np.percentile(values, q) with the default "linear" method, values given by sorted rank.
// T is the array dtype: NumPy subtracts the two neighbours in that type.
template <typename T, typename At>
double np_percentile(size_t n, double q, At && at)
{
	double virtual_index = double(n - 1) * (q / 100);
	if (virtual_index >= double(n - 1))
		return double(at(n - 1));
	double previous = std::floor(virtual_index);
	double t = virtual_index - previous;
	T a = at(size_t(previous));
	T b = at(size_t(previous) + 1);
	T diff = T(b - a);
	return t >= 0.5 ? double(b) - double(diff) * (1 - t) : double(a) + double(diff) * t;
}

// np.median of 8 bit samples.
double median_u8(std::vector<uint8_t> & v)
{
	size_t mid = v.size() / 2;
	std::nth_element(v.begin(), v.begin() + mid, v.end());
	double upper = v[mid];
	if (v.size() % 2)
		return upper;
	return (double(*std::max_element(v.begin(), v.begin() + mid)) + upper) / 2;
}

// np.median of float64 samples.
template <typename Container>
double median(const Container & c)
{
	std::vector<double> v(c.begin(), c.end());
	std::ranges::sort(v);
	size_t mid = v.size() / 2;
	return v.size() % 2 ? v[mid] : (v[mid - 1] + v[mid]) / 2;
}

std::vector<int> thresholds(const cv::Mat & clean)
{
	std::array<size_t, 256> cumulative{};
	for (int y = 0; y < clean.rows; ++y)
	{
		const uint8_t * row = clean.ptr<uint8_t>(y);
		for (int x = 0; x < clean.cols; ++x)
			++cumulative[row[x]];
	}
	int low = 0;
	while (cumulative[low] == 0)
		++low;
	for (size_t i = 1; i < cumulative.size(); ++i)
		cumulative[i] += cumulative[i - 1];
	auto at = [&](size_t rank) {
		return uint8_t(std::ranges::upper_bound(cumulative, rank) - cumulative.begin());
	};
	std::set<int> result{low + 10, low + 20};
	for (double q: {5, 10, 15, 20, 25, 30})
		result.insert(int(np_percentile<uint8_t>(clean.total(), q, at)));
	return {result.begin(), result.end()};
}

bool starts_with_tracking(const std::string & status)
{
	return status.starts_with("tracking");
}

// Pointer to the 400x400 frame of camera id `eye` inside the strip, or empty.
cv::Mat eye_frame(std::span<const uint8_t> strip, std::span<const int> camera_ids, int eye)
{
	if (strip.size() != camera_ids.size() * eye_camera_size * eye_camera_size)
		throw std::invalid_argument("Pupil strip size does not match its camera ids");
	auto it = std::ranges::find(camera_ids, eye);
	if (it == camera_ids.end())
		return {};
	size_t index = it - camera_ids.begin();
	return cv::Mat(eye_camera_size,
	               eye_camera_size,
	               CV_8UC1,
	               const_cast<uint8_t *>(strip.data()) + index * eye_camera_size,
	               camera_ids.size() * eye_camera_size);
}
} // namespace

std::optional<pupil_detection> detect_pupil(const cv::Mat & eye, std::optional<cv::Point2f> previous_center)
{
	if (eye.rows != eye_camera_size or eye.cols != eye_camera_size or eye.type() != CV_8UC1)
		throw std::invalid_argument("Pupil detection expects one 400x400 grayscale eye frame");
	// Copy, so that the median filter never reads pixels outside the region (treat the region as a whole image).
	cv::Mat roi = eye(cv::Rect(roi_x, roi_y, roi_width, roi_height)).clone();
	double lowest, highest;
	cv::minMaxLoc(roi, &lowest, &highest);
	if (highest - lowest < 18 or cv::sum(roi)[0] / double(roi.total()) < 8)
		return std::nullopt;
	// Median filtering removes bright headset LED glints inside the pupil.
	cv::Mat clean;
	cv::medianBlur(roi, clean, 7);

	static const cv::Mat close_kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, {7, 7});
	static const cv::Mat open_kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, {3, 3});

	std::optional<pupil_detection> best;
	double best_score = 0;
	std::unordered_set<std::string> seen_contours;
	std::vector<std::vector<cv::Point>> contours;
	std::vector<uint8_t> inside, ring;
	cv::Mat dark, closed, opened, inner, outer;
	for (int threshold: thresholds(clean))
	{
		cv::threshold(clean, dark, threshold, 255, cv::THRESH_BINARY_INV);
		cv::morphologyEx(dark, closed, cv::MORPH_CLOSE, close_kernel);
		cv::morphologyEx(closed, opened, cv::MORPH_OPEN, open_kernel);
		cv::findContours(opened, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
		for (const auto & contour: contours)
		{
			// Several thresholds often produce the same boundary. Its score
			// uses the same median image, so evaluating it again adds no signal.
			if (not seen_contours.emplace(reinterpret_cast<const char *>(contour.data()), contour.size() * sizeof(cv::Point)).second)
				continue;
			double area = cv::contourArea(contour);
			if (not(100 <= area and area <= 6500) or contour.size() < 5)
				continue;
			cv::RotatedRect fit = cv::fitEllipse(contour);
			double x = fit.center.x, y = fit.center.y;
			double major = std::max(fit.size.width, fit.size.height);
			double minor = std::min(fit.size.width, fit.size.height);
			if (not(14 <= major and major <= 75) or minor < 10 or minor / major < 0.48)
				continue;
			if (not(15 <= x and x <= clean.cols - 15) or not(15 <= y and y <= clean.rows - 15))
				continue;
			double ellipse_area = pi * major * minor / 4;
			double fill = area / ellipse_area;
			if (not(0.60 <= fill and fill <= 1.30))
				continue;
			double perimeter = cv::arcLength(contour, true);
			double circularity = perimeter ? 4 * pi * area / (perimeter * perimeter) : 0;
			if (circularity < 0.42)
				continue;
			// Require the fitted shape to be darker than its immediate annulus.
			// Only rasterise around the ellipse; integer translation preserves it.
			int radius = int(std::ceil(major * 0.95)) + 3;
			int left = std::max(0, int(x) - radius), top = std::max(0, int(y) - radius);
			int right = std::min(clean.cols, int(x) + radius + 1), bottom = std::min(clean.rows, int(y) + radius + 1);
			cv::Mat patch = clean(cv::Range(top, bottom), cv::Range(left, right));
			inner = cv::Mat::zeros(patch.size(), CV_8UC1);
			outer = cv::Mat::zeros(patch.size(), CV_8UC1);
			cv::Point2f local_center(float(x - left), float(y - top));
			auto scaled = [&](double factor) {
				return cv::RotatedRect(local_center, cv::Size2f(float(fit.size.width * factor), float(fit.size.height * factor)), fit.angle);
			};
			cv::ellipse(inner, scaled(0.70), 255, -1);
			cv::ellipse(outer, scaled(1.90), 255, -1);
			cv::ellipse(outer, scaled(1.20), 0, -1);
			inside.clear();
			ring.clear();
			for (int row = 0; row < patch.rows; ++row)
			{
				const uint8_t * p = patch.ptr<uint8_t>(row);
				const uint8_t * in = inner.ptr<uint8_t>(row);
				const uint8_t * out = outer.ptr<uint8_t>(row);
				for (int col = 0; col < patch.cols; ++col)
				{
					if (in[col])
						inside.push_back(p[col]);
					if (out[col])
						ring.push_back(p[col]);
				}
			}
			if (inside.size() < 20 or ring.size() < 30)
				continue;
			double inside_median = median_u8(inside);
			double contrast = median_u8(ring) - inside_median;
			if (inside_median > 45 or contrast < 25)
				continue;
			double center_x = x + roi_x, center_y = y + roi_y;
			double prior_penalty = previous_center ? 0.15 * std::hypot(center_x - previous_center->x, center_y - previous_center->y) : 0;
			double location_penalty = 0.07 * std::hypot(center_x - 275, center_y - 215);
			double oversize_penalty = 1.5 * std::max(0.0, major - 48);
			double score = contrast + 20 * circularity + 7 * fill - prior_penalty - location_penalty - oversize_penalty;
			if (not best or score > best_score)
			{
				best_score = score;
				best = pupil_detection{
				        .cx = float(center_x),
				        .cy = float(center_y),
				        .diameter_px = float(major),
				        .confidence = float(std::clamp(contrast / 50 * circularity, 0.0, 1.0)),
				        .axis_a = fit.size.width,
				        .axis_b = fit.size.height,
				        .angle = fit.angle,
				};
			}
		}
	}
	return best;
}

relative_pupil_eye::relative_pupil_eye(float sensitivity) :
        sensitivity(sensitivity), half_span(std::sqrt(min_span))
{
	if (not(1.0f <= sensitivity and sensitivity <= 3.0f))
		throw std::invalid_argument("Pupil sensitivity must be between 1.0 and 3.0");
}

std::optional<float> relative_pupil_eye::invalid(const std::string & reason)
{
	recent_sizes.clear();
	if (not baseline)
	{
		baseline_samples.clear();
		status_ = reason;
		return std::nullopt;
	}
	++invalid_frames;
	if (not smoothed)
	{
		status_ = reason;
		return std::nullopt;
	}
	if (invalid_frames <= 5)
	{
		status_ = reason + "; holding last estimate";
		return float(*smoothed);
	}
	// Ease toward neutral during longer occlusion instead of jumping to 5.
	*smoothed += 0.14 * (5.0 - *smoothed);
	status_ = reason + "; easing to neutral";
	if (std::abs(*smoothed - 5.0) >= 0.05)
		return float(*smoothed);
	return std::nullopt;
}

void relative_pupil_eye::learn_range()
{
	// Session range: 2nd..98th percentile of the recent valid sizes.
	std::vector<float> sorted(history.begin(), history.end());
	std::ranges::sort(sorted);
	auto at = [&](size_t rank) { return sorted[rank]; };
	double low = np_percentile<float>(sorted.size(), 2, at);
	double high = np_percentile<float>(sorted.size(), 98, at);
	if (high / std::max(low, 1e-3) < min_span)
		return;
	center = std::sqrt(low * high);
	half_span = std::sqrt(high / low);
}

std::optional<float> relative_pupil_eye::update(const std::optional<pupil_detection> & detection)
{
	if (not detection or detection->confidence < 0.20)
		return invalid("pupil not visible");
	std::optional<double> reference = center ? center : baseline;
	double diameter = detection->diameter_px;
	if (reference and not(1 / plausible_ratio <= diameter / *reference and diameter / *reference <= plausible_ratio))
		return invalid("size outside plausible range");
	recent_sizes.push_back(diameter);
	if (recent_sizes.size() > recent_frames)
		recent_sizes.pop_front();
	double size = median(recent_sizes);
	if (not baseline)
	{
		baseline_samples.push_back(size);
		if (baseline_samples.size() > baseline_frames)
			baseline_samples.pop_front();
		if (baseline_samples.size() < baseline_frames)
		{
			status_ = "warming " + std::to_string(baseline_samples.size()) + "/" + std::to_string(baseline_frames);
			return std::nullopt;
		}
		auto [min, max] = std::ranges::minmax(baseline_samples);
		auto half = baseline_samples.begin() + baseline_samples.size() / 2;
		double size_trend = std::abs(median(std::ranges::subrange(baseline_samples.begin(), half)) - median(std::ranges::subrange(half, baseline_samples.end())));
		if (max - min > 10 or size_trend > 4)
		{
			status_ = "warming: hold gaze steady";
			return std::nullopt;
		}
		baseline = median(baseline_samples);
	}
	history.push_back(float(size));
	if (history.size() > history_frames)
		history.pop_front();
	if (++updates % 24 == 0)
		learn_range();
	invalid_frames = 0;
	// -1..1 across the learned range (log scale), then a soft limit.
	double position = std::log(size / (center ? *center : *baseline)) / std::log(half_span);
	double estimate = 5.0 + 3.0 * std::tanh(1.6 * position * sensitivity / 1.4);
	if (not smoothed)
		smoothed = estimate;
	else
	{
		double response = 0.32 - 0.06 * (sensitivity - 1.0);
		double step = response * (estimate - *smoothed);
		*smoothed += std::clamp(step, -0.27, 0.27);
	}
	status_ = center ? "tracking" : "tracking (learning range)";
	return float(*smoothed);
}

pupil_tracker::pupil_tracker(float sensitivity) :
        eyes{relative_pupil_eye(sensitivity), relative_pupil_eye(sensitivity)}
{
}

pupil_tracker::result pupil_tracker::update(std::span<const uint8_t> strip, std::span<const int> camera_ids)
{
	eye_result results[2];
	for (int eye: {0, 1})
	{
		cv::Mat frame = eye_frame(strip, camera_ids, eye);
		auto & r = results[eye];
		if (not frame.empty())
			r.detection = detect_pupil(frame, previous_centers[eye]);
		if (r.detection and r.detection->confidence >= 0.20)
			previous_centers[eye] = cv::Point2f(r.detection->cx, r.detection->cy);
		r.mm = eyes[eye].update(r.detection);
		r.status = eyes[eye].status();
	}
	// Pupils constrict and dilate together: an eye that could not be
	// measured this frame (eyelid over the pupil, blink, glare) follows
	// the eye that was, instead of easing toward neutral.
	bool measured[2] = {starts_with_tracking(results[0].status), starts_with_tracking(results[1].status)};
	if (measured[0] and not measured[1])
		results[1].mm = results[0].mm;
	else if (measured[1] and not measured[0])
		results[0].mm = results[1].mm;
	return {std::move(results[0]), std::move(results[1])};
}

std::vector<uint8_t> pupil_tracker::preview_jpeg(std::span<const uint8_t> strip, std::span<const int> camera_ids, const result & r, int quality) const
{
	const cv::Scalar green(70, 240, 110), amber(90, 200, 255);
	// Dark outline keeps the text readable over bright skin.
	auto label = [](cv::Mat & image, const std::string & text, int y, double scale, cv::Scalar color) {
		cv::putText(image, text, {10, y}, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(0, 0, 0), 3, cv::LINE_AA);
		cv::putText(image, text, {10, y}, cv::FONT_HERSHEY_SIMPLEX, scale, color, 1, cv::LINE_AA);
	};
	std::vector<cv::Mat> tiles;
	for (int eye: {0, 1})
	{
		cv::Mat frame = eye_frame(strip, camera_ids, eye);
		if (frame.empty())
			continue;
		const eye_result & e = eye ? r.right : r.left;
		cv::Mat & tile = tiles.emplace_back();
		cv::cvtColor(frame, tile, cv::COLOR_GRAY2BGR);
		label(tile, eye ? "camera 1 - right eye" : "camera 0 - left eye", 25, 0.55, cv::Scalar(60, 255, 60));
		if (e.detection)
		{
			const pupil_detection & d = *e.detection;
			cv::ellipse(tile, cv::RotatedRect({d.cx, d.cy}, {d.axis_a, d.axis_b}, d.angle), green, 2);
			cv::circle(tile, cv::Point(int(std::lrint(d.cx)), int(std::lrint(d.cy))), 2, green, -1);
		}
		char caption[64];
		if (e.mm)
			std::snprintf(caption, sizeof(caption), "relative pupil %.2f", *e.mm);
		else
			std::snprintf(caption, sizeof(caption), "pupil warming/invalid");
		label(tile, caption, 49, 0.48, e.mm ? green : amber);
		label(tile, e.status, 71, 0.43, cv::Scalar(220, 220, 220));
	}
	std::vector<uint8_t> jpeg;
	if (tiles.empty())
		return jpeg;
	cv::Mat image;
	cv::hconcat(tiles, image);
	cv::imencode(".jpg", image, jpeg, {cv::IMWRITE_JPEG_QUALITY, quality});
	return jpeg;
}

} // namespace wivrn::qpro
