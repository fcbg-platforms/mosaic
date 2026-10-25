// The 3D Gaze view (Gaze3DViewW): every person's head and gaze on the room's
// stage (RoomStage.qml: the floor, the cameras, the views). `gaze3d`
// (Gaze3DScene) gives what to draw, in the stage's Y-up scene in
// millimetres; this page lays out the heads, the gaze rays and points, the
// regions and the plane, the labels, the People panel, and follows a person.
import QtQuick
import QtQuick3D
import Mosaic.Room3D

RoomStage {
    id: page
    stage: gaze3d.stage
    info: gaze3d.info
    emptyText: "No 3D gaze result loaded. Run Multi-Camera Gaze Fusion for this session."

    Connections {
        target: gaze3d
        function onFrameChanged() {
            // Only when the followed person is in this frame: otherwise the
            // view stays where it is.
            if (gaze3d.follow !== "" && gaze3d.followFound)
                page.target = gaze3d.followTarget
            page.clearHover() // the head under the mouse has moved
        }
    }

    buttons: [
        RoomPill { text: "Trails"; on: gaze3d.trails; onClicked: gaze3d.trails = !gaze3d.trails }
    ]

    // Made per index (model: count), so a new tick only moves them.
    scene: [
        // Heads: hover one for who it is and what they look at.
        Repeater3D {
            model: gaze3d.heads.length
            delegate: Model {
                required property int index
                readonly property var modelData: gaze3d.heads[index] || ({})
                property string info: modelData.info
                source: "#Sphere"
                position: modelData.pos
                scale: Qt.vector3d(1.9, 1.9, 1.9) // 95 mm radius
                pickable: true
                castsShadows: true
                materials: PrincipledMaterial { baseColor: modelData.color; roughness: 0.4 }
            }
        },
        // Gaze rays, from the eyes to where the gaze lands; brighter when
        // mutual.
        Repeater3D {
            model: gaze3d.rays.length
            delegate: Model {
                required property int index
                readonly property var modelData: gaze3d.rays[index] || ({})
                source: "#Cylinder"
                position: modelData.pos
                rotation: modelData.rot
                scale: Qt.vector3d(0.24, modelData.len / 100, 0.24) // 12 mm radius
                castsShadows: false
                materials: PrincipledMaterial {
                    baseColor: modelData.color
                    emissiveFactor: modelData.mutual ? Qt.vector3d(0.9, 0.9, 0.9) : Qt.vector3d(0.25, 0.25, 0.25)
                    roughness: 0.3
                }
            }
        },
        // Where each gaze lands.
        Repeater3D {
            model: gaze3d.points.length
            delegate: Model {
                required property int index
                readonly property var modelData: gaze3d.points[index] || ({})
                source: "#Sphere"
                position: modelData.pos
                scale: Qt.vector3d(0.8, 0.8, 0.8) // 40 mm radius
                castsShadows: false
                materials: PrincipledMaterial {
                    baseColor: Qt.lighter(modelData.color, 1.3)
                    emissiveFactor: Qt.vector3d(0.7, 0.7, 0.7)
                }
            }
        },
        // The named regions, lit in the looker's colour while looked at.
        Repeater3D {
            model: gaze3d.regions.length
            delegate: Model {
                required property int index
                readonly property var modelData: gaze3d.regions[index] || ({})
                source: "#Rectangle"
                position: modelData.pos
                rotation: modelData.rot
                scale: Qt.vector3d(modelData.width / 100, modelData.height / 100, 1)
                castsShadows: false
                materials: PrincipledMaterial {
                    baseColor: modelData.color
                    opacity: modelData.lit ? 0.7 : 0.3
                    emissiveFactor: modelData.lit ? Qt.vector3d(0.5, 0.5, 0.5) : Qt.vector3d(0, 0, 0)
                    cullMode: Material.NoCulling
                }
            }
        },
        // The calibrated plane, as a square around its point. Each binding
        // has its own fallback: when the result goes, the plane's values go
        // before `visible` turns false.
        Model {
            visible: gaze3d.plane.visible === true
            source: "#Rectangle"
            position: gaze3d.plane.pos || Qt.vector3d(0, 0, 0)
            rotation: gaze3d.plane.rot || Qt.quaternion(1, 0, 0, 0)
            scale: Qt.vector3d((gaze3d.plane.size || 100) / 100, (gaze3d.plane.size || 100) / 100, 1)
            castsShadows: false
            materials: PrincipledMaterial {
                baseColor: gaze3d.plane.color || "white"
                opacity: gaze3d.plane.lit === true ? 0.45 : 0.15
                cullMode: Material.NoCulling
            }
        },
        Model {
            geometry: SceneMesh { scene: gaze3d; part: "trails" }
            castsShadows: false
            materials: DefaultMaterial { vertexColorsEnabled: true; lighting: DefaultMaterial.NoLighting; opacity: 0.99 }
        }
    ]

    // ── Labels: each person and what they look at; each region's name ──
    Repeater {
        model: gaze3d.tags.length
        delegate: RoomTag {
            required property int index
            readonly property var modelData: gaze3d.tags[index] || ({ pos: Qt.vector3d(0, 0, 0) })
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
    Repeater {
        model: gaze3d.regions.length
        delegate: RoomTag {
            required property int index
            readonly property var modelData: gaze3d.regions[index] || ({ pos: Qt.vector3d(0, 0, 0) })
            readonly property vector3d s: page.screen(modelData.pos)
            text: modelData.name || ""
            tint: modelData.lit ? modelData.color : "#a0a6d0"
            pageHeight: page.height
            x: s.x - width / 2
            y: s.y - height / 2
            visible: s.z > 0
        }
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
        visible: gaze3d.hasResult
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
                    model: gaze3d.people
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
                                onClicked: gaze3d.set_shown(modelData.id, !modelData.shown)
                            }
                        }
                        Text {
                            text: modelData.label + "  ·  " + modelData.seconds.toFixed(1) + " s"
                            color: modelData.color
                            font.pixelSize: 12
                            font.bold: true
                        }
                        RoomPill {
                            text: gaze3d.follow === modelData.id ? "Following" : "Follow"
                            on: gaze3d.follow === modelData.id
                            height: 20
                            onClicked: gaze3d.follow = (gaze3d.follow === modelData.id ? "" : modelData.id)
                        }
                    }
                }
                Text {
                    visible: gaze3d.people.length === 0
                    text: "Nobody's gaze"
                    color: "#9090b0"
                    font.pixelSize: 12
                }
            }
        }
    }
}
