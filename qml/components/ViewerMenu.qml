import QtQuick
import QtQuick.Controls.Basic

// The viewer's menu, and each of its View, Share and Edit submenus. Held
// inside the window with a margin, never taller than it: a list that does
// not fit scrolls with the wheel and the arrow keys, and fades at the edge
// that has more.
Menu {
    id: menu

    // The rows, as the window built them; heights are read from these.
    property var rows: []
    // Which submenu this is: "view", "share" or "edit". Empty at the top.
    property string groupId: ""
    // The window it opens in, whose height it must fit.
    property var hostWindow: null

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    // An item, not a separate window: a window menu is placed by the
    // compositor and was left running off the bottom of a short window.
    popupType: Popup.Item
    margins: 8
    overlap: 4
    topPadding: 4
    bottomPadding: 4
    // Sized from the rows themselves. The stock list measures its own
    // contentHeight, which changes with the height a tall menu is held to
    // and loops; the rows' heights are known (see ViewerMenuItem).
    readonly property real rowsHeight: {
        let total = 0
        for (const row of menu.rows) {
            total += row.separator === true ? 9
                     : row.available === false && row.hint ? 46 : 30
        }
        return total
    }
    readonly property real maximumHeight: menu.hostWindow ? menu.hostWindow.height - 2 * 8 : Infinity
    height: Math.min(implicitHeight, maximumHeight)

    contentItem: ListView {
        id: list
        objectName: menu.objectName + "List"
        implicitHeight: menu.rowsHeight
        model: menu.contentModel
        interactive: implicitHeight + menu.topPadding + menu.bottomPadding > menu.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        currentIndex: menu.currentIndex
        highlightFollowsCurrentItem: false
        // The keyboard's row stays in view.
        onCurrentIndexChanged: if (currentIndex >= 0 && interactive)
            positionViewAtIndex(currentIndex, ListView.Contain)

        ScrollBar.vertical: ScrollBar {
            policy: list.interactive ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            width: 6
        }

        // The wheel and touchpad scroll it whatever the pointer is over, and
        // the event stops here rather than reaching the picture.
        WheelHandler {
            target: null
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function (event) {
                if (!list.interactive) {
                    event.accepted = true
                    return
                }
                // A wheel notch moves two rows. Only a touchpad's swipe, which
                // comes in phases, follows its pixels; Wayland gives a wheel
                // a pixel delta too, of a few pixels a notch.
                const delta = event.phase !== Qt.NoScrollPhase && event.pixelDelta.y !== 0
                              ? event.pixelDelta.y : event.angleDelta.y / 2
                const maximum = Math.max(0, list.contentHeight - list.height)
                list.contentY = Math.max(list.originY,
                                         Math.min(list.originY + maximum, list.contentY - delta))
                event.accepted = true
            }
        }

        // Fades at the edge that has more rows past it.
        Rectangle {
            objectName: menu.objectName + "TopFade"
            parent: list
            anchors.top: parent.top
            width: parent.width
            height: 18
            z: 2
            visible: list.interactive && !list.atYBeginning
            gradient: Gradient {
                GradientStop { position: 0.0; color: menu.shade(Theme.background, 0.97) }
                GradientStop { position: 1.0; color: menu.shade(Theme.background, 0) }
            }
        }
        Rectangle {
            objectName: menu.objectName + "BottomFade"
            parent: list
            anchors.bottom: parent.bottom
            width: parent.width
            height: 22
            z: 2
            visible: list.interactive && !list.atYEnd
            gradient: Gradient {
                GradientStop { position: 0.0; color: menu.shade(Theme.background, 0) }
                GradientStop { position: 1.0; color: menu.shade(Theme.background, 0.97) }
            }
        }
    }

    background: Rectangle {
        implicitWidth: 236
        color: menu.shade(Theme.background, 0.97)
        border.width: 1
        border.color: menu.shade(Theme.foreground, 0.16)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
    }
}
