pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

import io.github.wivrn.wivrn

Kirigami.ScrollablePage {
    id: pupil_page
    title: i18n("Pupil tracking")

    // The configuration is written through the running server, as the Settings page does
    property bool server_started: WivrnServer.serverStatus == WivrnServer.Started
    property bool streaming: QproPupil.state == "streaming"

    ColumnLayout {
        anchors.fill: parent

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18n("Pupil sizes reach games only with 'Face tracking over OSC (VRChat, Resonite)' enabled in Settings.")
            type: Kirigami.MessageType.Information
            visible: Settings.qproPupils && !Settings.vrchatOsc
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true

            RowLayout {
                Controls.Switch {
                    text: i18n("Camera pupil tracking (rooted Quest Pro)")
                    enabled: pupil_page.server_started
                    checked: Settings.qproPupils
                    onToggled: {
                        Settings.qproPupils = checked;
                        Settings.save(WivrnServer);
                    }
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("wivrn-server measures your pupils with the Quest Pro eye cameras. Needs a rooted headset reachable over wireless ADB.\nApplies on the next headset connection.")
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Sensitivity:")
                enabled: pupil_page.server_started && Settings.qproPupils
                Controls.Slider {
                    id: sensitivity
                    Layout.fillWidth: true
                    from: 1.0
                    to: 3.0
                    stepSize: 0.1
                    value: Settings.qproPupilSensitivity
                    // One save per drag, or per key press
                    onMoved: if (!pressed) pupil_page.save_sensitivity()
                    onPressedChanged: if (!pressed) pupil_page.save_sensitivity()
                }
                Controls.Label {
                    text: sensitivity.value.toFixed(1) + "×"
                    Layout.preferredWidth: 35
                    Layout.alignment: Qt.AlignRight
                }
            }

            Controls.Label {
                text: pupil_page.server_started ? i18n("Changes are saved immediately and apply on the next headset connection.") : i18n("Start the server to change these settings.")
                font: Kirigami.Theme.smallFont
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
            }

            Controls.Label {
                Kirigami.FormData.label: i18n("Status:")
                font.bold: true
                text: {
                    switch (QproPupil.state) {
                    case "off":
                        return Settings.qproPupils ? i18n("Waiting for the headset") : i18n("Off");
                    case "starting":
                        return i18n("Starting");
                    case "streaming":
                        return isNaN(QproPupil.fps) ? i18n("Tracking") : i18n("Tracking (%1 fps)", QproPupil.fps.toFixed(1));
                    case "error":
                        return i18n("Error");
                    default:
                        return QproPupil.state;
                    }
                }
            }

            Controls.Label {
                visible: text != ""
                text: QproPupil.message
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            Controls.Label {
                Kirigami.FormData.label: i18n("Left eye:")
                visible: pupil_page.streaming
                text: pupil_page.eye(QproPupil.pupilLeft, QproPupil.leftStatus)
            }

            Controls.Label {
                Kirigami.FormData.label: i18n("Right eye:")
                visible: pupil_page.streaming
                text: pupil_page.eye(QproPupil.pupilRight, QproPupil.rightStatus)
            }

            Controls.CheckBox {
                text: i18n("Show cameras")
                checked: QproPupil.showCameras
                onToggled: QproPupil.showCameras = checked
            }

            Kirigami.Separator {
                Kirigami.FormData.isSection: true
            }

            RowLayout {
                Controls.Button {
                    id: wireless_adb
                    text: i18n("Enable wireless ADB (USB)")
                    icon.name: "network-wireless"
                    property bool running: false
                    enabled: Adb.adbInstalled && !running
                    onClicked: {
                        wireless_adb.running = true;
                        wireless_adb_result.text = i18n("Running adb tcpip 5555…");
                        Adb.enableWirelessAdb().then(message => {
                            wireless_adb_result.text = message;
                            wireless_adb.running = false;
                        });
                    }
                }
                Kirigami.ContextualHelpButton {
                    toolTipText: i18n("wivrn-server reaches the headset over wireless ADB. Run this once after each headset reboot, with the headset plugged in over USB.")
                }
            }

            Controls.Label {
                id: wireless_adb_result
                visible: text != ""
                text: Adb.adbInstalled ? "" : i18n("ADB is not installed")
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }

        Image {
            Layout.fillWidth: true
            Layout.preferredHeight: QproPupil.cameraFrame != "" ? width * implicitHeight / Math.max(1, implicitWidth) : Kirigami.Units.gridUnit * 3
            visible: QproPupil.showCameras && pupil_page.streaming
            fillMode: Image.PreserveAspectFit
            cache: false
            source: QproPupil.cameraFrame

            Controls.Label {
                anchors.centerIn: parent
                visible: QproPupil.cameraFrame == ""
                text: i18n("Waiting for the cameras…")
            }
        }
    }

    function save_sensitivity() {
        if (sensitivity.value == Settings.qproPupilSensitivity)
            return;
        Settings.qproPupilSensitivity = sensitivity.value;
        Settings.save(WivrnServer);
    }

    function eye(mm: real, status: string): string {
        let size = isNaN(mm) ? "–" : i18nc("pupil diameter", "%1 mm", mm.toFixed(2));
        return status == "" ? size : i18nc("pupil diameter, tracking status", "%1 (%2)", size, status);
    }

    footer: Controls.DialogButtonBox {
        standardButtons: Controls.DialogButtonBox.NoButton
        onAccepted: applicationWindow().pageStack.pop()

        Controls.Button {
            text: i18nc("go back to the home page", "Back")
            icon.name: "go-previous"
            Controls.DialogButtonBox.buttonRole: Controls.DialogButtonBox.AcceptRole
        }
    }

    Shortcut {
        sequences: [StandardKey.Cancel]
        onActivated: {if (isCurrentPage) applicationWindow().pageStack.pop();}
    }

    Component.onCompleted: {
        // Fresh copy of the server's configuration: the immediate saves above write all of it back
        if (server_started)
            Settings.load(WivrnServer);
    }
}
