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

// Parameter matching and encoding follow VRCFaceTracking (Apache-2.0):
// BaseParameter.cs, BinaryBaseParameter.cs, ParamContainers.cs (EParam)

#include "params.h"

#include "osc.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <optional>

#include <nlohmann/json.hpp>

namespace wivrn::vrchat_osc
{

namespace
{
constexpr std::string_view defaults[] = {
        // oscavmgr default_combined
        "BrowExpressionLeft",
        "BrowExpressionRight",
        "EyeLidLeft",
        "EyeLidRight",
        "JawX",
        "LipFunnelLower",
        "LipFunnelUpper",
        "LipPucker",
        "MouthLowerDown",
        "MouthStretchTightenLeft",
        "MouthStretchTightenRight",
        "MouthUpperUp",
        "MouthX",
        "SmileSadLeft",
        "SmileSadRight",
        // oscavmgr default_unified
        "CheekPuffLeft",
        "CheekPuffRight",
        "EyeSquintLeft",
        "EyeSquintRight",
        "JawOpen",
        "MouthClosed",
};

enum class kind
{
	main,
	negative,
	bit,
};

struct match
{
	size_t index;
	kind k;
	int bit = 0;
};

bool is_tracking_active(std::string_view name)
{
	return name.ends_with("TrackingActive");
}

std::optional<size_t> find(std::span<const std::string_view> names, std::string_view name)
{
	auto it = std::ranges::find(names, name);
	if (it == names.end())
		return std::nullopt;
	return it - names.begin();
}

// leaf: last path component, e.g. "JawOpen", "MouthX4", "MouthXNegative"
std::optional<match> match_leaf(std::span<const std::string_view> names, std::string_view leaf)
{
	if (auto i = find(names, leaf))
		return match{*i, kind::main};

	if (leaf.ends_with("Negative"))
	{
		if (auto i = find(names, leaf.substr(0, leaf.size() - 8)))
			return match{*i, kind::negative};
	}

	size_t digits = leaf.find_last_not_of("0123456789") + 1;
	if (digits == 0 or digits == leaf.size() or leaf.size() - digits > 3)
		return std::nullopt;
	int n = std::stoi(std::string(leaf.substr(digits)));
	if (n < 1 or n > 128 or (n & (n - 1)))
		return std::nullopt;
	if (auto i = find(names, leaf.substr(0, digits)))
		return match{*i, kind::bit, std::countr_zero(unsigned(n))};
	return std::nullopt;
}

// Mirrors VRCFT NativeParameters.cs conditions
void update_eye_flags(std::span<const std::string_view> names, const auto & entries, bool & gaze, bool & lid)
{
	auto has = [](std::string_view s, std::string_view sub) { return s.find(sub) != s.npos; };
	gaze = lid = false;
	for (const auto & e: entries)
	{
		std::string_view n = names[e.index];
		gaze |= has(n, "Eye") and (n.ends_with('X') or n.ends_with('Y'));
		lid |= has(n, "Eye") and (has(n, "Open") or has(n, "Lid"));
	}
}

void walk(const nlohmann::json & node, auto && on_leaf, int depth = 0)
{
	if (depth > max_json_depth)
		return;
	if (not node.is_object())
		return;
	if (auto it = node.find("FULL_PATH"); it != node.end() and it->is_string() and it->get_ref<const std::string &>().find("OSCm") != std::string::npos)
		return;

	if (auto it = node.find("CONTENTS"); it != node.end())
	{
		for (const auto & child: *it)
			walk(child, on_leaf, depth + 1);
		return;
	}

	auto path = node.find("FULL_PATH");
	auto type = node.find("TYPE");
	if (path == node.end() or type == node.end() or not path->is_string() or not type->is_string())
		return;
	on_leaf(path->get<std::string>(), type->get<std::string>());
}
} // namespace

avatar_params::avatar_params(std::span<const std::string_view> names) :
        names(names)
{
}

void avatar_params::load_defaults()
{
	entries.clear();
	for (auto name: defaults)
	{
		auto i = find(names, name);
		if (not i)
			continue;
		entries.push_back({
		        .index = *i,
		        .tracking_active = false,
		        .main = "/avatar/parameters/FT/v2/" + std::string(name),
		});
	}

	// Avatar unknown: keep VRChat native eye tracking on as well.
	gaze = lid = false;
}

void avatar_params::load(const nlohmann::json & avatar_node)
{
	entries.clear();
	walk(avatar_node, [&](const std::string & path, const std::string & type) {
		bool is_float = type == "f";
		bool is_bool = type == "T" or type == "F";
		if (not is_float and not is_bool)
			return;

		std::string_view leaf = path;
		leaf = leaf.substr(leaf.rfind('/') + 1);
		auto m = match_leaf(names, leaf);
		if (not m)
			return;

		std::string_view name = names[m->index];
		if (is_tracking_active(name))
		{
			// VRCFT: bool only, name without "v2/" prefix (e.g. /avatar/parameters/EyeTrackingActive)
			if (m->k != kind::main or not is_bool)
				return;
		}
		else
		{
			// VRCFT: address must end with "/v2/<leaf>"
			std::string_view p = path;
			if (not p.substr(0, p.size() - leaf.size()).ends_with("/v2/"))
				return;
			// Negative and binary parameters are bool only
			if (m->k != kind::main and not is_bool)
				return;
		}

		auto it = std::ranges::find(entries, m->index, &entry::index);
		if (it == entries.end())
		{
			entries.push_back({.index = m->index, .tracking_active = is_tracking_active(name)});
			it = entries.end() - 1;
		}

		switch (m->k)
		{
			case kind::main:
				it->main = path;
				it->main_is_bool = is_bool;
				break;
			case kind::negative:
				it->neg = path;
				break;
			case kind::bit:
				if (it->bits[m->bit].empty())
					++it->num_bits;
				it->bits[m->bit] = path;
				break;
		}
	});

	update_eye_flags(names, entries, gaze, lid);
}

void avatar_params::send(std::span<const float> values, osc_sender & osc)
{
	for (auto & e: entries)
	{
		float v = std::isnan(values[e.index]) ? 0 : values[e.index];

		if (not e.main.empty())
		{
			if (e.main_is_bool)
			{
				// VRCFT EParam bool is (value < 0.5); ConditionalBoolParameter is the value itself
				bool b = e.tracking_active ? v >= 0.5f : v < 0.5f;
				if (not e.sent or b != e.last_main)
				{
					osc.add(e.main, b);
					e.last_main = b;
				}
			}
			else
			{
				float f = std::clamp(v, -1.f, 1.f);
				if (not e.sent or std::abs(f - e.last_value) > 0.01f)
				{
					osc.add(e.main, f);
					e.last_value = f;
				}
			}
		}

		if (not e.neg.empty())
		{
			bool b = v < 0;
			if (not e.sent or b != e.last_neg)
			{
				osc.add(e.neg, b);
				e.last_neg = b;
			}
		}

		if (e.num_bits)
		{
			// VRCFT BinaryBaseParameter::ProcessBinary
			int big = 0; // negative values are cut without a Negative parameter
			if (v >= 0 or not e.neg.empty())
				big = std::abs(v) > 0.99999f ? ~0 : int(std::abs(v) * (1 << e.num_bits));

			for (size_t i = 0; i < e.bits.size(); ++i)
			{
				if (e.bits[i].empty())
					continue;
				bool b = (big >> i) & 1;
				if (not e.sent or b != e.last_bits[i])
				{
					osc.add(e.bits[i], b);
					e.last_bits[i] = b;
				}
			}
		}

		e.sent = true;
	}
}

} // namespace wivrn::vrchat_osc
