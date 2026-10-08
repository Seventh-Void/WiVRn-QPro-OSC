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

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wivrn::qpro
{

// Loopback-only (127.0.0.1) HTTP/1.0 server for the dashboard:
//   GET /preview.mjpg  multipart/x-mixed-replace stream of the latest JPEG (boundary "frame")
//   GET /status.json   latest status object, or {}
// One thread polls listener + clients + an eventfd; slow clients skip frames, the producer never blocks.
class preview_server
{
	int listen_fd = -1;
	int wake_fd = -1;
	uint16_t port_ = 0;
	std::atomic<bool> quit = false;
	std::atomic<int> viewers = 0;

	std::mutex mutex;
	std::shared_ptr<const std::vector<uint8_t>> jpeg;
	uint64_t jpeg_seq = 0;
	std::string status;

	std::thread thread;

	void run();
	void wake();

public:
	// port 0 picks an ephemeral port (see port()). Bind failure leaves the server disabled: bound() is false.
	explicit preview_server(uint16_t port = 8081);
	~preview_server();
	preview_server(const preview_server &) = delete;
	preview_server & operator=(const preview_server &) = delete;

	bool bound() const
	{
		return thread.joinable();
	}
	uint16_t port() const
	{
		return port_;
	}

	void publish_jpeg(std::vector<uint8_t> jpeg);
	void publish_status(std::string json);

	// true while at least one /preview.mjpg client is connected
	bool has_viewers() const
	{
		return viewers > 0;
	}
};

} // namespace wivrn::qpro
