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
    property int visibilityBeforeViewerFullScreen: Window.Windowed
    property bool viewerFolderOnly: false

    readonly property bool anySheetOpen: confirm.visible || detail.visible
                                         || matteSheet.visible || settingsSheet.visible
                                         || albumNameSheet.visible

    function say(message) {
        notice.text = message
        noticeTimer.restart()
    }

    function setViewerFullScreen(enabled) {
        if (enabled) {
            root.visibilityBeforeViewerFullScreen = root.visibility
            root.visibility = Window.FullScreen
        } else {
            root.visibility = root.visibilityBeforeViewerFullScreen === Window.FullScreen
                              ? Window.Windowed : root.visibilityBeforeViewerFullScreen
        }
    }

    function currentPath() { return Captures.pathAt(library.currentIndex) }

    // Every action funnels through here so a keystroke, a click in the detail
    // sidebar and a bulk operation all take the same path.
    function perform(id, path, knownVideo) {
        if (path === "") {
            return
        }

        // A letter pressed on the wrong medium: say so rather than hand a PNG
        // to omacut or an mp4 to the matte composer.
        const row = Captures.rowOf(path)
        const video = knownVideo === undefined ? Captures.isVideoAt(row) : knownVideo
        if (id !== "open" && !Registry.appliesTo(id, video)) {
            root.say(video ? "That one is for screenshots and pictures"
                           : "That one is for recordings and videos")
            return
        }

        switch (id) {
        case "matte":
            matteSheet.path = path
            matteSheet.fileName = path.substring(path.lastIndexOf("/") + 1)
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

    // Space runs the user's default action for the medium.
    function performPrimary(index) {
        if (index < 0) {
            return
        }
        const video = Captures.isVideoAt(index)
        root.perform(video ? Settings.videoPrimaryAction : Settings.imagePrimaryAction,
                     Captures.pathAt(index), video)
    }

    function dismissTopLayer() {
        if (confirm.visible) {
            confirm.close()
        } else if (settingsSheet.visible) {
            settingsSheet.close()
        } else if (matteSheet.visible) {
            matteSheet.close()
        } else if (albumNameSheet.visible) {
            albumNameSheet.close()
        } else if (detail.visible) {
            detail.dismiss()
        }
    }

    // A persisted Downloads filter with Downloads switched off would show
    // "Nothing matches" under a bar with no active pill.
    function reconcileFilter() {
        if (Captures.kindFilter === filters.kindDownload && !Settings.scanDownloads) {
            Captures.kindFilter = filters.kindAll
        }
    }
    Component.onCompleted: {
        root.reconcileFilter()
        if (InitialPath !== "") {
            root.openPath(InitialPath)
        } else if (InitialFolderPath !== "") {
            root.openFolder(InitialFolderPath)
        }
    }
    Connections {
        target: Settings
        function onScanDownloadsChanged() { root.reconcileFilter() }
        function onAlbumsChanged() {
            if (Captures.albumFilter === "") {
                return
            }
            if (Settings.albumNames.indexOf(Captures.albumFilter) < 0) {
                Captures.setAlbumFilter("", [])
            } else {
                Captures.setAlbumFilter(Captures.albumFilter,
                                        Settings.albumPaths(Captures.albumFilter))
            }
        }
    }

    // A laptop that slept through midnight fires no timer, so the labels are
    // checked whenever the window comes back.
    onActiveChanged: if (active) Library.checkDayRollover()

    // "Open with Omaroll" on a file: once the scan that includes it lands,
    // select it and open straight into its actions.
    property string pendingInitialPath: ""
    // A file a tracked action just saved: select it once the scan lands, but
    // stay in the grid rather than opening the viewer over whatever the user
    // is doing now. Filters are only cleared when they would hide it.
    property string pendingRevealPath: ""
    function showAllMedia(folder) {
        Captures.kindFilter = filters.kindAll
        Captures.searchText = ""
        Captures.favoritesOnly = false
        Captures.setAlbumFilter("", [])
        Captures.folderFilter = folder === undefined ? "" : folder
    }
    function openFolder(path) {
        root.pendingInitialPath = ""
        root.showAllMedia(path)
        library.forceActiveFocus()
    }
    function openPath(path) {
        // An explicit Open With request wins over a stale library filter.
        root.showAllMedia(path.substring(0, path.lastIndexOf("/")))
        if (Settings.isHidden(path)) {
            Captures.showHidden = true
        }
        const row = Captures.rowOf(path)
        if (row >= 0) {
            root.pendingInitialPath = ""
            library.currentIndex = row
            root.openDetail(row, true)
        } else {
            root.pendingInitialPath = path
        }
    }
    Connections {
        target: Captures
        function onCountChanged() {
            if (detail.visible) {
                const detailRow = Captures.rowOf(detail.path)
                if (detailRow < 0) {
                    detail.close()
                    root.say("That file is no longer available")
                } else {
                    detail.canNavigate = root.adjacentViewerPath(detail.path, 1) !== ""
                }
            }
            if (root.pendingRevealPath !== "") {
                let revealRow = Captures.rowOf(root.pendingRevealPath)
                if (revealRow < 0 && Library.rowOf(root.pendingRevealPath) >= 0) {
                    // Scanned in but hidden by the current view; bring the
                    // library to it the way "Open with" does.
                    root.showAllMedia(root.pendingRevealPath.substring(
                        0, root.pendingRevealPath.lastIndexOf("/")))
                    revealRow = Captures.rowOf(root.pendingRevealPath)
                }
                if (revealRow >= 0) {
                    root.pendingRevealPath = ""
                    library.currentIndex = revealRow
                }
            }
            if (root.pendingInitialPath === "") {
                return
            }
            const row = Captures.rowOf(root.pendingInitialPath)
            if (row >= 0) {
                root.openPath(root.pendingInitialPath)
            }
        }
    }

    // Bulk marks set rather than toggle: "Favourite" on a mixed selection
    // favourites everything, and only a fully favourited one reverts.
    // marksVersion is read inside allChecked() purely so bindings that call
    // it re-evaluate when a mark changes; reads inside a called function are
    // captured as dependencies.
    property int marksVersion: 0
    function allChecked(predicate) {
        void root.marksVersion
        const paths = library.checkedPaths()
        return paths.length > 0 && paths.every(predicate)
    }
    function markChecked(which) {
        const paths = library.checkedPaths()
        if (which === "favorite") {
            const on = !root.allChecked(function (p) { return Settings.isFavorite(p) })
            Settings.setFavorite(paths, on)
            root.say((on ? "Added " : "Removed ") + paths.length + (on ? " to favourites" : " from favourites"))
        } else {
            const on = !root.allChecked(function (p) { return Settings.isHidden(p) })
            Settings.setHidden(paths, on)
            root.say(on ? "Hidden " + paths.length : "Shown " + paths.length + " again")
        }
    }
    Connections {
        target: Settings
        function onMarksChanged() { root.marksVersion++ }
    }

    function requestDelete(path) {
        if (path === "") {
            return
        }
        root.pendingDeletePath = path
        root.pendingDeleteBatch = []
        confirm.title = "Move this item to Trash?"
        confirm.detail = path
        confirm.open()
    }

    function requestDeleteBatch(paths) {
        if (paths.length === 0) {
            return
        }
        root.pendingDeletePath = ""
        root.pendingDeleteBatch = paths
        confirm.title = "Move " + paths.length + " items to Trash?"
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
        } else if (view === "slideshow") {
            root.openDetail(0, false)
            detail.slideshowPausedForRender = true
            detail.slideshowRunning = true
            detail.showInfo = false
        } else if (view === "video") {
            for (let row = 0; row < library.count; row++) {
                if (Captures.isVideoAt(row)) {
                    library.currentIndex = row
                    root.openDetail(row)
                    break
                }
            }
        } else if (view === "matte") {
            root.perform("matte", Captures.pathAt(0))
        } else if (view === "settings") {
            settingsSheet.open()
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
                text: "Omaroll"
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
                    const total = Captures.sourceCount
                    const noun = shown === 1 ? " item" : " items"
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
            // header stays quiet. Each acts on every checked file at once.
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0
                label: "Send"
                onClicked: Registry.runBatch("send", library.checkedPaths())
            }
            PillButton {
                id: albumActionButton
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0
                label: "+ Album"
                onClicked: albumActionMenu.visible ? albumActionMenu.close()
                                                       : albumActionMenu.popup(
                                                             albumActionButton, 0,
                                                             albumActionButton.height + 4)
            }
            // The three secondary ones give way on a narrow window rather
            // than run into the title; every one is still reachable by key.
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0 && root.width >= 900
                label: "Copy"
                onClicked: Actions.copyUris(library.checkedPaths())
            }
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0 && root.width >= 900
                label: root.allChecked(function (p) { return Settings.isFavorite(p) })
                       ? "Unfavourite" : "Favourite"
                onClicked: root.markChecked("favorite")
            }
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0 && root.width >= 900
                label: root.allChecked(function (p) { return Settings.isHidden(p) })
                       ? "Unhide" : "Hide"
                onClicked: root.markChecked("hide")
            }
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
        // Leaving the search field must land focus back on the grid, or the
        // arrow keys go nowhere until a tile is clicked.
        onDone: library.forceActiveFocus()
        onCreateAlbumRequested: albumNameSheet.open([])
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

        onChosen: function (index) { root.performPrimary(index) }
        onDeleteRequested: function (path) { root.requestDelete(path) }
        onDetailRequested: function (index) { root.openDetail(index, false) }
    }

    function adjacentViewerPath(path, direction) {
        return root.viewerFolderOnly
               ? Captures.adjacentPathInFolder(path, direction)
               : Captures.adjacentPath(path, direction)
    }

    function openDetail(index, folderOnly) {
        if (index < 0) {
            return
        }
        if (folderOnly !== undefined) {
            root.viewerFolderOnly = folderOnly
        }
        // The player and the still both bind on path together with isVideo.
        // Clearing the path first means neither ever sees the new path paired
        // with the previous kind, which handed an image to the video player
        // for a moment on every step from a recording to a picture.
        detail.path = ""
        detail.isVideo = Captures.isVideoAt(index)
        detail.path = Captures.pathAt(index)
        detail.fileName = Captures.fileNameAt(index)
        detail.kind = Captures.kindAt(index)
        detail.kindLabel = Captures.kindLabelAt(index)
        detail.dayLabel = Captures.dayLabelAt(index)
        detail.timeLabel = Captures.timeLabelAt(index)
        detail.sizeLabel = Captures.sizeLabelAt(index)
        detail.stamp = Captures.stampAt(index)
        detail.favorite = Settings.isFavorite(detail.path)
        detail.canNavigate = root.adjacentViewerPath(detail.path, 1) !== ""
        detail.open()
    }

    function navigateDetail(direction) {
        let path = root.adjacentViewerPath(detail.path, direction)
        let row = Captures.rowOf(path)
        if (detail.slideshowRunning && !Settings.slideshowVideos) {
            let checked = 0
            while (row >= 0 && Captures.isVideoAt(row) && checked < Captures.count) {
                path = root.adjacentViewerPath(path, direction)
                row = Captures.rowOf(path)
                checked++
            }
            if (row < 0 || Captures.isVideoAt(row)) {
                detail.setSlideshow(false)
                return
            }
        }
        if (row < 0) {
            return
        }
        library.currentIndex = row
        root.openDetail(row)
    }

    // Empty state. Two different empties: nothing exists, or nothing matches
    // the current filter. Telling them apart is the difference between useful
    // and useless.
    Column {
        anchors.centerIn: library
        width: Math.min(420, parent.width - 80)
        spacing: 10
        visible: Captures.count === 0 && !Library.scanning

        readonly property bool filtered: Captures.sourceCount > 0
        readonly property bool folderScoped: Captures.folderFilter !== ""
        readonly property bool albumScoped: Captures.albumFilter !== ""

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: parent.albumScoped ? "This album is empty"
                  : parent.folderScoped ? "No media in this folder"
                  : (parent.filtered ? "Nothing matches" : "No media yet")
            font.family: Theme.fontFamily
            font.pixelSize: 16
            font.weight: Font.DemiBold
            color: Theme.brightForeground
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: parent.albumScoped
                  ? "Choose All media from Browse, select files, then use + Album. "
                    + "Unavailable files remain remembered."
                  : parent.folderScoped
                  ? "The folder may be empty, unavailable, or contain no supported images or videos."
                  : parent.filtered
                  ? "Try a different filter, or clear the search."
                  : (Theme.omarchyAvailable
                     ? "Take a screenshot with Super + Shift + S, or record from the Capture menu. "
                     : "")
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
            // A transcode runs for minutes with its output invisible, so the
            // in-flight names stay pinned here between transient notices.
            readonly property string making: {
                const pending = Actions.pendingOutputs
                if (pending.length === 0) {
                    return ""
                }
                const name = pending[0].substring(pending[0].lastIndexOf("/") + 1)
                return "Making " + name
                       + (pending.length > 1 ? " and " + (pending.length - 1) + " more…" : "…")
            }
            text: notice.text !== ""
                  ? notice.text
                  : making !== ""
                  ? making
                  : "Enter details   ·   Space act   ·   M matte   ·   T trim   ·   V favourite   ·   Del trash   ·   / search"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: notice.text !== "" || making !== ""
                   ? Theme.accent : root.shade(Theme.foreground, 0.42)
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
        onNavigateRequested: function (direction) { root.navigateDetail(direction) }
        onFullScreenRequested: function (enabled) { root.setViewerFullScreen(enabled) }
        onActionTriggered: function (id) {
            if (id !== "play" && id !== "view") {
                detail.close()
            }
            root.perform(id, detail.path, detail.isVideo)
        }
    }
    AlbumNameSheet {
        id: albumNameSheet
        onSaved: function (name, count) {
            if (count === 0) {
                Captures.folderFilter = ""
                Captures.setAlbumFilter(name, Settings.albumPaths(name))
                root.say("Created album " + name)
            } else {
                root.say("Added " + count + (count === 1 ? " item" : " items")
                         + " to " + name)
            }
        }
    }

    Menu {
        id: albumActionMenu

        background: Rectangle {
            implicitWidth: 220
            color: root.shade(Theme.background, 0.97)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.16)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        }

        MenuItem {
            id: removeFromAlbumRow
            visible: Captures.albumFilter !== ""
            height: visible ? 30 : 0
            contentItem: Text {
                text: "Remove from " + Captures.albumFilter
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.foreground
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
            }
            background: Rectangle {
                color: removeFromAlbumRow.hovered
                       ? root.shade(Theme.foreground, 0.08) : "transparent"
            }
            onTriggered: {
                const paths = library.checkedPaths()
                Settings.removeFromAlbum(Captures.albumFilter, paths)
                library.clearChecked()
                root.say("Removed " + paths.length + (paths.length === 1 ? " item" : " items"))
            }
        }

        Repeater {
            model: Settings.albumNames

            MenuItem {
                id: addAlbumRow
                required property string modelData
                height: 30
                contentItem: Text {
                    text: "Add to " + addAlbumRow.modelData
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.foreground
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 12
                }
                background: Rectangle {
                    color: addAlbumRow.hovered
                           ? root.shade(Theme.foreground, 0.08) : "transparent"
                }
                onTriggered: {
                    const paths = library.checkedPaths()
                    const changed = Settings.addToAlbum(addAlbumRow.modelData, paths)
                    root.say(changed ? "Added to " + addAlbumRow.modelData
                                     : "Already in " + addAlbumRow.modelData)
                }
            }
        }

        MenuItem {
            id: newAlbumRow
            height: 30
            contentItem: Text {
                text: "+ New album"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.accent
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
            }
            background: Rectangle {
                color: newAlbumRow.hovered
                       ? root.shade(Theme.foreground, 0.08) : "transparent"
            }
            onTriggered: albumNameSheet.open(library.checkedPaths())
        }
    }

    MatteSheet {
        id: matteSheet
    }

    SettingsSheet {
        id: settingsSheet
        onRescanRequested: Library.refresh()
        onCreateAlbumRequested: {
            settingsSheet.close()
            albumNameSheet.open([])
        }
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
                if (moved > 0) {
                    root.say(moved === root.pendingDeleteBatch.length
                             ? "Moved " + moved + (moved === 1 ? " item" : " items") + " to Trash"
                             : "Moved " + moved + " of " + root.pendingDeleteBatch.length + " items to Trash")
                }
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
        // A finished transcode lands next to its source, not at the top, so
        // show where it went: select it once the rescan brings it in.
        function onOutputSettled(path, saved) {
            if (saved) {
                root.pendingRevealPath = path
            }
        }
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
    // Ctrl rather than a bare H, which the grid uses for vim-style movement.
    // Modified keys are not swallowed by the search field, so guard it.
    Shortcut {
        sequences: ["Ctrl+H"]
        enabled: !root.anySheetOpen && !filters.searchActive
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
    // 1-7 jump between sections, in the order the filter bar shows them.
    Repeater {
        model: 7
        Item {
            id: sectionKey
            required property int index
            Shortcut {
                sequences: [String(sectionKey.index + 1)]
                enabled: !root.anySheetOpen
                onActivated: filters.selectSection(sectionKey.index)
            }
        }
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
    // Sheets can move focus into a child control, so Escape is owned here at
    // window scope. With no sheet, the search field still owns its Escape.
    Shortcut {
        sequences: ["Escape"]
        enabled: root.anySheetOpen || !filters.searchActive
        onActivated: {
            if (root.anySheetOpen) {
                root.dismissTopLayer()
            } else if (library.checkedCount > 0) {
                library.clearChecked()
            } else {
                root.close()
            }
        }
    }
}
