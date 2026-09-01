import QtQuick
import QtQuick.Controls.Basic

// A small toggle in the filter bar. Deliberately quiet: the pictures are the
// loud part of this window, and a row of bright chips would compete with them.
Item {
    id: root

    property string label: ""
    property bool active: false
    property bool floating: false
    property int badge: -1
    property string toolTip: ""

    signal clicked()

    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: root.label
    Accessible.onPressAction: if (root.enabled) root.clicked()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    implicitWidth: content.implicitWidth + (floating ? 26 : 22)
    implicitHeight: floating ? 34 : 26

    Rectangle {
        anchors.fill: parent
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        color: root.active
               ? root.shade(Theme.accent, root.floating ? 0.42 : 0.18)
               : (root.floating
                  ? root.shade(Theme.background, hover.hovered ? 0.92 : 0.72)
                  : root.shade(Theme.foreground, hover.hovered ? 0.09 : 0.0))
        border.width: root.activeFocus ? 2 : 1
        border.color: root.active
                      ? root.shade(Theme.accent, root.floating ? 0.90 : 0.55)
                      : root.activeFocus
                        ? Theme.accent
                        : root.shade(Theme.foreground,
                                     root.floating ? (hover.hovered ? 0.64 : 0.46)
                                                   : (hover.hovered ? 0.20 : 0.12))

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
                font.weight: root.active || root.floating ? Font.DemiBold : Font.Normal
                color: root.active ? Theme.brightForeground
                                   : (root.floating ? Theme.foreground : Theme.mutedText)

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

        ToolTip {
            visible: root.toolTip !== "" && hover.hovered
            text: root.toolTip
            delay: 500
        }

        TapHandler {
            enabled: root.enabled
            onSingleTapped: root.clicked()
        }
    }

    Keys.onPressed: function(event) {
        if (root.enabled && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                             || event.key === Qt.Key_Space)) {
            root.clicked()
            event.accepted = true
        }
    }
}
