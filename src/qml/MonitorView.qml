import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// Root monitoring view. Data comes from the C++ MonitorBridge context property "backend".
Rectangle {
    id: root
    color: "#0d0d1a"

    // Fallback values used only when backend is not available (e.g. in QML designer).
    readonly property bool   recording:   typeof backend !== "undefined" ? backend.recording   : false
    readonly property int    elapsedMs:   typeof backend !== "undefined" ? backend.elapsedMs   : 0
    readonly property int    cameraCount: typeof backend !== "undefined" ? backend.cameraCount  : 0
    readonly property string sessionPath: typeof backend !== "undefined" ? backend.sessionPath  : ""

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        // ── Camera grid ────────────────────────────────────────────────────
        SectionLabel { text: "Live Feeds" }

        GridLayout {
            id: cameraGrid
            Layout.fillWidth:  true
            Layout.fillHeight: true
            columns: Math.max(1, Math.ceil(Math.sqrt(Math.max(1, root.cameraCount))))

            Repeater {
                model: Math.max(1, root.cameraCount)

                delegate: CameraSlot {
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    cameraIndex: index
                    hasCamera:   index < root.cameraCount
                }
            }
        }

        // ── Session path (shown while recording) ───────────────────────────
        Label {
            Layout.fillWidth: true
            visible:  root.recording && root.sessionPath !== ""
            text:     "📁  " + root.sessionPath
            color:    "#446644"
            font.pixelSize: 10
            font.family: "Courier New, Courier, monospace"
            elide:    Text.ElideMiddle
        }

        // ── Recording controls ─────────────────────────────────────────────
        RecordingBar {
            Layout.fillWidth: true
            recording:  root.recording
            elapsedMs:  root.elapsedMs

            onStartRequested: {
                if (typeof backend !== "undefined")
                    backend.startRecording()
            }
            onStopRequested: {
                if (typeof backend !== "undefined")
                    backend.stopRecording()
            }
        }
    }

    // ── Internal component definitions ─────────────────────────────────────

    component SectionLabel : Label {
        color: "#6666aa"
        font.pixelSize: 10
        font.letterSpacing: 2
        font.capitalization: Font.AllUppercase
    }

    component CameraSlot : Rectangle {
        property int  cameraIndex: 0
        property bool hasCamera:   false

        color:         "#13132a"
        radius:        6
        border.color:  hasCamera ? "#2a2a55" : "#1a1a33"
        border.width:  1
        implicitHeight: 200

        Label {
            anchors.centerIn: parent
            text:  hasCamera ? ("Camera " + (cameraIndex + 1)) : "No camera"
            color: hasCamera ? "#44446a" : "#2a2a44"
            font.pixelSize: 13
        }

        // Connected indicator
        Rectangle {
            visible: hasCamera
            anchors { top: parent.top; right: parent.right; margins: 6 }
            width: 8; height: 8; radius: 4
            color: "#33cc66"
        }
    }

    component RecordingBar : Rectangle {
        id: bar
        property bool recording: false
        property int  elapsedMs: 0

        signal startRequested()
        signal stopRequested()

        height:       58
        radius:       8
        color:        recording ? "#1e0a0a" : "#0a0a1a"
        border.color: recording ? "#882222" : "#22224a"
        border.width: 1

        RowLayout {
            anchors { fill: parent; margins: 12 }
            spacing: 14

            // Blinking REC dot
            Rectangle {
                width: 10; height: 10; radius: 5
                color: recording ? "#ff4444" : "#33334a"

                SequentialAnimation on opacity {
                    running: recording
                    loops:   Animation.Infinite
                    NumberAnimation { to: 0.15; duration: 550; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 1.0;  duration: 550; easing.type: Easing.InOutSine }
                }
            }

            Label {
                text:  recording ? "REC" : "STANDBY"
                color: recording ? "#ff6666" : "#44446a"
                font { pixelSize: 11; bold: true; letterSpacing: 2 }
            }

            // Running timer
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: {
                    const h = Math.floor(elapsedMs / 3600000)
                    const m = Math.floor(elapsedMs % 3600000 / 60000)
                    const s = Math.floor(elapsedMs % 60000 / 1000)
                    return h.toString().padStart(2, "0") + ":" +
                           m.toString().padStart(2, "0") + ":" +
                           s.toString().padStart(2, "0")
                }
                color: recording ? "#eeeeff" : "#ccccdd"
                font { pixelSize: 26; family: "Courier New, Courier, monospace" }
            }

            // Start / Stop button
            Rectangle {
                id: recBtn
                width: 110; height: 34; radius: 6

                color: recording
                    ? (recArea.containsMouse ? "#3a1010" : "#2a0808")
                    : (recArea.containsMouse ? "#10301a" : "#0d2213")

                border.color: recording ? "#cc3333" : "#33cc66"
                border.width: 1

                Label {
                    anchors.centerIn: parent
                    text:  recording ? "■  Stop" : "●  Record"
                    color: recording ? "#ff6666" : "#44ff88"
                    font { pixelSize: 12; bold: true }
                }

                MouseArea {
                    id: recArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape:  Qt.PointingHandCursor
                    onClicked:    recording ? bar.stopRequested() : bar.startRequested()
                }
            }
        }
    }
}
