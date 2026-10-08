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
#include "oscquery.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace wivrn::vrchat_osc;
using namespace std::chrono;

static const std::string avatar_json = R"({"DESCRIPTION":"","FULL_PATH":"/avatar","ACCESS":0,"CONTENTS":{)"
                                       R"("change":{"DESCRIPTION":"","FULL_PATH":"/avatar/change","ACCESS":3,"TYPE":"s","VALUE":["avtr_x"]},)"
                                       R"("parameters":{"DESCRIPTION":"","FULL_PATH":"/avatar/parameters","ACCESS":0,"CONTENTS":{)"
                                       R"("VRCEmote":{"DESCRIPTION":"","FULL_PATH":"/avatar/parameters/VRCEmote","ACCESS":3,"TYPE":"i","VALUE":[0]},)"
                                       R"("FT":{"FULL_PATH":"/avatar/parameters/FT","ACCESS":0,"CONTENTS":{"v2":{"FULL_PATH":"/avatar/parameters/FT/v2","ACCESS":0,"CONTENTS":{)"
                                       R"("JawOpen":{"FULL_PATH":"/avatar/parameters/FT/v2/JawOpen","ACCESS":3,"TYPE":"f","VALUE":[0.0]}}}}}}}}})";

// Serves one connection with `response`, then waits for the client to close (keep-alive style)
static uint16_t serve_once(std::string response, std::thread & t)
{
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(ls, (sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(ls, 1) == 0);
	socklen_t len = sizeof(addr);
	getsockname(ls, (sockaddr *)&addr, &len);

	t = std::thread([ls, response = std::move(response)]() {
		int c = accept(ls, nullptr, nullptr);
		std::string req;
		char buf[1024];
		while (req.find("\r\n\r\n") == std::string::npos)
		{
			ssize_t n = recv(c, buf, sizeof(buf), 0);
			assert(n > 0);
			req.append(buf, n);
		}
		assert(req.starts_with("GET /avatar HTTP/1.1\r\nHost: 127.0.0.1:"));
		// send in two pieces to exercise partial reads
		send(c, response.data(), response.size() / 2, MSG_NOSIGNAL);
		std::this_thread::sleep_for(20ms);
		send(c, response.data() + response.size() / 2, response.size() - response.size() / 2, MSG_NOSIGNAL);
		while (recv(c, buf, sizeof(buf), 0) > 0)
			;
		close(c);
		close(ls);
	});
	return ntohs(addr.sin_port);
}

static std::optional<nlohmann::json> fetch(std::string response, bool until_timeout = false)
{
	std::thread t;
	uint16_t port = serve_once(std::move(response), t);
	auto start = steady_clock::now();
	auto j = fetch_avatar({"127.0.0.1", port}, 1000);
	// Content-Length / chunked terminator must end the read, not the timeout
	assert(steady_clock::now() - start < (until_timeout ? 1200ms : 500ms));
	t.join();
	return j;
}

static void check(const std::optional<nlohmann::json> & j)
{
	assert(j);
	assert((*j)["FULL_PATH"] == "/avatar");
	const auto & params = (*j)["CONTENTS"]["parameters"];
	assert(params["FULL_PATH"] == "/avatar/parameters");
	assert(params["CONTENTS"]["VRCEmote"]["FULL_PATH"] == "/avatar/parameters/VRCEmote");
	assert(params["CONTENTS"]["VRCEmote"]["TYPE"] == "i");
	assert(params["CONTENTS"]["FT"]["CONTENTS"]["v2"]["CONTENTS"]["JawOpen"]["FULL_PATH"] == "/avatar/parameters/FT/v2/JawOpen");
}

int main()
{
	// Content-Length
	check(fetch("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(avatar_json.size()) + "\r\n\r\n" + avatar_json));

	// Chunked, with a chunk extension and a split in the middle of the JSON
	{
		auto a = avatar_json.substr(0, 100), b = avatar_json.substr(100);
		char ha[32], hb[32];
		snprintf(ha, sizeof(ha), "%zx;ext=1\r\n", a.size());
		snprintf(hb, sizeof(hb), "%zX\r\n", b.size());
		check(fetch(std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: application/json\r\n\r\n") + ha + a + "\r\n" + hb + b + "\r\n0\r\n\r\n"));
	}

	// Errors
	assert(not fetch("HTTP/1.1 404 Not Found\r\nContent-Length: 19\r\n\r\nOSC Path not found."));
	assert(not fetch("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nnope!"));
	{
		// Chunk sizes near SIZE_MAX must not wrap the bounds check (used to append the body over and over)
		std::string evil = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
		for (int i = 0; i < 20000; ++i)
			evil += "fffffffffffffffe\r\n";
		assert(not fetch(evil, true)); // waits for more data until the 1 s deadline, memory stays flat
	}
	assert(not fetch_avatar({"not an ip", 1}, 100));
	{
		// nothing listening: refused, quickly
		auto start = steady_clock::now();
		assert(not fetch_avatar({"127.0.0.1", 1}, 300));
		assert(steady_clock::now() - start < 400ms);
	}

	// mDNS browse: VRChat may or may not be running, must respect the timeout
	{
		auto start = steady_clock::now();
		auto ep = find_vrchat(300);
		auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
		assert(elapsed < 400);
		if (ep)
		{
			printf("VRChat OSCQuery at %s:%d\n", ep->address.c_str(), ep->port);
			auto j = fetch_avatar(*ep, 1000);
			printf("/avatar: %s\n", j ? (*j)["FULL_PATH"].dump().c_str() : "fetch failed");
		}
		else
			printf("VRChat not found (%lld ms)\n", (long long)elapsed);
	}

	printf("all tests passed\n");
	return 0;
}
