import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// Root monitoring view. Data comes from the C++ MonitorBridge context property "backend".
Rectangle {
    id: root
    color: "#0a0a18"

    // Fallback values used only when backend is not available (e.g. in QML designer).
    readonly property bool   recording:   typeof backend !== "undefined" ? backend.recording   : false
    readonly property int    elapsedMs:   typeof backend !== "undefined" ? backend.elapsedMs   : 0
    readonly property int    cameraCount: typeof backend !== "undefined" ? backend.cameraCount  : 0
    readonly property string sessionPath: typeof backend !== "undefined" ? backend.sessionPath  : ""
    readonly property int    frameGen:    typeof backend !== "undefined" ? backend.frameGen     : 0
    // Per-camera generation counters — only the relevant slot reloads its image.
    readonly property var    frameGens:   typeof backend !== "undefined" ? backend.frameGens    : []
    // Column count for the camera grid — chosen to give the most square layout.
    readonly property int gridCols: {
        const n = Math.max(1, root.cameraCount)
        if (n === 1) return 1
        if (n <= 4) return 2
        if (n <= 6) return 3
        if (n <= 9) return 3
        return 4
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // ── Header row ─────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            // "LIVE FEEDS" label
            Label {
                text: "LIVE FEEDS"
                color: "#55557a"
                font { pixelSize: 10; bold: true; letterSpacing: 2 }
            }

            // Camera count badge
            Rectangle {
                visible: root.cameraCount > 0
                width: camCountLabel.implicitWidth + 12
                height: 18; radius: 9
                color: "#1a1a38"
                border.color: "#33335a"
                border.width: 1

                Label {
                    id: camCountLabel
                    anchors.centerIn: parent
                    text: root.cameraCount + " cam" + (root.cameraCount !== 1 ? "s" : "")
                    color: "#6666aa"
                    font { pixelSize: 9; bold: true }
                }
            }

            Item { Layout.fillWidth: true }
        }

        // ── Camera grid ────────────────────────────────────────────────────
        GridLayout {
            id: cameraGrid
            Layout.fillWidth:  true
            Layout.fillHeight: true
            columns:     root.gridCols
            rowSpacing:  6
            columnSpacing: 6

            Repeater {
                model: Math.max(1, root.cameraCount)

                delegate: CameraSlot {
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    cameraIndex: index
                    hasCamera:   index < root.cameraCount
                    // Each slot tracks only its own camera's generation counter,
                    // so unrelated camera updates don't trigger a reload here.
                    frameGen:    (root.frameGens && index < root.frameGens.length)
                                 ? root.frameGens[index] : 0
                }
            }
        }

        // ── Session path strip (recording only) ────────────────────────────
        Label {
            Layout.fillWidth: true
            visible:  root.recording && root.sessionPath !== ""
            text:     "▸  " + root.sessionPath
            color:    "#335533"
            font { pixelSize: 10; family: "Courier New, Courier, monospace" }
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

    // ── Component definitions ──────────────────────────────────────────────

    component CameraSlot : Rectangle {
        id: slotRoot
        property int  cameraIndex:   0
        property bool hasCamera:     false
        property int  frameGen:      0

        // True once the first frame has arrived for this slot.
        property bool hasFrame: false

        color:  "#0e0e22"
        radius: 6
        border.color: hasFrame ? "#2a3060" : (hasCamera ? "#1e1e3a" : "#151528")
        border.width: 1
        implicitHeight: 180
        clip: true

        // Subtle animated top-edge glow when receiving frames.
        Rectangle {
            anchors { top: parent.top; left: parent.left; right: parent.right }
            height: 1
            visible: slotRoot.hasFrame
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 0.5; color: "#44447a" }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }

        // ── Live camera feed ───────────────────────────────────────────────
        Image {
            id: feedImg
            anchors.fill: parent
            anchors.margins: 1
            visible: slotRoot.hasCamera
            cache: false
            fillMode: Image.PreserveAspectFit
            // Synchronous load eliminates the blank-flash that causes flickering.
            // The provider (VideoFeedProvider::requestImage) is a fast read-lock+copy.
            asynchronous: false
            source: slotRoot.hasCamera
                ? ("image://videofeed/" + slotRoot.cameraIndex + "?v=" + slotRoot.frameGen)
                : ""

            onStatusChanged: {
                if (status === Image.Ready) slotRoot.hasFrame = true
            }
        }

        // ── Waiting-for-frame overlay (shown before first frame) ───────────
        Rectangle {
            anchors.fill: parent
            color: "transparent"
            visible: slotRoot.hasCamera && !slotRoot.hasFrame

            // Pulsing scan-line animation
            Rectangle {
                id: scanLine
                width:  parent.width
                height: 1
                color:  "#22224a"
                y: 0

                SequentialAnimation on y {
                    running: slotRoot.hasCamera && !slotRoot.hasFrame
                    loops:   Animation.Infinite
                    NumberAnimation {
                        to: slotRoot.height; duration: 1800
                        easing.type: Easing.InOutSine
                    }
                    NumberAnimation { to: 0; duration: 0 }
                }
            }

            Label {
                anchors.centerIn: parent
                text: "Connecting…"
                color: "#33335a"
                font { pixelSize: 11 }
            }
        }

        // ── No-camera placeholder ──────────────────────────────────────────
        Label {
            anchors.centerIn: parent
            visible: !slotRoot.hasCamera
            text: "No camera"
            color: "#1e1e38"
            font { pixelSize: 12 }
        }

        // ── Top-left: camera index badge ───────────────────────────────────
        Rectangle {
            visible: slotRoot.hasCamera
            anchors { left: parent.left; top: parent.top; margins: 6 }
            width:  camLabel.implicitWidth + 10
            height: 16; radius: 8
            color:  slotRoot.hasFrame ? "#1a1a38" : "#131326"
            border.color: slotRoot.hasFrame ? "#33335a" : "#1e1e36"
            border.width: 1
            z: 10

            Label {
                id: camLabel
                anchors.centerIn: parent
                text:  "Cam " + (slotRoot.cameraIndex + 1)
                color: slotRoot.hasFrame ? "#6666aa" : "#33334a"
                font { pixelSize: 9; bold: true }
            }
        }

        // ── Top-right: live indicator dot ──────────────────────────────────
        Rectangle {
            visible: slotRoot.hasCamera
            anchors { top: parent.top; right: parent.right; margins: 7 }
            width: 7; height: 7; radius: 4
            z: 10

            // Green when receiving frames, dim amber when waiting.
            color: slotRoot.hasFrame ? "#33cc66" : "#554422"

            // Pulse when live.
            SequentialAnimation on opacity {
                running: slotRoot.hasFrame
                loops:   Animation.Infinite
                NumberAnimation { to: 0.4; duration: 700; easing.type: Easing.InOutSine }
                NumberAnimation { to: 1.0; duration: 700; easing.type: Easing.InOutSine }
            }

            // Static when waiting.
            opacity: slotRoot.hasFrame ? 1.0 : 0.5
        }
    }

    // ── Recording bar ──────────────────────────────────────────────────────

    component RecordingBar : Rectangle {
        id: bar
        property bool recording: false
        property int  elapsedMs: 0

        signal startRequested()
        signal stopRequested()

        height:       52
        radius:       7
        color:        recording ? "#180a0a" : "#09091a"
        border.color: recording ? "#772222" : "#1e1e40"
        border.width: 1

        // Park the Stop button's pulse when recording ends, so it can't be
        // left frozen at a half-lit value. The colour bindings guard on
        // `recording` anyway; this just keeps the stored value honest.
        onRecordingChanged: if (!recording) recBtn.pulse = 0.0

        RowLayout {
            anchors { fill: parent; margins: 10 }
            spacing: 12

            // Blinking REC dot
            Rectangle {
                width: 9; height: 9; radius: 5
                color: recording ? "#ff4444" : "#2a2a44"
                SequentialAnimation on opacity {
                    running: recording
                    loops:   Animation.Infinite
                    NumberAnimation { to: 0.15; duration: 550; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 1.0;  duration: 550; easing.type: Easing.InOutSine }
                }
            }

            Label {
                text:  recording ? "REC" : "STANDBY"
                color: recording ? "#ff6666" : "#33334a"
                font { pixelSize: 10; bold: true; letterSpacing: 2 }
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: {
                    const h = Math.floor(elapsedMs / 3600000)
                    const m = Math.floor(elapsedMs % 3600000 / 60000)
                    const s = Math.floor(elapsedMs % 60000 / 1000)
                    return h.toString().padStart(2,"0") + ":" +
                           m.toString().padStart(2,"0") + ":" +
                           s.toString().padStart(2,"0")
                }
                color: recording ? "#eeeeff" : "#aaaacc"
                font { pixelSize: 24; family: "Courier New, Courier, monospace" }
            }

            Rectangle {
                id: recBtn
                width: 100; height: 30; radius: 5

                // 0 -> 1 -> 0 while recording, driving the pulse below. Held
                // at 0 otherwise so the button can't be left frozen mid-pulse
                // when recording stops. Deliberately slower (900ms each way)
                // and shallower than the REC dot's own blink beside it — this
                // is a large element the operator sees constantly, so a fast
                // strobe would read as a fault rather than as "recording".
                property real pulse
                SequentialAnimation on pulse {
                    running: recording
                    loops:   Animation.Infinite
                    NumberAnimation { from: 0.0; to: 1.0; duration: 900; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 1.0; to: 0.0; duration: 900; easing.type: Easing.InOutSine }
                }
                color: recording
                    ? (recArea.containsMouse
                        ? "#3a1010"
                        : Qt.rgba(0.14 + 0.20 * pulse, 0.03, 0.03, 1.0))
                    : (recArea.containsMouse ? "#0f2a18" : "#0b1e12")
                border.color: recording
                    ? Qt.rgba(0.73, 0.13 + 0.22 * pulse, 0.13 + 0.22 * pulse, 1.0)
                    : "#22bb55"
                border.width: recording ? 2 : 1

                Label {
                    anchors.centerIn: parent
                    text:  recording ? "■  Stop" : "●  Record"
                    color: recording
                        ? Qt.rgba(1.0, 0.40 + 0.25 * recBtn.pulse, 0.40 + 0.25 * recBtn.pulse, 1.0)
                        : "#33ee77"
                    font { pixelSize: 11; bold: true }
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
