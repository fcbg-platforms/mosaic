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
    readonly property var    poseKps:     typeof backend !== "undefined" ? backend.allPoseKeypoints : []
    readonly property var    gazeData:   typeof backend !== "undefined" ? backend.allGazeData      : []

    property bool showPose: false
    property bool showGaze: false

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

            // Pose overlay toggle
            Rectangle {
                width: 100; height: 22; radius: 5
                color: root.showPose ? "#112211" : "#111122"
                border.color: root.showPose ? "#33bb55" : "#2a2a44"
                border.width: 1

                Label {
                    anchors.centerIn: parent
                    text: root.showPose ? "Pose  ON" : "Pose  OFF"
                    color: root.showPose ? "#44ff88" : "#44446a"
                    font { pixelSize: 10; bold: true; letterSpacing: 1 }
                }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.showPose = !root.showPose
                    onEntered: parent.border.color = root.showPose ? "#55dd77" : "#44446a"
                    onExited:  parent.border.color = root.showPose ? "#33bb55" : "#2a2a44"
                }
            }

            // Gaze overlay toggle
            Rectangle {
                width: 100; height: 22; radius: 5
                color: root.showGaze ? "#1a1500" : "#111122"
                border.color: root.showGaze ? "#aa8800" : "#2a2a44"
                border.width: 1

                Label {
                    anchors.centerIn: parent
                    text: root.showGaze ? "Gaze  ON" : "Gaze  OFF"
                    color: root.showGaze ? "#ffcc00" : "#44446a"
                    font { pixelSize: 10; bold: true; letterSpacing: 1 }
                }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.showGaze = !root.showGaze
                    onEntered: parent.border.color = root.showGaze ? "#ccaa00" : "#44446a"
                    onExited:  parent.border.color = root.showGaze ? "#aa8800" : "#2a2a44"
                }
            }
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
                    showPose:    root.showPose
                    showGaze:    root.showGaze
                    // Each slot tracks only its own camera's generation counter,
                    // so unrelated camera updates don't trigger a reload here.
                    frameGen:    (root.frameGens && index < root.frameGens.length)
                                 ? root.frameGens[index] : 0
                    poseKeypoints: (root.poseKps && index < root.poseKps.length)
                                   ? root.poseKps[index] : []
                    gazeInfo:    (root.gazeData && index < root.gazeData.length)
                                 ? root.gazeData[index] : null
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
        property bool showPose:      false
        property bool showGaze:      false
        property int  frameGen:      0
        property var  poseKeypoints: []
        property var  gazeInfo:      null

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

        // ── Pose skeleton overlay ──────────────────────────────────────────
        Canvas {
            id: poseCanvas
            anchors.fill: feedImg
            visible: slotRoot.showPose && slotRoot.hasCamera && slotRoot.hasFrame

            property var kps: slotRoot.poseKeypoints

            onKpsChanged: requestPaint()
            onVisibleChanged: if (visible) requestPaint()

            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                if (!kps || kps.length === 0) return
                var w = width, h = height
                ctx.fillStyle = "rgba(0,160,255,0.9)"
                for (var k = 0; k < kps.length; k++) {
                    if (kps[k].visibility < 0.4) continue
                    ctx.beginPath()
                    ctx.arc(kps[k].x * w, kps[k].y * h, 2, 0, 2 * Math.PI)
                    ctx.fill()
                }
            }
        }

        // ── Gaze overlay ──────────────────────────────────────────────────
        Canvas {
            id: gazeCanvas
            anchors.fill: feedImg
            visible: slotRoot.showGaze && slotRoot.hasCamera && slotRoot.hasFrame

            property var gz: slotRoot.gazeInfo

            onGzChanged:      requestPaint()
            onVisibleChanged: if (visible) requestPaint()

            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                if (!gz) return

                var w = width, h = height
                ctx.save()

                // ── Face bounding box ──────────────────────────────────────
                var fb = gz["face_box"]
                if (fb) {
                    ctx.strokeStyle = "rgba(255, 200, 0, 0.85)"
                    ctx.lineWidth = 1.5
                    ctx.strokeRect(fb["x"] * w, fb["y"] * h,
                                   fb["w"] * w, fb["h"] * h)
                }

                // ── Iris dots ──────────────────────────────────────────────
                var li = gz["left_iris"]
                var ri = gz["right_iris"]
                if (li && li["x"] !== undefined) {
                    ctx.beginPath()
                    ctx.arc(li["x"] * w, li["y"] * h, 5, 0, 2 * Math.PI)
                    ctx.fillStyle   = "rgba(255, 220, 50, 0.95)"
                    ctx.fill()
                    ctx.strokeStyle = "rgba(0, 0, 0, 0.6)"
                    ctx.lineWidth   = 1
                    ctx.stroke()
                }
                if (ri && ri["x"] !== undefined) {
                    ctx.beginPath()
                    ctx.arc(ri["x"] * w, ri["y"] * h, 5, 0, 2 * Math.PI)
                    ctx.fillStyle   = "rgba(255, 220, 50, 0.95)"
                    ctx.fill()
                    ctx.strokeStyle = "rgba(0, 0, 0, 0.6)"
                    ctx.lineWidth   = 1
                    ctx.stroke()
                }

                // ── Gaze direction arrow ───────────────────────────────────
                // Origin: midpoint between the two irises.
                // Direction: (gaze_dx, gaze_dy) scaled to 35% of the shorter
                //            canvas dimension so it's always clearly visible.
                if (fb && li && ri &&
                    li["x"] !== undefined && ri["x"] !== undefined) {
                    var ox = ((li["x"] + ri["x"]) / 2) * w
                    var oy = ((li["y"] + ri["y"]) / 2) * h
                    var dx = gz["gaze_dx"]
                    var dy = gz["gaze_dy"]
                    // gaze_dx/dy are 0 when looking straight; scale to pixels
                    var armLen = Math.min(w, h) * 0.35
                    var tx = ox + dx * armLen
                    var ty = oy + dy * armLen

                    // Dashed line from eye-midpoint to gaze target
                    ctx.setLineDash([4, 3])
                    ctx.strokeStyle = "rgba(255, 200, 0, 0.5)"
                    ctx.lineWidth   = 1.5
                    ctx.beginPath()
                    ctx.moveTo(ox, oy)
                    ctx.lineTo(tx, ty)
                    ctx.stroke()
                    ctx.setLineDash([])

                    // Gaze target crosshair
                    var r = 8
                    ctx.strokeStyle = "rgba(255, 200, 0, 0.95)"
                    ctx.lineWidth   = 2
                    ctx.beginPath()
                    ctx.arc(tx, ty, r, 0, 2 * Math.PI)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.moveTo(tx - r - 4, ty); ctx.lineTo(tx + r + 4, ty)
                    ctx.moveTo(tx, ty - r - 4); ctx.lineTo(tx, ty + r + 4)
                    ctx.stroke()
                }

                ctx.restore()
            }
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
                color: recording
                    ? (recArea.containsMouse ? "#3a1010" : "#240808")
                    : (recArea.containsMouse ? "#0f2a18" : "#0b1e12")
                border.color: recording ? "#bb2222" : "#22bb55"
                border.width: 1

                Label {
                    anchors.centerIn: parent
                    text:  recording ? "■  Stop" : "●  Record"
                    color: recording ? "#ff6666" : "#33ee77"
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
