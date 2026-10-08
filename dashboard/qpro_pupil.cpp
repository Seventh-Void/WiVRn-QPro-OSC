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


#include "qpro_pupil.h"

#include <KLocalizedString>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <utility>

namespace
{
// wivrn-server's pupil tracking (server side "qpro-pupils")
const QUrl status_url{"http://127.0.0.1:8081/status.json"};
// The five camera strip with pupil detection
const QUrl camera_stream{"http://127.0.0.1:8081/preview.mjpg"};
constexpr qsizetype max_stream_buffer = 16 << 20;
} // namespace

qpro::pupil_status qpro::parse_status(const QByteArray & json)
{
	QJsonParseError error;
	QJsonDocument document = QJsonDocument::fromJson(json, &error);
	if (error.error != QJsonParseError::NoError or not document.isObject())
		return {.state = "error", .message = i18n("Invalid status from wivrn-server")};

	QJsonObject object = document.object();
	auto number = [&](const char * key) {
		QJsonValue value = object[key];
		return value.isDouble() ? value.toDouble() : NAN;
	};
	return {
	        .state = object["state"].toString("off"),
	        .message = object["message"].toString(),
	        .left = number("left_mm"),
	        .right = number("right_mm"),
	        .left_status = object["left_status"].toString(),
	        .right_status = object["right_status"].toString(),
	        .fps = number("fps"),
	};
}

std::optional<QByteArray> qpro::take_mjpeg_frame(QByteArray & buffer)
{
	// "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: N\r\n\r\n" + N bytes + "\r\n"
	qsizetype header_end = buffer.indexOf("\r\n\r\n");
	if (header_end < 0)
		return std::nullopt;
	qsizetype body = header_end + 4;
	qsizetype field = buffer.indexOf("Content-Length:");
	bool ok = false;
	qsizetype length = 0;
	if (field >= 0 and field < header_end)
	{
		field += sizeof("Content-Length:") - 1;
		length = buffer.mid(field, buffer.indexOf("\r\n", field) - field).trimmed().toLongLong(&ok);
	}
	if (not ok or length < 0)
	{
		buffer.remove(0, body); // malformed part: skip its headers
		return std::nullopt;
	}
	if (buffer.size() < body + length)
		return std::nullopt;
	QByteArray jpeg = buffer.mid(body, length);
	buffer.remove(0, body + length);
	return jpeg;
}

QproPupil::QproPupil()
{
	m_show_cameras = QSettings().value("qpro_pupil/show_cameras", false).toBool();

	connect(&m_poll, &QTimer::timeout, this, &QproPupil::poll);
	m_poll.start(1000);
	QTimer::singleShot(0, this, &QproPupil::poll);
}

QproPupil::~QproPupil()
{
	// The replies are children of m_network; keep their finished handlers from running on a half destroyed object
	for (QNetworkReply * reply: {m_status_reply, m_stream})
		if (reply)
			reply->disconnect(this);
}

void QproPupil::setShowCameras(bool value)
{
	if (value == m_show_cameras)
		return;
	m_show_cameras = value;
	QSettings().setValue("qpro_pupil/show_cameras", value);
	showCamerasChanged();
	update_stream();
}

void QproPupil::poll()
{
	if (m_status_reply)
		return; // previous request still running

	QNetworkRequest request(status_url);
	request.setTransferTimeout(800);
	m_status_reply = m_network.get(request);
	connect(m_status_reply, &QNetworkReply::finished, this, [this] {
		QNetworkReply * reply = std::exchange(m_status_reply, nullptr);
		reply->deleteLater();
		// Nothing listening: wivrn-server is not running pupil tracking
		m_status = reply->error() == QNetworkReply::NoError ? qpro::parse_status(reply->readAll()) : qpro::pupil_status{};
		statusChanged();
		update_stream();
	});
}

// The camera strip streams only while the server tracks; a dropped stream is retried every second.
void QproPupil::update_stream()
{
	bool want = m_show_cameras and m_status.state == "streaming";
	if (want and not m_stream)
	{
		m_stream = m_network.get(QNetworkRequest(camera_stream));
		QNetworkReply * reply = m_stream;
		connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
			m_stream_buffer += reply->readAll();
			std::optional<QByteArray> latest;
			while (auto jpeg = qpro::take_mjpeg_frame(m_stream_buffer))
				latest = std::move(jpeg);
			if (m_stream_buffer.size() > max_stream_buffer)
				m_stream_buffer.clear();
			if (latest)
			{
				m_camera_frame = "data:image/jpeg;base64," + QString::fromLatin1(latest->toBase64());
				cameraFrameChanged();
			}
		});
		connect(reply, &QNetworkReply::finished, this, [this, reply] {
			reply->deleteLater();
			if (reply != m_stream)
				return;
			m_stream = nullptr;
			m_stream_buffer.clear();
			QTimer::singleShot(1000, this, &QproPupil::update_stream);
		});
	}
	else if (not want and m_stream)
	{
		QNetworkReply * reply = m_stream;
		m_stream = nullptr;
		m_stream_buffer.clear();
		reply->abort();
	}
	if (not want and not m_camera_frame.isEmpty())
	{
		m_camera_frame.clear();
		cameraFrameChanged();
	}
}
