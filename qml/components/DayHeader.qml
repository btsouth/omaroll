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
    property real maximumWidth: Infinity
    // In a date sort the pill is a button that opens Jump to date.
    property bool interactive: false
    signal clicked()

    implicitWidth: pill.width
    implicitHeight: pill.height
    activeFocusOnTab: root.interactive && root.visible
    Accessible.role: root.interactive ? Accessible.Button : Accessible.StaticText
    Accessible.name: root.interactive ? "Jump to date" : root.label
    Accessible.description: root.interactive && root.label !== "Jump to date" ? root.label : ""
    Accessible.ignored: !root.visible || (!root.interactive && root.label === "")
    Accessible.onPressAction: if (root.interactive) root.clicked()
    Keys.onPressed: function (event) {
        if (root.interactive && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                 || event.key === Qt.Key_Space || event.key === Qt.Key_Down)) {
            root.clicked()
            event.accepted = true
        }
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    Rectangle {
        id: pill
        width: Math.min(root.maximumWidth, text.implicitWidth + 22)
        height: text.implicitHeight + 12
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        color: root.interactive && hover.hovered ? Qt.tint(root.shade(Theme.background, 0.88), root.shade(Theme.foreground, 0.08))
                                                 : root.shade(Theme.background, 0.88)
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? Theme.accent
                                       : root.shade(Theme.foreground, root.interactive && hover.hovered ? 0.30 : 0.14)

        opacity: root.shown && root.label !== "" ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutQuad } }
        Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }

        Text {
            id: text
            anchors.centerIn: parent
            text: root.label
            width: Math.max(0, pill.width - 22)
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: Theme.brightForeground

            Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
        }
        HoverHandler {
            id: hover
            enabled: root.interactive
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            enabled: root.interactive
            onTapped: root.clicked()
        }
        ThemedToolTip {
            requested: root.interactive && hover.hovered
            text: "Jump to date · Shift+Enter in the list shows only that month"
        }
    }
}
