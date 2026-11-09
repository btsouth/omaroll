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
    // The toggle offers actual size (1:1) when true, otherwise Fit. A window
    // that knows more than "fitted or not" decides it.
    property bool offerActual: root.fitted && root.actualControl
    // The zoom level beside the zoom buttons, such as "57%"; empty shows none.
    property string zoomText: ""
    // Save's place is kept while a copy could be saved after a turn or flip,
    // so the bar never moves as the button comes and goes; it is greyed until
    // there is something to save.
    property bool saveSlotReserved: false
    property bool canStep: false
    property bool slideshowRunning: false
    // On a narrow window the zoom group gives way; the wheel and keys still zoom.
    property bool compact: false
    // Off while the picture could not be shown: nothing to zoom or turn.
    property bool viewControls: true
    // Flip gives way before zoom does; the keys and the menu still flip.
    property bool flipControls: true
    // Compare uses only the zoom group: it has nothing to step through or turn.
    property bool stepControls: true
    property bool transformControls: true
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

    // Where no place is reserved, Save's place still stays while the pointer
    // is on the bar after the button goes, so nothing moves under it.
    property bool holdSaveSlot: false
    onCanSaveCopyChanged: if (!canSaveCopy && hover.hovered) holdSaveSlot = true
    onHoveredChanged: if (!hovered) holdSaveSlot = false

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    implicitWidth: row.implicitWidth + 12
    implicitHeight: 46
    radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, height / 2) : 4
    // Dark over the picture in every theme; see Osd.
    color: Osd.panel
    border.width: 1
    border.color: Osd.border

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
            color: Osd.divider
        }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 2

        IconButton {
            objectName: root.namePrefix + "ZoomOut"
            osd: true
            enabled: root.viewControls
            visible: !root.compact
            icon: "zoom-out"
            toolTip: "Zoom out"
            shortcut: root.hint("zoom-out")
            onClicked: root.zoomOut()
        }
        IconButton {
            objectName: root.namePrefix + "FitToggle"
            osd: true
            enabled: root.viewControls
            visible: !root.compact
            label: root.offerActual ? "1:1" : "Fit"
            active: !root.offerActual && root.fitted
            toolTip: root.offerActual ? "Actual size" : "Fit to window"
            shortcut: root.hint(root.offerActual ? "actual" : "fit")
            onClicked: root.toggleFit()
        }
        IconButton {
            objectName: root.namePrefix + "ZoomIn"
            osd: true
            enabled: root.viewControls
            visible: !root.compact
            icon: "zoom-in"
            toolTip: "Zoom in"
            shortcut: root.hint("zoom-in")
            onClicked: root.zoomIn()
        }
        Text {
            objectName: root.namePrefix + "ZoomLevel"
            visible: !root.compact && root.zoomText !== ""
            // A fixed width, so the bar does not shift as the number changes.
            width: 44
            anchors.verticalCenter: parent.verticalCenter
            horizontalAlignment: Text.AlignHCenter
            text: root.zoomText
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Osd.mutedText
            Accessible.role: Accessible.StaticText
            Accessible.name: "Zoom " + root.zoomText
        }
        Divider { visible: !root.compact && (root.stepControls || root.transformControls) }

        IconButton {
            objectName: root.namePrefix + "ToolbarPrevious"
            osd: true
            visible: root.stepControls
            enabled: root.canStep
            icon: "chevron-left"
            toolTip: "Previous"
            shortcut: root.hint("previous")
            onClicked: root.previous()
        }
        IconButton {
            objectName: root.namePrefix + "SlideshowButton"
            osd: true
            visible: root.stepControls
            enabled: root.canStep
            icon: root.slideshowRunning ? "pause" : "play"
            active: root.slideshowRunning
            toolTip: root.slideshowRunning ? "Stop slideshow" : "Slideshow"
            shortcut: root.hint("slideshow")
            onClicked: root.slideshow()
        }
        IconButton {
            objectName: root.namePrefix + "ToolbarNext"
            osd: true
            visible: root.stepControls
            enabled: root.canStep
            icon: "chevron-right"
            toolTip: "Next"
            shortcut: root.hint("next")
            onClicked: root.next()
        }
        Divider { visible: root.stepControls && root.transformControls }

        IconButton {
            objectName: root.namePrefix + "RotateLeft"
            osd: true
            visible: root.transformControls
            enabled: root.viewControls
            icon: "rotate-left"
            toolTip: "Rotate left"
            shortcut: root.hint("rotate-left")
            onClicked: root.rotateLeft()
        }
        IconButton {
            objectName: root.namePrefix + "RotateRight"
            osd: true
            visible: root.transformControls
            enabled: root.viewControls
            icon: "rotate-right"
            toolTip: "Rotate right"
            shortcut: root.hint("rotate-right")
            onClicked: root.rotateRight()
        }
        IconButton {
            objectName: root.namePrefix + "FlipHorizontal"
            osd: true
            enabled: root.viewControls
            visible: root.flipControls && root.transformControls
            icon: "flip-horizontal"
            checkable: true
            active: root.flippedHorizontally
            toolTip: "Flip horizontally"
            shortcut: root.hint("flip-horizontal")
            onClicked: root.flipHorizontal()
        }
        IconButton {
            objectName: root.namePrefix + "FlipVertical"
            osd: true
            enabled: root.viewControls
            visible: root.flipControls && root.transformControls
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
            visible: root.transformControls
                     && (root.canSaveCopy || root.saveSlotReserved || root.holdSaveSlot)
            width: saveButton.implicitWidth
            height: saveButton.implicitHeight

            IconButton {
                id: saveButton
                objectName: root.namePrefix + "SaveCopy"
                osd: true
                anchors.fill: parent
                enabled: root.canSaveCopy
                icon: "save"
                toolTip: root.saveCopyLabel
                shortcut: root.hint("save-view-copy")
                onClicked: root.saveCopy()
            }
        }
        Divider { visible: root.trashControl || root.fullScreenControl }

        IconButton {
            objectName: root.namePrefix + "FullScreenButton"
            osd: true
            visible: root.fullScreenControl
            icon: root.fullScreen ? "fullscreen-exit" : "fullscreen"
            toolTip: root.fullScreen ? "Exit full screen" : "Full screen"
            shortcut: root.hint("fullscreen")
            onClicked: root.toggleFullScreen()
        }
        IconButton {
            objectName: root.namePrefix + "TrashButton"
            osd: true
            visible: root.trashControl
            icon: "trash"
            toolTip: "Move to Trash"
            shortcut: root.hint("trash")
            onClicked: root.trash()
        }
    }
}
