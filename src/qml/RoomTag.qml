// A label of the 3D room pages (RoomStage.qml), drawn over the scene where
// its 3D point lands: a camera's name, a person's, a distance.
import QtQuick

Rectangle {
    id: tag
    property string text
    property color tint
    property bool big: false
    // The page's height, so labels grow with the view.
    property real pageHeight: 600
    width: label.implicitWidth + 16
    height: label.implicitHeight + 8
    radius: 5
    color: Qt.rgba(0.04, 0.04, 0.1, 0.85)
    border.color: tint
    border.width: 1.5
    Text {
        id: label
        anchors.centerIn: parent
        text: tag.text
        color: tag.tint
        font.pixelSize: Math.max(12, Math.min(19, tag.pageHeight / (tag.big ? 34 : 40)))
        font.bold: true
    }
}
