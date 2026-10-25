// A button of the 3D room pages (RoomStage.qml): highlighted while `on`.
import QtQuick

Rectangle {
    id: pill
    property string text
    property bool on: false
    signal clicked
    width: t.implicitWidth + 18
    height: 26
    radius: 5
    color: on ? "#3a5fc8" : Qt.rgba(0.08, 0.08, 0.17, 0.85)
    border.color: on ? "#5a7fe8" : "#34345a"
    Text {
        id: t
        anchors.centerIn: parent
        text: pill.text
        color: pill.on ? "white" : "#c0c0e0"
        font.pixelSize: 12
    }
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: pill.clicked()
    }
}
