import QtQuick

// The common picture controls, in the order Windows Photo Viewer taught
// everyone: zoom, then previous, slideshow and next, then rotate and flip, then
// delete. Everything less common lives in the viewer's menu. The library's
// preview uses the same bar, with full screen in place of delete.
Rectangle {
    id: root

    // Object names start with this, so two bars in one window stay apart.
    property string namePrefix: "viewer"
    property bool trashControl: true
    property bool fullScreenControl: false
    property bool fullScreen: false
    // Off for a document: the toggle only fits it again.
    property bool actualControl: true
    // The key beside each button's name, from the window's own shortcut rows.
    property var hints: ({
        "zoom-out": ShortcutCatalog.quickHints["zoom-out"],
        "zoom-in": ShortcutCatalog.quickHints["zoom-in"],
        "actual": ShortcutCatalog.quickHints.actual,
        "fit": ShortcutCatalog.quickHints.fit,
        "previous": ShortcutCatalog.sequence("quick", "previous")[0],
        "next": ShortcutCatalog.sequence("quick", "previous")[1],
        "slideshow": ShortcutCatalog.quickHints.slideshow,
        "rotate-left": ShortcutCatalog.sequence("quick", "rotate")[1],
        "rotate-right": ShortcutCatalog.sequence("quick", "rotate")[0],
        "flip-horizontal": ShortcutCatalog.quickHints["flip-horizontal"],
        "flip-vertical": ShortcutCatalog.quickHints["flip-vertical"],
        "save-view-copy": ShortcutCatalog.quickHints["save-view-copy"],
        "fullscreen": ShortcutCatalog.quickHints.fullscreen,
        "trash": ShortcutCatalog.sequence("quick", "trash")[0]
    })
    function hint(id) { return root.hints[id] || "" }

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
    signal toggleFullScreen()

    // Once Save is up and the pointer is on the bar, its place stays when it
    // goes (as it does on a click, while the copy is written and after), so
    // the buttons beside it never move under the pointer. Off the bar, the
    // gap closes.
    property bool holdSaveSlot: false
    onCanSaveCopyChanged: if (!canSaveCopy && hover.hovered) holdSaveSlot = true
    onHoveredChanged: if (!hovered) holdSaveSlot = false

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
            objectName: root.namePrefix + "ZoomOut"
            enabled: root.viewControls
            visible: !root.compact
            icon: "zoom-out"
            toolTip: "Zoom out"
            shortcut: root.hint("zoom-out")
            onClicked: root.zoomOut()
        }
        IconButton {
            objectName: root.namePrefix + "FitToggle"
            enabled: root.viewControls
            visible: !root.compact
            label: root.fitted && root.actualControl ? "1:1" : "Fit"
            active: !root.actualControl && root.fitted
            toolTip: root.fitted && root.actualControl ? "Actual size" : "Fit to window"
            shortcut: root.hint(root.fitted && root.actualControl ? "actual" : "fit")
            onClicked: root.toggleFit()
        }
        IconButton {
            objectName: root.namePrefix + "ZoomIn"
            enabled: root.viewControls
            visible: !root.compact
            icon: "zoom-in"
            toolTip: "Zoom in"
            shortcut: root.hint("zoom-in")
            onClicked: root.zoomIn()
        }
        Divider { visible: !root.compact }

        IconButton {
            objectName: root.namePrefix + "ToolbarPrevious"
            enabled: root.canStep
            icon: "chevron-left"
            toolTip: "Previous"
            shortcut: root.hint("previous")
            onClicked: root.previous()
        }
        IconButton {
            objectName: root.namePrefix + "SlideshowButton"
            enabled: root.canStep
            icon: root.slideshowRunning ? "pause" : "play"
            active: root.slideshowRunning
            toolTip: root.slideshowRunning ? "Stop slideshow" : "Slideshow"
            shortcut: root.hint("slideshow")
            onClicked: root.slideshow()
        }
        IconButton {
            objectName: root.namePrefix + "ToolbarNext"
            enabled: root.canStep
            icon: "chevron-right"
            toolTip: "Next"
            shortcut: root.hint("next")
            onClicked: root.next()
        }
        Divider {}

        IconButton {
            objectName: root.namePrefix + "RotateLeft"
            enabled: root.viewControls
            icon: "rotate-left"
            toolTip: "Rotate left"
            shortcut: root.hint("rotate-left")
            onClicked: root.rotateLeft()
        }
        IconButton {
            objectName: root.namePrefix + "RotateRight"
            enabled: root.viewControls
            icon: "rotate-right"
            toolTip: "Rotate right"
            shortcut: root.hint("rotate-right")
            onClicked: root.rotateRight()
        }
        IconButton {
            objectName: root.namePrefix + "FlipHorizontal"
            enabled: root.viewControls
            visible: root.flipControls
            icon: "flip-horizontal"
            checkable: true
            active: root.flippedHorizontally
            toolTip: "Flip horizontally"
            shortcut: root.hint("flip-horizontal")
            onClicked: root.flipHorizontal()
        }
        IconButton {
            objectName: root.namePrefix + "FlipVertical"
            enabled: root.viewControls
            visible: root.flipControls
            icon: "flip-vertical"
            checkable: true
            active: root.flippedVertically
            toolTip: "Flip vertically"
            shortcut: root.hint("flip-vertical")
            onClicked: root.flipVertical()
        }
        Item {
            id: saveSlot
            objectName: root.namePrefix + "SaveCopySlot"
            visible: root.canSaveCopy || root.holdSaveSlot
            width: saveButton.implicitWidth
            height: saveButton.implicitHeight

            IconButton {
                id: saveButton
                objectName: root.namePrefix + "SaveCopy"
                anchors.fill: parent
                visible: root.canSaveCopy
                icon: "save"
                toolTip: root.saveCopyLabel
                shortcut: root.hint("save-view-copy")
                onClicked: root.saveCopy()
            }
        }
        Divider { visible: root.trashControl || root.fullScreenControl }

        IconButton {
            objectName: root.namePrefix + "FullScreenButton"
            visible: root.fullScreenControl
            icon: root.fullScreen ? "fullscreen-exit" : "fullscreen"
            toolTip: root.fullScreen ? "Exit full screen" : "Full screen"
            shortcut: root.hint("fullscreen")
            onClicked: root.toggleFullScreen()
        }
        IconButton {
            objectName: root.namePrefix + "TrashButton"
            visible: root.trashControl
            icon: "trash"
            toolTip: "Move to Trash"
            shortcut: root.hint("trash")
            onClicked: root.trash()
        }
    }
}
