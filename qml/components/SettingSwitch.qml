import QtQuick

// An on/off setting. The track takes the accent when on, and the knob sits at
// the end the setting points to, so the state reads without a word beside it.
// Square themes get square corners; rounded ones a pill.
Item {
    id: root

    property bool checked: false
    property string accessibleName: ""
    property string toolTip: ""

    // Asked for, not done: the setting it shows decides, and |checked|
    // follows it.
    signal toggled()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    implicitWidth: 40
    implicitHeight: 22
    activeFocusOnTab: true
    opacity: root.enabled ? 1 : 0.42
    Accessible.role: Accessible.CheckBox
    Accessible.name: root.accessibleName
    Accessible.checkable: true
    Accessible.checked: root.checked
    Accessible.onPressAction: if (root.enabled) root.toggled()
    Accessible.onToggleAction: if (root.enabled) root.toggled()

    Rectangle {
        id: track
        anchors.fill: parent
        radius: Theme.cornerRadius > 0 ? height / 2 : 3
        color: root.checked ? root.shade(Theme.accent, 0.85)
                            : root.shade(Theme.foreground, hover.hovered ? 0.22 : 0.16)
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? Theme.brightForeground
                      : root.checked ? root.shade(Theme.accent, 0.95)
                      : root.shade(Theme.foreground, 0.28)

        Behavior on color { ColorAnimation { duration: 120; easing.type: Easing.OutQuad } }

        Rectangle {
            id: knob
            width: parent.height - 6
            height: width
            y: 3
            x: root.checked ? parent.width - width - 3 : 3
            radius: Theme.cornerRadius > 0 ? width / 2 : 2
            color: root.checked ? Theme.background : Theme.foreground

            Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
        }
    }

    HoverHandler {
        id: hover
        cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
    }

    ThemedToolTip {
        owner: root
        requested: hover.hovered && root.toolTip !== ""
        keyboardRequested: root.toolTip !== "" && root.activeFocus && root.Window.window
                           && root.Window.window.keyboardNavigation === true
        text: root.toolTip
        delay: 500
    }

    TapHandler {
        enabled: root.enabled
        onTapped: root.toggled()
    }

    Keys.onPressed: function(event) {
        if (root.enabled && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                             || event.key === Qt.Key_Space)) {
            root.toggled()
            event.accepted = true
        }
    }
}
