import QtQuick

// The day of whatever is at the top of the view.
//
// A GridView has no sections, and breaking the grid into one sub-grid per day
// costs a lot of layout for something the eye only needs while scrolling. A
// single pill that tracks the top row gives the same orientation and keeps the
// grid a grid.
Item {
    id: root

    property string label: ""
    property bool shown: true

    implicitWidth: pill.width
    implicitHeight: pill.height

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    Rectangle {
        id: pill
        width: text.implicitWidth + 22
        height: text.implicitHeight + 12
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        color: root.shade(Theme.background, 0.88)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.14)

        opacity: root.shown && root.label !== "" ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutQuad } }
        Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }

        Text {
            id: text
            anchors.centerIn: parent
            text: root.label
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: Theme.brightForeground

            Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
        }
    }
}
