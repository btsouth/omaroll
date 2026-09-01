import QtQuick
import QtQuick.Controls.Basic
import Omaroll

ApplicationWindow {
    id: root

    width: 1180
    height: 780
    minimumWidth: 560
    minimumHeight: 420
    visible: true
    title: "Omaroll"

    // The window paints nothing. Chrome carries the theme's alpha, and every
    // thumbnail is drawn opaque on top of it.
    color: "transparent"

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    // Destructive actions act on a path captured when the sheet opened, not on
    // whatever is selected when it is confirmed. A rescan or sort change can
    // move the selection under a modal, and trashing the wrong file is
    // unforgivable.
    property string pendingDeletePath: ""
    property var pendingDeleteBatch: []

    readonly property bool anySheetOpen: confirm.visible || detail.visible
                                         || matteSheet.visible || settingsSheet.visible

    function say(message) {
        notice.text = message
        noticeTimer.restart()
    }

    function currentPath() { return Captures.pathAt(library.currentIndex) }

    // Every action funnels through here so a keystroke, a click in the detail
    // sidebar and a bulk operation all take the same path.
    function perform(id, path) {
        if (path === "") {
            return
        }

        switch (id) {
        case "matte":
            matteSheet.path = path
            matteSheet.fileName = Captures.fileNameAt(library.currentIndex)
            matteSheet.open()
            return
        case "favorite":
            Settings.toggleFavorite(path)
            root.say(Settings.isFavorite(path) ? "Added to favourites" : "Removed from favourites")
            return
        case "hide":
            Settings.toggleHidden(path)
            root.say(Settings.isHidden(path) ? "Hidden" : "Shown again")
            return
        case "trash":
            root.requestDelete(path)
            return
        case "open":
            Actions.open(path)
            return
        default:
            Registry.run(id, path)
        }
    }

    function requestDelete(path) {
        if (path === "") {
            return
        }
        root.pendingDeletePath = path
        root.pendingDeleteBatch = []
        confirm.title = "Move this capture to Trash?"
        confirm.detail = path
        confirm.open()
    }

    function requestDeleteBatch(paths) {
        if (paths.length === 0) {
            return
        }
        root.pendingDeletePath = ""
        root.pendingDeleteBatch = paths
        confirm.title = "Move " + paths.length + " captures to Trash?"
        confirm.detail = paths.length + " files. They stay recoverable from your file manager."
        confirm.open()
    }

    // Called by --render so a screenshot can be taken of a specific view
    // without anyone having to drive the UI by hand.
    function openViewForRender(view) {
        if (library.count === 0) {
            return
        }
        library.currentIndex = 0
        if (view === "detail") {
            root.openDetail(0)
        } else if (view === "matte") {
            root.perform("matte", Captures.pathAt(0))
        }
    }

    Chrome {
        anchors.fill: parent
    }

    // Header
    Item {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 58

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "omaroll"
                font.family: Theme.fontFamily
                font.pixelSize: 17
                font.weight: Font.Bold
                color: Theme.brightForeground
                Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 1
                height: 16
                color: root.shade(Theme.foreground, 0.18)
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    if (Library.scanning && Captures.count === 0) {
                        return "Scanning…"
                    }
                    if (library.checkedCount > 0) {
                        return library.checkedCount + " selected"
                    }
                    if (Captures.count === 0) {
                        return "Nothing to show"
                    }
                    const shown = Captures.count
                    const total = Captures.sourceCount()
                    const noun = shown === 1 ? " capture" : " captures"
                    return shown === total ? shown + noun : shown + " of " + total + noun
                }
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: library.checkedCount > 0 ? Theme.accent : Theme.mutedText
                Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            // Bulk actions appear only while a selection exists, so the resting
            // header stays quiet.
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0
                label: "Trash " + library.checkedCount
                onClicked: root.requestDeleteBatch(library.checkedPaths())
            }

            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0
                label: "Clear"
                onClicked: library.clearChecked()
            }

            DayHeader {
                anchors.verticalCenter: parent.verticalCenter
                label: library.currentDayLabel
                shown: Captures.count > 0 && library.checkedCount === 0
            }

            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                label: "⚙"
                onClicked: settingsSheet.open()
            }
        }
    }

    FilterBar {
        id: filters
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
    }

    Rectangle {
        id: divider
        anchors.top: filters.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: root.shade(Theme.foreground, 0.10)
        Behavior on color { ColorAnimation { duration: 180 } }
    }

    CaptureGrid {
        id: library
        anchors.top: divider.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        anchors.topMargin: 8
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.bottomMargin: 4
        focus: !root.anySheetOpen

        model: Captures
        visible: Captures.count > 0

        onChosen: function (path) { root.perform("matte", path) }
        onDeleteRequested: function (path) { root.requestDelete(path) }
        onDetailRequested: function (index) { root.openDetail(index) }
    }

    function openDetail(index) {
        if (index < 0) {
            return
        }
        detail.path = Captures.pathAt(index)
        detail.fileName = Captures.fileNameAt(index)
        detail.kind = Captures.kindAt(index)
        detail.kindLabel = Captures.kindLabelAt(index)
        detail.dayLabel = Captures.dayLabelAt(index)
        detail.timeLabel = Captures.timeLabelAt(index)
        detail.sizeLabel = Captures.sizeLabelAt(index)
        detail.isVideo = Captures.isVideoAt(index)
        detail.favorite = Settings.isFavorite(detail.path)
        detail.open()
    }

    // Empty state. Two different empties: nothing exists, or nothing matches
    // the current filter. Telling them apart is the difference between useful
    // and useless.
    Column {
        anchors.centerIn: library
        width: Math.min(420, parent.width - 80)
        spacing: 10
        visible: Captures.count === 0 && !Library.scanning

        readonly property bool filtered: Captures.sourceCount() > 0

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: parent.filtered ? "Nothing matches" : "No captures yet"
            font.family: Theme.fontFamily
            font.pixelSize: 16
            font.weight: Font.DemiBold
            color: Theme.brightForeground
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: parent.filtered
                  ? "Try a different filter, or clear the search."
                  : "Take a screenshot with Super + Shift + S, or record from the Capture menu. "
                    + "Anything that lands in your Pictures and Videos folders shows up here."
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.mutedText
        }
    }

    // Footer
    Item {
        id: footer
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 32

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: root.shade(Theme.foreground, 0.10)
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.right: themeLabel.left
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: notice.text !== ""
                  ? notice.text
                  : "Space preview   ·   M matte   ·   T trim   ·   V favourite   ·   Del trash   ·   / search"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: notice.text !== "" ? Theme.accent : root.shade(Theme.foreground, 0.42)
            Behavior on color { ColorAnimation { duration: 180 } }
        }

        Text {
            id: themeLabel
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: Library.scanning ? "Scanning…" : Theme.themeName
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: root.shade(Theme.foreground, 0.32)
        }
    }

    DetailSheet {
        id: detail
        onActionTriggered: function (id) {
            if (id !== "play" && id !== "view") {
                detail.close()
            }
            root.perform(id, detail.path)
        }
    }

    MatteSheet {
        id: matteSheet
    }

    SettingsSheet {
        id: settingsSheet
        onRescanRequested: Library.refresh()
    }

    ConfirmSheet {
        id: confirm
        confirmLabel: "Move to Trash"
        onAccepted: {
            if (root.pendingDeleteBatch.length > 0) {
                let moved = 0
                for (const path of root.pendingDeleteBatch) {
                    if (Actions.moveToTrash(path)) {
                        moved++
                    }
                }
                root.say("Moved " + moved + (moved === 1 ? " capture" : " captures") + " to Trash")
                library.clearChecked()
            } else if (Actions.moveToTrash(root.pendingDeletePath)) {
                root.say("Moved to Trash")
            }
            root.pendingDeletePath = ""
            root.pendingDeleteBatch = []
            Library.refresh()
        }
    }

    // Transient status line, so a result or a missing handler is reported where
    // the user is already looking rather than swallowed.
    QtObject {
        id: notice
        property string text: ""
    }

    Timer {
        id: noticeTimer
        interval: 4200
        onTriggered: notice.text = ""
    }

    Connections {
        target: Actions
        function onFailed(message) { root.say(message) }
        function onReported(message) { root.say(message) }
    }

    Connections {
        target: Matte
        function onComposed(outputPath) {
            root.say("Matte copied and saved beside the original")
            Library.refresh()
        }
        function onFailed(message) { root.say(message) }
    }

    // Shortcuts. All disabled while a sheet is open, which owns its own keys.
    Shortcut {
        sequences: ["M"]
        enabled: !root.anySheetOpen
        onActivated: root.perform("matte", root.currentPath())
    }
    Shortcut {
        sequences: ["T"]
        enabled: !root.anySheetOpen
        onActivated: root.perform("trim", root.currentPath())
    }
    Shortcut {
        sequences: ["P"]
        enabled: !root.anySheetOpen
        onActivated: root.perform("play", root.currentPath())
    }
    Shortcut {
        sequences: ["A"]
        enabled: !root.anySheetOpen
        onActivated: root.perform("annotate", root.currentPath())
    }
    Shortcut {
        sequences: ["C"]
        enabled: !root.anySheetOpen
        onActivated: root.perform("ocr", root.currentPath())
    }
    Shortcut {
        sequences: ["Y"]
        enabled: !root.anySheetOpen
        onActivated: root.perform("copy", root.currentPath())
    }
    Shortcut {
        sequences: ["S"]
        enabled: !root.anySheetOpen
        onActivated: root.perform("send", root.currentPath())
    }
    Shortcut {
        sequences: ["V"]
        enabled: !root.anySheetOpen
        onActivated: root.perform("favorite", root.currentPath())
    }
    Shortcut {
        sequences: ["H"]
        enabled: !root.anySheetOpen
        onActivated: root.perform("hide", root.currentPath())
    }
    Shortcut {
        sequences: ["F"]
        enabled: !root.anySheetOpen
        onActivated: root.perform("files", root.currentPath())
    }
    Shortcut {
        sequences: ["R"]
        enabled: !root.anySheetOpen
        onActivated: Library.refresh()
    }
    Shortcut {
        sequences: ["/"]
        enabled: !root.anySheetOpen
        onActivated: filters.focusSearch()
    }
    Shortcut {
        sequences: ["Ctrl+A"]
        enabled: !root.anySheetOpen
        onActivated: library.checkAll()
    }
    Shortcut {
        sequences: [StandardKey.Quit]
        onActivated: Qt.quit()
    }
    Shortcut {
        sequences: ["Escape"]
        enabled: !root.anySheetOpen
        onActivated: {
            if (library.checkedCount > 0) {
                library.clearChecked()
            } else {
                root.close()
            }
        }
    }
}
