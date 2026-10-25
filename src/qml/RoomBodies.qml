// The 3D Pose people of a room page: limbs, joints, a head with the way it
// faces, the torso, the trails and paths, the two-person line. `bodies` is a
// Room3DScene; the 3D Pose room view (Room3D.qml) and the 3D Gaze view
// (Gaze3D.qml, its bodies) both draw them with this.
//
// The objects are made per index (model: count), so a new frame only moves
// them; a list model would remake them all each tick.
import QtQuick
import QtQuick3D
import Mosaic.Room3D

Node {
    id: root
    property var bodies

    Repeater3D {
        model: root.bodies.bones.length
        delegate: Model {
            required property int index
            readonly property var modelData: root.bodies.bones[index] || ({})
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
        model: root.bodies.joints.length
        delegate: Model {
            required property int index
            readonly property var modelData: root.bodies.joints[index] || ({})
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
        model: root.bodies.heads.length
        delegate: Node {
            required property int index
            readonly property var modelData: root.bodies.heads[index] || ({})
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
        geometry: SceneMesh { scene: root.bodies; part: "torsos" }
        castsShadows: true
        materials: DefaultMaterial {
            vertexColorsEnabled: true
            cullMode: Material.NoCulling
            opacity: 0.99
        }
    }
    Model {
        visible: root.bodies.paths
        geometry: SceneMesh { scene: root.bodies; part: "paths" }
        castsShadows: false
        materials: DefaultMaterial { vertexColorsEnabled: true; lighting: DefaultMaterial.NoLighting; opacity: 0.99 }
    }
    Model {
        geometry: SceneMesh { scene: root.bodies; part: "trails" }
        castsShadows: false
        materials: DefaultMaterial { vertexColorsEnabled: true; lighting: DefaultMaterial.NoLighting; opacity: 0.99 }
    }
    // The two people of Dyad Analysis, joined at the hips.
    Model {
        visible: root.bodies.dyad.visible === true
        source: "#Cylinder"
        position: visible ? root.bodies.dyad.pos : Qt.vector3d(0, 0, 0)
        rotation: visible ? root.bodies.dyad.rot : Qt.quaternion(1, 0, 0, 0)
        scale: Qt.vector3d(0.12, visible ? root.bodies.dyad.len / 100 : 1, 0.12)
        materials: PrincipledMaterial { baseColor: "white"; emissiveFactor: Qt.vector3d(0.6, 0.6, 0.6) }
    }
}
