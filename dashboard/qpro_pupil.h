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

#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <cmath>
#include <optional>
#include <qqmlintegration.h>

// Pure helpers, exposed for test_qpro_pupil.cpp
namespace qpro
{
// wivrn-server's GET /status.json; every field optional, null or missing numbers become NaN.
struct pupil_status
{
	QString state = "off";
	QString message;
	double left = NAN;
	double right = NAN;
	QString left_status;
	QString right_status;
	double fps = NAN;
};
pupil_status parse_status(const QByteArray & json);

// Takes the next JPEG out of an MJPEG stream buffer ("--frame", headers with
// Content-Length, blank line, JPEG). nullopt while incomplete.
std::optional<QByteArray> take_mjpeg_frame(QByteArray & buffer);
} // namespace qpro

// Shows wivrn-server's Quest Pro camera pupil tracking (status and camera preview on 127.0.0.1:8081).
// Its configuration ("qpro-pupils") lives in Settings.
class QproPupil : public QObject
{
	Q_OBJECT
	QML_NAMED_ELEMENT(QproPupil)
	QML_SINGLETON

	Q_PROPERTY(QString state READ state NOTIFY statusChanged)
	Q_PROPERTY(QString message READ message NOTIFY statusChanged)
	Q_PROPERTY(double pupilLeft READ pupilLeft NOTIFY statusChanged)
	Q_PROPERTY(double pupilRight READ pupilRight NOTIFY statusChanged)
	Q_PROPERTY(QString leftStatus READ leftStatus NOTIFY statusChanged)
	Q_PROPERTY(QString rightStatus READ rightStatus NOTIFY statusChanged)
	Q_PROPERTY(double fps READ fps NOTIFY statusChanged)
	Q_PROPERTY(bool showCameras READ showCameras WRITE setShowCameras NOTIFY showCamerasChanged)
	Q_PROPERTY(QString cameraFrame READ cameraFrame NOTIFY cameraFrameChanged)

public:
	QproPupil();
	~QproPupil();

	QString state() const
	{
		return m_status.state;
	}
	QString message() const
	{
		return m_status.message;
	}
	double pupilLeft() const
	{
		return m_status.left;
	}
	double pupilRight() const
	{
		return m_status.right;
	}
	QString leftStatus() const
	{
		return m_status.left_status;
	}
	QString rightStatus() const
	{
		return m_status.right_status;
	}
	double fps() const
	{
		return m_status.fps;
	}
	bool showCameras() const
	{
		return m_show_cameras;
	}
	void setShowCameras(bool);
	QString cameraFrame() const
	{
		return m_camera_frame;
	}

Q_SIGNALS:
	void statusChanged();
	void showCamerasChanged();
	void cameraFrameChanged();

private:
	void poll();
	void update_stream();

	QTimer m_poll;
	qpro::pupil_status m_status;
	bool m_show_cameras;
	QNetworkAccessManager m_network;
	QNetworkReply * m_status_reply = nullptr;
	QNetworkReply * m_stream = nullptr;
	QByteArray m_stream_buffer;
	QString m_camera_frame; // data: URL of the latest camera strip
};
