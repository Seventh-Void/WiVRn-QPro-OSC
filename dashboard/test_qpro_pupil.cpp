// Checks the pure helpers of qpro_pupil.cpp (built with WIVRN_BUILD_TEST)
#undef NDEBUG // assert must run in RelWithDebInfo too
#include "qpro_pupil.h"

#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
	auto s = qpro::parse_status(R"({"state":"streaming","message":"ok","left_mm":5.2,"right_mm":null,"left_status":"tracking","right_status":"pupil not visible; easing to neutral","fps":23.9})");
	assert(s.state == "streaming" and s.message == "ok");
	assert(s.left == 5.2 and std::isnan(s.right) and s.fps == 23.9);
	assert(s.left_status == "tracking" and s.right_status == "pupil not visible; easing to neutral");
	s = qpro::parse_status("{}"); // every field optional
	assert(s.state == "off" and s.message.isEmpty() and std::isnan(s.left) and std::isnan(s.fps));
	s = qpro::parse_status(R"({"state":"error","message":"no root","left_mm":"x"})");
	assert(s.state == "error" and s.message == "no root" and std::isnan(s.left));
	assert(qpro::parse_status("not json").state == "error");
	assert(qpro::parse_status("[1]").state == "error");

	{
		// Two parts as the server writes them, fed in pieces
		QByteArray a("\xff\xd8\r\n\r\nA\xff\xd9", 8), b("\xff\xd8" "B\xff\xd9", 5);
		auto part = [](const QByteArray & jpeg) {
			return "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(jpeg.size()) + "\r\n\r\n" + jpeg + "\r\n";
		};
		QByteArray all = part(a) + part(b);
		QByteArray buffer = all.left(20);
		assert(not qpro::take_mjpeg_frame(buffer)); // headers incomplete
		buffer = all.left(part(a).size() - 3); // body incomplete
		assert(not qpro::take_mjpeg_frame(buffer));
		buffer = all;
		assert(qpro::take_mjpeg_frame(buffer) == a); // body may contain a blank line
		assert(qpro::take_mjpeg_frame(buffer) == b);
		assert(not qpro::take_mjpeg_frame(buffer));
		QByteArray bad = "--frame\r\nContent-Type: image/jpeg\r\n\r\n" + part(b);
		assert(not qpro::take_mjpeg_frame(bad)); // no length: headers skipped
		assert(qpro::take_mjpeg_frame(bad) == b);
	}

	std::puts("test-qpro-pupil: ok");
}
