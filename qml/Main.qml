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

    // Called only by the opt-in startup trace, before scene synchronization.
    // A decode failure is settled for slideshow purposes, but is not ready media.
    function startupReadiness() {
        if (modalOpen || popupOpen) return 0
        if (detail.visible) {
            return detail.imageReady && InitialPaths.length > 0
                    && detail.path === InitialPaths[0] ? 2 : 0
        }
        return !Library.scanning && library.viewportReady() ? 4 : 0
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function formatCount(value) {
        return Number(value).toLocaleString(Qt.locale(), "f", 0)
    }

    // Destructive actions act on a path captured when the sheet opened, not on
    // whatever is selected when it is confirmed. A rescan or sort change can
    // move the selection under a modal, and trashing the wrong file is
    // unforgivable.
    property var pendingDeleteTarget: ({})
    property var pendingDuplicateKeep: ({})
    property var pendingDeleteBatch: []
    // While a collection rename is in flight the old name is gone from the
    // model before the new one is known, so the filter-clearing handlers are
    // held off and onSaved repoints the active filter instead.
    property bool renamingCollection: false
    property string renameFilterBefore: ""
    property int visibilityBeforeViewerFullScreen: Window.Windowed
    property bool viewerFolderOnly: false

    // A modal above the library, and possibly above the viewer. Whatever sits
    // under an open sheet is disabled outright: pointer handlers take passive
    // grabs, so a scrim alone does not stop a tap on a sheet's control from
    // also reaching the tile, pill or viewer button under it.
    readonly property bool modalOpen: confirm.visible || permanentConfirm.visible || matteSheet.visible
                                      || correctionSheet.visible
                                      || batchSheet.visible
                                      || compareSheet.visible
                                      || libraryBrowser.visible || settingsSheet.visible
                                      || albumNameSheet.visible
                                      || exportSheet.visible || renameSheet.visible
                                      || tailscaleSheet.visible || textReviewSheet.visible
                                      || editorChooser.visible
    readonly property bool anySheetOpen: modalOpen || detail.visible
    // A popup menu is up; the press that closes it must not also land under it.
    readonly property bool popupOpen: filters.menuOpen || albumActionMenu.visible || contextMenu.visible || detail.contextMenuOpen
    // A menu takes the keyboard while it is up and does not give it back to
    // the grid on its own, which left the arrow keys dead until a tile was
    // clicked.
    onPopupOpenChanged: {
        if (!popupOpen && !anySheetOpen) {
            library.forceActiveFocus()
        }
    }

    function say(message) {
        notice.text = message
        noticeTimer.restart()
    }

    function restoreFocusAfterSheet() {
        Qt.callLater(function () {
            if (detail.visible) {
                detail.restoreFocus()
            } else if (!root.modalOpen) {
                library.forceActiveFocus()
            }
        })
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

    function frameStamp(milliseconds) {
        const value = Math.max(0, Math.floor(milliseconds))
        const hours = Math.floor(value / 3600000)
        const minutes = Math.floor((value % 3600000) / 60000)
        const seconds = Math.floor((value % 60000) / 1000)
        const millis = value % 1000
        const pad = function (number, width) { return String(number).padStart(width, "0") }
        return (hours > 0 ? pad(hours, 2) + "h" : "")
               + pad(minutes, 2) + "m" + pad(seconds, 2) + "s" + pad(millis, 3)
    }

    function openExport(paths, knownVideo) {
        if (paths.length === 0) {
            return
        }
        if (!Registry.available("export")) {
            root.say("omarchy-transcode comes with Omarchy and is not on this system")
            return
        }
        const firstRow = Captures.rowOf(paths[0])
        if (firstRow < 0 || Captures.isDocumentAt(firstRow)) {
            root.say("PDF documents cannot be converted here")
            return
        }
        const video = knownVideo === undefined ? Captures.isVideoAt(firstRow) : knownVideo
        for (const path of paths) {
            const row = Captures.rowOf(path)
            if (row < 0 || Captures.isDocumentAt(row)
                    || Captures.isVideoAt(row) !== video) {
                root.say("Select only pictures or only videos to convert together")
                return
            }
        }
        exportSheet.open(paths, video)
    }

    // Every action funnels through here so a keystroke, a click in the detail
    // sidebar and a bulk operation all take the same path.
    function perform(id, path, knownVideo) {
        root.performForTargets(id, path, knownVideo, undefined)
    }

    function performForTargets(id, path, knownVideo, targetPaths) {
        if (path === "") {
            return
        }

        // A letter pressed on the wrong medium: say so rather than hand a PNG
        // to omacut or an mp4 to the matte composer.
        const row = Captures.rowOf(path)
        const video = knownVideo === undefined ? Captures.isVideoAt(row) : knownVideo
        const document = row >= 0 && Captures.isDocumentAt(row)
        if (id !== "open" && id !== "companion" && id !== "preview" && id !== "organize" && id !== "permanent-delete" && !Registry.appliesToKind(id, video, document, path)) {
            root.say(id === "develop" || id === "choose-editor" ? "That action is for camera raws"
                     : document ? "That action does not apply to documents"
                     : video ? "That one is for screenshots and pictures"
                     : Registry.appliesToKind(id, false, false) ? "That one cannot open camera raws"
                             : "That one is for recordings and videos")
            return
        }

        switch (id) {
        case "preview":
            if (detail.visible && detail.path === path) return
            root.openDetail(row, false)
            return
        case "organize":
            albumActionMenu.targetPaths = [path]
            albumActionMenu.popup(root.contentItem, contextMenu.x, contextMenu.y)
            return
        case "companion": {
            const companion = Captures.companionPathAt(row)
            if (companion !== "") root.openPaths([companion])
            return
        }
        case "develop":
        case "choose-editor":
            editorChooser.request(path, id === "choose-editor")
            return
        case "matte":
            if (Matte.busy) { root.say("Background is still saving"); return }
            matteSheet.path = path
            matteSheet.fileName = path.substring(path.lastIndexOf("/") + 1)
            matteSheet.open()
            return
        case "corrections":
            correctionSheet.open(path, path.substring(path.lastIndexOf("/") + 1))
            return
        case "correctionsbatch": {
            const checked = targetPaths !== undefined ? targetPaths
                            : library.checkedCount > 1 ? library.checkedPaths() : []
            if (checked.length < 2) {
                root.say("Select two or more pictures to correct together")
                return
            }
            if (!root.allCorrectable(checked)) {
                root.say("Select only ordinary pictures to correct together")
                return
            }
            batchSheet.open(checked)
            return
        }
        case "compare": {
            const targets = targetPaths !== undefined ? targetPaths
                            : library.checkedCount > 1 ? library.checkedPaths() : []
            const still = root.comparisonPaths(path, targets)
            if (still.length < 2) {
                root.say("Select two or more pictures, or open one with copies to compare")
                return
            }
            compareSheet.open(still)
            return
        }
        case "tailscale":
            tailscaleSheet.open([path])
            return
        case "export":
            root.openExport([path], video)
            return
        case "rename":
            renameSheet.open(Captures.deletionPathAt(row), Captures.fileNameAt(row), path)
            return
        case "ocr":
            if (!TextIndex.available) {
                root.say("Text recognition needs tesseract")
                return
            }
            textReviewSheet.open(path, Captures.fileNameAt(row))
            return
        case "frame": {
            if (!detail.visible || detail.path !== path || !video) {
                root.say("Open a video and choose the frame first")
                return
            }
            const position = Math.max(0, detail.playbackPosition)
            Registry.runBatchWith("frame",
                                  {"seek": (position / 1000).toFixed(3),
                                   "frame": root.frameStamp(position)},
                                  [path])
            return
        }
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
        case "permanent-delete":
            root.requestPermanentDelete([path])
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
        const document = Captures.isDocumentAt(index)
        const action = document ? Registry.primaryActionForKind(false, true)
                                : (video ? Settings.videoPrimaryAction : Settings.imagePrimaryAction)
        if (action !== "preview" && (!Registry.available(action)
                || !Registry.appliesToKind(action, video, document, Captures.pathAt(index)))) {
            root.openDetail(index, false)
            root.say("Preferred action is unavailable; opened in Omaroll")
            return
        }
        root.perform(action, Captures.pathAt(index), video)
    }

    function dismissTopLayer() {
        if (confirm.visible) {
            confirm.close()
        } else if (permanentConfirm.visible) {
            permanentConfirm.close()
        } else if (editorChooser.visible) {
            editorChooser.close()
        } else if (exportSheet.visible) {
            exportSheet.close()
        } else if (renameSheet.visible) {
            renameSheet.close()
        } else if (tailscaleSheet.visible) {
            tailscaleSheet.close()
        } else if (textReviewSheet.visible) {
            textReviewSheet.close()
        } else if (libraryBrowser.visible) {
            libraryBrowser.close()
        } else if (settingsSheet.visible) {
            settingsSheet.close()
        } else if (matteSheet.visible) {
            matteSheet.close()
        } else if (correctionSheet.visible) {
            correctionSheet.close()
        } else if (batchSheet.visible) {
            batchSheet.close()
        } else if (compareSheet.visible) {
            compareSheet.close()
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
        if (InitialPaths.length > 0) {
            root.openPaths(InitialPaths)
        } else if (InitialFolderPath !== "") {
            root.openFolder(InitialFolderPath)
        }
        Qt.callLater(function () {
            if (!root.anySheetOpen && !root.popupOpen && !filters.searchActive) {
                library.forceActiveFocus()
            }
        })
    }
    Connections {
        target: Settings
        function onScanDownloadsChanged() { root.reconcileFilter() }
        function onAlbumsChanged() {
            if (root.renamingCollection || Captures.albumFilter === "") {
                return
            }
            if (Settings.albumNames.indexOf(Captures.albumFilter) < 0) {
                Captures.setAlbumFilter("", [])
            } else {
                Captures.setAlbumFilter(Captures.albumFilter,
                                        Settings.albumPaths(Captures.albumFilter))
            }
        }
        function onTagsChanged() {
            if (root.renamingCollection || Captures.tagFilter === "") {
                return
            }
            if (Settings.tagNames.indexOf(Captures.tagFilter) < 0) {
                Captures.setTagFilter("", [])
                Captures.clearSmartCollection()
            } else {
                Captures.setTagFilter(Captures.tagFilter,
                                      Settings.tagPaths(Captures.tagFilter))
            }
        }
        function onSmartCollectionsChanged() {
            if (Captures.smartCollectionFilter !== ""
                    && Settings.smartCollectionNames.indexOf(
                        Captures.smartCollectionFilter) < 0) {
                Captures.clearSmartCollection()
            }
        }
    }

    // A laptop that slept through midnight fires no timer, so the labels are
    // checked whenever the window comes back.
    onActiveChanged: if (active) Library.checkDayRollover()

    // "Open with Omaroll" on a file: once the scan that includes it lands,
    // select it and open straight into its actions.
    property string pendingInitialPath: ""
    property var viewerPaths: []
    // A file a tracked action just saved: select it once the scan lands, but
    // stay in the grid rather than opening the viewer over whatever the user
    // is doing now. Filters are only cleared when they would hide it.
    property string pendingRevealPath: ""
    property string pendingRenamedCheck: ""
    function showAllMedia(folder) {
        Captures.setExplicitPaths([])
        Captures.kindFilter = filters.kindAll
        Captures.searchText = ""
        Captures.favoritesOnly = false
        Captures.duplicatesOnly = false
        Captures.similarOnly = false
        Captures.clearDateRange()
        Captures.setTagFilter("", [])
        Captures.cameraFilter = ""
        Captures.lensFilter = ""
        Captures.minimumRating = 0
        Captures.clearSmartCollection()
        Captures.setAlbumFilter("", [])
        Captures.folderFilter = folder === undefined ? "" : folder
    }
    function openFolder(path) {
        root.viewerPaths = []
        root.pendingInitialPath = ""
        root.showAllMedia(path)
        library.forceActiveFocus()
    }
    function openPath(path) {
        root.openPaths([path])
    }
    function openPaths(paths) {
        if (paths.length === 0) return
        root.pendingInitialPath = ""
        root.viewerPaths = paths.length > 1 ? Array.from(paths) : []
        // An explicit Open With request wins over a stale library filter.
        const folder = paths.length > 1 ? ""
                       : paths[0].substring(0, paths[0].lastIndexOf("/"))
        root.showAllMedia(folder)
        Captures.setExplicitPaths(paths)
        for (const path of paths) {
            if (Settings.isHidden(path)) Captures.showHidden = true
        }
        root.pendingInitialPath = paths[0]
        root.finishPendingOpen()
    }
    function finishPendingOpen() {
        const row = Captures.rowOf(root.pendingInitialPath)
        if (row >= 0) {
            root.pendingInitialPath = ""
            library.currentIndex = row
            root.viewerFolderOnly = root.viewerPaths.length === 0
            root.openDetail(row)
        }
    }
    function refreshOpenDetail() {
        if (!detail.visible || detail.path === "") {
            return
        }
        const row = Captures.rowOf(detail.path)
        if (row < 0) {
            return
        }
        const video = Captures.isVideoAt(row)
        const document = Captures.isDocumentAt(row)
        const animated = Captures.isAnimatedAt(row)
        if (detail.isVideo !== video || detail.isDocument !== document
                || detail.isAnimatedImage !== animated) {
            const path = detail.path
            detail.path = ""
            detail.isVideo = video
            detail.isDocument = document
            detail.isAnimatedImage = animated
            detail.path = path
            detail.open()
        }
        detail.fileName = Captures.fileNameAt(row)
        detail.kind = Captures.kindAt(row)
        detail.kindLabel = Captures.kindLabelAt(row)
        detail.dayLabel = Captures.dayLabelAt(row)
        detail.timeLabel = Captures.timeLabelAt(row)
        detail.sizeLabel = Captures.sizeLabelAt(row)
        detail.stamp = Captures.stampAt(row)
        detail.canNavigate = root.adjacentViewerPath(detail.path, 1) !== ""
        if (!detail.isVideo && !detail.isDocument) {
            Qr.inspect(detail.path)
        }
    }
    Connections {
        target: Captures
        function onDataChanged() { root.refreshOpenDetail() }
        function onCountChanged() {
            if (root.pendingRenamedCheck !== "" && Captures.rowOf(root.pendingRenamedCheck) >= 0) {
                const renamed = root.pendingRenamedCheck
                root.pendingRenamedCheck = ""
                Qt.callLater(function() {
                    if (Captures.rowOf(renamed) >= 0 && !library.isChecked(renamed)) library.toggleChecked(renamed)
                })
            }
            if (detail.visible) {
                const detailRow = Captures.rowOf(detail.path)
                if (detailRow < 0) {
                    detail.close()
                    root.say("That file is no longer available")
                } else {
                    root.refreshOpenDetail()
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
                    const revealPath = root.pendingRevealPath
                    root.pendingRevealPath = ""
                    // GridView preserves the old current item while proxy rows
                    // move, one polish after this signal. Select the new file
                    // after that adjustment so it cannot overwrite us.
                    Qt.callLater(function () {
                        const stableRow = Captures.rowOf(revealPath)
                        if (stableRow >= 0) {
                            library.currentIndex = stableRow
                        }
                    })
                }
            }
            if (root.pendingInitialPath === "") {
                return
            }
            const row = Captures.rowOf(root.pendingInitialPath)
            if (row >= 0) {
                root.finishPendingOpen()
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
    function checkedHasDocuments() {
        const count = library.checkedCount
        if (count === 0) {
            return false
        }
        return library.checkedPaths().some(function (path) {
            const row = Captures.rowOf(path)
            return row >= 0 && Captures.isDocumentAt(row)
        })
    }
    // Stars go to the file in the viewer, else the selection, else the
    // highlighted tile. Zero clears.
    function rate(stars) {
        const paths = detail.visible ? [detail.path]
                      : library.checkedCount > 0 ? library.checkedPaths()
                                                 : [root.currentPath()]
        if (paths.length === 0 || paths[0] === "") {
            return
        }
        Settings.setRating(paths, stars)
        root.say(stars > 0 ? "Rated " + "★".repeat(stars)
                           : "Rating cleared")
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
        function onMarksChanged() {
            root.marksVersion++
            // The star in the viewer's sidebar follows a favourite toggled
            // while it is open.
            detail.favorite = Settings.isFavorite(detail.path)
            detail.rating = Settings.rating(detail.path)
            detail.caption = Settings.caption(detail.path)
        }
    }

    function captureTrashTarget(path) {
        const entry = Captures.deletionPathAt(Captures.rowOf(path))
        const target = Actions.captureFileAction(entry, path)
        if (target.path !== undefined) target.viewedPath = path
        return target
    }

    function requestDelete(path) {
        if (path === "") return
        const target = root.captureTrashTarget(path)
        if (target.path === undefined) return
        root.pendingDeleteTarget = target
        root.pendingDeleteBatch = []
        root.pendingDuplicateKeep = ({})
        confirm.title = "Move this item to Trash?"
        confirm.detail = target.path
        confirm.open()
    }

    function requestDeleteBatch(paths, title, detail) {
        if (paths.length === 0) return
        const targets = []
        for (const path of paths) {
            const target = root.captureTrashTarget(path)
            if (target.path === undefined) return
            targets.push(target)
        }
        root.pendingDeleteTarget = ({})
        root.pendingDeleteBatch = targets
        root.pendingDuplicateKeep = ({})
        confirm.title = title === undefined
                        ? "Move " + paths.length + " items to Trash?" : title
        confirm.detail = detail === undefined
                         ? paths.length + " files. They stay recoverable from your file manager."
                         : detail
        confirm.open()
    }

    function requestPermanentDelete(paths) {
        if (paths.length === 0 || root.modalOpen || root.popupOpen) return
        // Copy the targets now. A rescan or selection change cannot retarget the prompt.
        const targets = []
        const seen = new Set()
        for (const path of paths) {
            const entry = Captures.deletionPathAt(Captures.rowOf(path))
            if (entry === "" || seen.has(entry)) continue
            seen.add(entry)
            const deletion = Actions.capturePermanentDelete(entry, path)
            if (deletion.parent === undefined) return
            targets.push({path: entry, viewedPath: path, deletion: deletion})
        }
        if (targets.length === 0) return
        if (!Settings.confirmPermanentDelete) {
            root.deletePermanently(targets)
            return
        }
        permanentConfirm.paths = targets
        permanentConfirm.title = targets.length === 1 ? "Permanently delete this file?"
                                 : "Permanently delete " + targets.length + " files?"
        const names = targets.slice(0, 3).map(function(target) { return target.path.substring(target.path.lastIndexOf("/") + 1) })
        permanentConfirm.detail = "This skips Trash and cannot be undone.\n\n"
                                 + (targets.length === 1 ? targets[0].path : names.join("\n")
                                    + (targets.length > 3 ? "\nand " + (targets.length - 3) + " more selected files" : ""))
        permanentConfirm.open()
    }

    function deletePermanently(paths) {
        let deleted = 0
        const failed = []
        for (const target of paths) {
            if (Actions.deleteCapturedPermanently(target.deletion)) deleted++
            else failed.push(target.viewedPath)
        }
        if (deleted > 0) {
            root.say(deleted === paths.length
                     ? "Permanently deleted " + deleted + (deleted === 1 ? " file" : " files")
                     : "Permanently deleted " + deleted + " of " + paths.length + " files; some could not be deleted")
        }
        // Failed targets remain selected so a partial result is visible and retryable.
        library.clearChecked()
        for (const path of failed) library.toggleChecked(path)
        Library.refresh()
    }

    function keepSelectedDuplicate(path) {
        if (!Duplicates.ready) {
            root.say("Wait for the exact-copy check to finish")
            return
        }
        const others = Duplicates.otherCopies(path)
        if (others.length === 0) {
            root.say("No other exact copies remain")
            return
        }
        const keep = root.captureTrashTarget(path)
        if (keep.path === undefined) return
        const name = path.substring(path.lastIndexOf("/") + 1)
        root.requestDeleteBatch(others,
                                "Keep " + name + " and trash " + others.length
                                + (others.length === 1 ? " copy?" : " copies?"),
                                "Only byte-for-byte identical copies are moved. They remain recoverable from your file manager.")
        if (confirm.visible) root.pendingDuplicateKeep = keep
    }

    // Called by --render so a screenshot can be taken of a specific view
    // without anyone having to drive the UI by hand.
    function openViewForRender(view) {
        if (library.count === 0) {
            return
        }
        library.currentIndex = 0
        if (view === "context-menu") {
            root.openContextMenu(0, 100, 100)
        } else if (view === "selection-menu") {
            library.toggleChecked(Captures.pathAt(0))
            library.toggleChecked(Captures.pathAt(1))
            root.openContextMenu(0, 100, 100)
        } else if (view === "detail") {
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
                    detail.videoPausedForRender = true
                    break
                }
            }
        } else if (view === "document") {
            // The demo library's PDF, so the page surfaces are rendered and
            // reviewed like every other view.
            for (let row = 0; row < library.count; row++) {
                if (Captures.isDocumentAt(row)) {
                    library.currentIndex = row
                    root.openDetail(row)
                    break
                }
            }
        } else if (view === "matte") {
            root.perform("matte", Captures.pathAt(0))
        } else if (view === "corrections") {
            root.perform("corrections", Captures.pathAt(0))
        } else if (view === "compare") {
            const stills = []
            for (let row = 0; row < library.count && stills.length < 2; ++row) {
                if (!Captures.isVideoAt(row) && !Captures.isDocumentAt(row)) {
                    stills.push(Captures.pathAt(row))
                }
            }
            compareSheet.open(stills)
        } else if (view === "export") {
            root.perform("export", Captures.pathAt(0))
        } else if (view === "rename") {
            root.perform("rename", Captures.pathAt(0))
        } else if (view === "ocr") {
            root.perform("ocr", Captures.pathAt(0))
        } else if (view === "duplicates") {
            Captures.duplicatesOnly = true
        } else if (view === "browser") {
            libraryBrowser.open()
        } else if (view === "settings") {
            settingsSheet.open()
        } else if (view === "editors") {
            editorChooser.openFor("Camera photo.ARW")
        } else if (view === "pins") {
            const path = Captures.pathAt(0)
            Settings.pinFolderPath(path.substring(0, path.lastIndexOf("/")))
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
        enabled: !root.anySheetOpen && !albumActionMenu.visible

        Row {
            id: headerTitle
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, header.width - headerActions.width - 52)
            spacing: 12

            Text {
                anchors.verticalCenter: parent.verticalCenter
                id: appTitle
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
                objectName: "libraryStatus"
                width: Math.max(0, headerTitle.width - appTitle.width - 25)
                elide: Text.ElideRight
                text: {
                    if (Duplicates.scanning && Captures.duplicatesOnly) {
                        return Duplicates.total > 0
                               ? "Finding exact duplicates · " + Duplicates.completed
                                 + " of " + Duplicates.total
                               : "Finding exact duplicates"
                    }
                    if (Similarities.scanning && Captures.similarOnly) {
                        return Similarities.total > 0
                               ? "Finding similar pictures · " + Similarities.completed
                                 + " of " + Similarities.total
                               : "Finding similar pictures"
                    }
                    if (TextIndex.indexing && Captures.searchText !== "") {
                        return TextIndex.total > 0
                               ? "Searching image text · " + TextIndex.completed
                                 + " of " + TextIndex.total
                               : "Searching image text…"
                    }
                    if (Library.scanning && Captures.count === 0) {
                        return "Scanning…"
                    }
                    if (library.checkedCount > 0) {
                        return library.checkedCount + " selected"
                    }
                    if (MediaMetadata.indexing) {
                        return MediaMetadata.total > 0
                               ? "Reading media details · " + MediaMetadata.completed
                                 + " of " + MediaMetadata.total
                               : "Reading media details…"
                    }
                    if (Captures.count === 0) {
                        return "Nothing to show"
                    }
                    const shown = Captures.count
                    const total = Captures.sourceCount
                    // "1 of 420 items": the noun follows the number it sits
                    // next to, which is the total once a filter narrows it.
                    const noun = (shown === total ? shown : total) === 1 ? " item" : " items"
                    return shown === total
                           ? root.formatCount(shown) + noun
                           : root.formatCount(shown) + " of " + root.formatCount(total) + noun
                }
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: library.checkedCount > 0 ? Theme.accent : Theme.mutedText
                Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
            }
        }

        Row {
            id: headerActions
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            PillButton {
                objectName: "selectionActionsButton"
                visible: library.checkedCount > 0
                label: "Actions"
                onClicked: {
                    root.prepareContextMenu(library.checkedPaths())
                    contextMenu.popup(this, 0, height + 4)
                }
            }
            // Bulk actions appear only while a selection exists, so the resting
            // header stays quiet. Each acts on every checked file at once.
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0 && root.width >= 700
                label: "Send"
                shortcut: Registry.shortcutFor("send")
                onClicked: Registry.runBatch("send", library.checkedPaths())
            }
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0 && root.width >= 1050
                         && Registry.available("export") && !root.checkedHasDocuments()
                label: "Export"
                shortcut: Registry.shortcutFor("export")
                onClicked: root.openExport(library.checkedPaths())
            }
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0 && root.width >= 900
                         && Registry.available("tailscale")
                label: "Tailscale"
                onClicked: tailscaleSheet.open(library.checkedPaths())
            }
            PillButton {
                id: albumActionButton
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0 && root.width >= 700
                label: "Organize"
                onClicked: {
                    albumActionMenu.targetPaths = library.checkedPaths().slice()
                    if (albumActionMenu.visible) albumActionMenu.close()
                    else albumActionMenu.popup(albumActionButton, 0, albumActionButton.height + 4)
                }
            }
            // The three secondary ones give way on a narrow window rather
            // than run into the title; every one is still reachable by key.
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0 && root.width >= 900
                label: "Copy"
                shortcut: Registry.shortcutFor("copy")
                onClicked: Actions.copyUris(library.checkedPaths())
            }
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0 && root.width >= 900
                label: root.allChecked(function (p) { return Settings.isFavorite(p) })
                       ? "Unfavourite" : "Favourite"
                shortcut: Registry.shortcutFor("favorite")
                onClicked: root.markChecked("favorite")
            }
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0 && root.width >= 900
                label: root.allChecked(function (p) { return Settings.isHidden(p) })
                       ? "Unhide" : "Hide"
                shortcut: Registry.shortcutFor("hide")
                onClicked: root.markChecked("hide")
            }
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0
                label: "Trash " + library.checkedCount
                shortcut: Registry.shortcutFor("trash")
                onClicked: root.requestDeleteBatch(library.checkedPaths())
            }
            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount > 0
                label: "Clear"
                onClicked: library.clearChecked()
            }

            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: library.checkedCount === 0 && Captures.duplicatesOnly
                         && Duplicates.ready
                         && library.currentIndex >= 0 && Duplicates.groupCount > 0
                         && Duplicates.otherCopies(root.currentPath()).length > 0
                label: "Keep selected"
                toolTip: "Trash the other byte-for-byte identical copies"
                onClicked: root.keepSelectedDuplicate(root.currentPath())
            }

            DayHeader {
                anchors.verticalCenter: parent.verticalCenter
                visible: shown
                maximumWidth: Math.max(80, root.width * 0.24)
                label: library.currentDayLabel
                shown: Captures.count > 0 && library.checkedCount === 0
            }

            PillButton {
                anchors.verticalCenter: parent.verticalCenter
                label: "⚙"
                accessibleName: "Settings"
                toolTip: "Settings"
                onClicked: settingsSheet.open()
            }
        }
    }

    FilterBar {
        id: filters
        objectName: "filters"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        enabled: !root.anySheetOpen && !albumActionMenu.visible
        browserOpen: libraryBrowser.visible
        // Leaving the search field must land focus back on the grid, or the
        // arrow keys go nowhere until a tile is clicked.
        onDone: library.forceActiveFocus()
        onBrowseRequested: libraryBrowser.open()
    }

    Item {
        id: folderShortcuts
        anchors.top: filters.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: pins.hasPins || Captures.folderFilter !== "" ? 42 : 0
        enabled: !root.anySheetOpen

        PinnedFolders {
            id: pins
            objectName: "pinnedFolders"
            anchors.left: parent.left
            anchors.right: pinCurrentFolder.left
            anchors.rightMargin: 8
            height: parent.height
            onChosen: function(path) { root.openFolder(path) }
        }
        PillButton {
            id: pinCurrentFolder
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            visible: Captures.folderFilter !== ""
            width: visible ? implicitWidth : 0
            readonly property bool pinned: {
                void Settings.pinnedFolders
                return Settings.isFolderPinned(Captures.folderFilter)
            }
            label: pinned ? "Unpin folder" : "Pin folder"
            onClicked: {
                if (pinned) Settings.unpinFolder(Captures.folderFilter)
                else if (!Settings.pinFolderPath(Captures.folderFilter)) root.say("That folder is unavailable")
            }
        }
    }

    Rectangle {
        id: divider
        anchors.top: folderShortcuts.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: root.shade(Theme.foreground, 0.10)
        Behavior on color { ColorAnimation { duration: 180 } }
    }

    CaptureGrid {
        id: library
        objectName: "library"
        anchors.top: divider.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        anchors.topMargin: 8
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.bottomMargin: 4
        focus: !root.anySheetOpen
        enabled: !root.anySheetOpen && !root.popupOpen
        // Tab walks the sections the way it walks tabs in a browser, while
        // the grid has the keyboard. The search field, sheets and menus keep
        // their own Tab because the grid never sees it then.
        onSectionStepRequested: function (step) { filters.cycleSection(step) }

        model: Captures
        visible: Captures.count > 0

        onChosen: function (index) { root.performPrimary(index) }
        // Del with a selection asks about the whole selection, like the
        // header's Trash button; the highlighted tile alone otherwise.
        onDeleteRequested: function (path) {
            if (library.checkedCount > 0) {
                root.requestDeleteBatch(library.checkedPaths())
            } else {
                root.requestDelete(path)
            }
        }
        onDetailRequested: function (index) { root.openDetail(index, false) }
        onContextRequested: function(index, x, y) { root.openContextMenu(index, x, y) }
    }

    function allCorrectable(paths) {
        return paths.every(function(path) {
            const row = Captures.rowOf(path)
            return row >= 0 && !Captures.isVideoAt(row) && !Captures.isDocumentAt(row)
                   && Registry.appliesToKind("corrections", false, false, path)
        })
    }

    function isComparableImage(path) {
        const row = Captures.rowOf(path)
        return row >= 0 && !Captures.isVideoAt(row) && !Captures.isDocumentAt(row)
    }

    function comparisonPaths(path, targets) {
        // Keep menus current when duplicate/similarity groups finish indexing.
        void Duplicates.groupCount
        void Similarities.groupCount
        let candidates = targets
        if (candidates.length < 2) {
            const copies = Duplicates.groupPaths(path)
            candidates = copies.length > 1 ? copies : Similarities.groupPaths(path)
        }
        return candidates.filter(root.isComparableImage)
    }

    function previewSelection(path) {
        const checked = library.checkedCount > 1 ? library.checkedPaths() : []
        return checked.indexOf(path) >= 0 ? checked : []
    }

    function singleEntries(path) {
        const index = Captures.rowOf(path)
        if (index < 0) return []
        const rows = Registry.actionsForKind(Captures.isVideoAt(index), Captures.isDocumentAt(index), path)
            .filter(function(row) {
                return ["qr", "correctionsbatch", "frame"].indexOf(row.id) < 0
                    && (row.id !== "compare" || root.comparisonPaths(path, [path]).length > 1)
            }).map(function(row) {
                if (row.id === "favorite") row.label = Settings.isFavorite(path) ? "Unfavourite" : "Favourite"
                if (row.id === "hide") row.label = Settings.isHidden(path) ? "Unhide" : "Hide"
                return row
            })
        rows.unshift({id: "preview", label: "Open in Omaroll", available: true, group: "File"})
        rows.push({id: "organize", label: "Albums and tags…", available: true, group: "Organize"})
        return rows
    }

    function selectionEntries(paths) {
        if (paths.length === 0) return []
        if (paths.length === 1) return root.singleEntries(paths[0])
        const rows = [
            {id: "copy", label: "Copy files", group: "File", available: Registry.available("copy"), hint: "wl-clipboard"},
            {id: "send", label: "Send with LocalSend", group: "Tools and sharing", available: Registry.available("send"), hint: "localsend"},
            {id: "tailscale", label: "Send to a machine", group: "Tools and sharing", available: Registry.available("tailscale"), hint: "tailscale"}]
        const first = Captures.rowOf(paths[0])
        const canExport = paths.every(function(path) {
            const row = Captures.rowOf(path)
            return row >= 0 && !Captures.isDocumentAt(row)
                   && Captures.isVideoAt(row) === Captures.isVideoAt(first)
        })
        if (paths.length > 1 && paths.every(root.isComparableImage))
            rows.push({id: "compare", label: "Compare side by side", group: "Edit and finish", available: true, shortcut: Registry.shortcutFor("compare")})
        if (paths.every(function(path) {
            const row = Captures.rowOf(path)
            return row >= 0 && Registry.appliesToKind("print", Captures.isVideoAt(row), Captures.isDocumentAt(row), path)
        })) rows.push({id: "print", label: "Print selected files", group: "File", available: Registry.available("print"), hint: "cups"})
        if (canExport) rows.push({id: "export", label: "Convert / resize", group: "Edit and finish", available: Registry.available("export"), hint: "Omarchy"})
        if (paths.length > 1 && root.allCorrectable(paths)) rows.push({id: "correctionsbatch", label: "Correct selected pictures", group: "Edit and finish", available: true})
        rows.push({id: "organize", label: "Albums and tags…", group: "Organize"},
                  {id: "favorite", label: paths.every(function(p) { return Settings.isFavorite(p) }) ? "Unfavourite" : "Favourite", group: "Organize"},
                  {id: "hide", label: paths.every(function(p) { return Settings.isHidden(p) }) ? "Unhide" : "Hide", group: "Organize"},
                  {id: "trash", label: "Move to Trash", group: "Organize"})
        return rows.map(function(row) { row.shortcut = Registry.shortcutFor(row.id); return row })
    }

    function prepareContextMenu(paths) {
        contextMenu.targets = paths.slice()
        const path = contextMenu.targets[0]
        const index = Captures.rowOf(path)
        const batch = contextMenu.targets.length > 1
        contextMenu.selectionMode = batch
        const rows = root.selectionEntries(contextMenu.targets)
        contextMenu.entries = [{id: "title", label: batch ? contextMenu.targets.length + " selected files" : path.substring(path.lastIndexOf("/") + 1), available: false, hint: "", group: "Title"}]
            .concat(contextMenu.grouped(rows))
        // Append the asynchronous action without rebuilding or moving existing rows.
        if (!batch) {
            const qr = Registry.actionsForKind(Captures.isVideoAt(index), Captures.isDocumentAt(index), path)
                .find(function(row) { return row.id === "qr" })
            if (qr) contextMenu.entries = contextMenu.entries.concat([
                {separator: true, conditional: true}, Object.assign({}, qr, {conditional: true})])
            Qr.inspect(path)
        }
    }

    function openContextMenu(index, x, y) {
        const path = Captures.pathAt(index)
        if (path === "") return
        root.prepareContextMenu(library.isChecked(path) ? library.checkedPaths() : [path])
        contextMenu.popup(root.contentItem, x, y)
    }

    function performContextAction(id, paths, batch) {
        if (!batch && paths.length === 1) { root.performForTargets(id, paths[0], undefined, paths); return }
        switch (id) {
        case "organize":
            albumActionMenu.targetPaths = paths.slice()
            albumActionMenu.popup(root.contentItem, contextMenu.x, contextMenu.y); break
        case "copy": Actions.copyUris(paths); break
        case "print": Registry.runBatch("print", paths); break
        case "send": Registry.runBatch("send", paths); break
        case "tailscale": tailscaleSheet.open(paths); break
        case "export": root.openExport(paths); break
        case "compare":
        case "correctionsbatch": root.performForTargets(id, paths[0], undefined, paths); break
        case "favorite": Settings.setFavorite(paths, !paths.every(function(p) { return Settings.isFavorite(p) })); break
        case "hide": Settings.setHidden(paths, !paths.every(function(p) { return Settings.isHidden(p) })); break
        case "trash": root.requestDeleteBatch(paths); break
        }
    }

    ActionMenu {
        id: contextMenu
        objectName: "libraryContextMenu"
        property var targets: []
        property bool selectionMode: false
        conditionalEntriesVisible: !selectionMode && targets.length === 1
                                   && Qr.path === targets[0] && Qr.detected
        onTriggered: function(id) { root.performContextAction(id, targets.slice(), selectionMode) }
        onClosed: {
            if (!detail.visible) Qr.clear()
            if (!root.anySheetOpen && !root.popupOpen) library.forceActiveFocus()
        }
    }

    function adjacentViewerPath(path, direction) {
        if (root.viewerPaths.length > 0) {
            const start = root.viewerPaths.indexOf(path)
            if (start < 0) return ""
            for (let step = 1; step < root.viewerPaths.length; step++) {
                const index = (start + direction * step + root.viewerPaths.length)
                              % root.viewerPaths.length
                const candidate = root.viewerPaths[index]
                if (Captures.rowOf(candidate) >= 0) return candidate
            }
            return ""
        }
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
            root.viewerPaths = []
        }
        // The player and the still both bind on path together with isVideo.
        // Clearing the path first means neither ever sees the new path paired
        // with the previous kind, which handed an image to the video player
        // for a moment on every step from a recording to a picture.
        detail.finishCaptionEdit()
        detail.path = ""
        detail.isVideo = Captures.isVideoAt(index)
        detail.isDocument = Captures.isDocumentAt(index)
        detail.isAnimatedImage = Captures.isAnimatedAt(index)
        detail.path = Captures.pathAt(index)
        detail.fileName = Captures.fileNameAt(index)
        detail.kind = Captures.kindAt(index)
        detail.kindLabel = Captures.kindLabelAt(index)
        detail.dayLabel = Captures.dayLabelAt(index)
        detail.timeLabel = Captures.timeLabelAt(index)
        detail.sizeLabel = Captures.sizeLabelAt(index)
        detail.stamp = Captures.stampAt(index)
        detail.favorite = Settings.isFavorite(detail.path)
        detail.rating = Settings.rating(detail.path)
        detail.caption = Settings.caption(detail.path)
        detail.canNavigate = root.adjacentViewerPath(detail.path, 1) !== ""
        detail.open()
        if (detail.isVideo || detail.isDocument) {
            Qr.clear()
        } else {
            Qr.inspect(detail.path)
        }
    }

    function navigateDetail(direction) {
        // Shuffled slideshow picks any other eligible picture rather than the
        // next one in order.
        if (detail.slideshowRunning && Settings.slideshowShuffle) {
            const count = root.viewerPaths.length || Captures.count
            const eligible = []
            for (let index = 0; index < count; ++index) {
                const candidate = root.viewerPaths.length ? root.viewerPaths[index]
                                                          : Captures.pathAt(index)
                if (candidate === detail.path) {
                    continue
                }
                if (root.viewerFolderOnly
                        && candidate.substring(0, candidate.lastIndexOf("/"))
                           !== detail.path.substring(0, detail.path.lastIndexOf("/"))) {
                    continue
                }
                const candidateRow = Captures.rowOf(candidate)
                if (candidateRow < 0) {
                    continue
                }
                if (!Settings.slideshowVideos && Captures.isVideoAt(candidateRow)) {
                    continue
                }
                eligible.push(candidateRow)
            }
            if (eligible.length > 0) {
                const row = eligible[Math.floor(Math.random() * eligible.length)]
                library.currentIndex = row
                root.openDetail(row)
                return
            }
            detail.setSlideshow(false)
            return
        }
        let path = root.adjacentViewerPath(detail.path, direction)
        let row = Captures.rowOf(path)
        if (detail.slideshowRunning && !Settings.slideshowVideos) {
            let checked = 0
            const limit = root.viewerPaths.length || Captures.count
            while (row >= 0 && Captures.isVideoAt(row) && checked < limit) {
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
        visible: Captures.count === 0 && !Library.scanning && !TextIndex.indexing
                 && !(Captures.duplicatesOnly && Duplicates.scanning)
                 && !(Captures.similarOnly && Similarities.scanning)

        readonly property bool filtered: Captures.sourceCount > 0
        readonly property bool folderScoped: Captures.folderFilter !== ""
        readonly property bool albumScoped: Captures.albumFilter !== ""
        readonly property bool tagScoped: Captures.tagFilter !== ""
        readonly property bool dateScoped: Captures.dateFrom !== ""
                                           || Captures.modifiedAfter !== ""
        readonly property bool smartScoped: Captures.smartCollectionFilter !== ""
        readonly property int scopedItems: albumScoped
                                           ? Settings.albumPaths(Captures.albumFilter).length
                                           : folderScoped
                                             ? Captures.folderItemCount(Captures.folderFilter) : 0
        readonly property bool scopeFilteredOut: (albumScoped || folderScoped)
                                                 && scopedItems > 0

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: Captures.duplicatesOnly ? "No exact duplicates"
                  : Captures.similarOnly ? "No similar pictures"
                  : parent.smartScoped ? "No items match this saved view"
                  : parent.tagScoped ? "This tag has no available items"
                  : parent.dateScoped ? "No media in this date range"
                  : parent.albumScoped
                    ? (parent.scopeFilteredOut ? "Nothing matches in this album"
                                                : "This album is empty")
                  : parent.folderScoped
                    ? (parent.scopeFilteredOut ? "Nothing matches in this folder"
                                                : "No media in this folder")
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
            text: Captures.duplicatesOnly
                  ? "Files are compared by content. Nothing is removed automatically."
                  : Captures.similarOnly
                  ? "Pictures are compared visually. Review every suggestion before removing anything."
                  : parent.scopeFilteredOut
                  ? "Try a different media type, turn off Favourites, or clear the search."
                  : parent.albumScoped
                  ? "Choose Whole library from Browse, select files, then use Organize. "
                    + "Unavailable files remain remembered."
                  : parent.tagScoped || parent.dateScoped || parent.smartScoped
                  ? "Choose another view from Browse or clear a filter."
                  : parent.folderScoped
                  ? "The folder may be empty, unavailable, or contain no supported media."
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
                  : "Enter details   ·   Space act   ·   M background   ·   T trim   ·   V favourite   ·   Del trash   ·   / search"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: notice.text !== "" || making !== ""
                   ? Theme.accent : Theme.mutedText
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
            color: Theme.mutedText
        }
    }

    DetailSheet {
        id: detail
        canCompare: root.comparisonPaths(detail.path,
            root.previewSelection(detail.path)).length > 1
        onRateRequested: function (stars) { root.rate(stars) }
        onCaptionEdited: function (text) {
            Settings.setCaption(detail.path, text)
            root.say(text !== "" ? "Caption saved" : "Caption removed")
        }
        onStatusRequested: function (message) { root.say(message) }
        objectName: "detail"
        selectionLabel: root.viewerPaths.indexOf(detail.path) < 0 ? ""
                        : (root.viewerPaths.indexOf(detail.path) + 1)
                          + " of " + root.viewerPaths.length + " selected files"
        enabled: !root.modalOpen && !root.popupOpen
        qrDetected: Qr.path === detail.path && Qr.detected
        onVisibleChanged: if (!visible) Qr.clear()
        onNavigateRequested: function (direction) { root.navigateDetail(direction) }
        onFullScreenRequested: function (enabled) { root.setViewerFullScreen(enabled) }
        // Anything that leaves the file where it is keeps the viewer open,
        // with the result said inside it. Favouriting a picture and being
        // dropped back into the grid read as a mistake. What removes the
        // file from view (trash, hide), opens another sheet (matte) or hands
        // off to an editor still closes it.
        readonly property var keepsViewer: ["preview", "play", "view", "open-document", "frame", "background", "export", "favorite",
                                            "copy", "ocr", "qr", "send", "tailscale", "files", "open-with"]
        onActionTriggered: function (id) {
            const targets = ["compare", "correctionsbatch"].indexOf(id) >= 0
                ? root.previewSelection(detail.path) : undefined
            if (id === "matte" && Matte.busy) { root.say("Background is still saving"); return }
            if (detail.keepsViewer.indexOf(id) < 0) {
                detail.close()
            }
            root.performForTargets(id, detail.path, detail.isVideo, targets)
        }
        status: notice.text
    }
    AlbumNameSheet {
        id: albumNameSheet
        objectName: "albumNameSheet"
        onSaved: function (name, count, mode) {
            if (mode === "albumRename") {
                if (root.renameFilterBefore === albumNameSheet.renameFrom) {
                    Captures.setAlbumFilter(name, Settings.albumPaths(name))
                }
                root.renamingCollection = false
                root.renameFilterBefore = ""
                root.say("Renamed album to " + name)
                return
            }
            if (mode === "tagRename") {
                if (root.renameFilterBefore === albumNameSheet.renameFrom) {
                    Captures.setTagFilter(name, Settings.tagPaths(name))
                }
                root.renamingCollection = false
                root.renameFilterBefore = ""
                root.say("Renamed tag to " + name)
                return
            }
            if (mode === "smart") {
                const view = Settings.smartCollection(name)
                Captures.applyView(name, view,
                                   view.tag ? Settings.tagPaths(String(view.tag)) : [])
                root.say("Saved smart collection " + name)
            } else if (mode === "tag" && count === 0) {
                Captures.setTagFilter(name, Settings.tagPaths(name))
                root.say("Created tag " + name)
            } else if (count === 0) {
                Captures.folderFilter = ""
                Captures.setAlbumFilter(name, Settings.albumPaths(name))
                root.say("Created album " + name)
            } else {
                root.say("Added " + count + (count === 1 ? " item" : " items")
                         + " to " + name)
            }
        }
        // A cancelled rename must not leave the filter handlers held off.
        onVisibleChanged: if (!visible) {
            root.renamingCollection = false
            root.renameFilterBefore = ""
            root.restoreFocusAfterSheet()
        }
    }

    Menu {
        id: albumActionMenu
        property var targetPaths: []
        objectName: "albumActionMenu"
        // Modal, undimmed: the press that closes the menu is consumed here
        // rather than also landing on the tile or pill under it.
        modal: true
        dim: false

        background: Rectangle {
            implicitWidth: 220
            color: root.shade(Theme.background, 0.97)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.16)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        }

        MenuItem {
            id: removeFromAlbumRow
            text: "Remove from " + Captures.albumFilter
            Accessible.name: text
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
                const paths = albumActionMenu.targetPaths
                Settings.removeFromAlbum(Captures.albumFilter, paths)
                library.clearChecked()
                root.say("Removed " + paths.length + (paths.length === 1 ? " item" : " items"))
            }
        }

        Repeater {
            model: Settings.albumNames

            MenuItem {
                id: addAlbumRow
                text: "Add to " + addAlbumRow.modelData
                Accessible.name: text
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
                    const paths = albumActionMenu.targetPaths
                    const changed = Settings.addToAlbum(addAlbumRow.modelData, paths)
                    root.say(changed ? "Added to " + addAlbumRow.modelData
                                     : "Already in " + addAlbumRow.modelData)
                }
            }
        }

        MenuItem {
            id: newAlbumRow
            text: "+ New album"
            Accessible.name: text
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
            onTriggered: albumNameSheet.open(albumActionMenu.targetPaths)
        }

        MenuSeparator {
            visible: Settings.tagNames.length > 0 || Captures.tagFilter !== ""
        }

        MenuItem {
            id: removeTagRow
            text: "Remove tag " + Captures.tagFilter
            Accessible.name: text
            visible: Captures.tagFilter !== ""
            height: visible ? 30 : 0
            contentItem: Text {
                text: "Remove tag " + Captures.tagFilter
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.foreground
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
            }
            background: Rectangle {
                color: removeTagRow.hovered
                       ? root.shade(Theme.foreground, 0.08) : "transparent"
            }
            onTriggered: {
                const paths = albumActionMenu.targetPaths
                Settings.removeTag(Captures.tagFilter, paths)
                library.clearChecked()
                root.say("Removed tag from " + paths.length
                         + (paths.length === 1 ? " item" : " items"))
            }
        }

        Repeater {
            model: Settings.tagNames

            MenuItem {
                id: addTagRow
                text: "Tag as " + addTagRow.modelData
                Accessible.name: text
                required property string modelData
                height: 30
                contentItem: Text {
                    text: "Tag as " + addTagRow.modelData
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.foreground
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 12
                }
                background: Rectangle {
                    color: addTagRow.hovered
                           ? root.shade(Theme.foreground, 0.08) : "transparent"
                }
                onTriggered: {
                    const paths = albumActionMenu.targetPaths
                    const changed = Settings.addTag(addTagRow.modelData, paths)
                    root.say(changed ? "Tagged as " + addTagRow.modelData
                                     : "Already tagged as " + addTagRow.modelData)
                }
            }
        }

        MenuItem {
            id: newTagRow
            text: "+ New tag"
            Accessible.name: text
            height: 30
            contentItem: Text {
                text: "+ New tag"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.accent
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
            }
            background: Rectangle {
                color: newTagRow.hovered
                       ? root.shade(Theme.foreground, 0.08) : "transparent"
            }
            onTriggered: albumNameSheet.open(albumActionMenu.targetPaths, "tag")
        }

    }

    MatteSheet {
        id: matteSheet
        objectName: "matteSheet"
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }

    CorrectionSheet {
        id: correctionSheet
        objectName: "correctionSheet"
        onCopied: root.say("Region copied to the clipboard")
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }

    BatchCorrectionSheet {
        id: batchSheet
        objectName: "batchCorrectionSheet"
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }

    CompareSheet {
        id: compareSheet
        objectName: "compareSheet"
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }

    ExportSheet {
        id: exportSheet
        objectName: "exportSheet"
        onExported: function (count, description) {
            root.say("Converting " + (count === 1 ? "1 file" : count + " files")
                     + " to " + description)
            library.clearChecked()
        }
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }

    RenameSheet {
        id: renameSheet
        objectName: "renameSheet"
        onRenamed: function (oldPath, newPath, fileName) {
            if (library.isChecked(oldPath)) {
                library.toggleChecked(oldPath)
                root.pendingRenamedCheck = renameSheet.renamedMediaPath
            }
            Settings.relocatePath(oldPath, newPath)
            Library.addExtraFiles([newPath])
            root.viewerPaths = root.viewerPaths.map(function (path) {
                return path === oldPath ? newPath : path
            })
            root.pendingRevealPath = renameSheet.renamedMediaPath
            root.say("Renamed to " + fileName)
            Library.refresh()
        }
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }

    TailscaleSheet {
        id: tailscaleSheet
        objectName: "tailscaleSheet"
        onSent: function (machine, count) {
            root.say("Sending " + (count === 1 ? "1 file" : count + " files") + " to " + machine)
        }
        // Opened over the viewer, it took the keyboard; hand it back.
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }
    TextReviewSheet {
        id: textReviewSheet
        objectName: "textReviewSheet"
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }

    LibraryBrowserSheet {
        id: libraryBrowser
        objectName: "libraryBrowser"
        // Browse restores focus after it closes. Open the next sheet on the
        // following event-loop turn so its text field wins that handoff.
        onCreateAlbumRequested: Qt.callLater(function () { albumNameSheet.open([]) })
        onCreateTagRequested: Qt.callLater(function () { albumNameSheet.open([], "tag") })
        onRenameCollectionRequested: Qt.callLater(function () {
            root.renamingCollection = true
            root.renameFilterBefore = mode === "album" ? Captures.albumFilter : Captures.tagFilter
            albumNameSheet.openRename(mode, name)
        })
        onSaveSmartCollectionRequested: Qt.callLater(function () {
            albumNameSheet.open([], "smart", Captures.currentView())
        })
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }

    EditorChooser {
        id: editorChooser
        objectName: "editorChooser"
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }

    SettingsSheet {
        id: settingsSheet
        objectName: "settingsSheet"
        onRescanRequested: Library.refresh()
        onCreateAlbumRequested: {
            settingsSheet.close()
            albumNameSheet.open([])
        }
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }

    ConfirmSheet {
        id: confirm
        objectName: "confirm"
        confirmLabel: "Move to Trash"
        onAccepted: {
            if (root.pendingDuplicateKeep.path !== undefined) {
                Actions.moveDuplicateCopiesToTrash(root.pendingDuplicateKeep, root.pendingDeleteBatch)
                root.say("Checking exact copies before moving them")
            } else if (root.pendingDeleteBatch.length > 0) {
                let moved = 0
                const failed = []
                for (const target of root.pendingDeleteBatch) {
                    if (Actions.moveCapturedToTrash(target)) moved++
                    else failed.push(target.viewedPath)
                }
                if (moved > 0) {
                    root.say(moved === root.pendingDeleteBatch.length
                             ? "Moved " + moved + (moved === 1 ? " item" : " items") + " to Trash"
                             : "Moved " + moved + " of " + root.pendingDeleteBatch.length + " items to Trash")
                }
                library.clearChecked()
                for (const path of failed) library.toggleChecked(path)
                if (failed.length > 0) root.say("Moved " + moved + " of " + root.pendingDeleteBatch.length
                                              + " items to Trash; " + failed.length + " could not be moved")
                Library.refresh()
            } else if (Actions.moveCapturedToTrash(root.pendingDeleteTarget)) {
                root.say("Moved to Trash")
                Library.refresh()
            }
            root.pendingDeleteTarget = ({})
            root.pendingDeleteBatch = []
            root.pendingDuplicateKeep = ({})
        }
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
    }

    ConfirmSheet {
        id: permanentConfirm
        objectName: "permanentConfirm"
        property var paths: []
        confirmLabel: "Delete permanently"
        onAccepted: {
            const targets = permanentConfirm.paths.slice()
            permanentConfirm.paths = []
            root.deletePermanently(targets)
        }
        onVisibleChanged: if (!visible) root.restoreFocusAfterSheet()
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
        function onDuplicateCleanupFinished(moved, skipped) {
            if (skipped.length === 0) root.say("Moved " + moved + " exact copies to Trash")
            library.clearChecked()
            Library.refresh()
        }
        function onFailed(message) { root.say(message) }
        function onReported(message) { root.say(message) }
        // A finished transcode lands next to its source, not at the top, so
        // show where it went: select it once the rescan brings it in.
        function onOutputSettled(path, saved) {
            if (saved) {
                Library.addExtraFiles([path])
                root.pendingRevealPath = path
            }
        }
        // The file was made earlier: open straight onto it. Selection plus a
        // footer line was too subtle to read as anything happening at all.
        function onOutputAlreadyDone(path) {
            Library.addExtraFiles([path])
            root.openPath(path)
        }
    }

    Connections {
        target: ImageEdit
        function onSaved(outputPath, jobId) {
            Library.addExtraFiles([outputPath])
            Library.refresh()
            root.say("Saved a corrected copy beside the original")
        }
        function onBatchFinished(succeeded, failed, results, jobId) {
            const outputs = []
            for (const result of results) {
                if (result.output !== "") outputs.push(result.output)
            }
            Library.addExtraFiles(outputs)
            Library.refresh()
            root.say(failed > 0
                     ? "Corrected " + succeeded + " of " + (succeeded + failed) + " files"
                     : "Saved corrected copies beside the originals")
        }
    }

    Connections {
        target: Matte
        function onSaved(outputPath) {
            Library.addExtraFiles([outputPath])
            Library.refresh()
        }
        function onFinished(outputPath, saved, copied) {
            if (copied) root.say(saved ? "Image copied and saved beside the original" : "Image copied")
        }
        function onFailed(message) { root.say(message) }
    }

    // Shortcuts. All disabled while a sheet is open, which owns its own keys.
    Shortcut {
        sequences: [Registry.shortcutFor("matte")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: root.perform("matte", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("corrections")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: root.perform("corrections", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("correctionsbatch")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: root.perform("correctionsbatch", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("compare")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: root.perform("compare", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("trim")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: root.perform("trim", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("play")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: root.perform("play", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("annotate")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: root.perform("annotate", root.currentPath())
    }
    Shortcut {
        sequences: ["D", "O"]
        enabled: !root.anySheetOpen && !root.popupOpen && !filters.searchActive
        onActivated: root.perform("develop", root.currentPath())
    }
    Shortcut {
        sequences: ["Shift+D", "Shift+O"]
        enabled: !root.anySheetOpen && !root.popupOpen && !filters.searchActive
        onActivated: root.perform("choose-editor", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("ocr")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: root.perform("ocr", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("export")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: library.checkedCount > 0
                     ? root.openExport(library.checkedPaths())
                     : root.perform("export", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("rename")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: root.perform("rename", root.currentPath())
    }
    // With a selection, the keys that have a bulk form act on all of it, as
    // the header buttons and a drag do; otherwise on the highlighted tile.
    Shortcut {
        sequences: [Registry.shortcutFor("copy")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: library.checkedCount > 0 ? Actions.copyUris(library.checkedPaths())
                                              : root.perform("copy", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("send")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: library.checkedCount > 0 ? Registry.runBatch("send", library.checkedPaths())
                                              : root.perform("send", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("favorite")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: library.checkedCount > 0 ? root.markChecked("favorite")
                                              : root.perform("favorite", root.currentPath())
    }
    // Undo the destructive organization marks, from the grid or the viewer.
    Shortcut {
        sequences: ["Ctrl+Z"]
        enabled: !root.modalOpen && !root.popupOpen && !filters.searchActive
                 && !detail.captionEditing && Settings.undoAvailable
        onActivated: {
            Settings.undo()
            root.say("Undone")
        }
    }
    Shortcut {
        sequences: ["Shift+Del"]
        autoRepeat: false
        enabled: !root.anySheetOpen && !root.popupOpen && !filters.searchActive
        onActivated: root.requestPermanentDelete(library.checkedCount > 0
                      ? library.checkedPaths() : [root.currentPath()])
    }
    // Ctrl rather than a bare H, which the grid uses for vim-style movement.
    // Modified keys are not swallowed by the search field, so guard it.
    Shortcut {
        sequences: [Registry.shortcutFor("hide")]
        enabled: !root.anySheetOpen && !root.popupOpen && !filters.searchActive
        onActivated: library.checkedCount > 0 ? root.markChecked("hide")
                                              : root.perform("hide", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("files")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: root.perform("files", root.currentPath())
    }
    Shortcut {
        sequences: [Registry.shortcutFor("open-with")]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: root.perform("open-with", root.currentPath())
    }
    Shortcut {
        sequences: ["R"]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: Library.refresh()
    }
    Shortcut {
        sequences: ["/"]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: filters.focusSearch()
    }
    // Number keys jump between sections, in the order the filter bar shows them.
    Repeater {
        model: 8
        Item {
            id: sectionKey
            required property int index
            Shortcut {
                sequences: [filters.sectionShortcut(sectionKey.index)]
                enabled: !root.anySheetOpen && !root.popupOpen
                onActivated: filters.selectSection(sectionKey.index)
            }
        }
    }
    // Alt with a digit rates. Plain digits already jump between sections,
    // and the viewer keeps 0 and 1 for zoom, so the modifier is the same in
    // both places.
    Repeater {
        model: 6
        Item {
            id: ratingKey
            required property int index
            Shortcut {
                sequences: ["Alt+" + ratingKey.index]
                enabled: !root.modalOpen && !root.popupOpen && !filters.searchActive
                onActivated: root.rate(ratingKey.index)
            }
        }
    }
    Shortcut {
        sequences: ["Ctrl+A"]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: library.checkAll()
    }
    // Tile size, the way every browser and file manager does it.
    Shortcut {
        sequences: ["Ctrl++", "Ctrl+="]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: Settings.tileWidth += library.tileStep
    }
    Shortcut {
        sequences: ["Ctrl+-"]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: Settings.tileWidth -= library.tileStep
    }
    Shortcut {
        sequences: ["Ctrl+0"]
        enabled: !root.anySheetOpen && !root.popupOpen
        onActivated: Settings.tileWidth = 240
    }
    Shortcut {
        sequences: [StandardKey.Quit]
        onActivated: Qt.quit()
    }
    // Sheets can move focus into a child control, so Escape is owned here at
    // window scope. With no sheet, the search field still owns its Escape.
    Shortcut {
        sequences: ["Escape"]
        enabled: !root.popupOpen && (root.anySheetOpen || !filters.searchActive)
        onActivated: {
            // Unwind the viewer's editor/menu before PDF selection or preview.
            // A nested sheet keeps Escape until it closes.
            if (!root.modalOpen && detail.visible && detail.dismissTransient()) {
                return
            }
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
