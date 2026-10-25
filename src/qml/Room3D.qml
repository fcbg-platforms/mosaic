// The 3D Pose room view (Skeleton3DRoomViewW): every person on the room's
// stage (RoomStage.qml: the floor, the cameras, the views). `room3d`
// (Room3DScene) gives what to draw, in the stage's Y-up scene in
// millimetres; this page lays out the people, their trails and paths, the
// People panel, and follows a person.
import QtQuick
import QtQuick3D
import Mosaic.Room3D

RoomStage {
    id: page
    stage: room3d.stage
    info: room3d.info
    emptyText: "No 3D pose result loaded. Run 3D Pose Reconstruction for this session."

    Connections {
        target: room3d
        function onFrameChanged() {
            // Only when the followed person is in this frame: otherwise the
            // view stays where it is.
            if (room3d.follow >= 0 && room3d.followFound)
                page.target = room3d.followTarget
            page.clearHover() // the joint under the mouse has moved
        }
    }

    buttons: [
        RoomPill { text: "Trails"; on: room3d.trails; onClicked: room3d.trails = !room3d.trails },
        RoomPill { text: "Paths"; on: room3d.paths; onClicked: room3d.paths = !room3d.paths }
    ]

    // ── People: limbs, joints, a head with the way it faces, the torso ──
    scene: [ RoomBodies { bodies: room3d } ]

    // ── Labels: each person's name, the two people's distance ──
    Repeater {
        model: room3d.tags.length
        delegate: RoomTag {
            required property int index
            readonly property var modelData: room3d.tags[index] || ({ pos: Qt.vector3d(0, 0, 0) })
            readonly property vector3d s: page.screen(modelData.pos)
            text: modelData.text
            tint: modelData.color
            big: true
            pageHeight: page.height
            x: s.x - width / 2
            y: s.y - height
            visible: s.z > 0
        }
    }
    RoomTag {
        readonly property vector3d s: room3d.dyad.visible === true ? page.screen(room3d.dyad.pos) : Qt.vector3d(0, 0, -1)
        visible: s.z > 0
        text: room3d.dyad.label || ""
        tint: "white"
        pageHeight: page.height
        x: s.x - width / 2
        y: s.y - height / 2
    }

    // ── People, top left: show or hide each, follow one ──
    Rectangle {
        id: panel
        z: 3
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 8
        anchors.topMargin: panelRow.width + 16 > page.width - page.buttonsWidth - 16 ? page.buttonsHeight + 16 : 8
        width: column.implicitWidth + 16
        height: Math.min(column.implicitHeight + 12, page.height / 2)
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
                                  + (modelData.brief ? "  (brief)" : "")
                            color: modelData.color
                            font.pixelSize: 12
                            font.bold: true
                        }
                        RoomPill {
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
}
