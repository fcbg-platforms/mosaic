// The Body button of the room pages (Room3D.qml, Gaze3D.qml): with 3D Avatar
// bodies, draws people as the body, the skeleton, or both. `bodies` is a
// Room3DScene; `shown` false hides it with the bodies.
import QtQuick

RoomPill {
    property var bodies
    property bool shown: true
    readonly property var modes: ["avatar", "skeleton", "both"]
    readonly property var names: ({ avatar: "Avatar", skeleton: "Skeleton", both: "Avatar + skeleton" })
    visible: shown && bodies.avatars.length > 0
    text: "Body: " + names[bodies.bodyMode]
    on: bodies.bodyMode !== "skeleton"
    onClicked: bodies.bodyMode = modes[(modes.indexOf(bodies.bodyMode) + 1) % modes.length]
}
