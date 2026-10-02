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
    // Per-camera {fps, state}, refreshed ~1 Hz by MainWindow's poll of
    // VideoManager::camera_stats(). `state` is mosaic::CameraHealth:
    // 0 Unknown, 1 Ok, 2 Lagging, 3 Stalled.
    readonly property var    cameraHealth: typeof backend !== "undefined" ? backend.cameraHealth : []
    // Seconds left before recording starts; 0 when no countdown is pending.
    readonly property int    countdown:   typeof backend !== "undefined" ? backend.countdownSeconds : 0
    // True from the Record click until recording is actually live (or the
    // attempt is cancelled/fails) — spans the gap after the countdown hits 0
    // while RecordManager::start() runs. See MonitorBridge's own doc comment.
    readonly property bool   startPending: typeof backend !== "undefined" ? backend.startPending : false
    // RecordSettings::hidePreviewsWhileRecording, mirrored by MonitorBridge.
    readonly property bool   hidePreviews: typeof backend !== "undefined" ? backend.hidePreviews : false

    // ── Interview mode ─────────────────────────────────────────────────────
    // One camera recorded, so one camera shown. cameraCount keeps meaning the
    // configured count (see MonitorBridge): tiles are addressed by configured
    // index, so interview mode picks *which* index to show instead.
    readonly property bool   interviewMode:        typeof backend !== "undefined" ? backend.interviewMode        : false
    readonly property int    interviewCameraIndex: typeof backend !== "undefined" ? backend.interviewCameraIndex : 0
    readonly property real   interviewFps:         typeof backend !== "undefined" ? backend.interviewFps         : 0
    readonly property bool   interviewSwitching:   typeof backend !== "undefined" ? backend.interviewSwitching   : false
    // What the interview camera really does (-1 = not known yet): its own
    // reported maximum for the crop, then its measured rate. The badge shows
    // these, not interviewFps — that is only the rate asked for, and the
    // camera can deliver less (a crop's rows set its limit).
    readonly property real   interviewCameraMaxFps: typeof backend !== "undefined" ? backend.interviewCameraMaxFps : -1
    readonly property real   interviewMeasuredFps:  typeof backend !== "undefined" ? backend.interviewMeasuredFps  : -1
    readonly property real   interviewActualFps: root.interviewMeasuredFps > 0 ? root.interviewMeasuredFps
                                               : root.interviewCameraMaxFps > 0 ? root.interviewCameraMaxFps
                                               : -1
    // Below what was asked for: worth a different colour and the requested
    // rate beside it, so "36.7 fps" is never read as the setting.
    readonly property bool   interviewShortOfRate: root.interviewMode
                                                   && root.interviewActualFps > 0
                                                   && root.interviewActualFps < root.interviewFps * 0.97

    // Configured indices of the cameras this view is about — the ones that can
    // deliver frames. Empty while none are configured or a switch is reopening.
    readonly property var liveCameras: {
        if (root.interviewMode)
            return root.interviewCameraIndex < root.cameraCount ? [root.interviewCameraIndex] : []
        let all = []
        for (let i = 0; i < root.cameraCount; ++i) all.push(i)
        return all
    }
    // What the grid lays out: the live cameras, or one placeholder tile so the
    // grid never collapses to nothing — labelled as the interview camera when
    // that is what is coming back.
    readonly property var shownCameras: root.liveCameras.length > 0
        ? root.liveCameras
        : [root.interviewMode ? root.interviewCameraIndex : 0]

    // ── Session identity ───────────────────────────────────────────────────
    // Who and what the next recording is of. Session and task are optional; a
    // subject is not — clicking Record without one opens a dialog asking for
    // it, rather than quietly producing a timestamp-only folder name nobody can
    // trace back to a participant. (A trigger-started recording still can:
    // nobody is at the keyboard to be asked.)
    readonly property string subjectLabel:    typeof backend !== "undefined" ? backend.subjectLabel    : ""
    readonly property string sessionLabel:    typeof backend !== "undefined" ? backend.sessionLabel    : ""
    readonly property string taskLabel:       typeof backend !== "undefined" ? backend.taskLabel       : ""
    readonly property string sessionNotes:    typeof backend !== "undefined" ? backend.notes           : ""
    readonly property string folderPreview:   typeof backend !== "undefined" ? backend.folderPreview   : ""
    readonly property string identityWarning: typeof backend !== "undefined" ? backend.identityWarning : ""

    // Identity is fixed the moment the folder is created, so the fields lock
    // as soon as Record is pressed. Declared here rather than as a
    // Q_PROPERTY because it derives from two separate NOTIFY signals.
    readonly property bool identityEditable: !recording && !startPending

    // Previews go dark from the moment Record is clicked, not only once
    // recording is live — the countdown exists precisely because both subject
    // and experimenter are looking at this screen at that moment, so it should
    // already be calm when t=0 arrives. Gated on startPending rather than
    // countdown so the previews can't flash back on during start() itself.
    readonly property bool previewsHidden: (recording || startPending) && hidePreviews

    // Column count for the camera grid — chosen to give the most square layout.
    readonly property int gridCols: {
        const n = Math.max(1, root.shownCameras.length)
        if (n === 1) return 1
        if (n <= 4) return 2
        if (n <= 6) return 3
        if (n <= 9) return 3
        return 4
    }

    ColumnLayout {
        anchors.fill: parent
        // Tightened from 10/8. With six rows there are five gaps and two
        // margins, so a couple of pixels off each is ~14px back to the video —
        // free, because none of it was doing anything.
        anchors.margins: 8
        spacing: 6

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

            // Camera count badge — in interview mode, which camera and at
            // what rate, since that is what the operator needs to confirm.
            Rectangle {
                visible: root.cameraCount > 0
                width: camCountLabel.implicitWidth + 12
                height: 18; radius: 9
                color: root.interviewMode ? "#2a2110" : "#1a1a38"
                border.color: root.interviewShortOfRate ? "#aa8833"
                            : root.interviewMode ? "#6a5220" : "#33335a"
                border.width: 1

                Label {
                    id: camCountLabel
                    anchors.centerIn: parent
                    text: !root.interviewMode
                        ? root.cameraCount + " cam" + (root.cameraCount !== 1 ? "s" : "")
                        : "Cam " + (root.interviewCameraIndex + 1) + " only · "
                          + (root.interviewActualFps > 0
                             ? root.interviewActualFps.toFixed(1) + " fps"
                               + (root.interviewShortOfRate
                                  ? " (asked " + root.interviewFps.toFixed(0) + ")" : "")
                             // Nothing reported (stub builds, a camera
                             // without the node): the setting, as before.
                             : root.interviewFps.toFixed(0) + " fps")
                    color: root.interviewShortOfRate ? "#ffcc66"
                         : root.interviewMode ? "#ddaa55" : "#6666aa"
                    font { pixelSize: 9; bold: true }
                }

                HoverHandler { id: badgeHover }
                ToolTip.visible: badgeHover.hovered && root.interviewMode
                ToolTip.delay: 300
                ToolTip.text: root.interviewShortOfRate
                    ? "Camera " + (root.interviewCameraIndex + 1) + " delivers "
                      + root.interviewActualFps.toFixed(1) + " fps, less than the "
                      + root.interviewFps.toFixed(0) + " fps asked for. A shorter crop (fewer "
                      + "rows) raises it — Settings → Video → Interview mode."
                    : "The rate the camera really delivers, not the setting."
            }

            // ── Per-camera delivery health ─────────────────────────────
            // In the header row, in space that was an empty spacer, so it
            // costs no vertical room at all — the point of this screen is the
            // video, and a health readout that shrank the previews to report
            // on them would be self-defeating.
            //
            // Shows the measured frame rate, not a percentage. A percentage
            // here would invite comparison with sync_manifest.json's
            // coverage_pct, which is a different quantity computed a different
            // way, and the same fault reads as two different numbers. See
            // src/video/camera_health.hpp.
            Repeater {
                // The cameras this view is about (all of them, or the one in
                // interview mode), by configured index.
                model: root.liveCameras

                delegate: Rectangle {
                    id: healthChip

                    required property int modelData

                    readonly property var entry:
                        (root.cameraHealth && healthChip.modelData < root.cameraHealth.length)
                            ? root.cameraHealth[healthChip.modelData] : null
                    // Defaults of 0 double as the "nothing known" case, which
                    // is why the guard above can be this brief: state 0 is
                    // CameraHealth::Unknown and hides the chip entirely.
                    readonly property int  healthState: healthChip.entry ? healthChip.entry.state : 0
                    readonly property real fps:         healthChip.entry ? healthChip.entry.fps   : 0

                    // Nothing measured yet, or the camera is not grabbing:
                    // stay silent rather than report a fault that isn't one.
                    visible: healthChip.healthState !== 0

                    implicitWidth: healthLabel.implicitWidth + 12
                    height: 18
                    radius: 9
                    // Only ever coloured when something is actually wrong. A
                    // healthy rig reads as a quiet row, which is what makes
                    // amber worth looking at when it does appear.
                    color: healthChip.healthState === 3 ? "#3a1414"
                         : healthChip.healthState === 2 ? "#332813" : "#1a1a38"
                    border.color: healthChip.healthState === 3 ? "#aa3333"
                                : healthChip.healthState === 2 ? "#aa8833" : "#33335a"
                    border.width: 1

                    Label {
                        id: healthLabel
                        anchors.centerIn: parent
                        // 1-based like every other camera label on screen ("Cam 3").
                        text: "Cam " + (healthChip.modelData + 1) + "  " + healthChip.fps.toFixed(1)
                        color: healthChip.healthState === 3 ? "#ff8888"
                             : healthChip.healthState === 2 ? "#ffcc66" : "#6666aa"
                        font { pixelSize: 9; bold: true }
                    }

                    ToolTip.visible: healthArea.containsMouse
                    ToolTip.delay:   300
                    ToolTip.text: healthChip.healthState === 3
                        ? "Camera " + (healthChip.modelData + 1) + " is delivering far fewer frames than the " +
                          "others (" + healthChip.fps.toFixed(1) + " fps). Usually GigE packet loss " +
                          "— check its cable and network port before recording."
                        : healthChip.healthState === 2
                            ? "Camera " + (healthChip.modelData + 1) + " is behind the other cameras (" +
                              healthChip.fps.toFixed(1) + " fps)."
                            : "Camera " + (healthChip.modelData + 1) + ": " +
                              healthChip.fps.toFixed(1) + " fps"

                    MouseArea {
                        id: healthArea
                        anchors.fill: parent
                        hoverEnabled: true
                    }
                }
            }

            Item { Layout.fillWidth: true }

            Label {
                visible: root.interviewSwitching
                text: "reopening cameras…"
                color: "#7878a0"
                font { pixelSize: 10; italic: true }
            }

            ModeToggle {}
        }

        // ── Camera grid ────────────────────────────────────────────────────
        GridLayout {
            id: cameraGrid
            Layout.fillWidth:  true
            Layout.fillHeight: true
            visible:     !root.previewsHidden
            columns:     root.gridCols
            rowSpacing:  6
            columnSpacing: 6

            Repeater {
                // Configured indices, not 0..n: in interview mode the one tile
                // is camera 3's, and its feed URL and frame counter must say so.
                model: root.shownCameras

                delegate: CameraSlot {
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    cameraIndex: modelData
                    hasCamera:   modelData < root.cameraCount
                    // Hiding the grid alone would still let the source binding
                    // re-evaluate on every frameGen tick and hit
                    // VideoFeedProvider::requestImage; this makes "hidden"
                    // actually mean idle on the QML side.
                    previewActive: !root.previewsHidden
                    // Each slot tracks only its own camera's generation counter,
                    // so unrelated camera updates don't trigger a reload here.
                    frameGen:    (root.frameGens && modelData < root.frameGens.length)
                                 ? root.frameGens[modelData] : 0
                }
            }
        }

        // ── Previews-hidden placeholder ────────────────────────────────────
        // Takes the grid's place so the column doesn't jump. Keeps the one
        // piece of feedback the operator actually loses by hiding the video:
        // whether each camera is still delivering frames.
        PreviewHiddenStrip {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            visible: root.previewsHidden
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

        // ── Session identity ───────────────────────────────────────────────
        // Directly above the Record button, because these change per
        // recording: burying per-session values in a settings pane means
        // they stop being filled in.
        SessionIdentityBar {
            id: identityBar
            Layout.fillWidth: true
            visible: root.identityEditable
            // Without a minimum, a ColumnLayout is free to shrink this to
            // nothing: the camera grid above takes fillHeight, and once it is
            // squeezed flat everything below shares what is left. Bound to the
            // content rather than a magic number because the warning label
            // wraps, so the bar is two lines tall at narrow widths.
            Layout.minimumHeight: implicitHeight
        }

        // ── Operator notes ─────────────────────────────────────────────────
        // Deliberately *not* hidden while recording: the note worth having is
        // usually the one written once something has actually happened.
        SessionNotesBox {
            Layout.fillWidth: true
            Layout.minimumHeight: implicitHeight // same squeeze as above
        }

        // ── Recording controls ─────────────────────────────────────────────
        RecordingBar {
            Layout.fillWidth: true
            recording:  root.recording
            elapsedMs:  root.elapsedMs
            countdown:  root.countdown
            // The one control that must never be squeezable — see the
            // implicitHeight note on the component itself.
            Layout.minimumHeight: implicitHeight

            onStartRequested: {
                if (typeof backend !== "undefined")
                    backend.startRecording()
            }
            // Also the cancel path: MonitorBridge::stopRecording() cancels a
            // pending countdown instead of stopping a recording.
            onStopRequested: {
                if (typeof backend !== "undefined")
                    backend.stopRecording()
            }
        }
    }

    // ── Start countdown overlay ────────────────────────────────────────────
    // A sibling of the ColumnLayout rather than a child, so it paints over
    // everything without taking part in — or disturbing — the layout.
    Rectangle {
        anchors.fill: parent
        z: 100
        visible: root.countdown > 0
        color: "#d00a0a18"

        Column {
            anchors.centerIn: parent
            spacing: 10

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "RECORDING STARTS IN"
                color: "#ddaa44"
                font { pixelSize: 12; bold: true; letterSpacing: 3 }
            }

            Label {
                id: countdownNumber
                anchors.horizontalCenter: parent.horizontalCenter
                text:  root.countdown
                color: "#ffcc55"
                font { pixelSize: 128; bold: true; family: "Courier New, Courier, monospace" }

                // One pulse per tick, so the number visibly "lands" rather
                // than silently swapping. Restarting an already-running
                // animation is safe and is what makes each tick read.
                onTextChanged: if (root.countdown > 0) tickPulse.restart()

                SequentialAnimation {
                    id: tickPulse
                    NumberAnimation {
                        target: countdownNumber; property: "scale"
                        from: 1.25; to: 1.0; duration: 320
                        easing.type: Easing.OutCubic
                    }
                }
            }

            // The cancel affordance lives *in* the overlay rather than being
            // a pointer to the record button below: this overlay covers the
            // whole view, so the bar's own "✕ Cancel" is dimmed to near
            // invisibility while it's up. Telling the operator to click
            // something they can't see would be worse than useless at the one
            // moment they're trying to abort.
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 130; height: 32; radius: 5
                color: cancelArea.containsMouse ? "#3a2c10" : "#221a0c"
                border.color: "#ddaa44"
                border.width: 1

                Label {
                    anchors.centerIn: parent
                    text: "✕  Cancel"
                    color: "#ffcc66"
                    font { pixelSize: 12; bold: true }
                }

                MouseArea {
                    id: cancelArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape:  Qt.PointingHandCursor
                    onClicked: {
                        if (typeof backend !== "undefined")
                            backend.stopRecording()
                    }
                }
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "or press Ctrl+."
                color: "#77779a"
                font { pixelSize: 11 }
            }
        }
    }

    // ── Component definitions ──────────────────────────────────────────────

    component CameraSlot : Rectangle {
        id: slotRoot
        property int  cameraIndex:   0
        property bool hasCamera:     false
        property int  frameGen:      0
        // False while previews are hidden during a recording/countdown, which
        // clears the Image source so this slot stops requesting frames from
        // VideoFeedProvider rather than merely rendering them invisibly.
        property bool previewActive: true

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
            source: (slotRoot.hasCamera && slotRoot.previewActive)
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

    // ── Previews-hidden strip ──────────────────────────────────────────────

    // Shown in the camera grid's place while previews are hidden.
    //
    // Reads root.cameraHealth, the same source as the header-row chips, so the
    // two can never disagree about whether a camera is delivering. This used to
    // derive liveness in QML from frameGens deltas on its own 1 s timer and
    // could only say "the counter is still advancing" — frameGens counts
    // *throttled preview* frames, so a rate taken from it is a plausible-looking
    // wrong number. cameraHealth carries the grabber's real measured rate, which
    // is exactly what this strip wanted and could not have; the timer and the
    // delta arithmetic are gone with it.
    component PreviewHiddenStrip : Item {
        id: stripRoot

        Column {
            anchors.centerIn: parent
            width: parent.width
            spacing: 14

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Previews hidden — cameras are still capturing"
                color: "#55557a"
                font { pixelSize: 12; bold: true; letterSpacing: 1 }
            }

            // One row, always: the cameras are read side by side, and a Flow
            // capped at 720 px wrapped the sixth chip onto a line of its own.
            // When the window is too narrow for the row, it is scaled down to
            // fit rather than wrapped — still one glance across.
            Row {
                id: stripRow
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8
                scale: implicitWidth > parent.width && implicitWidth > 0
                       ? parent.width / implicitWidth : 1.0
                transformOrigin: Item.Top

                Repeater {
                    model: root.liveCameras

                    delegate: Rectangle {
                        id: stripChip

                        // The camera's configured index — the model is
                        // root.liveCameras, so in interview mode the one chip
                        // here is Camera 3's, not position 0's.
                        required property int modelData

                        readonly property var entry:
                            (root.cameraHealth && stripChip.modelData < root.cameraHealth.length)
                                ? root.cameraHealth[stripChip.modelData] : null
                        readonly property int healthState: stripChip.entry ? stripChip.entry.state : 0
                        readonly property real fps:        stripChip.entry ? stripChip.entry.fps : 0
                        // Ok or Lagging both mean frames are arriving, which is
                        // what the pulse is reporting. Stalled and Unknown do not.
                        readonly property bool live: stripChip.healthState === 1 ||
                                                     stripChip.healthState === 2

                        width: chipRow.implicitWidth + 20
                        height: 26
                        radius: 13
                        color: "#131326"
                        // Unknown (state 0) gets its own neutral colour rather
                        // than falling through to the not-delivering one. It
                        // means "no verdict available", not "dead": a rig with
                        // a single open camera has no peers to judge it
                        // against, and painting a perfectly healthy camera red
                        // at the exact moment the operator loses the video is
                        // the worst possible time to be wrong.
                        border.color: stripChip.healthState === 3 ? "#aa3333"
                                    : stripChip.healthState === 2 ? "#aa8833"
                                    : stripChip.healthState === 0 ? "#2a2a4a"
                                    : stripChip.live ? "#2a4a38" : "#33223a"
                        border.width: 1

                        Row {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: 7

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 7; height: 7; radius: 4
                                color: stripChip.healthState === 3 ? "#ff5555"
                                     : stripChip.healthState === 2 ? "#ddaa44"
                                     : stripChip.healthState === 0 ? "#55557a"
                                     : stripChip.live ? "#33cc66" : "#aa4444"

                                // The pulse drives its own property rather than
                                // `opacity` directly: an `Animation on opacity`
                                // replaces the binding, so stopping mid-cycle
                                // would strand a newly-dead dot at whatever
                                // faded value it happened to reach — rendering
                                // the alarm state *weaker* than the healthy one.
                                // No initialiser: an `Animation on <prop>` is a
                                // value source, and combining it with one is a
                                // duplicate-property-binding. The opacity
                                // binding below supplies the not-live value
                                // anyway, so pulseT's resting value is moot.
                                property real pulseT
                                opacity: stripChip.live ? pulseT : 1.0

                                SequentialAnimation on pulseT {
                                    running: stripChip.live
                                    loops:   Animation.Infinite
                                    NumberAnimation { to: 0.4; duration: 700; easing.type: Easing.InOutSine }
                                    NumberAnimation { to: 1.0; duration: 700; easing.type: Easing.InOutSine }
                                }
                            }

                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                // The rate this strip could never show before.
                                // While previews are hidden it is the only
                                // number saying what the cameras are doing.
                                text: "Cam " + (stripChip.modelData + 1) +
                                      (stripChip.healthState === 0
                                           ? ""
                                           : "  —  " + stripChip.fps.toFixed(1) + " fps")
                                color: stripChip.healthState === 3 ? "#ff8888"
                                     : stripChip.healthState === 2 ? "#ffcc66"
                                     : stripChip.healthState === 0 ? "#7070a0"
                                     : stripChip.live ? "#88aaff" : "#886677"
                                font { pixelSize: 11; bold: true }
                            }
                        }
                    }
                }
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: root.cameraCount === 0
                text: "No cameras configured"
                color: "#33334a"
                font { pixelSize: 11 }
            }
        }
    }

    // ── Room / Interview toggle ────────────────────────────────────────────
    //
    // Hand-drawn like every other control in this view (it uses no
    // QtQuick.Controls buttons). Only *asks*: MainWindow reopens the cameras
    // and may refuse, and the highlighted segment follows the backend's
    // interviewMode, never the click — so a refused switch cannot leave it
    // showing a mode that is not in effect. Locked while recording, during a
    // pending start, and while a switch is still reopening the cameras.
    component ModeToggle : Rectangle {
        id: toggleRoot
        readonly property bool locked: root.recording || root.startPending || root.interviewSwitching

        implicitWidth:  segRow.implicitWidth + 6
        implicitHeight: 22
        radius: 11
        color: "#12122a"
        border { color: "#2a2a52"; width: 1 }
        opacity: locked ? 0.55 : 1.0

        HoverHandler { id: toggleHover }
        ToolTip.visible: toggleHover.hovered
        ToolTip.delay: 400
        ToolTip.text: toggleRoot.locked
            ? (root.interviewSwitching ? "Reopening the cameras…"
                                       : "Locked while recording — switching reopens the cameras")
            : "Room: every camera.  Interview: Cam " + (root.interviewCameraIndex + 1)
              + " only, cropped, at up to " + root.interviewFps.toFixed(0)
              + " fps (set up in Settings → Video)."

        Row {
            id: segRow
            anchors.centerIn: parent
            spacing: 2

            Repeater {
                model: [
                    { label: "Room",                                              interview: false },
                    { label: "Interview · Cam " + (root.interviewCameraIndex + 1), interview: true  }
                ]

                delegate: Rectangle {
                    readonly property bool active: modelData.interview === root.interviewMode

                    width:  segLabel.implicitWidth + 18
                    height: 18
                    radius: 9
                    color: active
                        ? (modelData.interview ? "#3a2a10" : "#1e2a50")
                        : (segArea.containsMouse ? "#1c1c3a" : "transparent")
                    border {
                        color: active ? (modelData.interview ? "#aa7a22" : "#3a4a8a") : "transparent"
                        width: 1
                    }

                    Label {
                        id: segLabel
                        anchors.centerIn: parent
                        text:  modelData.label
                        color: parent.active ? (modelData.interview ? "#ffcc66" : "#aabbff") : "#6a6a90"
                        font { pixelSize: 10; bold: parent.active }
                    }

                    MouseArea {
                        id: segArea
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: !toggleRoot.locked && !parent.active
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: if (typeof backend !== "undefined")
                                       backend.requestInterviewMode(modelData.interview)
                    }
                }
            }
        }
    }

    // ── Recording bar ──────────────────────────────────────────────────────

    // ── Session identity bar ───────────────────────────────────────────────
    //
    // The first text inputs in this view — everything else here is a
    // hand-drawn Rectangle — so the styling is explicit rather than inherited
    // from the Controls theme, which would look nothing like the rest.
    component SessionIdentityBar : Rectangle {
        color: "#0d0d20"
        border { color: "#1e1e40"; width: 1 }
        radius: 5
        implicitHeight: idCol.implicitHeight + 16

        ColumnLayout {
            id: idCol
            anchors.fill: parent
            anchors.margins: 8
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                // Inline rather than on its own row: it labels the fields just
                // as well from here and gives a whole line back to the video.
                Label {
                    text: "SESSION"
                    color: "#55557a"
                    font { pixelSize: 10; bold: true; letterSpacing: 2 }
                }

                IdentityField {
                    id: subjField
                    placeholder: "subject"
                    value: root.subjectLabel
                    Layout.preferredWidth: 110
                    onEdited: (t) => { if (typeof backend !== "undefined") backend.subjectLabel = t }
                }
                IdentityField {
                    placeholder: "session"
                    value: root.sessionLabel
                    Layout.preferredWidth: 90
                    onEdited: (t) => { if (typeof backend !== "undefined") backend.sessionLabel = t }
                }
                IdentityField {
                    placeholder: "task"
                    value: root.taskLabel
                    Layout.fillWidth: true
                    onEdited: (t) => { if (typeof backend !== "undefined") backend.taskLabel = t }
                }

                // Clearing between participants should be one click, not three
                // select-alls.
                Rectangle {
                    Layout.preferredWidth: 54
                    Layout.preferredHeight: 26
                    radius: 4
                    color: clearArea.containsMouse ? "#26264a" : "#16162e"
                    border { color: "#2a2a52"; width: 1 }
                    Label {
                        anchors.centerIn: parent
                        text: "Clear"
                        color: "#8888aa"
                        font.pixelSize: 11
                    }
                    MouseArea {
                        id: clearArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (typeof backend !== "undefined") backend.clearIdentity()
                    }
                }
            }

            // What will actually be created. The whole point is that the
            // operator never has to guess how their typing becomes a folder —
            // including the run number, which is otherwise only revealed by
            // the duplicate prompt.
            Label {
                Layout.fillWidth: true
                text: "▸  " + root.folderPreview
                color: "#5a5a80"
                font { pixelSize: 10; family: "Courier New, Courier, monospace" }
                elide: Text.ElideMiddle
            }

            // Only appears when the typed text and the resulting label differ,
            // or the name is too long to be safe.
            Label {
                Layout.fillWidth: true
                visible: root.identityWarning !== ""
                text: "⚠  " + root.identityWarning
                color: "#ddaa44"
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }
        }
    }

    // A dark-themed single-line field. `value` is the C++ side's text;
    // `edited` carries user input back. Kept one-way-in/one-way-out rather
    // than a two-way binding so a re-published value can't fight the cursor.
    component IdentityField : Rectangle {
        id: fieldRoot
        property string placeholder: ""
        property string value: ""
        signal edited(string text)

        implicitHeight: 26
        color: "#09091a"
        border { color: input.activeFocus ? "#4a4a90" : "#1e1e40"; width: 1 }
        radius: 4

        // Typing into a TextField breaks the declarative binding on `text`,
        // so after the first keystroke the field would stop following the C++
        // side — and Clear would visibly do nothing. Re-assign explicitly
        // instead, guarded so it can't fight the cursor mid-edit.
        onValueChanged: if (input.text !== value) input.text = value

        TextField {
            id: input
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            verticalAlignment: TextInput.AlignVCenter
            text: fieldRoot.value
            placeholderText: fieldRoot.placeholder
            color: "#c8c8e0"
            placeholderTextColor: "#3a3a55"
            font.pixelSize: 12
            selectByMouse: true
            background: null
            // onTextEdited, not onTextChanged: the latter also fires when the
            // binding above rewrites the text, which would loop.
            onTextEdited: fieldRoot.edited(text)
        }
    }

    // ── Operator notes ─────────────────────────────────────────────────────
    component SessionNotesBox : Rectangle {
        // 32, down from 48. Two lines of the 11px font rather than three —
        // still enough to see what you are typing, and the box scrolls, so
        // nothing is lost but empty space above the previews.
        implicitHeight: 32
        color: "#09091a"
        border { color: notesInput.activeFocus ? "#4a4a90" : "#1e1e40"; width: 1 }
        radius: 4

        ScrollView {
            anchors.fill: parent
            anchors.margins: 5
            clip: true

            // Same binding-break problem as IdentityField above.
            Connections {
                target: root
                function onSessionNotesChanged() {
                    if (notesInput.text !== root.sessionNotes)
                        notesInput.text = root.sessionNotes
                }
            }

            TextArea {
                id: notesInput
                text: root.sessionNotes
                // Deliberately does not promise post-hoc editing: this box
                // clears when a recording ends, because from that moment it
                // belongs to the next session. Editing the finished one
                // happens in the Session Health dialog or the Session Browser.
                placeholderText: root.recording
                    ? "Note what's happening — saved with this recording"
                    : "Notes for the next recording (optional)"
                color: "#c8c8e0"
                placeholderTextColor: "#3a3a55"
                font.pixelSize: 12
                selectByMouse: true
                wrapMode: TextArea.Wrap
                background: null
                onTextChanged: {
                    // TextArea has no onTextEdited, so guard the binding
                    // write-back explicitly instead.
                    if (typeof backend !== "undefined" && text !== backend.notes)
                        backend.notes = text
                }
            }
        }
    }

    component RecordingBar : Rectangle {
        id: bar
        property bool recording: false
        property int  elapsedMs: 0
        // Seconds left before recording starts; 0 when idle. Mutually
        // exclusive with `recording` — the bridge clears it before start().
        property int  countdown: 0

        readonly property bool pending: countdown > 0

        signal startRequested()
        signal stopRequested()

        // implicitHeight, not height: a ColumnLayout takes its preferred-height
        // hint from implicitHeight or Layout.preferredHeight and ignores a
        // literal height binding, so this bar was advertising no preferred
        // height at all and was first in line to be squeezed — unlike its two
        // siblings above, which both set implicitHeight.
        implicitHeight: 52
        radius:         7
        color:        recording ? "#180a0a" : (pending ? "#1a1408" : "#09091a")
        border.color: recording ? "#772222" : (pending ? "#7a5c22" : "#1e1e40")
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
                text:  recording ? "REC" : (bar.pending ? "STARTING" : "STANDBY")
                color: recording ? "#ff6666" : (bar.pending ? "#ddaa44" : "#33334a")
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
                // Three states, deliberately distinct: red while recording
                // (pulsing), amber while a start countdown is pending, green
                // when idle. The pulse stays parked during the countdown —
                // `recording` is still false then — so "armed" never gets
                // mistaken for "already recording".
                color: recording
                    ? (recArea.containsMouse
                        ? "#3a1010"
                        : Qt.rgba(0.14 + 0.20 * pulse, 0.03, 0.03, 1.0))
                    : bar.pending
                        ? (recArea.containsMouse ? "#3a2c10" : "#2a2010")
                        : (recArea.containsMouse ? "#0f2a18" : "#0b1e12")
                border.color: recording
                    ? Qt.rgba(0.73, 0.13 + 0.22 * pulse, 0.13 + 0.22 * pulse, 1.0)
                    : (bar.pending ? "#ddaa44" : "#22bb55")
                border.width: recording ? 2 : 1

                Label {
                    anchors.centerIn: parent
                    text:  recording ? "■  Stop"
                                     : (bar.pending ? "✕  Cancel " + bar.countdown : "●  Record")
                    color: recording
                        ? Qt.rgba(1.0, 0.40 + 0.25 * recBtn.pulse, 0.40 + 0.25 * recBtn.pulse, 1.0)
                        : (bar.pending ? "#ffcc66" : "#33ee77")
                    font { pixelSize: 11; bold: true }
                }

                MouseArea {
                    id: recArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape:  Qt.PointingHandCursor
                    // stopRequested() doubles as "cancel the pending start" —
                    // see MonitorBridge::stopRecording(). Record is never
                    // disabled: clicking it without a subject opens the dialog
                    // that asks for one, which is more use than a dead button
                    // beside fields the operator has not found.
                    onClicked:    (recording || bar.pending) ? bar.stopRequested()
                                                             : bar.startRequested()
                }
            }
        }
    }
}
