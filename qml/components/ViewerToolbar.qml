import QtQuick

// The common picture controls, in the order Windows Photo Viewer taught
// everyone: zoom, then previous, slideshow and next, then rotate and flip, then
// delete. Everything less common lives in the viewer's menu.
Rectangle {
    id: root

    property bool fitted: true
    property bool canStep: false
    property bool slideshowRunning: false
    // On a narrow window the zoom group gives way; the wheel and keys still zoom.
    property bool compact: false
    // Off while the picture could not be shown: nothing to zoom or turn.
    property bool viewControls: true
    // Flip gives way before zoom does; the keys and the menu still flip.
    property bool flipControls: true
    property bool flippedHorizontally: false
    property bool flippedVertically: false
    // Turned or flipped: a copy of what is on screen can be saved.
    property bool canSaveCopy: false
    property string saveCopyLabel: "Save rotated copy"
    readonly property bool hovered: hover.hovered

    signal zoomOut()
    signal zoomIn()
    signal toggleFit()
    signal previous()
    signal next()
    signal slideshow()
    signal rotateLeft()
    signal rotateRight()
    signal flipHorizontal()
    signal flipVertical()
    signal saveCopy()
    signal trash()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    implicitWidth: row.implicitWidth + 12
    implicitHeight: 46
    radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, height / 2) : 4
    color: root.shade(Theme.background, 0.84)
    border.width: 1
    border.color: root.shade(Theme.foreground, 0.12)

    HoverHandler { id: hover }

    // A press between the buttons stays on the bar.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
    }

    component Divider: Item {
        width: 9
        height: 20
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        Rectangle {
            anchors.centerIn: parent
            width: 1
            height: parent.height
            // Inline components cannot see this file's ids, so no root.shade.
            color: Qt.rgba(Theme.foreground.r, Theme.foreground.g, Theme.foreground.b, 0.14)
        }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 2

        IconButton {
            objectName: "viewerZoomOut"
            enabled: root.viewControls
            visible: !root.compact
            icon: "zoom-out"
            toolTip: "Zoom out"
            shortcut: ShortcutCatalog.quickHints["zoom-out"]
            onClicked: root.zoomOut()
        }
        IconButton {
            objectName: "viewerFitToggle"
            enabled: root.viewControls
            visible: !root.compact
            label: root.fitted ? "1:1" : "Fit"
            toolTip: root.fitted ? "Actual size" : "Fit to window"
            shortcut: root.fitted ? ShortcutCatalog.quickHints.actual : ShortcutCatalog.quickHints.fit
            onClicked: root.toggleFit()
        }
        IconButton {
            objectName: "viewerZoomIn"
            enabled: root.viewControls
            visible: !root.compact
            icon: "zoom-in"
            toolTip: "Zoom in"
            shortcut: ShortcutCatalog.quickHints["zoom-in"]
            onClicked: root.zoomIn()
        }
        Divider { visible: !root.compact }

        IconButton {
            objectName: "viewerToolbarPrevious"
            enabled: root.canStep
            icon: "chevron-left"
            toolTip: "Previous"
            shortcut: ShortcutCatalog.sequence("quick", "previous")[0]
            onClicked: root.previous()
        }
        IconButton {
            objectName: "viewerSlideshowButton"
            enabled: root.canStep
            icon: root.slideshowRunning ? "pause" : "play"
            active: root.slideshowRunning
            toolTip: root.slideshowRunning ? "Stop slideshow" : "Slideshow"
            shortcut: ShortcutCatalog.quickHints.slideshow
            onClicked: root.slideshow()
        }
        IconButton {
            objectName: "viewerToolbarNext"
            enabled: root.canStep
            icon: "chevron-right"
            toolTip: "Next"
            shortcut: ShortcutCatalog.sequence("quick", "previous")[1]
            onClicked: root.next()
        }
        Divider {}

        IconButton {
            objectName: "viewerRotateLeft"
            enabled: root.viewControls
            icon: "rotate-left"
            toolTip: "Rotate left"
            shortcut: ShortcutCatalog.sequence("quick", "rotate")[1]
            onClicked: root.rotateLeft()
        }
        IconButton {
            objectName: "viewerRotateRight"
            enabled: root.viewControls
            icon: "rotate-right"
            toolTip: "Rotate right"
            shortcut: ShortcutCatalog.sequence("quick", "rotate")[0]
            onClicked: root.rotateRight()
        }
        IconButton {
            objectName: "viewerFlipHorizontal"
            enabled: root.viewControls
            visible: root.flipControls
            icon: "flip-horizontal"
            checkable: true
            active: root.flippedHorizontally
            toolTip: "Flip horizontally"
            shortcut: ShortcutCatalog.quickHints["flip-horizontal"]
            onClicked: root.flipHorizontal()
        }
        IconButton {
            objectName: "viewerFlipVertical"
            enabled: root.viewControls
            visible: root.flipControls
            icon: "flip-vertical"
            checkable: true
            active: root.flippedVertically
            toolTip: "Flip vertically"
            shortcut: ShortcutCatalog.quickHints["flip-vertical"]
            onClicked: root.flipVertical()
        }
        IconButton {
            objectName: "viewerSaveCopy"
            visible: root.canSaveCopy
            icon: "save"
            toolTip: root.saveCopyLabel
            shortcut: ShortcutCatalog.quickHints["save-view-copy"]
            onClicked: root.saveCopy()
        }
        Divider {}

        IconButton {
            objectName: "viewerTrashButton"
            icon: "trash"
            toolTip: "Move to Trash"
            shortcut: ShortcutCatalog.sequence("quick", "trash")[0]
            onClicked: root.trash()
        }
    }
}
