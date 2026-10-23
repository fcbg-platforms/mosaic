// The 3D Pose room view (Skeleton3DRoomViewW): every person, the cameras and
// the floor, in Qt Quick 3D. `room3d` (Room3DScene) gives what to draw, in
// a Y-up scene in millimetres with the floor at y = 0; this page lays it
// out, lights it, and handles the views, the People panel and hovering.
import QtQuick
import QtQuick3D
import Mosaic.Room3D

Rectangle {
    id: root
    // A soft gradient behind the scene: lighter at the horizon.
    gradient: Gradient {
        GradientStop { position: 0.0; color: "#07080f" }
        GradientStop { position: 0.55; color: "#141a2e" }
        GradientStop { position: 1.0; color: "#0b0d16" }
    }

    // ── Views ──────────────────────────────────────────────────────────────
    // 3D: an orbit around `target`. Top, Front, Side: flat (orthographic).
    property int preset: 0 // 0 3D, 1 Top, 2 Front, 3 Side
    property real yaw: -35
    property real pitch: 28
    property real distance: 6000
    property real orthoHalf: 2500
    property vector3d target: Qt.vector3d(0, 1000, 0)
    readonly property bool flat: preset !== 0

    function reset() {
        target = room3d.home
        distance = Math.max(room3d.extent * 2.4, 1500)
        orthoHalf = Math.max(room3d.extent * 1.15, 800)
        if (preset === 0) { yaw = -35; pitch = 28 }
        else if (preset === 1) { yaw = 0; pitch = 89.9 }
        else if (preset === 2) { yaw = 0; pitch = 0 }
        else { yaw = 90; pitch = 0 }
    }
    Connections {
        target: room3d
        function onResultChanged() { root.reset() }
        function onFrameChanged() {
            // Only when the followed person is in this frame: otherwise the
            // view stays where it is.
            if (room3d.follow >= 0 && room3d.followFound)
                root.target = room3d.followTarget
            hover.info = "" // the joint under the mouse has moved
        }
    }
    Component.onCompleted: reset()

    // Where the eye is, from yaw (around up), pitch (above the floor) and
    // distance; the view looks at `target`.
    readonly property vector3d eyeDir: {
        const y = yaw * Math.PI / 180, p = pitch * Math.PI / 180
        return Qt.vector3d(Math.sin(y) * Math.cos(p), Math.sin(p), Math.cos(y) * Math.cos(p))
    }

    View3D {
        id: view
        anchors.fill: parent
        camera: root.flat ? ortho : persp
        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Transparent
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.VeryHigh
            aoEnabled: true
            aoStrength: 60
            aoDistance: 120
            aoSoftness: 40
            tonemapMode: SceneEnvironment.TonemapModeFilmic
        }

        PerspectiveCamera {
            id: persp
            fieldOfView: 45
            clipNear: 20
            clipFar: 200000
            position: root.target.plus(root.eyeDir.times(root.distance))
            Component.onCompleted: lookAt(root.target)
            onPositionChanged: lookAt(root.target)
        }
        OrthographicCamera {
            id: ortho
            clipNear: -100000
            clipFar: 100000
            // Pixels per millimetre, the same both ways.
            horizontalMagnification: view.height / (2 * root.orthoHalf)
            verticalMagnification: view.height / (2 * root.orthoHalf)
            position: root.target.plus(root.eyeDir.times(20000))
            Component.onCompleted: lookAt(root.target)
            onPositionChanged: lookAt(root.target)
        }

        // Light: a key light from above that casts the shadows, a soft fill
        // from the other side, and a little ambient.
        DirectionalLight {
            eulerRotation: Qt.vector3d(-65, -30, 0)
            brightness: 1.1
            castsShadow: true
            shadowMapQuality: Light.ShadowMapQualityVeryHigh
            shadowFactor: 55
            shadowBias: 8
            pcfFactor: 6
            softShadowQuality: Light.PCF16
            shadowMapFar: 30000
            csmNumSplits: 2
        }
        DirectionalLight {
            eulerRotation: Qt.vector3d(-25, 150, 0)
            brightness: 0.35
            ambientColor: Qt.rgba(0.16, 0.17, 0.22, 1)
        }

        // ── The floor: a plate, its grid every 0.5 m (brighter each metre) ──
        Model {
            source: "#Rectangle"
            position: Qt.vector3d(room3d.floorCentre.x, -1, room3d.floorCentre.z)
            eulerRotation.x: -90
            scale: Qt.vector3d(room3d.floorSize / 100, room3d.floorSize / 100, 1)
            receivesShadows: true
            materials: PrincipledMaterial {
                baseColor: "#1b1f33"
                roughness: 0.85
                metalness: 0
            }
        }
        Model {
            geometry: SceneMesh { scene: room3d; part: "grid" }
            castsShadows: false
            materials: DefaultMaterial {
                vertexColorsEnabled: true
                lighting: DefaultMaterial.NoLighting
            }
        }

        // ── Cameras: a body with a lens, and what each sees ──
        Repeater3D {
            model: room3d.cameras
            delegate: Node {
                required property var modelData
                position: modelData.pos
                rotation: modelData.hasRotation ? modelData.rot : Qt.quaternion(1, 0, 0, 0)
                // No shadows: hung near the ceiling, theirs fall far away on
                // the floor and read as objects.
                Model {
                    source: "#Cube"
                    castsShadows: false
                    scale: Qt.vector3d(1.1, 0.75, 1.4) // 110 x 75 x 140 mm
                    materials: PrincipledMaterial { baseColor: "#2a2d3a"; roughness: 0.4; metalness: 0.6 }
                }
                Model {
                    visible: modelData.hasRotation
                    source: "#Cylinder"
                    castsShadows: false
                    position: Qt.vector3d(0, 0, -95)
                    eulerRotation.x: 90
                    scale: Qt.vector3d(0.6, 0.5, 0.6)
                    materials: PrincipledMaterial { baseColor: modelData.color; roughness: 0.3; metalness: 0.2 }
                }
            }
        }
        Model {
            geometry: SceneMesh { scene: room3d; part: "frusta" }
            castsShadows: false
            materials: DefaultMaterial { vertexColorsEnabled: true; lighting: DefaultMaterial.NoLighting }
        }
        Model {
            geometry: SceneMesh { scene: room3d; part: "frustumFaces" }
            castsShadows: false
            materials: DefaultMaterial {
                vertexColorsEnabled: true
                lighting: DefaultMaterial.NoLighting
                cullMode: Material.NoCulling
                opacity: 0.99 // drawn with the transparent pass, so vertex alpha counts
            }
        }

        // ── People: limbs, joints, a head with the way it faces, the torso ──
        Repeater3D {
            model: room3d.bones.length
            delegate: Model {
                required property int index
                readonly property var modelData: room3d.bones[index] || ({})
                source: "#Cylinder"
                position: modelData.pos
                rotation: modelData.rot
                scale: Qt.vector3d(0.5, modelData.len / 100, 0.5) // 25 mm radius
                castsShadows: true
                materials: PrincipledMaterial {
                    baseColor: Qt.darker(modelData.color, 1.15)
                    roughness: 0.45
                }
            }
        }
        Repeater3D {
            model: room3d.joints.length
            delegate: Model {
                required property int index
                readonly property var modelData: room3d.joints[index] || ({})
                property string info: modelData.info
                source: "#Sphere"
                position: modelData.pos
                scale: Qt.vector3d(0.7, 0.7, 0.7) // 35 mm radius
                pickable: true
                castsShadows: true
                materials: PrincipledMaterial {
                    baseColor: Qt.lighter(modelData.color, 1.2)
                    roughness: 0.35
                }
            }
        }
        Repeater3D {
            model: room3d.heads.length
            delegate: Node {
                required property int index
                readonly property var modelData: room3d.heads[index] || ({})
                Model {
                    source: "#Sphere"
                    position: modelData.pos
                    scale: Qt.vector3d(1.9, 1.9, 1.9) // 95 mm radius
                    castsShadows: true
                    materials: PrincipledMaterial { baseColor: modelData.color; roughness: 0.4 }
                }
                // Which way the face points, flat on the floor.
                Model {
                    visible: modelData.hasFacing
                    source: "#Cone"
                    position: modelData.hasFacing ? modelData.facingPos : Qt.vector3d(0, 0, 0)
                    rotation: modelData.hasFacing ? modelData.facingRot : Qt.quaternion(1, 0, 0, 0)
                    scale: Qt.vector3d(0.6, 1.2, 0.6)
                    materials: PrincipledMaterial { baseColor: Qt.lighter(modelData.color, 1.35); roughness: 0.3 }
                }
            }
        }
        Model {
            geometry: SceneMesh { scene: room3d; part: "torsos" }
            castsShadows: true
            materials: DefaultMaterial {
                vertexColorsEnabled: true
                cullMode: Material.NoCulling
                opacity: 0.99
            }
        }
        Model {
            geometry: SceneMesh { scene: room3d; part: "trails" }
            castsShadows: false
            materials: DefaultMaterial { vertexColorsEnabled: true; lighting: DefaultMaterial.NoLighting; opacity: 0.99 }
        }
        // The two people of Dyad Analysis, joined at the hips.
        Model {
            visible: room3d.dyad.visible === true
            source: "#Cylinder"
            position: visible ? room3d.dyad.pos : Qt.vector3d(0, 0, 0)
            rotation: visible ? room3d.dyad.rot : Qt.quaternion(1, 0, 0, 0)
            scale: Qt.vector3d(0.12, visible ? room3d.dyad.len / 100 : 1, 0.12)
            materials: PrincipledMaterial { baseColor: "white"; emissiveFactor: Qt.vector3d(0.6, 0.6, 0.6) }
        }
    }

    // ── Labels, drawn over the scene where their 3D points land ──
    // `view.camera` and the eye are read so they move with the view.
    function screen(p) {
        void(root.yaw); void(root.pitch); void(root.distance); void(root.orthoHalf)
        void(root.target); void(root.preset); void(view.width); void(view.height)
        return view.mapFrom3DScene(p)
    }
    component Tag: Rectangle {
        property string text
        property color tint
        property bool big: false
        width: label.implicitWidth + 16
        height: label.implicitHeight + 8
        radius: 5
        color: Qt.rgba(0.04, 0.04, 0.1, 0.85)
        border.color: tint
        border.width: 1.5
        Text {
            id: label
            anchors.centerIn: parent
            text: parent.text
            color: parent.tint
            font.pixelSize: Math.max(12, Math.min(19, root.height / (parent.big ? 34 : 40)))
            font.bold: true
        }
    }
    Repeater {
        model: room3d.cameras
        delegate: Tag {
            required property var modelData
            readonly property vector3d s: root.screen(modelData.pos.plus(Qt.vector3d(0, 140, 0)))
            text: modelData.label
            tint: modelData.color
            x: s.x - width / 2
            y: s.y - height
            visible: s.z > 0
        }
    }
    Repeater {
        model: room3d.tags.length
        delegate: Tag {
            required property int index
            readonly property var modelData: room3d.tags[index] || ({ pos: Qt.vector3d(0, 0, 0) })
            readonly property vector3d s: root.screen(modelData.pos)
            text: modelData.text
            tint: modelData.color
            big: true
            x: s.x - width / 2
            y: s.y - height
            visible: s.z > 0
        }
    }
    Tag {
        readonly property vector3d s: room3d.dyad.visible === true ? root.screen(room3d.dyad.pos) : Qt.vector3d(0, 0, -1)
        visible: s.z > 0
        text: room3d.dyad.label || ""
        tint: "white"
        x: s.x - width / 2
        y: s.y - height / 2
    }

    // The people's 3D objects are made per index (model: count), so a new
    // frame only moves them; a list model would remake them all each tick.

    // ── Mouse: drag to turn (3D) or pan (flat), right-drag pans, wheel zooms,
    //    double-click resets; hover a joint for its coordinates ──
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        hoverEnabled: true
        property point last
        property int dragButton: 0
        onPressed: (m) => { last = Qt.point(m.x, m.y); dragButton = m.button; hover.info = "" }
        onReleased: dragButton = 0
        onDoubleClicked: root.reset()
        onPositionChanged: (m) => {
            if (dragButton === 0) {
                const hit = view.pick(m.x, m.y)
                hover.info = hit.objectHit && hit.objectHit.info !== undefined ? hit.objectHit.info : ""
                hover.x = m.x + 14; hover.y = m.y + 12
                return
            }
            const dx = m.x - last.x, dy = m.y - last.y
            last = Qt.point(m.x, m.y)
            if (dragButton === Qt.LeftButton && !root.flat) {
                root.yaw -= dx * 0.4
                root.pitch = Math.max(-10, Math.min(89, root.pitch + dy * 0.4))
                return
            }
            // Pan in the plane facing the viewer.
            const perPx = root.flat ? 2 * root.orthoHalf / root.height
                                    : 2 * root.distance * Math.tan(22.5 * Math.PI / 180) / root.height
            const cam = view.camera
            root.target = root.target.plus(cam.right.times(-dx * perPx)).plus(cam.up.times(dy * perPx))
        }
        onWheel: (w) => {
            const f = Math.pow(0.999, w.angleDelta.y)
            if (root.flat) root.orthoHalf = Math.max(150, Math.min(50000, root.orthoHalf * f))
            else root.distance = Math.max(300, Math.min(100000, root.distance * f))
        }
        onExited: hover.info = ""
    }
    Rectangle {
        id: hover
        property string info: ""
        visible: info !== ""
        width: hoverText.implicitWidth + 14
        height: hoverText.implicitHeight + 10
        radius: 5
        color: Qt.rgba(0.05, 0.05, 0.12, 0.92)
        border.color: "#7888e6"
        Text { id: hoverText; anchors.centerIn: parent; text: hover.info; color: "#ececfa"; font.pixelSize: 13 }
    }

    // ── View buttons, top right ──
    component Pill: Rectangle {
        property string text
        property bool on: false
        signal clicked
        width: t.implicitWidth + 18
        height: 26
        radius: 5
        color: on ? "#3a5fc8" : Qt.rgba(0.08, 0.08, 0.17, 0.85)
        border.color: on ? "#5a7fe8" : "#34345a"
        Text { id: t; anchors.centerIn: parent; text: parent.text; color: parent.on ? "white" : "#c0c0e0"; font.pixelSize: 12 }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: parent.clicked() }
    }
    Row {
        id: views
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        spacing: 3
        Repeater {
            model: ["3D", "Top", "Front", "Side"]
            delegate: Pill {
                required property int index
                required property string modelData
                text: modelData
                on: root.preset === index
                onClicked: { root.preset = index; root.reset() }
            }
        }
        Item { width: 8; height: 1 }
        Pill { text: "Trails"; on: room3d.trails; onClicked: room3d.trails = !room3d.trails }
    }

    // ── People, top left: show or hide each, follow one ──
    Rectangle {
        id: panel
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 8
        anchors.topMargin: panelRow.width + 16 > root.width - views.width - 16 ? views.height + 16 : 8
        width: column.implicitWidth + 16
        height: Math.min(column.implicitHeight + 12, root.height / 2)
        radius: 6
        color: Qt.rgba(0.05, 0.05, 0.12, 0.85)
        border.color: "#34345a"
        visible: room3d.hasResult
        clip: true
        Item { id: panelRow; width: column.implicitWidth }
        Flickable {
            anchors.fill: parent
            anchors.margins: 6
            contentHeight: column.implicitHeight
            Column {
                id: column
                spacing: 3
                Text { text: "People"; color: "#d0d0ec"; font.bold: true; font.pixelSize: 12 }
                Repeater {
                    model: room3d.people
                    delegate: Row {
                        required property var modelData
                        spacing: 6
                        Rectangle {
                            width: 14; height: 14; radius: 3
                            color: modelData.shown ? modelData.color : "transparent"
                            border.color: modelData.color
                            anchors.verticalCenter: parent.verticalCenter
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: room3d.set_shown(modelData.id, !modelData.shown)
                            }
                        }
                        Text {
                            text: modelData.label + "  ·  " + modelData.seconds.toFixed(1) + " s"
                            color: modelData.color
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Pill {
                            text: room3d.follow === modelData.id ? "Following" : "Follow"
                            on: room3d.follow === modelData.id
                            height: 20
                            onClicked: room3d.follow = (room3d.follow === modelData.id ? -1 : modelData.id)
                        }
                    }
                }
                Text {
                    visible: room3d.people.length === 0
                    text: "Nobody in 3D"
                    color: "#9090b0"
                    font.pixelSize: 12
                }
            }
        }
    }

    // ── Bottom right: the room's axes as the view turns them, so the x, y
    //    and z of the hover readout can be found ──
    Item {
        id: axesMarker
        width: 70; height: 70
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 6
        Repeater {
            model: room3d.axes
            delegate: Item {
                required property var modelData
                anchors.fill: parent
                // The axis on screen: where a point 0.3 m along it lands,
                // seen from the target.
                readonly property vector3d a: root.screen(root.target)
                readonly property vector3d b: root.screen(root.target.plus(modelData.dir.times(300)))
                readonly property real dx: b.x - a.x
                readonly property real dy: b.y - a.y
                readonly property real n: Math.max(1, Math.sqrt(dx * dx + dy * dy))
                Rectangle {
                    x: 35; y: 35
                    width: 26 * Math.min(1, n / 40) + 1; height: 2
                    color: modelData.color
                    transformOrigin: Item.Left
                    rotation: Math.atan2(dy, dx) * 180 / Math.PI
                }
                Text {
                    x: 35 + (dx / n) * 30 - 4
                    y: 35 + (dy / n) * 30 - 8
                    text: modelData.name
                    color: modelData.color
                    font.pixelSize: 12
                    font.bold: true
                }
            }
        }
    }

    // ── Bottom: what is on show ──
    Text {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 8
        color: "#8080a8"
        font.pixelSize: 12
        text: room3d.hasResult
              ? room3d.info + "  ·  grid 0.5 m  ·  x y z: " + room3d.frameCamera + "'s frame, metres"
              : "No 3D pose result loaded. Run 3D Pose Reconstruction for this session."
    }
}
