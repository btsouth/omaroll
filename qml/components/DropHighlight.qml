import QtQuick

// Where dragged files will land: the theme's accent around the window and one
// line saying what the drop does. It only draws; the DropArea decides.
Item {
    id: root

    property bool shown: false
    property string label: ""

    anchors.fill: parent
    opacity: root.shown ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 6
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.accent, 0.10)
        border.width: 2
        border.color: Theme.accent
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(dropText.implicitWidth + 36, parent.width - 40)
        height: dropText.height + 20
        radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, height / 2) : 3
        color: root.shade(Theme.background, 0.94)
        border.width: 1
        border.color: root.shade(Theme.accent, 0.7)

        Text {
            id: dropText
            objectName: "dropHighlightLabel"
            anchors.centerIn: parent
            width: parent.width - 36
            text: root.label
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            color: Theme.brightForeground
        }
    }
}
