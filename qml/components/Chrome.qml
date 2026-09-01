import QtQuick

// The translucent surface everything else sits on.
//
// This is the whole transparency story: the window itself paints nothing
// (ApplicationWindow.color is "transparent"), this gradient carries the alpha
// the active theme asked for, and every child above it is drawn fully opaque.
// So the chrome recedes and the photographs stay true.
//
// Alpha comes from shell.toml's [launcher] background-alpha, the same key
// omakade reads, never from a constant here.
Item {
    id: root

    property color tint: Theme.surfaceBackground
    property real surfaceAlpha: Theme.surfaceAlpha

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    Rectangle {
        anchors.fill: parent

        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: root.shade(root.tint, root.surfaceAlpha)
                Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
            }
            GradientStop {
                position: 0.48
                color: root.shade(root.tint, root.surfaceAlpha * 0.88)
                Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
            }
            GradientStop {
                position: 1.0
                color: root.shade(root.tint, root.surfaceAlpha)
                Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
            }
        }
    }

    // A single accent wash, low enough to read as depth rather than colour.
    Rectangle {
        anchors.fill: parent
        color: root.shade(Theme.accent, 0.05)
        Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
    }
}
