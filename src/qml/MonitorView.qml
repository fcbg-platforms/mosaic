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
    // Seconds left before recording starts; 0 when no countdown is pending.
    readonly property int    countdown:   typeof backend !== "undefined" ? backend.countdownSeconds : 0
    // True from the Record click until recording is actually live (or the
    // attempt is cancelled/fails) — spans the gap after the countdown hits 0
    // while RecordManager::start() runs. See MonitorBridge's own doc comment.
    readonly property bool   startPending: typeof backend !== "undefined" ? backend.startPending : false
    // RecordSettings::hidePreviewsWhileRecording, mirrored by MonitorBridge.
    readonly property bool   hidePreviews: typeof backend !== "undefined" ? backend.hidePreviews : false

    // ── Session identity ───────────────────────────────────────────────────
    // Who and what the next recording is of. All optional — left blank, the
    // session keeps the timestamp-only folder name, so an operator is never
    // blocked from pressing Record.
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
            visible:     !root.previewsHidden
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
                    // Hiding the grid alone would still let the source binding
                    // re-evaluate on every frameGen tick and hit
                    // VideoFeedProvider::requestImage; this makes "hidden"
                    // actually mean idle on the QML side.
                    previewActive: !root.previewsHidden
                    // Each slot tracks only its own camera's generation counter,
                    // so unrelated camera updates don't trigger a reload here.
                    frameGen:    (root.frameGens && index < root.frameGens.length)
                                 ? root.frameGens[index] : 0
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
            Layout.fillWidth: true
            visible: root.identityEditable
        }

        // ── Operator notes ─────────────────────────────────────────────────
        // Deliberately *not* hidden while recording: the note worth having is
        // usually the one written once something has actually happened.
        SessionNotesBox {
            Layout.fillWidth: true
        }

        // ── Recording controls ─────────────────────────────────────────────
        RecordingBar {
            Layout.fillWidth: true
            recording:  root.recording
            elapsedMs:  root.elapsedMs
            countdown:  root.countdown

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

    // Shown in the camera grid's place while previews are hidden. Deliberately
    // reports only what can be stated honestly with the data QML already has:
    // "camera N's frame counter is still advancing". Real capture fps is not
    // shown — frameGens counts *throttled preview* frames, so deriving fps
    // here would produce a plausible-looking wrong number; the Perf tab
    // remains the place for the true per-camera rate.
    component PreviewHiddenStrip : Item {
        id: stripRoot

        // One entry per camera: true while its frame counter advanced during
        // the last tick. Rebuilt wholesale by the timer below.
        property var liveFlags: []
        property var lastGens:  []

        Timer {
            interval: 1000
            repeat:   true
            running:  stripRoot.visible
            // Prime the baseline on show, and seed every camera as live for
            // the first tick. Optimistic on purpose: the previews were
            // rendering a moment ago, so "delivering" is the honest prior —
            // whereas defaulting to dead would flash every camera red for a
            // full second at exactly the moment the operator loses the video
            // and is watching this strip hardest. One stale-but-recently-true
            // second beats a false alarm.
            onRunningChanged: if (running) {
                stripRoot.lastGens = (root.frameGens || []).slice()
                let seed = []
                for (let i = 0; i < root.cameraCount; ++i) seed.push(true)
                stripRoot.liveFlags = seed
            }
            onTriggered: {
                const now   = (root.frameGens || []).slice()
                const prev  = stripRoot.lastGens || []
                let flags = []
                for (let i = 0; i < root.cameraCount; ++i) {
                    const a = i < now.length  ? now[i]  : 0
                    const b = i < prev.length ? prev[i] : 0
                    flags.push(a > b)
                }
                stripRoot.liveFlags = flags
                stripRoot.lastGens  = now
            }
        }

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

            Flow {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(parent.width, 720)
                spacing: 8

                Repeater {
                    model: Math.max(0, root.cameraCount)

                    delegate: Rectangle {
                        readonly property bool live:
                            (stripRoot.liveFlags && index < stripRoot.liveFlags.length)
                            ? stripRoot.liveFlags[index] : false

                        width: chipRow.implicitWidth + 20
                        height: 26
                        radius: 13
                        color: "#131326"
                        border.color: live ? "#2a4a38" : "#33223a"
                        border.width: 1

                        Row {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: 7

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 7; height: 7; radius: 4
                                color: live ? "#33cc66" : "#aa4444"

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
                                opacity: live ? pulseT : 1.0

                                SequentialAnimation on pulseT {
                                    running: live
                                    loops:   Animation.Infinite
                                    NumberAnimation { to: 0.4; duration: 700; easing.type: Easing.InOutSine }
                                    NumberAnimation { to: 1.0; duration: 700; easing.type: Easing.InOutSine }
                                }
                            }

                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                text:  "Cam " + (index + 1)
                                color: live ? "#88aaff" : "#886677"
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
        implicitHeight: 48
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

        height:       52
        radius:       7
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
                    // see MonitorBridge::stopRecording().
                    onClicked:    (recording || bar.pending) ? bar.stopRequested()
                                                             : bar.startRequested()
                }
            }
        }
    }
}
