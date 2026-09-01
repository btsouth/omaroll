import QtQuick

// A small toggle in the filter bar. Deliberately quiet: the pictures are the
// loud part of this window, and a row of bright chips would compete with them.
Item {
    id: root

    property string label: ""
    property bool active: false
    property int badge: -1

    signal clicked()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    implicitWidth: content.implicitWidth + 22
    implicitHeight: 26

    Rectangle {
        anchors.fill: parent
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        color: root.active
               ? root.shade(Theme.accent, 0.18)
               : root.shade(Theme.foreground, hover.hovered ? 0.09 : 0.0)
        border.width: 1
        border.color: root.active
                      ? root.shade(Theme.accent, 0.55)
                      : root.shade(Theme.foreground, hover.hovered ? 0.20 : 0.12)

        Behavior on color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }
        Behavior on border.color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }

        Row {
            id: content
            anchors.centerIn: parent
            spacing: 6

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.label
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: root.active ? Font.DemiBold : Font.Normal
                color: root.active ? Theme.brightForeground : Theme.mutedText

                Behavior on color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.badge >= 0
                text: root.badge
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: root.shade(root.active ? Theme.brightForeground : Theme.foreground, 0.5)
            }
        }

        HoverHandler {
            id: hover
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            onSingleTapped: root.clicked()
        }
    }
}
