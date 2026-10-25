// The room of the 3D pages (the 3D Pose room view, the 3D Gaze view), in Qt
// Quick 3D: the floor and its grid, the cameras with what each one sees,
// the lights, the views (3D, Top, Front, Side, and from any camera), the
// mouse, Save image and the room's axes. `stage` (Room3DStage) says where
// everything is, in a Y-up scene in millimetres with the floor at y = 0.
// A page puts its 3D contents in `scene`, its own buttons in `buttons`, and
// its 2D overlays as children, placed with screen().
import QtQuick
import QtQuick3D
import Mosaic.Room3D

Rectangle {
    id: root
    property var stage
    property alias scene: content.data
    property alias buttons: extraButtons.data
    property string info // the bottom line: what is on show
    property string emptyText // shown instead, when there is no room
    readonly property real buttonsWidth: views.width
    readonly property real buttonsHeight: views.height

    // A soft gradient behind the scene: lighter at the horizon.
    gradient: Gradient {
        GradientStop { position: 0.0; color: "#07080f" }
        GradientStop { position: 0.55; color: "#141a2e" }
        GradientStop { position: 1.0; color: "#0b0d16" }
    }

    // ── Views ──────────────────────────────────────────────────────────────
    // 3D: an orbit around `target`. Top, Front, Side: flat (orthographic).
    // From a camera: where that camera is, looking where it looks, with its
    // vertical field of view (click its label; any view button leaves it).
    property int preset: 0 // 0 3D, 1 Top, 2 Front, 3 Side
    property int fromCamera: -1 // the index in stage.cameras, -1 for none
    property real cameraSince: 0 // when it was chosen, so a double-click does not leave it
    readonly property var viewCam: fromCamera >= 0 && stage ? stage.cameras[fromCamera] : null
    property real yaw: -35
    property real pitch: 28
    property real distance: 6000
    property real orthoHalf: 2500
    property vector3d target: Qt.vector3d(0, 1000, 0)
    readonly property bool flat: preset !== 0

    function reset() {
        fromCamera = -1
        if (!stage)
            return
        target = stage.home
        distance = Math.max(stage.extent * 2.4, 1500)
        orthoHalf = Math.max(stage.extent * 1.15, 800)
        if (preset === 0) { yaw = -35; pitch = 28 }
        else if (preset === 1) { yaw = 0; pitch = 89.9 }
        else if (preset === 2) { yaw = 0; pitch = 0 }
        else { yaw = 90; pitch = 0 }
    }
    // The page's contents moved: what the mouse was over has too.
    function clearHover() { hover.info = "" }
    Connections {
        target: root.stage
        function onRoomChanged() { root.reset() }
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
        camera: root.viewCam ? camView : (root.flat ? ortho : persp)
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
        // The view from a camera: the cameras' nodes already look down -z
        // with image-up +y, as a scene camera does. No lens distortion, and
        // the view's shape, not the camera's, sets how much shows sideways.
        PerspectiveCamera {
            id: camView
            fieldOfView: root.viewCam ? root.viewCam.fovY : 45
            clipNear: 20
            clipFar: 200000
            position: root.viewCam ? root.viewCam.pos : Qt.vector3d(0, 0, 0)
            rotation: root.viewCam ? root.viewCam.rot : Qt.quaternion(1, 0, 0, 0)
        }
        OrthographicCamera {
            id: ortho
            clipNear: -100000
            clipFar: 100000
            // Pixels per millimetre, the same both ways; never 0, which the
            // camera refuses, while the page has no size yet.
            horizontalMagnification: Math.max(1, view.height) / (2 * root.orthoHalf)
            verticalMagnification: Math.max(1, view.height) / (2 * root.orthoHalf)
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
            position: root.stage ? Qt.vector3d(root.stage.floorCentre.x, -1, root.stage.floorCentre.z)
                                 : Qt.vector3d(0, -1, 0)
            eulerRotation.x: -90
            scale: root.stage ? Qt.vector3d(root.stage.floorSize / 100, root.stage.floorSize / 100, 1)
                              : Qt.vector3d(1, 1, 1)
            receivesShadows: true
            materials: PrincipledMaterial {
                baseColor: "#1b1f33"
                roughness: 0.85
                metalness: 0
            }
        }
        Model {
            geometry: SceneMesh { scene: root.stage; part: "grid" }
            castsShadows: false
            materials: DefaultMaterial {
                vertexColorsEnabled: true
                lighting: DefaultMaterial.NoLighting
            }
        }

        // ── Cameras: a body with a lens, and what each sees ──
        Repeater3D {
            model: root.stage ? root.stage.cameras : []
            delegate: Node {
                required property var modelData
                required property int index
                visible: index !== root.fromCamera // not in the way of its own view
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
            visible: !root.viewCam // the camera's own pyramid would tint its view
            geometry: SceneMesh { scene: root.stage; part: "frusta" }
            castsShadows: false
            materials: DefaultMaterial { vertexColorsEnabled: true; lighting: DefaultMaterial.NoLighting }
        }
        Model {
            visible: !root.viewCam
            geometry: SceneMesh { scene: root.stage; part: "frustumFaces" }
            castsShadows: false
            materials: DefaultMaterial {
                vertexColorsEnabled: true
                lighting: DefaultMaterial.NoLighting
                cullMode: Material.NoCulling
                opacity: 0.99 // drawn with the transparent pass, so vertex alpha counts
            }
        }

        // The page's own contents.
        Node { id: content }
    }

    // ── Labels, drawn over the scene where their 3D points land ──
    // `view.camera` and the eye are read so they move with the view.
    function screen(p) {
        void(root.yaw); void(root.pitch); void(root.distance); void(root.orthoHalf)
        void(root.target); void(root.preset); void(root.fromCamera); void(view.width); void(view.height)
        void(view.camera); void(camView.position); void(camView.rotation); void(camView.fieldOfView)
        return view.mapFrom3DScene(p)
    }
    Repeater {
        model: root.stage ? root.stage.cameras : []
        delegate: RoomTag {
            required property var modelData
            required property int index
            readonly property vector3d s: root.screen(modelData.pos.plus(Qt.vector3d(0, 140, 0)))
            text: modelData.label
            tint: modelData.color
            pageHeight: root.height
            x: s.x - width / 2
            y: s.y - height
            visible: s.z > 0 && index !== root.fromCamera
            z: 2 // above the view's mouse area, which is declared later
            // Click a camera's label for the view from it.
            MouseArea {
                anchors.fill: parent
                enabled: modelData.hasRotation && modelData.fovY > 0
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: { root.fromCamera = index; root.cameraSince = Date.now() }
            }
        }
    }

    // ── Mouse: drag to turn (3D) or pan (flat), right-drag pans, wheel zooms,
    //    double-click resets; hover anything with an `info` to read it ──
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        hoverEnabled: true
        property point last
        property int dragButton: 0
        onPressed: (m) => { last = Qt.point(m.x, m.y); dragButton = m.button; hover.info = "" }
        onReleased: dragButton = 0
        // A double-click on a camera's label is its first click, then this.
        onDoubleClicked: if (Date.now() - root.cameraSince > 600) root.reset()
        onPositionChanged: (m) => {
            if (dragButton === 0) {
                const hit = view.pick(m.x, m.y)
                hover.info = hit.objectHit && hit.objectHit.info !== undefined ? hit.objectHit.info : ""
                hover.x = m.x + 14; hover.y = m.y + 12
                return
            }
            if (root.viewCam) return // the camera's own view stays put
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
            if (root.viewCam) return
            const f = Math.pow(0.999, w.angleDelta.y)
            if (root.flat) root.orthoHalf = Math.max(150, Math.min(50000, root.orthoHalf * f))
            else root.distance = Math.max(300, Math.min(100000, root.distance * f))
        }
        onExited: hover.info = ""
    }
    Rectangle {
        id: hover
        z: 3 // the buttons, the panels and the readout above the camera labels (z 2)
        property string info: ""
        visible: info !== ""
        width: hoverText.implicitWidth + 14
        height: hoverText.implicitHeight + 10
        radius: 5
        color: Qt.rgba(0.05, 0.05, 0.12, 0.92)
        border.color: "#7888e6"
        Text { id: hoverText; anchors.centerIn: parent; text: hover.info; color: "#ececfa"; font.pixelSize: 13 }
    }

    // ── View buttons, top right; the page's own between them and Save ──
    Row {
        id: views
        z: 3
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        spacing: 3
        Repeater {
            model: ["3D", "Top", "Front", "Side"]
            delegate: RoomPill {
                required property int index
                required property string modelData
                text: modelData
                on: root.preset === index && !root.viewCam
                onClicked: { root.preset = index; root.reset() }
            }
        }
        RoomPill {
            visible: !!root.viewCam
            text: root.viewCam ? root.viewCam.label + "'s view" : ""
            on: true
            onClicked: root.reset()
        }
        Item { width: 8; height: 1 }
        Row { id: extraButtons; spacing: 3; visible: children.length > 0 }
        Item { width: 8; height: 1; visible: extraButtons.visible }
        RoomPill { text: "Save image"; onClicked: root.stage.request_save_image() }
    }

    // ── Bottom right: the room's axes as the view turns them, so the x, y
    //    and z of the hover readout can be found ──
    Item {
        id: axesMarker
        z: 1 // above the page's own labels, below the camera labels
        width: 70; height: 70
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 6
        Repeater {
            model: root.stage ? root.stage.axes : []
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
        z: 1
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 8
        color: "#8080a8"
        font.pixelSize: 12
        text: root.stage && root.stage.hasRoom
              ? root.info + "  ·  grid 0.5 m  ·  x y z: " + root.stage.frameCamera + "'s frame, metres"
                + (root.viewCam ? "" : "  ·  click a camera's label to look from it")
              : root.emptyText
    }
}
