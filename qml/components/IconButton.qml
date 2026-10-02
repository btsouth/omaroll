import QtQuick
import QtQuick.Controls.Basic

// One icon on a quiet button. The viewer's controls sit over the picture, so
// they read as glass rather than as a toolbar. Raised buttons carry their own
// backing for the places with no scrim behind them.
//
// A MouseArea rather than a TapHandler: it accepts the press, so a click here
// never also reaches the stage underneath, where a tap plays or pauses.
Item {
    id: root

    property string icon: ""
    property string toolTip: ""
    property string shortcut: ""
    property bool active: false
    property bool raised: false
    property real iconSize: 18
    readonly property bool hovered: mouse.containsMouse
    readonly property string resolvedToolTip: root.shortcut !== ""
                                              ? root.toolTip + "  ·  " + root.shortcut
                                              : root.toolTip

    signal clicked()

    implicitWidth: 34
    implicitHeight: 34
    opacity: root.enabled ? 1 : 0.42
    Accessible.role: Accessible.Button
    Accessible.name: root.toolTip
    Accessible.description: root.shortcut !== "" ? "Shortcut " + root.shortcut : ""
    Accessible.onPressAction: if (root.enabled) root.clicked()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    Rectangle {
        anchors.fill: parent
        radius: Math.min(height / 2, Theme.cornerRadius > 0 ? Theme.cornerRadius : 3)
        color: root.active ? root.shade(Theme.accent, 0.30)
               : root.raised ? root.shade(Theme.background, root.hovered ? 0.92 : 0.74)
               : root.shade(Theme.foreground, root.hovered ? 0.12 : 0.0)
        border.width: root.raised ? 1 : 0
        border.color: root.shade(Theme.foreground, root.hovered ? 0.28 : 0.14)
        Behavior on color { ColorAnimation { duration: 120; easing.type: Easing.OutQuad } }
    }

    Icon {
        anchors.centerIn: parent
        name: root.icon
        size: root.iconSize
        color: root.active ? Theme.brightForeground
               : root.hovered ? Theme.brightForeground : Theme.foreground
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }

    ToolTip {
        visible: root.resolvedToolTip !== "" && root.hovered
        text: root.resolvedToolTip
        delay: 500
    }
}
