import QtQuick
import QtQuick.Controls.Basic
import QtMultimedia

// One capture, large, with everything you can do to it.
//
// The action list is not hard-coded here: it comes from the registry, which
// knows which already-installed tool owns each job. An action whose program is
// missing is shown greyed with the package to install rather than hidden, so
// the window is also a map of what the system can do.
Item {
    id: root

    property string path: ""
    readonly property bool captionEditing: captionField.activeFocus
    property bool canCompare: false
    readonly property bool contextMenuOpen: imageContextMenu.visible
    property int actionsRevision: 0

    Connections {
        target: Captures
        ignoreUnknownSignals: true
        function onDataChanged() { root.actionsRevision++ }
        function onRowsInserted() { root.actionsRevision++ }
        function onRowsRemoved() { root.actionsRevision++ }
        function onModelReset() { root.actionsRevision++ }
        function onPairRawJpegChanged() { root.actionsRevision++ }
    }
    Connections {
        target: Settings
        function onMarksChanged() { root.actionsRevision++ }
    }
    property string fileName: ""
    property string selectionLabel: ""
    property string kindLabel: ""
    property string dayLabel: ""
    property string timeLabel: ""
    property string sizeLabel: ""
    property bool audible: true
    property bool isVideo: false
    // Keep playback state after first use, but do not initialize the multimedia
    // backend just to browse pictures. Loader creation is synchronous.
    readonly property var player: playerLoader.item
    readonly property var audio: player ? player.audioOutput : null
    onIsVideoChanged: if (isVideo) {
        Settings.prepareVideoPlayback()
        playerLoader.active = true
    }
    property bool isDocument: false
    property int pdfPage: 1
    property int pdfMatchIndex: 0
    // True scrolls the pages continuously at the window width; false fits one
    // whole page in the stage, where the zoom and pan controls apply.
    property bool pdfFitWidth: true
    // While text selection is on, a drag on the page picks the words under the
    // pointer instead of panning or scrolling.
    property bool pdfSelectMode: false
    // Set while a scroll updates the current page, so the page-changed handler
    // does not reposition the list back onto a page boundary.
    property bool pdfScrollFromList: false
    property double stamp: 0
    property bool favorite: false
    property int rating: 0
    property string caption: ""
    property int kind: 0
    property string playbackError: ""
    property bool canNavigate: false
    property real imageZoom: 1.0
    property int imageRotation: 0
    property bool imageFlipHorizontal: false
    property bool imageFlipVertical: false
    property bool animationPlaying: true
    property real imageSourceWidth: 0
    property real imageSourceHeight: 0
    property bool stillReady: false
    readonly property bool imageReady: visible && !isVideo && !isDocument
                                       && stillLoader.item !== null
                                       && stillLoader.item.status === Image.Ready
                                       && stillLoader.width > 0 && stillLoader.height > 0
    property bool fullScreen: false
    // The inspector is on demand: a preview opens on the media alone and the
    // details surface appears only when asked for.
    property bool showInfo: false
    // The top header carries the file name and the whole action menu, so it
    // stays while the inspector is closed and in full screen. A slideshow is
    // the one mode that shows nothing but the media.
    readonly property bool headerShown: !root.slideshowRunning
    // Codec, bitrate and the rest of the stream table sit behind a disclosure.
    property bool technicalExpanded: false
    property bool slideshowRunning: false
    property bool slideshowPausedForRender: false
    property bool videoPausedForRender: false
    property bool qrDetected: false
    property bool resumeVideoAfterRestore: false
    // The window's status line, repeated here: the footer sits under the
    // backdrop, so a result said there while the viewer is open goes unread.
    property string status: ""
    readonly property int slideshowInterval: Settings.slideshowIntervalSeconds * 1000
    readonly property int mediaWidth: Math.round(root.isVideo ? output.sourceRect.width
                                                               : root.imageSourceWidth)
    readonly property int mediaHeight: Math.round(root.isVideo ? output.sourceRect.height
                                                                : root.imageSourceHeight)
    readonly property string dimensionsLabel: root.mediaWidth > 0 && root.mediaHeight > 0
                                               ? root.mediaWidth + " × " + root.mediaHeight : ""
    readonly property string durationLabel: root.isVideo && player && player.duration > 0
                                             ? root.formatDuration(player.duration) : ""
    readonly property real playbackPosition: player ? player.position : 0
    readonly property int playbackState: player ? player.playbackState : MediaPlayer.StoppedState
    readonly property real playbackRate: player ? player.playbackRate : 1
    readonly property real playbackVolume: audio ? audio.volume : Settings.videoVolume
    readonly property bool playbackMuted: audio ? audio.muted : Settings.videoMuted
    // A saved spot offered when a video is reopened; the marker is taken as
    // soon as the media is loaded.
    property bool resumeAvailable: false
    property real resumePosition: 0
    // Set when a video is opened, so the resume spot is offered once and not
    // again by the reload a seek causes.
    property bool resumePending: false
    // Sidecar subtitle files beside the video, and the one currently chosen.
    // External cues are drawn by omaroll; embedded tracks are drawn by the
    // backend.
    property var subtitleFiles: []
    property string externalSubtitle: ""
    property int subtitleChoice: 0
    // A timing nudge for sidecar subtitles, which are the ones omaroll draws
    // itself. Positive shows the cue later.
    property int subtitleOffsetMs: 0
    readonly property string externalCueText: {
        void Subtitles.revision
        return externalSubtitle !== "" && player
        ? Subtitles.textAt(externalSubtitle, Math.round(player.position) - root.subtitleOffsetMs)
        : ""
    }
    readonly property string subtitleOverlayText: externalSubtitle !== ""
        ? externalCueText
        : (output.videoSink ? output.videoSink.subtitleText : "")
    readonly property bool hasSubtitleChoices: (player && player.subtitleTracks.length > 0)
                                               || subtitleFiles.length > 0
    readonly property string subtitleChoiceLabel: {
        const choices = root.subtitleChoices()
        return root.subtitleChoice >= 0 && root.subtitleChoice < choices.length
                ? choices[root.subtitleChoice].label : ""
    }
    readonly property int playbackLoops: player ? player.loops : 1
    property bool isAnimatedImage: false
    readonly property real displayedImageScale: stillViewport.fittedScale * root.imageZoom
    readonly property string mediaTechnical: root.dimensionsLabel !== "" && root.durationLabel !== ""
                                             ? root.dimensionsLabel + "  ·  " + root.durationLabel
                                             : (root.dimensionsLabel !== "" ? root.dimensionsLabel
                                                                            : root.durationLabel)
    readonly property string technicalLabel: root.mediaTechnical
                                             + (root.isDocument && PdfInfo.pageCount > 0
                                                ? (root.mediaTechnical !== "" ? "  ·  " : "")
                                                  + PdfInfo.pageCount
                                                  + (PdfInfo.pageCount === 1 ? " page" : " pages")
                                                : "")
    // The stage's bottom row holds the image controls or the video transport
    // beside the slideshow group. Labels shrink to icons when the row at its
    // long labels would not fit; the widths come from hidden copies drawn in
    // the theme font, so the decision follows the font rather than a guess.
    readonly property bool compactControls: root.fullScreen
        || stage.width < stage.rowChrome + wideTopMeasure.implicitWidth
           + (root.isVideo && !root.slideshowRunning ? transport.minimumWidth
                                                      : wideImageMeasure.implicitWidth)
    // A document's rows carry words, so each gives way as its own row runs out of
    // stage. The match steppers go first, while the row they share still fits
    // whole. The page row shortens its labels before it loses anything: the arrow
    // and glyph labels cost nothing, while the page box and Copy page text are
    // only dropped when even those do not fit. The widths come from hidden copies
    // of the labels, so the decision follows the theme font rather than a guess.
    readonly property bool pdfDropMatches: root.isDocument
        && stage.width < 24 + pdfSearchMeasure.implicitWidth
    readonly property bool compactPdfSearch: root.isDocument
        && stage.width < 24 + pdfSearchCoreMeasure.implicitWidth
    readonly property bool compactPdfPage: root.isDocument
        && stage.width < 24 + pdfPageMeasure.implicitWidth
    readonly property bool narrowPdfPage: root.compactPdfPage
        && stage.width < 24 + pdfPageGlyphMeasure.implicitWidth
    readonly property var viewerShortcuts: ({
        previous: { key: Qt.Key_Left, label: "Left" },
        next: { key: Qt.Key_Right, label: "Right" },
        slideshow: { key: Qt.Key_F5, label: "F5" },
        info: { key: Qt.Key_I, label: "I" },
        fullscreen: { key: Qt.Key_F11, label: "F11" },
        fit: { key: Qt.Key_0, label: "0" },
        actual: { key: Qt.Key_1, label: "1" },
        zoomOut: { key: Qt.Key_Minus, label: "−" },
        zoomIn: { key: Qt.Key_Plus, label: "+" },
        rotate: { key: Qt.Key_R, label: "R" },
        flipHorizontal: { key: Qt.Key_H, label: "Shift+H" },
        flipVertical: { key: Qt.Key_V, label: "Shift+V" }
    })
    property bool actionNavigationActive: false
    property string focusedActionId: ""
    onQrDetectedChanged: {
        if (actionNavigationActive) {
            Qt.callLater(restoreFocus)
        }
    }

    signal actionTriggered(string id)
    signal rateRequested(int stars)
    signal captionEdited(string text)
    signal navigateRequested(int direction)
    signal fullScreenRequested(bool enabled)
    // The status line lives on the window, not here; asking for a message
    // through a signal keeps that binding intact.
    signal statusRequested(string message)

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function formatDuration(milliseconds) {
        const total = Math.max(0, Math.floor(milliseconds / 1000))
        const hours = Math.floor(total / 3600)
        const minutes = Math.floor((total % 3600) / 60)
        const seconds = total % 60
        if (hours > 0) {
            return hours + ":" + String(minutes).padStart(2, "0")
                   + ":" + String(seconds).padStart(2, "0")
        }
        return minutes + ":" + String(seconds).padStart(2, "0")
    }

    function firstAvailableAction() {
        for (let index = 0; index < actions.count; ++index) {
            if (actions.model[index].available) {
                return index
            }
        }
        return -1
    }

    function visibleActions() {
        void root.actionsRevision
        const rows = Registry.actionsForKind(root.isVideo, root.isDocument, root.path)
        const companion = Captures.companionPathAt(Captures.rowOf(root.path))
        if (companion !== "") {
            rows.unshift({id: "companion", label: "View " + companion.substring(companion.lastIndexOf(".") + 1).toUpperCase() + " companion",
                          available: true, native: true, shortcut: "", hint: "", primary: false, group: "File"})
        }
        for (const row of rows) {
            if (row.id === "favorite") row.label = Settings.isFavorite(root.path) ? "Unfavourite" : "Favourite"
            if (row.id === "hide") row.label = Settings.isHidden(root.path) ? "Unhide" : "Hide"
        }
        return rows.filter(function (row) {
            return row.id !== "correctionsbatch" && (row.id !== "qr" || root.qrDetected)
                && (row.id !== "compare" || root.canCompare)
        })
    }

    // Primary actions in the inspector; the full registry stays in the menu.
    readonly property string preferredAction: root.isDocument ? "open-document"
        : root.isVideo ? Settings.videoPrimaryAction : Settings.imagePrimaryAction
    readonly property var primaryActionIds: root.isDocument ? ["open-document"]
        : root.isVideo ? [Settings.videoPrimaryAction === "play" ? "play" : "trim", "frame"]
        : [Settings.imagePrimaryAction === "preview"
           ? (Registry.available("omaframe") && Registry.appliesToKind("omaframe", root.isVideo, root.isDocument, root.path) ? "omaframe" : "matte")
           : Settings.imagePrimaryAction, "corrections"]
    readonly property var primaryActionRows: {
        const rows = Registry.actionsForKind(root.isVideo, root.isDocument, root.path)
        const out = []
        for (const id of root.primaryActionIds) {
            for (const row of rows) {
                if (row.id === id && !out.some(function(previous) { return previous.id === id })) {
                    out.push(row)
                    break
                }
            }
        }
        return out
    }
    // Videos name the action for what it is; the registry's "Play" is mpv, and
    // must not read as embedded playback.
    readonly property var primaryActionLabels: ({ frame: "Save frame" })
    function primaryLabel(row) {
        return root.primaryActionLabels[row.id] !== undefined
               ? root.primaryActionLabels[row.id] : row.label
    }

    function invokeAction(id) {
        // Nested sheets restore the focused row when they return. Immediate
        // actions finish the menu interaction and return to the preview.
        if (actionNavigationActive && ["export", "tailscale", "ocr"].indexOf(id) < 0) {
            focusPreview()
        }
        actionTriggered(id)
    }

    function focusAction(index) {
        if (index < 0 || index >= actions.count || !actions.model[index].available) {
            return false
        }
        actionNavigationActive = true
        actions.currentIndex = index
        focusedActionId = actions.model[index].id
        actions.positionViewAtIndex(index, ListView.Contain)
        Qt.callLater(function () {
            const item = actions.itemAtIndex(index)
            if (item && root.visible && root.actionNavigationActive) {
                item.forceActiveFocus()
            }
        })
        return true
    }

    function focusFirstAction() {
        return focusAction(firstAvailableAction())
    }

    function focusActionById(id) {
        if (id === "") {
            return false
        }
        for (let index = 0; index < actions.count; ++index) {
            if (actions.model[index].id === id && actions.model[index].available) {
                return focusAction(index)
            }
        }
        return false
    }

    function focusRelativeAction(direction) {
        if (actions.count === 0) {
            return false
        }
        let index = actions.currentIndex
        for (let checked = 0; checked < actions.count; ++checked) {
            index = (index + direction + actions.count) % actions.count
            if (actions.model[index].available) {
                return focusAction(index)
            }
        }
        return false
    }

    function focusPreview() {
        actionNavigationActive = false
        focusedActionId = ""
        actions.currentIndex = -1
        forceActiveFocus()
    }

    function restoreFocus() {
        if (actionNavigationActive) {
            if (focusActionById(focusedActionId) || focusFirstAction()) {
                return
            }
        }
        focusPreview()
    }

    function open() {
        videoPausedForRender = false
        resumeAvailable = false
        resumePosition = 0
        resumePending = true
        externalSubtitle = ""
        subtitleChoice = 0
        subtitleOffsetMs = 0
        subtitleFiles = Subtitles.files(path)
        const keepActionFocus = visible && actionNavigationActive
        const previousActionId = focusedActionId
        playbackError = ""
        pdfPage = 1
        if (!visible) {
            // A preview opens on the media alone: the inspector is asked for.
            showInfo = false
            technicalExpanded = false
            slideshowRunning = false
        }
        resetImageView()
        visible = true
        if (keepActionFocus) {
            Qt.callLater(function () {
                if (!root.focusActionById(previousActionId)) {
                    root.focusFirstAction()
                }
            })
        } else {
            focusPreview()
        }
        if (slideshowRunning && isVideo) {
            Qt.callLater(function () {
                player.position = 0
                player.play()
            })
        }
    }

    function setFullScreen(enabled) {
        if (fullScreen === enabled) {
            return
        }
        fullScreen = enabled
        fullScreenRequested(enabled)
    }

    function resetImageView() {
        imageZoom = 1.0
        imageRotation = 0
        imageFlipHorizontal = false
        imageFlipVertical = false
        Qt.callLater(stillViewport.centerContent)
    }

    function adjustImageZoom(factor) {
        imageZoom = Math.max(0.001, Math.min(64, imageZoom * factor))
        Qt.callLater(stillViewport.centerContent)
    }

    function showActualImageSize() {
        if (stillViewport.fittedScale <= 0) {
            return
        }
        imageZoom = 1 / stillViewport.fittedScale
        Qt.callLater(stillViewport.centerContent)
    }

    function toggleActualImageSize() {
        if (Math.abs(root.displayedImageScale - 1.0) <= 0.001) {
            root.imageZoom = 1.0
            Qt.callLater(stillViewport.centerContent)
        } else {
            root.showActualImageSize()
        }
    }

    function rotateImage() {
        imageRotation = (imageRotation + 90) % 360
        imageZoom = 1.0
        Qt.callLater(stillViewport.centerContent)
    }

    function playVideo() {
        if (!root.player) return
        if (root.resumeAvailable) root.resumeVideo()
        else root.player.play()
    }

    function toggleVideoPlayback() {
        if (player.playbackState === MediaPlayer.PlayingState) {
            player.pause()
        } else {
            root.resumeAvailable = false
            player.play()
        }
    }

    function seekTo(milliseconds) {
        if (!root.player) return
        root.resumeAvailable = false
        root.resumePending = false
        const maximum = player.duration > 0 ? player.duration - 1 : Math.max(0, milliseconds)
        player.position = Math.max(0, Math.min(maximum, milliseconds))
    }

    function seekVideo(milliseconds) {
        root.seekTo(player.position + milliseconds)
    }

    function findInPdf() {
        PdfInfo.find(pdfSearch.text)
    }

    function stepPdfMatch(delta) {
        const matches = PdfInfo.matches
        if (matches.length === 0) {
            return
        }
        let index = root.pdfMatchIndex + delta
        if (index < 0) {
            index = matches.length - 1
        } else if (index >= matches.length) {
            index = 0
        }
        root.pdfMatchIndex = index
        root.stillReady = false
        root.pdfPage = matches[index]
    }

    function scrollPdfToPage(page) {
        if (pdfList.count === 0) {
            return
        }
        const index = Math.max(0, Math.min(page - 1, pdfList.count - 1))
        pdfList.positionViewAtIndex(index, ListView.Beginning)
    }

    function adjustVolume(amount) {
        const next = Math.max(0, Math.min(1, Settings.videoVolume + amount))
        Settings.videoVolume = next
        if (next > 0) {
            Settings.videoMuted = false
        }
    }

    function resumeVideo() {
        if (!player) {
            return
        }
        const end = player.duration > 0 ? player.duration - 1 : root.resumePosition
        root.seekTo(Math.max(0, Math.min(root.resumePosition, end)))
        player.play()
    }

    function restartVideo() {
        if (!player) {
            return
        }
        root.seekTo(0)
        Settings.clearVideoPosition(root.path)
        player.play()
    }

    function adjustPlaybackRate(amount) {
        player.playbackRate = Math.max(0.25, Math.min(4, player.playbackRate + amount))
    }

    function cyclePlaybackRate() {
        const rates = [0.5, 1, 1.25, 1.5, 2]
        for (let index = 0; index < rates.length; ++index) {
            if (player.playbackRate < rates[index] - 0.01) {
                player.playbackRate = rates[index]
                return
            }
        }
        player.playbackRate = rates[0]
    }

    // Off, then each embedded track, then each sidecar file. Cycling the one
    // list keeps a single control for both kinds.
    function subtitleChoices() {
        const choices = [{kind: "off", label: "Off", index: -1, path: ""}]
        if (player) {
            const tracks = player.subtitleTracks
            for (let i = 0; i < tracks.length; ++i) {
                choices.push({kind: "embedded", label: root.embeddedTrackLabel(tracks[i], i),
                              index: i, path: ""})
            }
        }
        for (let i = 0; i < root.subtitleFiles.length; ++i) {
            choices.push({kind: "external", label: Subtitles.label(root.subtitleFiles[i]),
                          index: -1, path: root.subtitleFiles[i]})
        }
        return choices
    }

    function embeddedTrackLabel(track, index) {
        try {
            const language = track.stringValue(QMediaMetaData.Language)
            if (language && language.length > 0) {
                return language.toUpperCase()
            }
        } catch (error) {
            // A value without stringValue falls back to the ordinal.
        }
        return "Track " + (index + 1)
    }

    function cycleSubtitleTrack() {
        const choices = subtitleChoices()
        if (choices.length <= 1) {
            return
        }
        subtitleChoice = (subtitleChoice + 1) % choices.length
        applySubtitleChoice()
    }

    function applySubtitleChoice() {
        const choices = subtitleChoices()
        if (subtitleChoice < 0 || subtitleChoice >= choices.length) {
            subtitleChoice = 0
        }
        const choice = choices[subtitleChoice]
        if (choice.kind === "embedded") {
            // The backend draws embedded tracks; clear the sidecar so the two
            // overlays cannot both show.
            player.activeSubtitleTrack = choice.index
            externalSubtitle = ""
        } else if (choice.kind === "external") {
            player.activeSubtitleTrack = -1
            externalSubtitle = choice.path
        } else {
            player.activeSubtitleTrack = -1
            externalSubtitle = ""
        }
        statusRequested(choice.kind === "off" ? "Subtitles off" : "Subtitles: " + choice.label)
    }

    function cycleAudioTrack() {
        const count = player.audioTracks.length
        if (count > 1) {
            player.activeAudioTrack = (player.activeAudioTrack + 1) % count
        }
    }

    function setSlideshow(enabled) {
        if (enabled && !canNavigate) {
            return
        }
        if (slideshowRunning === enabled) {
            return
        }
        if (enabled) {
            finishCaptionEdit()
            focusPreview()
        }
        slideshowRunning = enabled
        if (enabled) {
            resumeAvailable = false
            showInfo = false
            setFullScreen(true)
            if (isVideo && !Settings.slideshowVideos) {
                Qt.callLater(function () { root.requestNavigation(1) })
            } else if (isVideo) {
                player.position = 0
                player.play()
            } else if (stillReady) {
                slideshowTimer.restart()
            }
        } else {
            slideshowTimer.stop()
        }
    }

    function requestNavigation(direction) {
        slideshowTimer.stop()
        navigateRequested(direction)
    }

    function close() {
        finishCaptionEdit()
        setSlideshow(false)
        setFullScreen(false)
        actionNavigationActive = false
        focusedActionId = ""
        actions.currentIndex = -1
        // The selection belongs to the document that was open.
        pdfSelectMode = false
        PdfInfo.clearSelection()
        visible = false
    }

    // Leaves PDF text selection behind and says whether it did. Escape asks
    // this first, so the first Escape after a selection leaves the selection
    // and the second closes the viewer.
    function leaveTextSelection() {
        if (!pdfSelectMode) {
            return false
        }
        pdfSelectMode = false
        PdfInfo.clearSelection()
        return true
    }

    function finishCaptionEdit() {
        if (captionField.activeFocus) {
            captionField.finish()
        }
    }

    function dismissTransient() {
        if (captionField.activeFocus) {
            captionField.text = root.caption
            captionField.focus = false
            focusPreview()
            return true
        }
        if (actionNavigationActive) {
            focusPreview()
            return true
        }
        return leaveTextSelection()
    }

    function dismiss() {
        if (dismissTransient()) {
            return
        }
        if (slideshowRunning) {
            setSlideshow(false)
            setFullScreen(false)
        } else if (fullScreen) {
            setFullScreen(false)
        } else {
            close()
        }
    }

    onPathChanged: {
        stillReady = false
        animationPlaying = true
        imageSourceWidth = 0
        imageSourceHeight = 0
        // The mode and its selection belong to the document that was open.
        pdfSelectMode = false
        if (isDocument) {
            PdfInfo.inspect(path)
        } else {
            PdfInfo.clear()
            MediaInfo.inspect(path, isVideo)
        }
    }
    onStillReadyChanged: {
        if (slideshowRunning && !slideshowPausedForRender && !isVideo && stillReady) {
            slideshowTimer.restart()
        }
    }
    onCanNavigateChanged: {
        if (!canNavigate) {
            setSlideshow(false)
        }
    }
    onShowInfoChanged: {
        if (!showInfo) {
            finishCaptionEdit()
        }
    }

    visible: false
    anchors.fill: parent
    focus: visible

    // Own wheel input for the whole modal, as Settings does. The still
    // viewport zooms and the action list scrolls; both are visited first.
    // Anything they do not accept must not reach the library underneath.
    WheelHandler {
        target: null
        enabled: root.visible
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (event) { event.accepted = true }
    }

    Timer {
        id: slideshowTimer
        interval: root.slideshowInterval
        repeat: false
        running: root.visible && root.slideshowRunning && !root.isVideo
                 && !root.slideshowPausedForRender && root.canNavigate
                 && root.stillReady && stage.windowShown
        onTriggered: root.requestNavigation(1)
    }

    Timer {
        id: slideshowErrorTimer
        interval: 1500
        repeat: false
        onTriggered: if (root.slideshowRunning) root.requestNavigation(1)
    }

    Rectangle {
        id: backdrop
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.62)
        // Every button is swallowed so nothing reaches the library; only a
        // left click on the dimmed area reads as "close".
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
            onClicked: function (mouse) {
                if (mouse.button === Qt.LeftButton) {
                    root.close()
                }
            }
        }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: root.fullScreen ? root.width : Math.min(1000, root.width - 60)
        height: root.fullScreen ? root.height : Math.min(700, root.height - 60)
        radius: root.fullScreen ? 0 : (Theme.cornerRadius > 0 ? Theme.cornerRadius : 4)
        color: root.shade(Theme.background, 0.97)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
        }

        // Header. Always up except during a slideshow, so the file name, the
        // favourite, the inspector and the whole action menu stay reachable in
        // full screen and in a narrow window.
        Rectangle {
            id: previewHeader
            objectName: "viewerHeader"
            visible: root.headerShown
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 1
            anchors.leftMargin: 1
            anchors.rightMargin: 1
            height: 46
            color: root.shade(Theme.background, 0.45)

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.right: headerButtons.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1

                Text {
                    objectName: "viewerHeaderName"
                    width: Math.max(0, parent.width)
                    text: root.fileName
                    elide: Text.ElideMiddle
                    font.family: Theme.fontFamily
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Theme.brightForeground
                }

                Text {
                    objectName: "viewerSelectionLabel"
                    width: Math.max(0, parent.width)
                    visible: root.selectionLabel !== ""
                    text: root.selectionLabel
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }
            }

            Row {
                id: headerButtons
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                IconButton {
                    objectName: "viewerFavouriteButton"
                    icon: root.favorite ? "star-filled" : "star"
                    active: root.favorite
                    toolTip: root.favorite ? "Remove from favourites" : "Add to favourites"
                    shortcut: "V"
                    onClicked: root.actionTriggered("favorite")
                }
                IconButton {
                    objectName: "viewerInfoButton"
                    icon: "info"
                    active: root.showInfo
                    toolTip: root.showInfo ? "Hide details" : "Show details"
                    shortcut: root.viewerShortcuts.info.label
                    onClicked: root.showInfo = !root.showInfo
                }
                IconButton {
                    objectName: "viewerOverflowButton"
                    icon: "more"
                    active: root.actionNavigationActive
                    toolTip: "All actions"
                    onClicked: root.actionNavigationActive ? root.focusPreview()
                                                           : root.focusFirstAction()
                }
            }
        }

        // Preview
        Rectangle {
            id: stage
            anchors.left: parent.left
            anchors.top: root.headerShown ? previewHeader.bottom : parent.top
            anchors.bottom: parent.bottom
            anchors.right: sidebar.left
            anchors.margins: 1
            color: root.shade(Theme.darkerBackground, 0.85)

            // Margins around the bottom row: 16 either side, 12 between the
            // groups, 10 of padding inside the slideshow group's panel.
            readonly property int rowChrome: 16 + 12 + 10 + 16
            readonly property real videoFooterHeight: 56
                + (root.slideshowRunning ? 0 : (resumePrompt.visible ? resumePrompt.height + 10 : 0)
                   + (topControlsPanel.stacked ? 44 : 0))

            // Nothing here reads compactControls, which keeps it loop-free.
            Row {
                id: wideTopMeasure
                visible: false
                spacing: 6
                Repeater {
                    model: root.isVideo ? ["Fullscreen"]
                                        : ["Start slideshow"]
                    PillButton { label: modelData }
                }
            }
            Row {
                id: wideImageMeasure
                visible: false
                spacing: 6
                Repeater {
                    model: ["Fit", "Actual", "−", "+", "Rotate", "Flip H", "Flip V", "Full"]
                    PillButton { label: modelData }
                }
                Item { width: 42; height: 1 }
            }
            Row {
                id: compactImageMeasure
                visible: false
                spacing: 6
                Repeater {
                    model: ["Fit", "1:1", "−", "+", "↻", "↔", "↕", "⛶"]
                    PillButton { label: modelData }
                }
            }
            // The document rows at full width, and the search row as it stands
            // once the match steppers have given way. Above the document
            // threshold each of these fits beside the stage margins.
            Row {
                id: pdfSearchMeasure
                visible: false
                spacing: 8
                Rectangle { width: 220; height: 28 }
                Repeater {
                    model: ["Find", "Previous match", "Next match", "Select text", "Copy selection"]
                    PillButton { label: modelData }
                }
            }
            Row {
                id: pdfSearchCoreMeasure
                visible: false
                spacing: 8
                Rectangle { width: 220; height: 28 }
                Repeater {
                    model: ["Find", "Select text", "Copy selection"]
                    PillButton { label: modelData }
                }
            }
            Row {
                id: pdfPageMeasure
                visible: false
                spacing: 8
                Repeater {
                    model: ["Previous page", "Next page", "Fit page", "Fit width", "Copy page text"]
                    PillButton { label: modelData }
                }
                Rectangle { width: 54; height: 28 }
            }
            // The page row after the labels have shortened: the arrows and glyphs
            // cost nothing, so this is what has to fit before the page box and
            // Copy page text are dropped.
            Row {
                id: pdfPageGlyphMeasure
                visible: false
                spacing: 8
                Repeater {
                    model: ["←", "→", "⛶", "↔", "Copy page text"]
                    PillButton { label: modelData }
                }
                Rectangle { width: 54; height: 28 }
                PillButton { label: "Page 1 of 2" }
            }

            // A recording shows its thumbnail until the first decoded frame.
            Image {
                id: videoPoster
                objectName: "detailVideoPoster"
                anchors.fill: parent
                anchors.margins: 16
                anchors.bottomMargin: stage.videoFooterHeight
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true
                mipmap: true
                visible: root.isVideo && !(player && player.hasVideo)
                // The thumbnail provider applies the ratio from its URL once.
                sourceSize: Qt.size(Math.round(width), Math.round(height))
                source: !root.visible || !root.isVideo || root.path === "" ? ""
                        : "image://thumbs/" + Screen.devicePixelRatio + "@40~"
                          + root.stamp + encodeURIComponent(root.path)
            }

            // Stills fit on open, zoom to 4x without throwing away decoded
            // detail, pan naturally once larger than the viewport, and rotate
            // for inspection without ever rewriting the file.
            Flickable {
                id: stillViewport
                anchors.fill: parent
                anchors.margins: 16
                anchors.bottomMargin: 54
                visible: !root.isVideo && !(root.isDocument && root.pdfFitWidth)
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentWidth > width || contentHeight > height
                contentWidth: Math.max(width, imageCanvas.width)
                contentHeight: Math.max(height, imageCanvas.height)

                readonly property bool sideways: root.imageRotation === 90
                                                 || root.imageRotation === 270
                readonly property real sourceWidth: root.imageSourceWidth
                readonly property real sourceHeight: root.imageSourceHeight
                readonly property real fittedScale: sourceWidth > 0 && sourceHeight > 0
                    ? Math.min(width / (sideways ? sourceHeight : sourceWidth),
                               height / (sideways ? sourceWidth : sourceHeight)) : 0

                function centerContent() {
                    contentX = Math.max(0, (contentWidth - width) / 2)
                    contentY = Math.max(0, (contentHeight - height) / 2)
                    returnToBounds()
                }

                Item {
                    id: imageCanvas
                    width: (stillViewport.sideways ? stillViewport.sourceHeight
                                                   : stillViewport.sourceWidth)
                           * stillViewport.fittedScale * root.imageZoom
                    height: (stillViewport.sideways ? stillViewport.sourceWidth
                                                    : stillViewport.sourceHeight)
                            * stillViewport.fittedScale * root.imageZoom
                    x: (stillViewport.contentWidth - width) / 2
                    y: (stillViewport.contentHeight - height) / 2

                    Image {
                        id: transparencyGrid
                        objectName: "transparencyGrid"
                        anchors.centerIn: parent
                        width: stillViewport.sourceWidth * stillViewport.fittedScale
                               * root.imageZoom
                        height: stillViewport.sourceHeight * stillViewport.fittedScale
                                * root.imageZoom
                        rotation: root.imageRotation
                        visible: !root.isDocument
                        fillMode: Image.Tile
                        smooth: false
                        source: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQAQMAAAAlPW0iAAAABlBMVEVFUExmZmbID7WAAAAAEElEQVQI12NgYGQgHv3/BwAEowINzPTc0QAAAABJRU5ErkJggg=="
                    }

                    // A document's page is paper, not a transparent surface:
                    // without a sheet and an edge it would disappear into a
                    // light theme. The checkerboard stays for everything else.
                    Rectangle {
                        objectName: "pdfPageSheet"
                        anchors.centerIn: parent
                        width: transparencyGrid.width
                        height: transparencyGrid.height
                        rotation: root.imageRotation
                        visible: root.isDocument
                        color: "white"
                    }

                    Loader {
                        id: stillLoader
                        anchors.centerIn: parent
                        width: stillViewport.sourceWidth * stillViewport.fittedScale
                               * root.imageZoom
                        height: stillViewport.sourceHeight * stillViewport.fittedScale
                                * root.imageZoom
                        rotation: root.imageRotation
                        transform: Scale {
                            origin.x: stillLoader.width / 2
                            origin.y: stillLoader.height / 2
                            xScale: root.imageFlipHorizontal ? -1 : 1
                            yScale: root.imageFlipVertical ? -1 : 1
                        }
                        sourceComponent: root.isDocument ? pdfStill
                                         : root.isAnimatedImage
                                         ? animatedStill : staticStill
                    }

                    // The fitted page covers the sheet above, so its edge goes on
                    // top of it for the same reason the list's edge does.
                    Rectangle {
                        objectName: "pdfFittedPageEdge"
                        anchors.centerIn: parent
                        width: stillLoader.width
                        height: stillLoader.height
                        rotation: root.imageRotation
                        visible: root.isDocument
                        color: "transparent"
                        border.width: 1
                        border.color: root.shade(Theme.foreground, 0.45)
                    }
                }

                // Qt on Wayland can report a mouse wheel as a touchpad, which
                // the default devices left out, so those wheels never zoomed.
                // Wheels and swipes are told apart by what the events do: a
                // touchpad swipe comes in phases with pixel deltas and pans;
                // a wheel notch has no phase and zooms.
                WheelHandler {
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: function (event) {
                        if (event.phase !== Qt.NoScrollPhase
                                && (event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0)) {
                            stillViewport.contentX = Math.max(0, Math.min(
                                stillViewport.contentWidth - stillViewport.width,
                                stillViewport.contentX - event.pixelDelta.x))
                            stillViewport.contentY = Math.max(0, Math.min(
                                stillViewport.contentHeight - stillViewport.height,
                                stillViewport.contentY - event.pixelDelta.y))
                            event.accepted = true
                            return
                        }
                        const delta = event.angleDelta.y !== 0
                                      ? event.angleDelta.y : event.pixelDelta.y
                        // ScrollBegin and ScrollEnd may carry no movement.
                        // Consume them without treating zero as a zoom out.
                        if (delta === 0) {
                            event.accepted = true
                            return
                        }
                        root.adjustImageZoom(delta > 0 ? 1.2 : 1 / 1.2)
                        event.accepted = true
                    }
                }

                TapHandler {
                    onDoubleTapped: root.imageZoom > 1
                                    ? root.resetImageView() : root.adjustImageZoom(2)
                }
            }

            // Continuous PDF: the pages scroll at the window width. Zoom and
            // pan belong to the fit-page mode above.
            Item {
                id: pdfScroll
                objectName: "pdfScroll"
                anchors.fill: parent
                anchors.margins: 16
                anchors.bottomMargin: 54
                visible: root.isDocument && root.pdfFitWidth

                ListView {
                    id: pdfList
                    objectName: "pdfPageList"
                    anchors.fill: parent
                    clip: true
                    spacing: 10
                    model: PdfInfo.pageCount
                    onMovementEnded: {
                        // indexAt returns -1 in the gap between delegates, so
                        // look past the spacing before giving up.
                        let first = indexAt(contentX, contentY + 1)
                        if (first < 0) {
                            first = indexAt(contentX, contentY + spacing + 1)
                        }
                        if (first >= 0) {
                            // Mark this as a scroll-originated change so the
                            // page-changed handler does not snap the list back
                            // to the page beginning.
                            root.pdfScrollFromList = true
                            root.pdfPage = first + 1
                            root.pdfScrollFromList = false
                        }
                    }
                    delegate: Item {
                        id: pageCell
                        required property int index
                        width: pdfList.width
                        height: {
                            const image = pageImage
                            // The loaded page's own size, not the request: the
                            // request carries a width and leaves the height
                            // zero, so measuring it would collapse every cell to
                            // a sliver the moment the page arrived.
                            if (image.status === Image.Ready && image.implicitWidth > 0) {
                                return Math.max(1, width * image.implicitHeight / image.implicitWidth)
                            }
                            // A4 while the page renders, so the list has a shape.
                            return Math.max(1, width * 1.4142)
                        }
                        Rectangle {
                            anchors.fill: parent
                            color: "white"
                        }
                        Image {
                            id: pageImage
                            objectName: "pdfPageImage"
                            anchors.fill: parent
                            source: root.visible && root.pdfFitWidth && root.path !== ""
                                    ? "image://pdf/" + (pageCell.index + 1) + "~" + root.stamp
                                      + encodeURIComponent(root.path)
                                    : ""
                            // Ask for the width it is drawn at: the renderer
                            // supplies the height the page itself needs, so a
                            // tall page stays as sharp as a short one.
                            sourceSize: Qt.size(Math.max(600, Math.round(width
                                                        * Screen.devicePixelRatio)), 0)
                            asynchronous: true
                            smooth: true
                            fillMode: Image.PreserveAspectFit
                            onStatusChanged: {
                                if (!root.stillReady && (status === Image.Ready || status === Image.Error)) {
                                    root.stillReady = true
                                }
                                if (status === Image.Error) {
                                    root.playbackError = PdfInfo.available
                                                         ? "Could not display this PDF page"
                                                         : "PDF support needs Poppler"
                                }
                            }

                            PdfTextSelection {
                                objectName: "pdfSelectionScroll"
                                anchors.fill: parent
                                page: pageCell.index + 1
                                selecting: root.pdfSelectMode
                            }
                        }
                        // The page covers the cell, so its edge is drawn over the
                        // page rather than under it. White paper on a light theme
                        // has nothing else to separate it from the stage.
                        Rectangle {
                            objectName: "pdfPageEdge"
                            anchors.fill: parent
                            color: "transparent"
                            border.width: 1
                            border.color: root.shade(Theme.foreground, 0.45)
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }

            Component {
                id: staticStill
                Image {
                    id: staticImage
                    source: root.visible && !root.isVideo && root.path !== ""
                            ? Library.fileUrl(root.path) : ""
                    asynchronous: true
                    autoTransform: true
                    smooth: true
                    mipmap: true
                    fillMode: Image.Stretch
                    // A camera raw shows its embedded preview; measure it by
                    // the raw when the two have the same shape.
                    Connections {
                        target: Library
                        function onRawSizeRead(path, source, size) {
                            const still = staticImage
                            if (path !== root.path || source.toString() !== still.source.toString()
                                    || still.status !== Image.Ready
                                    || size.width <= 0 || size.height <= 0) {
                                return
                            }
                            const shown = still.sourceSize
                            if (Math.abs(size.width / size.height
                                         / (shown.width / shown.height) - 1) < 0.02) {
                                root.imageSourceWidth = size.width
                                root.imageSourceHeight = size.height
                            }
                        }
                    }
                    onStatusChanged: {
                        if (status === Image.Ready) {
                            root.imageSourceWidth = sourceSize.width
                            root.imageSourceHeight = sourceSize.height
                            root.stillReady = true
                            Library.readRawSize(root.path, source)
                        } else if (status === Image.Error) {
                            root.playbackError = "Could not display this image"
                            root.stillReady = true
                        }
                    }
                }
            }

            Component {
                id: pdfStill
                Image {
                    source: root.visible && root.isDocument && root.path !== ""
                            ? "image://pdf/" + root.pdfPage + "~" + root.stamp
                              + encodeURIComponent(root.path) : ""
                    sourceSize: Qt.size(Math.max(800, Math.round(stage.width * Screen.devicePixelRatio)),
                                        Math.max(800, Math.round(stage.height * Screen.devicePixelRatio)))
                    asynchronous: true
                    smooth: true
                    mipmap: true
                    fillMode: Image.Stretch
                    onStatusChanged: {
                        if (status === Image.Ready) {
                            // The loaded page's own size, not sourceSize: that is
                            // the request, which for this surface describes the
                            // stage box, and a page fitted to that box's aspect
                            // instead of its own would be distorted.
                            root.imageSourceWidth = implicitWidth
                            root.imageSourceHeight = implicitHeight
                            root.stillReady = true
                        } else if (status === Image.Error) {
                            root.playbackError = PdfInfo.available
                                                 ? "Could not display this PDF page"
                                                 : "PDF support needs Poppler"
                            root.stillReady = true
                        }
                    }

                    // The fit-page surface, where the page can also be zoomed
                    // and panned. The layer sits on the image, so a highlight
                    // follows both without any of that being recomputed here.
                    PdfTextSelection {
                        objectName: "pdfSelectionStill"
                        anchors.fill: parent
                        page: root.pdfPage
                        selecting: root.pdfSelectMode
                    }
                }
            }

            Component {
                id: animatedStill
                AnimatedImage {
                    objectName: "animatedImage"
                    source: root.visible && !root.isVideo && root.path !== ""
                            ? Library.fileUrl(root.path) : ""
                    asynchronous: true
                    autoTransform: true
                    cache: false
                    playing: root.visible && root.animationPlaying
                    smooth: true
                    fillMode: Image.Stretch
                    onStatusChanged: {
                        if (status === Image.Ready) {
                            root.imageSourceWidth = sourceSize.width
                            root.imageSourceHeight = sourceSize.height
                            root.stillReady = true
                        } else if (status === Image.Error) {
                            root.playbackError = "Could not display this animation"
                            root.stillReady = true
                        }
                    }
                }
            }

            // A recording plays in place with the normal controls expected of
            // a default media viewer. Specialist editing can still be handed
            // to the action list without weakening playback here.
            Loader {
                id: playerLoader
                active: false
                sourceComponent: Component {
                    MediaPlayer {
                        id: mediaPlayer
                        objectName: "videoPlayer"
                        property string resumeIdentity: ""
                        source: root.visible && root.isVideo && root.path !== ""
                                ? Library.fileUrl(root.path) : ""
                        videoOutput: videoSurface.item
                        audioOutput: AudioOutput {
                            volume: Settings.videoVolume
                            muted: Settings.videoMuted || !root.audible
                        }
                        loops: 1
                        onSourceChanged: {
                            if (source.toString() !== "") {
                                mediaPlayer.resumeIdentity = Settings.videoIdentity(root.path)
                                Mpris.track(mediaPlayer, root)
                                play()
                            } else {
                                Mpris.release(mediaPlayer)
                            }
                        }
                        // A clip the ffmpeg backend cannot decode must say so rather
                        // than sit behind a dead play button.
                        onErrorOccurred: function (error, errorString) {
                            root.playbackError = errorString !== "" ? errorString : "Could not play this file"
                            if (root.slideshowRunning) {
                                slideshowErrorTimer.restart()
                            }
                        }
                        onPositionChanged: {
                            if (root.videoPausedForRender && mediaPlayer.position >= 1000
                                    && playbackState === MediaPlayer.PlayingState) {
                                pause()
                            }
                        }
                        // Remember the spot when the user pauses; the timer
                        // covers long uninterrupted playback.
                        onPlaybackStateChanged: {
                            // Loading can start playback after LoadedMedia's
                            // pause request. Hold until the saved-spot choice.
                            if (playbackState === MediaPlayer.PlayingState && root.resumeAvailable) {
                                pause()
                                return
                            }
                            if (playbackState === MediaPlayer.PausedState && duration > 0
                                    && position >= 5000 && position < duration - 3000) {
                                Settings.setVideoPosition(root.path, Math.round(position), mediaPlayer.resumeIdentity)
                            }
                        }
                        onMediaStatusChanged: {
                            if (mediaStatus === MediaPlayer.LoadedMedia && root.isVideo
                                    && root.resumePending && !root.slideshowRunning) {
                                root.resumePending = false
                                const saved = Settings.videoPosition(root.path)
                                if (saved >= 5000 && (duration <= 0 || saved < duration - 3000)) {
                                    root.resumePosition = saved
                                    root.resumeAvailable = true
                                    pause()
                                }
                            }
                            if (mediaStatus !== MediaPlayer.EndOfMedia) {
                                return
                            }
                            if (root.slideshowRunning) {
                                root.requestNavigation(1)
                                return
                            }
                            // A finished clip stops and the backend drops its
                            // frame, leaving a blank stage that reads as broken.
                            // Rewind to the first frame and hold it, so the
                            // picture stays and the play button means replay.
                            pause()
                            position = 0
                            Settings.clearVideoPosition(root.path)
                            root.resumeAvailable = false
                            root.resumePending = false
                        }
                    }
                }
            }

            // Periodically records the playback spot, so a long clip is still
            // resumable if the window is closed without pausing first.
            Timer {
                id: resumeTimer
                interval: 5000
                repeat: true
                running: root.visible && root.isVideo && player !== null
                         && player.playbackState === MediaPlayer.PlayingState
                onTriggered: {
                    if (player.duration > 0 && player.position >= 5000
                            && player.position < player.duration - 3000) {
                        Settings.setVideoPosition(root.path, Math.round(player.position), player.resumeIdentity)
                    }
                }
            }

            // Do not decode while minimized, and preserve an intentional pause.
            readonly property bool windowShown: Window.visibility !== Window.Minimized
                                                && Window.visibility !== Window.Hidden
            onWindowShownChanged: {
                if (!root.isVideo || !root.visible) {
                    return
                }
                if (windowShown) {
                    if (root.resumeVideoAfterRestore) {
                        player.play()
                    }
                    root.resumeVideoAfterRestore = false
                } else {
                    root.resumeVideoAfterRestore =
                        player.playbackState === MediaPlayer.PlayingState
                    player.pause()
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: transport.top
                anchors.bottomMargin: 10
                visible: root.playbackError !== ""
                text: root.playbackError + (root.isVideo ? "  ·  Play opens it in mpv" : "")
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.red
            }

            Item {
                id: output
                anchors.fill: parent
                anchors.margins: 16
                anchors.bottomMargin: stage.videoFooterHeight
                visible: root.isVideo && player && player.hasVideo
                readonly property rect sourceRect: videoSurface.item
                                                   ? videoSurface.item.sourceRect : Qt.rect(0, 0, 0, 0)
                readonly property QtObject videoSink: videoSurface.item ? videoSurface.item.videoSink : null

                // VideoOutput creates a backend video sink even while hidden.
                // Defer it alongside the player, preserving the stage geometry.
                Loader {
                    id: videoSurface
                    anchors.fill: parent
                    active: playerLoader.active
                    sourceComponent: VideoOutput {
                        objectName: "videoOutput"
                        fillMode: VideoOutput.PreserveAspectFit
                    }
                }

                TapHandler {
                    onDoubleTapped: root.setFullScreen(!root.fullScreen)
                }
            }

            Text {
                objectName: "subtitleOverlay"
                anchors.horizontalCenter: output.horizontalCenter
                anchors.bottom: output.bottom
                anchors.bottomMargin: 24
                width: Math.max(0, output.width - 80)
                visible: root.isVideo && root.subtitleOverlayText !== ""
                text: root.subtitleOverlayText
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                font.family: Theme.fontFamily
                font.pixelSize: Math.max(16, Math.min(28, output.height / 24))
                font.weight: Font.DemiBold
                color: "white"
                style: Text.Outline
                styleColor: "black"
            }

            PillButton {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                visible: root.canNavigate
                label: "←"
                floating: true
                toolTip: "Previous"
                shortcut: root.viewerShortcuts.previous.label
                onClicked: root.requestNavigation(-1)
            }

            PillButton {
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                visible: root.canNavigate
                label: "→"
                floating: true
                toolTip: "Next"
                shortcut: root.viewerShortcuts.next.label
                onClicked: root.requestNavigation(1)
            }

            // A new result set jumps to its first page.
            Connections {
                target: PdfInfo
                function onMatchesChanged() {
                    if (PdfInfo.matchCount > 0) {
                        root.pdfMatchIndex = 0
                        root.stillReady = false
                        root.pdfPage = PdfInfo.matches[0]
                    } else {
                        root.pdfMatchIndex = 0
                    }
                }
                function onTextCopied(page) {
                    root.statusRequested("Copied page " + page + " text")
                }
                function onTextCopyFailed(message) {
                    root.statusRequested(message)
                }
                function onSelectionCopied(words) {
                    root.statusRequested("Copied " + words
                                         + (words === 1 ? " selected word" : " selected words"))
                }
                function onSelectionFailed(message) {
                    root.statusRequested(message)
                }
            }

            // Keep the continuous list on the current page.
            Connections {
                target: root
                function onPdfPageChanged() {
                    if (root.isDocument && root.pdfFitWidth && !root.pdfScrollFromList) {
                        root.scrollPdfToPage(root.pdfPage)
                    }
                    // One page at a time is shown when it is fitted, so a
                    // selection made on the page before would highlight nothing
                    // while still being copied. The continuous view is
                    // deliberately the other way: scrolling keeps it.
                    if (root.isDocument && !root.pdfFitWidth) {
                        PdfInfo.clearSelection()
                    }
                }
                function onPdfFitWidthChanged() {
                    if (root.isDocument && root.pdfFitWidth) {
                        root.scrollPdfToPage(root.pdfPage)
                    }
                }
            }

            // PDF text search. Page navigation stays in the row below it.
            Row {
                objectName: "pdfSearchRow"
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 100
                spacing: 8
                visible: root.isDocument && PdfInfo.available && PdfInfo.textSearchAvailable

                Rectangle {
                    width: root.compactPdfSearch ? 110 : 220
                    height: 28
                    radius: Theme.cornerRadius > 0 ? Math.min(4, Theme.cornerRadius) : 3
                    color: root.shade(Theme.foreground, 0.06)
                    border.width: 1
                    border.color: pdfSearch.activeFocus ? root.shade(Theme.accent, 0.65)
                                                        : root.shade(Theme.foreground, 0.16)

                    TextInput {
                        id: pdfSearch
                        objectName: "pdfSearchInput"
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: Theme.foreground
                        selectionColor: root.shade(Theme.accent, 0.5)
                        selectedTextColor: Theme.brightForeground
                        Keys.onReturnPressed: root.findInPdf()
                        Keys.onEnterPressed: root.findInPdf()
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        visible: pdfSearch.text === ""
                        text: "Find in document"
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: Theme.mutedText
                    }
                }
                PillButton {
                    label: "Find"
                    onClicked: root.findInPdf()
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: PdfInfo.matchCount > 0 && !root.pdfDropMatches
                    text: (root.pdfMatchIndex + 1) + " / " + PdfInfo.matchCount
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.foreground
                }
                PillButton {
                    visible: PdfInfo.matchCount > 0 && !root.pdfDropMatches
                    label: "Previous match"
                    onClicked: root.stepPdfMatch(-1)
                }
                PillButton {
                    visible: PdfInfo.matchCount > 0 && !root.pdfDropMatches
                    label: "Next match"
                    onClicked: root.stepPdfMatch(1)
                }

                // The same text work the row above does, one page's words at a
                // time. They live here rather than in the page row, which is
                // already wider than the stage it is centred in.
                PillButton {
                    objectName: "pdfSelectText"
                    label: "Select text"
                    toolTip: "Drag over the page to pick words, then copy the selection"
                    active: root.pdfSelectMode
                    onClicked: {
                        if (root.pdfSelectMode) {
                            root.leaveTextSelection()
                        } else {
                            root.pdfSelectMode = true
                            root.statusRequested("Drag over the page to select text")
                        }
                    }
                }
                PillButton {
                    objectName: "pdfCopySelection"
                    label: "Copy selection"
                    toolTip: "Copy the selected words to the clipboard"
                    // On a narrow stage the row has room for entering the mode
                    // but not for this pill; Ctrl+C still copies the selection.
                    visible: !root.compactPdfSearch
                    // It stays in place and dims until there is something to
                    // copy, so the row never shifts under the pointer.
                    enabled: PdfInfo.hasSelection
                    onClicked: PdfInfo.copySelection()
                }
            }

            Row {
                objectName: "pdfPageRow"
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 62
                spacing: 8
                visible: root.isDocument

                PillButton {
                    label: root.compactPdfPage ? "←" : "Previous page"
                    toolTip: "Previous page"
                    enabled: root.pdfPage > 1
                    onClicked: if (root.pdfPage > 1) {
                        root.stillReady = false
                        root.pdfPage--
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: PdfInfo.pageCount > 0
                          ? "Page " + root.pdfPage + " of " + PdfInfo.pageCount
                          : (PdfInfo.error !== "" ? PdfInfo.error : "Reading pages…")
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.foreground
                }
                PillButton {
                    label: root.compactPdfPage ? "→" : "Next page"
                    toolTip: "Next page"
                    enabled: PdfInfo.pageCount > 0 && root.pdfPage < PdfInfo.pageCount
                    onClicked: if (root.pdfPage < PdfInfo.pageCount) {
                        root.stillReady = false
                        root.pdfPage++
                    }
                }

                PillButton {
                    objectName: "pdfFitPage"
                    label: root.compactPdfPage ? "⛶" : "Fit page"
                    toolTip: "Fit page"
                    active: !root.pdfFitWidth
                    onClicked: root.pdfFitWidth = false
                }
                PillButton {
                    objectName: "pdfFitWidth"
                    label: root.compactPdfPage ? "↔" : "Fit width"
                    toolTip: "Fit width"
                    active: root.pdfFitWidth
                    onClicked: root.pdfFitWidth = true
                }

                PillButton {
                    objectName: "pdfCopyPageText"
                    label: "Copy page text"
                    toolTip: "Copy this page's text to the clipboard"
                    // Only the smallest stages drop it: this action has no other
                    // home, so it goes after every shortened label has been tried.
                    visible: !root.narrowPdfPage
                    enabled: PdfInfo.textSearchAvailable
                    onClicked: PdfInfo.copyPageText(root.pdfPage)
                }

                // Jump straight to a page by number.
                Rectangle {
                    width: 54
                    height: 28
                    visible: !root.narrowPdfPage
                    anchors.verticalCenter: parent.verticalCenter
                    radius: Theme.cornerRadius > 0 ? Math.min(4, Theme.cornerRadius) : 3
                    color: root.shade(Theme.foreground, 0.06)
                    border.width: 1
                    border.color: pdfPageInput.activeFocus ? root.shade(Theme.accent, 0.65)
                                                           : root.shade(Theme.foreground, 0.16)

                    TextInput {
                        id: pdfPageInput
                        objectName: "pdfPageInput"
                        anchors.fill: parent
                        horizontalAlignment: TextInput.AlignHCenter
                        verticalAlignment: TextInput.AlignVCenter
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: Theme.foreground
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { bottom: 1; top: Math.max(1, PdfInfo.pageCount) }
                        onAccepted: {
                            const value = parseInt(text)
                            if (value >= 1 && value <= PdfInfo.pageCount) {
                                root.stillReady = false
                                root.pdfPage = value
                            }
                            text = ""
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: pdfPageInput.text === ""
                        text: "Page #"
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }
            }

            Rectangle {
                id: topControlsPanel
                readonly property bool stacked: root.isVideo && !root.slideshowRunning
                                                && stage.width < transport.minimumWidth + width + 44
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.bottomMargin: stacked ? 62 + (resumePrompt.visible ? resumePrompt.height + 10 : 0) : 11
                width: topControls.implicitWidth + 10
                height: topControls.implicitHeight + 10
                radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
                color: Qt.rgba(Theme.darkerBackground.r, Theme.darkerBackground.g,
                               Theme.darkerBackground.b, 0.70)
                border.width: 1
                border.color: Qt.rgba(Theme.foreground.r, Theme.foreground.g,
                                      Theme.foreground.b, 0.16)

                Row {
                    id: topControls
                    anchors.centerIn: parent
                    spacing: 6

                    PillButton {
                        objectName: "viewerSlideshowButton"
                        visible: !root.isVideo || root.slideshowRunning
                        enabled: root.canNavigate
                        label: root.compactControls
                               ? (root.slideshowRunning ? "Ⅱ" : "▶")
                               : (root.slideshowRunning ? "Pause slideshow" : "Start slideshow")
                        toolTip: root.slideshowRunning ? "Pause slideshow" : "Start slideshow"
                        shortcut: root.viewerShortcuts.slideshow.label
                        active: root.slideshowRunning
                        onClicked: root.setSlideshow(!root.slideshowRunning)
                    }
                    PillButton {
                        visible: root.isVideo || root.slideshowRunning
                        label: root.fullScreen ? "↙" : (root.compactControls ? "⛶" : "Fullscreen")
                        toolTip: root.fullScreen ? "Exit fullscreen"
                                                 : "Fullscreen"
                        shortcut: root.viewerShortcuts.fullscreen.label
                        onClicked: root.setFullScreen(!root.fullScreen)
                    }
                }
            }

            Row {
                id: imageControls
                anchors.right: topControlsPanel.left
                anchors.rightMargin: 12
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 16
                visible: !root.isVideo && !root.slideshowRunning
                spacing: 6

                Rectangle {
                    parent: stage
                    visible: imageControls.visible
                    anchors.fill: imageControls
                    anchors.margins: -5
                    z: -1
                    radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
                    color: Qt.rgba(Theme.darkerBackground.r, Theme.darkerBackground.g,
                                   Theme.darkerBackground.b, 0.72)
                    border.width: 1
                    border.color: Qt.rgba(Theme.foreground.r, Theme.foreground.g,
                                          Theme.foreground.b, 0.18)
                }

                PillButton {
                    label: "Fit"
                    toolTip: "Fit image"
                    shortcut: root.viewerShortcuts.fit.label
                    active: root.imageZoom === 1 && root.imageRotation === 0
                            && !root.imageFlipHorizontal && !root.imageFlipVertical
                    onClicked: root.resetImageView()
                }
                PillButton {
                    objectName: "actualSizeButton"
                    visible: !root.isDocument && stage.width >= 430
                    label: root.compactControls ? "1:1" : "Actual"
                    toolTip: "Actual size (1 toggles fit and actual size)"
                    Accessible.description: "Show actual size. Press 1 to toggle fit and actual size."
                    active: Math.abs(root.displayedImageScale - 1) < 0.01
                    onClicked: root.showActualImageSize()
                }
                PillButton {
                    label: "−"
                    toolTip: "Zoom out"
                    shortcut: root.viewerShortcuts.zoomOut.label
                    onClicked: root.adjustImageZoom(1 / 1.25)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !root.compactControls
                    width: 42
                    horizontalAlignment: Text.AlignHCenter
                    text: Math.round(root.displayedImageScale * 100) + "%"
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }
                PillButton {
                    label: "+"
                    toolTip: "Zoom in"
                    shortcut: root.viewerShortcuts.zoomIn.label
                    onClicked: root.adjustImageZoom(1.25)
                }
                PillButton {
                    label: root.compactControls ? "↻" : "Rotate"
                    toolTip: "Rotate"
                    shortcut: root.viewerShortcuts.rotate.label
                    onClicked: root.rotateImage()
                }
                PillButton {
                    objectName: "flipHorizontalButton"
                    visible: !root.isDocument && stage.width >= 560
                    label: root.compactControls ? "↔" : "Flip H"
                    toolTip: "Flip horizontally"
                    shortcut: root.viewerShortcuts.flipHorizontal.label
                    active: root.imageFlipHorizontal
                    onClicked: root.imageFlipHorizontal = !root.imageFlipHorizontal
                }
                PillButton {
                    objectName: "flipVerticalButton"
                    visible: !root.isDocument && stage.width >= 560
                    label: root.compactControls ? "↕" : "Flip V"
                    toolTip: "Flip vertically"
                    shortcut: root.viewerShortcuts.flipVertical.label
                    active: root.imageFlipVertical
                    onClicked: root.imageFlipVertical = !root.imageFlipVertical
                }
                PillButton {
                    // At the minimum window width with details shown, even the
                    // icon row is a few pixels too wide. F11 still works.
                    visible: !root.compactControls
                             || stage.width >= stage.rowChrome + compactImageMeasure.implicitWidth
                                               + topControls.implicitWidth
                    label: root.fullScreen ? "↙" : (root.compactControls ? "⛶" : "Full")
                    toolTip: root.fullScreen ? "Exit fullscreen"
                                             : "Fullscreen"
                    shortcut: root.viewerShortcuts.fullscreen.label
                    onClicked: root.setFullScreen(!root.fullScreen)
                }
            }

            // Reserve a row below the picture for the saved spot, so the
            // choice never covers video content or stacked playback controls.
            Flow {
                id: resumePrompt
                objectName: "resumePrompt"
                anchors.left: parent.left
                anchors.leftMargin: 16
                width: parent.width - 32
                anchors.bottom: transport.top
                anchors.bottomMargin: 10
                spacing: 8
                visible: root.isVideo && root.resumeAvailable && !root.slideshowRunning

                PillButton {
                    objectName: "resumeButton"
                    label: "Resume " + transport.clock(root.resumePosition)
                    active: true
                    onClicked: root.resumeVideo()
                }
                PillButton {
                    objectName: "restartVideoButton"
                    label: "Restart"
                    onClicked: root.restartVideo()
                }
            }

            // Transport: play/pause, scrub, clock, tracks, speed, and sound.
            Item {
                id: transport
                visible: root.isVideo && !root.slideshowRunning
                anchors.left: parent.left
                anchors.right: topControlsPanel.stacked ? parent.right : topControlsPanel.left
                anchors.bottom: parent.bottom
                anchors.leftMargin: 16
                anchors.rightMargin: topControlsPanel.stacked ? 16 : 12
                anchors.bottomMargin: 16
                height: 40

                // Keep the scrub useful at the minimum window width. Track
                // selectors appear only when the file has alternatives.
                readonly property int scrubMinimum: 120
                // Measure offered controls independently of their visibility,
                // so fitting the row never feeds its width back into itself.
                readonly property real fullMediaWidth: speedButton.implicitWidth + 6
                    + soundButton.implicitWidth
                    + (player && player.audioTracks.length > 1 ? audioButton.implicitWidth + 4 : 0)
                    + (root.hasSubtitleChoices ? subtitleButton.implicitWidth + 4 : 0)
                readonly property real minimumWidth: playButton.width + 14 + scrubMinimum
                                                     + 14 + clockLabel.implicitWidth + 12
                                                     + (subtitleOffsetOffered
                                                        ? subtitleOffsetWidth + 8 : 0)
                                                     + fullMediaWidth
                // Whether a sidecar's timing nudge is on offer at all, apart
                // from the room it gets: the top controls stack on this
                // minimum, and the nudge's visibility follows that width.
                readonly property bool subtitleOffsetOffered: root.externalSubtitle !== ""
                                                              && root.hasSubtitleChoices
                // Measured from the parts, which keep their size while hidden,
                // so the nudge's own visibility can depend on it. The clock
                // makes room for the label as shown; the nudge itself counts
                // the label at its widest, so a click that shows it cannot
                // push the nudge off the row.
                readonly property real subtitleOffsetWidth: subtitleEarlier.implicitWidth + 6
                                                            + subtitleLater.implicitWidth
                                                            + (subtitleOffsetLabel.visible
                                                               ? 6 + subtitleOffsetLabel.implicitWidth : 0)
                readonly property real subtitleOffsetWidest: subtitleEarlier.implicitWidth + 6
                                                             + subtitleLater.implicitWidth + 6
                                                             + Math.ceil(Math.max(widestEarlier.advanceWidth,
                                                                                  widestLater.advanceWidth))

                function clock(ms) {
                    const total = Math.max(0, Math.round(ms / 1000))
                    const minutes = Math.floor(total / 60)
                    const seconds = total % 60
                    return minutes + ":" + (seconds < 10 ? "0" : "") + seconds
                }

                IconButton {
                    id: playButton
                    objectName: "videoPlayButton"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    height: 30
                    icon: root.playbackState === MediaPlayer.PlayingState ? "pause" : "play"
                    iconSize: 16
                    toolTip: root.playbackState === MediaPlayer.PlayingState ? "Pause" : "Play"
                    shortcut: "Space"
                    onClicked: root.toggleVideoPlayback()
                }

                Item {
                    id: scrub
                    objectName: "videoSeek"
                    anchors.left: playButton.right
                    anchors.right: clockLabel.visible ? clockLabel.left
                                 : subtitleOffsetControls.visible ? subtitleOffsetControls.left
                                 : mediaControls.left
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height

                    readonly property real fraction: player && player.duration > 0
                                                     ? player.position / player.duration : 0

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        height: scrubHover.hovered || scrubDrag.active ? 6 : 4
                        radius: height / 2
                        color: root.shade(Theme.foreground, 0.18)
                        Behavior on height { NumberAnimation { duration: 100 } }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: parent.width * scrub.fraction
                            radius: parent.radius
                            color: Theme.accent
                        }
                    }

                    function seekTo(x) {
                        if (player && player.duration > 0) {
                            root.resumeAvailable = false
                            player.position = Math.max(0, Math.min(1, x / width)) * player.duration
                        }
                    }

                    HoverHandler { id: scrubHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: function (point) { scrub.seekTo(point.position.x) } }
                    DragHandler {
                        id: scrubDrag
                        target: null
                        onCentroidChanged: if (active) scrub.seekTo(centroid.position.x)
                    }
                }

                Text {
                    id: clockLabel
                    objectName: "videoClock"
                    // The clock goes first when the transport is short of room.
                    visible: transport.width >= transport.minimumWidth
                    anchors.right: subtitleOffsetControls.visible ? subtitleOffsetControls.left
                                                                  : mediaControls.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: transport.clock(root.playbackPosition) + " / " + transport.clock(player ? player.duration : 0)
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }

                // A timing nudge for sidecar subtitles only, which omaroll draws.
                // It sits between the clock and the track controls, and leaves
                // with the CC button it belongs to, or before the scrub would
                // shrink below its minimum, when the row runs short.
                Row {
                    id: subtitleOffsetControls
                    objectName: "subtitleOffsetControls"
                    visible: root.externalSubtitle !== "" && subtitleButton.width > 0
                             && transport.width >= playButton.width + 14
                                + transport.scrubMinimum + 14
                                + transport.subtitleOffsetWidest + 8
                                + transport.fullMediaWidth
                    anchors.right: mediaControls.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    // First in the row, which is anchored on its right, so
                    // the label grows leftward and the buttons stay put.
                    Text {
                        id: subtitleOffsetLabel
                        objectName: "subtitleOffsetLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: root.subtitleOffsetMs !== 0
                        text: (root.subtitleOffsetMs > 0 ? "+" : "")
                              + (root.subtitleOffsetMs / 1000).toFixed(1) + "s"
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: Theme.mutedText

                        TextMetrics {
                            id: widestEarlier
                            font: subtitleOffsetLabel.font
                            text: "-10.0s"
                        }
                        TextMetrics {
                            id: widestLater
                            font: subtitleOffsetLabel.font
                            text: "+10.0s"
                        }
                    }
                    PillButton {
                        id: subtitleEarlier
                        objectName: "subtitleEarlier"
                        label: "Sub −"
                        toolTip: "Show subtitles earlier"
                        onClicked: root.subtitleOffsetMs = Math.max(-10000, root.subtitleOffsetMs - 500)
                    }
                    PillButton {
                        id: subtitleLater
                        objectName: "subtitleLater"
                        label: "Sub +"
                        toolTip: "Show subtitles later"
                        onClicked: root.subtitleOffsetMs = Math.min(10000, root.subtitleOffsetMs + 500)
                    }
                }

                // Explicit anchors rather than a Row: an optional control that
                // appears late (a sidecar subtitle) was not being laid out by
                // the positioner and ended up on top of its neighbour. Widths
                // collapse to zero when a control is unavailable, so the chain
                // still closes up, and the pill hides with its width: a pill
                // with no width still paints its label, over the clock.
                Item {
                    id: mediaControls
                    objectName: "videoMediaControls"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    implicitHeight: 30
                    readonly property int audioGap: audioButton.width > 0 && subtitleButton.width > 0
                                                    ? 4 : 0
                    readonly property int subtitleGap: subtitleButton.width > 0 ? 4 : 0
                    implicitWidth: audioButton.width + audioGap + subtitleButton.width
                                   + subtitleGap + speedButton.width + 6 + soundButton.width

                    // A timestamped icon row rather than a line of outlined
                    // mini pills: the track selectors appear only when the
                    // file offers alternatives and collapse to zero width
                    // otherwise, which keeps the chain closed.
                    IconButton {
                        id: audioButton
                        objectName: "audioTrackButton"
                        visible: width > 0
                        width: player && player.audioTracks.length > 1 && transport.width >= 520
                               ? implicitWidth : 0
                        height: 30
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        label: "Audio " + (player ? player.activeAudioTrack + 1 : 1)
                        toolTip: "Switch audio track"
                        onClicked: root.cycleAudioTrack()
                    }
                    IconButton {
                        id: subtitleButton
                        objectName: "subtitleButton"
                        visible: width > 0
                        width: root.hasSubtitleChoices && transport.width >= 440
                               ? implicitWidth : 0
                        height: 30
                        anchors.left: audioButton.right
                        anchors.leftMargin: mediaControls.audioGap
                        anchors.verticalCenter: parent.verticalCenter
                        icon: "captions"
                        iconSize: 16
                        toolTip: root.subtitleChoice > 0
                                 ? "Subtitles: " + root.subtitleChoiceLabel
                                 : "Choose subtitles"
                        active: root.subtitleChoice > 0
                        onClicked: root.cycleSubtitleTrack()
                    }
                    IconButton {
                        id: speedButton
                        anchors.left: subtitleButton.right
                        anchors.leftMargin: mediaControls.subtitleGap
                        anchors.verticalCenter: parent.verticalCenter
                        objectName: "playbackSpeedButton"
                        height: 30
                        label: Number(root.playbackRate.toFixed(2)) + "×"
                        toolTip: "Playback speed"
                        shortcut: "[  ]"
                        active: Math.abs(root.playbackRate - 1) > 0.01
                        onClicked: root.cyclePlaybackRate()
                    }
                    IconButton {
                        id: soundButton
                        anchors.left: speedButton.right
                        anchors.leftMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        objectName: "videoSoundButton"
                        height: 30
                        icon: root.playbackMuted || root.playbackVolume === 0
                              ? "volume-muted" : "volume"
                        iconSize: 16
                        toolTip: root.playbackMuted ? "Unmute"
                                 : "Volume " + Math.round(root.playbackVolume * 100) + "% · Mute"
                        shortcut: "M"
                        active: !root.playbackMuted
                        onClicked: Settings.videoMuted = !Settings.videoMuted
                    }
                }
            }

            Rectangle {
                id: statusToast
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 14
                width: Math.min(statusText.implicitWidth + 28, stage.width - 32)
                height: statusText.implicitHeight + 14
                radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
                color: Qt.rgba(Theme.darkerBackground.r, Theme.darkerBackground.g,
                               Theme.darkerBackground.b, 0.86)
                border.width: 1
                border.color: Qt.rgba(Theme.foreground.r, Theme.foreground.g,
                                      Theme.foreground.b, 0.18)
                opacity: root.visible && root.status !== "" ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutQuad } }

                Text {
                    id: statusText
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.width - 28)
                    elide: Text.ElideRight
                    text: root.status
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Theme.accent
                }
            }

            Rectangle {
                id: slideshowVideoProgress
                visible: root.isVideo && root.slideshowRunning && player && player.duration > 0
                anchors.left: parent.left
                anchors.right: topControlsPanel.left
                anchors.leftMargin: 16
                anchors.rightMargin: 12
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 27
                height: 2
                radius: 1
                color: root.shade(Theme.foreground, 0.14)

                Rectangle {
                    width: player && player.duration > 0
                           ? parent.width * Math.max(0, Math.min(1, player.position / player.duration)) : 0
                    height: parent.height
                    radius: parent.radius
                    color: root.shade(Theme.accent, 0.78)
                }
            }

        }

        // Inspector. On demand, scrollable and spaced: the media keeps the
        // room the old metadata-and-action wall used to take.
        Item {
            id: sidebar
            objectName: "viewerInspector"
            visible: root.showInfo
            anchors.right: parent.right
            anchors.top: root.headerShown ? previewHeader.bottom : parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: root.showInfo ? (panel.width < 760 ? 240 : 300) : 0

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            Flickable {
                id: inspectorScroll
                objectName: "inspectorScroll"
                anchors.fill: parent
                anchors.margins: 1
                contentWidth: width
                contentHeight: inspectorColumn.implicitHeight + 36
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                function revealFocusedControl() {
                    const focused = root.Window.window ? root.Window.window.activeFocusItem : null
                    let ancestor = focused
                    while (ancestor && ancestor !== inspectorColumn) ancestor = ancestor.parent
                    if (!focused || ancestor !== inspectorColumn) return
                    const top = focused.mapToItem(contentItem, 0, 0).y
                    const bottom = top + focused.height
                    const maximum = Math.max(0, contentHeight - height)
                    if (top < contentY) contentY = Math.max(0, top)
                    else if (bottom > contentY + height) contentY = Math.min(maximum, bottom - height)
                }
                Connections {
                    target: root.Window.window
                    function onActiveFocusItemChanged() {
                        if (root.visible) Qt.callLater(inspectorScroll.revealFocusedControl)
                    }
                }

                Column {
                    id: inspectorColumn
                    x: 18
                    y: 18
                    width: parent.width - 36
                    spacing: 12

                    Text {
                        objectName: "viewerKindLabel"
                        width: parent.width
                        visible: text !== ""
                        text: root.kindLabel
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: Theme.mutedText
                    }

                    Text {
                        objectName: "viewerInfoName"
                        width: parent.width
                        text: root.fileName
                        elide: Text.ElideMiddle
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: Theme.brightForeground
                    }

                    Column {
                        width: parent.width
                        spacing: 3

                        Text {
                            objectName: "mediaMetadata"
                            width: parent.width
                            visible: root.technicalLabel !== ""
                            text: root.technicalLabel
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            color: Theme.foreground
                        }

                        Text {
                            objectName: "viewerDateLabel"
                            width: parent.width
                            visible: text !== ""
                            text: [root.dayLabel + (sidebar.width >= 280 ? " " + root.timeLabel : ""),
                                   root.sizeLabel].filter(function (part) {
                                return part !== "" && part.trim() !== ""
                            }).join("  ·  ")
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            color: Theme.mutedText
                        }

                        Text {
                            objectName: "pdfInfoLabel"
                            width: parent.width
                            visible: root.isDocument && PdfInfo.pageCount === 0
                            text: PdfInfo.error !== "" ? PdfInfo.error
                                  : (PdfInfo.available ? "Reading pages…"
                                                      : "PDF support needs Poppler")
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            color: PdfInfo.error !== "" ? Theme.red : Theme.mutedText
                        }
                    }

                    Flow {
                        objectName: "viewerPrimaryActions"
                        width: parent.width
                        spacing: 6
                        visible: root.primaryActionRows.length > 0

                        Repeater {
                            model: root.primaryActionRows
                            PillButton {
                                required property var modelData
                                label: root.primaryLabel(modelData)
                                toolTip: modelData.available ? root.primaryLabel(modelData)
                                         : root.primaryLabel(modelData) + " needs " + modelData.hint
                                shortcut: modelData.shortcut
                                enabled: modelData.available
                                onClicked: root.actionTriggered(modelData.id)
                            }
                        }
                    }

                    // Rating. Click a star to set it, or the lit star again to
                    // clear. Alt+1 to Alt+5 and Alt+0 do the same from the keyboard.
                    Row {
                        objectName: "viewerRating"
                        spacing: 2

                        Repeater {
                            model: 5

                            Item {
                                required property int index
                                width: 28
                                height: 28
                                readonly property bool lit: index < root.rating
                                Accessible.role: Accessible.Button
                                Accessible.name: (index + 1) + (index === 0 ? " star" : " stars")
                                Accessible.checkable: true
                                Accessible.checked: index + 1 === root.rating
                                Accessible.onPressAction: root.rateRequested(index + 1 === root.rating ? 0 : index + 1)
                                activeFocusOnTab: true
                                Keys.onPressed: function(event) {
                                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                            || event.key === Qt.Key_Space) {
                                        root.rateRequested(index + 1 === root.rating ? 0 : index + 1)
                                        event.accepted = true
                                    }
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    color: "transparent"
                                    radius: 3
                                    border.width: parent.activeFocus ? 2 : 0
                                    border.color: Theme.accent
                                }

                                Icon {
                                    anchors.centerIn: parent
                                    name: parent.lit ? "star-filled" : "star"
                                    size: 20
                                    color: parent.lit ? Theme.yellow : Theme.mutedText
                                }

                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    onTapped: root.rateRequested(index + 1 === root.rating
                                                                 ? 0 : index + 1)
                                }
                            }
                        }
                    }

                    // Caption. It rests as text and the editor appears on
                    // demand. Enter or leaving the field saves, Escape reverts.
                    Item {
                        id: captionBox
                        width: parent.width
                        height: Math.max(30, captionField.contentHeight + 12)

                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 4) : 3
                            color: captionField.activeFocus ? root.shade(Theme.foreground, 0.10)
                                   : captionHover.hovered ? root.shade(Theme.foreground, 0.05)
                                                          : "transparent"
                            border.width: captionField.activeFocus ? 1 : 0
                            border.color: root.shade(Theme.accent, 0.6)
                            Behavior on color { ColorAnimation { duration: 120 } }
                        }

                        TextInput {
                            id: captionField
                            objectName: "viewerCaption"
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            anchors.topMargin: 6
                            wrapMode: TextInput.Wrap
                            clip: true
                            activeFocusOnTab: true
                            Accessible.name: "Caption"
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            color: Theme.foreground
                            selectionColor: root.shade(Theme.accent, 0.5)
                            selectedTextColor: Theme.brightForeground
                            // Typing breaks a plain binding, so follow the saved
                            // caption explicitly when the file or its marks change.
                            readonly property string saved: root.caption
                            onSavedChanged: text = saved
                            Component.onCompleted: text = saved
                            function commit() {
                                if (text.trim() !== root.caption) {
                                    root.captionEdited(text.trim())
                                }
                            }
                            // Enter saves outright. Handing focus back to the
                            // sheet is not enough on its own: a focus scope
                            // returns focus to its current item, which is this
                            // field, so the focus-out save would never fire.
                            function finish() {
                                commit()
                                focus = false
                                root.focusPreview()
                            }
                            onEditingFinished: commit()
                            Keys.onEscapePressed: {
                                text = root.caption
                                focus = false
                                root.focusPreview()
                            }
                            Keys.onReturnPressed: finish()
                            Keys.onEnterPressed: finish()
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            y: 6
                            visible: captionField.text === "" && !captionField.activeFocus
                            text: "Add a caption"
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            color: Theme.mutedText
                        }

                        HoverHandler {
                            id: captionHover
                            cursorShape: Qt.IBeamCursor
                        }
                        TapHandler {
                            onTapped: captionField.forceActiveFocus()
                        }
                    }

                    // Technical details: codec, bitrate and stream lines,
                    // collapsed until asked for so the surface stays calm.
                    Column {
                        width: parent.width
                        spacing: 2

                        Item {
                            id: technicalToggle
                            objectName: "viewerTechnicalToggle"
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: "Technical details"
                            Accessible.checkable: true
                            Accessible.checked: root.technicalExpanded
                            Accessible.onPressAction: root.technicalExpanded = !root.technicalExpanded
                            Keys.onPressed: function(event) {
                                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                        || event.key === Qt.Key_Space) {
                                    root.technicalExpanded = !root.technicalExpanded
                                    event.accepted = true
                                }
                            }
                            Rectangle {
                                anchors.fill: parent
                                color: "transparent"
                                radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 4) : 3
                                border.width: technicalToggle.activeFocus ? 2 : 0
                                border.color: Theme.accent
                            }
                            width: parent.width
                            height: 30

                            Row {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                Icon {
                                    name: "chevron-right"
                                    size: 14
                                    rotation: root.technicalExpanded ? 90 : 0
                                    transformOrigin: Item.Center
                                    color: Theme.mutedText
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "Technical details"
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 13
                                    color: Theme.foreground
                                }
                            }

                            HoverHandler { id: techHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.technicalExpanded = !root.technicalExpanded }
                            ToolTip {
                                visible: techHover.hovered
                                text: root.technicalExpanded
                                      ? "Hide codec and stream details"
                                      : "Show codec and stream details"
                                delay: 500
                            }
                        }

                        Column {
                            id: technicalDetails
                            objectName: "viewerTechnicalDetails"
                            width: parent.width
                            visible: root.technicalExpanded
                            spacing: 3

                            Text {
                                width: parent.width
                                visible: MediaInfo.path === root.path && MediaInfo.loading
                                text: "Reading details…"
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                                color: Theme.mutedText
                            }

                            Repeater {
                                model: MediaInfo.path === root.path ? MediaInfo.lines : []
                                Text {
                                    required property string modelData
                                    width: technicalDetails.width
                                    text: modelData
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 13
                                    color: Theme.mutedText
                                }
                            }
                        }
                    }
                }
            }
        }

        // Consume the dismissal click before it can play media or change a mark.
        MouseArea {
            anchors.fill: parent
            visible: root.actionNavigationActive
            z: 1
            acceptedButtons: Qt.AllButtons
            onClicked: root.focusPreview()
            WheelHandler {
                target: null
                onWheel: function (event) { event.accepted = true }
            }
        }

        // Action menu. The whole registry, on demand, under the header like an
        // overflow: secondary, destructive and external actions with their
        // shortcuts kept quiet on the right.
        Rectangle {
            id: actionPopup
            z: 2
            objectName: "viewerActionPopup"
            visible: root.actionNavigationActive
            anchors.top: root.headerShown ? previewHeader.bottom : parent.top
            anchors.topMargin: 6
            anchors.right: parent.right
            anchors.rightMargin: 10
            width: Math.min(280, panel.width - 40)
            height: Math.min(380, Math.max(140, panel.height - (root.headerShown ? 62 : 20)))
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
            color: root.shade(Theme.background, 0.98)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.18)

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                preventStealing: true
            }

            // Actions, from the registry
            ListView {
                id: actions
                objectName: "viewerActions"
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                spacing: 1
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                model: root.visible ? root.visibleActions() : []
                section.property: "group"
                section.criteria: ViewSection.FullString
                section.delegate: Text {
                    required property string section
                    width: actions.width
                    height: 24
                    text: section
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    color: Theme.mutedText
                    verticalAlignment: Text.AlignVCenter
                }

                delegate: FocusScope {
                    id: row
                    required property int index
                    required property var modelData
                    objectName: "viewerAction_" + modelData.id
                    width: actions.width
                    height: 30
                    activeFocusOnTab: false

                    readonly property bool usable: modelData.available
                    readonly property bool primary: modelData.id === root.preferredAction
                    readonly property bool destructive: modelData.id === "trash"
                                                        || modelData.id === "hide"
                    // mpv is an external player; say so rather than let "Play"
                    // read as embedded playback.
                    readonly property string displayLabel: modelData.id === "play"
                                                           ? "Open in mpv" : modelData.label
                    readonly property string shortcut: modelData.shortcut
                    readonly property string toolTipText: modelData.shortcut !== ""
                                                           ? row.displayLabel + "  ·  "
                                                             + modelData.shortcut
                                                           : row.displayLabel

                    Accessible.role: Accessible.Button
                    Accessible.name: row.displayLabel
                    Accessible.description: modelData.shortcut !== ""
                                            ? "Shortcut " + modelData.shortcut : ""
                    Accessible.ignored: !row.usable
                    Accessible.onPressAction: if (row.usable) root.invokeAction(row.modelData.id)

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
                        color: row.activeFocus
                               ? root.shade(Theme.accent, 0.18)
                               : rowHover.hovered && row.usable
                               ? root.shade(Theme.foreground, 0.09)
                               : "transparent"
                        border.width: row.activeFocus ? 2 : 0
                        border.color: Theme.accent
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: shortcut.left
                        anchors.rightMargin: 8
                        elide: Text.ElideRight
                        text: row.displayLabel
                              + (row.usable ? "" : "  ·  needs " + row.modelData.hint)
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: row.primary ? Font.DemiBold : Font.Normal
                        color: !row.usable
                               ? Theme.mutedText
                               : row.destructive ? Theme.red
                               : (row.primary ? Theme.accent : Theme.foreground)
                    }

                    Text {
                        id: shortcut
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.shortcut
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }

                    HoverHandler {
                        id: rowHover
                        cursorShape: row.usable ? Qt.PointingHandCursor : Qt.ArrowCursor
                    }

                    ToolTip {
                        visible: rowHover.hovered && row.toolTipText !== ""
                        text: row.toolTipText
                        delay: 500
                    }

                    TapHandler {
                        enabled: row.usable
                        onSingleTapped: {
                            root.focusAction(row.index)
                            root.invokeAction(row.modelData.id)
                        }
                    }

                    Keys.onPressed: function (event) {
                        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                            root.focusRelativeAction(event.key === Qt.Key_Up ? -1 : 1)
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_Backtab
                                || (event.key === Qt.Key_Tab
                                    && (event.modifiers & Qt.ShiftModifier))) {
                            root.focusPreview()
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_Tab) {
                            root.focusRelativeAction(1)
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                || event.key === Qt.Key_Space) {
                            root.invokeAction(row.modelData.id)
                            event.accepted = true
                        }
                    }
                }
            }
        }
    }

    // The same letters that work on the grid work here, read off the rows the
    // registry supplied rather than hard-coded a second time.
    function shortcutLabel(event) {
        if (event.key === Qt.Key_Delete) {
            return event.modifiers & Qt.ShiftModifier ? "Shift+Del" : "Del"
        }
        // From the key, not the text: with Ctrl held the text is a control
        // character, so Ctrl+H would otherwise never match its label.
        let letter = ""
        if (event.key >= Qt.Key_A && event.key <= Qt.Key_Z) {
            letter = String.fromCharCode(event.key)
        } else if (event.text !== "") {
            letter = event.text.toUpperCase()
        } else {
            return ""
        }
        return (event.modifiers & Qt.ControlModifier) ? "Ctrl+" + letter : letter
    }

    Connections {
        target: root
        function onPathChanged() { imageContextMenu.close() }
    }
    ActionMenu {
        id: imageContextMenu
        objectName: "detailContextMenu"
        entryPrefix: "detailContextAction_"
        conditionalEntriesVisible: root.qrDetected
        onTriggered: function(id) { root.invokeAction(id) }
        onClosed: root.restoreFocus()
    }
    function openImageContextMenu(x, y) {
        imageContextMenu.entries = imageContextMenu.grouped(root.visibleActions().filter(function(row) { return row.id !== "qr" }))
        const qr = Registry.actionsForKind(root.isVideo, root.isDocument, root.path)
            .find(function(row) { return row.id === "qr" })
        if (qr) imageContextMenu.entries = imageContextMenu.entries.concat([
            {separator: true, conditional: true}, Object.assign({}, qr, {conditional: true})])
        imageContextMenu.popup(stage, x, y)
    }
    TapHandler {
        parent: stage
        acceptedButtons: Qt.RightButton
        onSingleTapped: function(point) { root.openImageContextMenu(point.position.x, point.position.y) }
    }

    // Mouse side buttons navigate the same files as Left and Right. This is
    // deliberately only those two buttons: clicks, video controls and PDF
    // selection keep their existing meaning.
    MouseArea {
        objectName: "detailSideButtonNavigation"
        anchors.fill: parent
        acceptedButtons: Qt.BackButton | Qt.ForwardButton
        enabled: root.visible && root.canNavigate && !root.contextMenuOpen
                 && !root.actionNavigationActive && !root.pdfSelectMode
        onClicked: function (mouse) {
            root.requestNavigation(mouse.button === Qt.BackButton ? -1 : 1)
        }
    }

    Shortcut {
        sequences: ["Alt+C"]
        enabled: root.visible && !root.contextMenuOpen && root.enabled
        onActivated: {
            root.actionNavigationActive = false
            root.showInfo = true
            captionField.forceActiveFocus()
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Delete && (event.modifiers & Qt.ShiftModifier)
                && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier))) {
            if (pdfSearch.activeFocus) return
            if (!event.isAutoRepeat && !root.contextMenuOpen) root.invokeAction("permanent-delete")
            event.accepted = true
            return
        }
        if (!(event.modifiers & (Qt.ControlModifier | Qt.AltModifier))
                && (event.key === Qt.Key_D || event.key === Qt.Key_O)
                && Registry.appliesToKind("develop", root.isVideo, root.isDocument, root.path)) {
            root.invokeAction(event.modifiers & Qt.ShiftModifier ? "choose-editor" : "develop")
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Backtab
                || (event.key === Qt.Key_Tab && (event.modifiers & Qt.ShiftModifier))) {
            if (root.Window.window && root.Window.window.activeFocusItem !== root) return
            root.focusPreview()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Tab) {
            if (root.Window.window && root.Window.window.activeFocusItem !== root) return
            root.focusFirstAction()
            event.accepted = true
            return
        }
        if (event.key === root.viewerShortcuts.slideshow.key) {
            root.setSlideshow(!root.slideshowRunning)
            event.accepted = true
            return
        }
        if (event.key === root.viewerShortcuts.fullscreen.key) {
            root.setFullScreen(!root.fullScreen)
            event.accepted = true
            return
        }
        if (event.key === root.viewerShortcuts.info.key) {
            root.showInfo = !root.showInfo
            event.accepted = true
            return
        }
        // Ctrl+C copies the words picked on the page rather than the file.
        if (root.isDocument && PdfInfo.hasSelection && event.key === Qt.Key_C
                && (event.modifiers & Qt.ControlModifier)) {
            PdfInfo.copySelection()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Escape) {
            // The window owns Escape through its own shortcut; this is the
            // fallback for the paths that shortcut does not cover, and it
            // leaves a text selection the same way.
            root.dismiss()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Space) {
            if (root.isVideo) {
                root.toggleVideoPlayback()
            } else if (root.isAnimatedImage) {
                root.animationPlaying = !root.animationPlaying
            } else {
                root.actionTriggered(root.isDocument
                                     ? Registry.primaryActionForKind(false, true)
                                     : Settings.imagePrimaryAction)
            }
            event.accepted = true
            return
        }
        if (event.key === root.viewerShortcuts.previous.key
                || event.key === root.viewerShortcuts.next.key) {
            root.requestNavigation(event.key === root.viewerShortcuts.previous.key ? -1 : 1)
            event.accepted = true
            return
        }
        if (!root.isVideo && (event.key === root.viewerShortcuts.zoomIn.key
                              || event.key === Qt.Key_Equal)) {
            root.adjustImageZoom(1.25)
            event.accepted = true
            return
        }
        if (!root.isVideo && event.key === root.viewerShortcuts.zoomOut.key) {
            root.adjustImageZoom(1 / 1.25)
            event.accepted = true
            return
        }
        if (!root.isVideo && event.key === root.viewerShortcuts.fit.key) {
            root.resetImageView()
            event.accepted = true
            return
        }
        if (!root.isVideo && !root.isDocument
                && event.key === root.viewerShortcuts.actual.key) {
            root.toggleActualImageSize()
            event.accepted = true
            return
        }
        if (!root.isVideo && event.key === root.viewerShortcuts.rotate.key) {
            root.rotateImage()
            event.accepted = true
            return
        }
        if (!root.isVideo && !root.isDocument && (event.modifiers & Qt.ShiftModifier)
                && (event.key === root.viewerShortcuts.flipHorizontal.key
                    || event.key === root.viewerShortcuts.flipVertical.key)) {
            if (event.key === root.viewerShortcuts.flipHorizontal.key) {
                root.imageFlipHorizontal = !root.imageFlipHorizontal
            } else {
                root.imageFlipVertical = !root.imageFlipVertical
            }
            event.accepted = true
            return
        }
        if (root.isDocument && (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp)) {
            const next = root.pdfPage + (event.key === Qt.Key_PageDown ? 1 : -1)
            if (next >= 1 && next <= PdfInfo.pageCount) {
                root.stillReady = false
                root.pdfPage = next
            }
            event.accepted = true
            return
        }
        if (root.isVideo && (event.key === Qt.Key_J || event.key === Qt.Key_L)) {
            root.seekVideo(event.key === Qt.Key_J ? -5000 : 5000)
            event.accepted = true
            return
        }
        if (root.isVideo && event.key === Qt.Key_M) {
            Settings.videoMuted = !Settings.videoMuted
            event.accepted = true
            return
        }
        if (root.isVideo && (event.key === Qt.Key_Up || event.key === Qt.Key_Down
                             || event.key === Qt.Key_9 || event.key === Qt.Key_0)) {
            root.adjustVolume(event.key === Qt.Key_Up || event.key === Qt.Key_0 ? 0.05 : -0.05)
            event.accepted = true
            return
        }
        if (root.isVideo && (event.key === Qt.Key_BracketLeft
                             || event.key === Qt.Key_BracketRight)) {
            root.adjustPlaybackRate(event.key === Qt.Key_BracketLeft ? -0.25 : 0.25)
            event.accepted = true
            return
        }
        if (root.isVideo && event.key === Qt.Key_Backspace) {
            player.playbackRate = 1
            event.accepted = true
            return
        }
        if (root.isVideo && (event.key === Qt.Key_Home || event.key === Qt.Key_End)) {
            player.position = event.key === Qt.Key_Home ? 0 : player.duration
            event.accepted = true
            return
        }
        const label = root.shortcutLabel(event)
        if (label === "") {
            return
        }
        for (const row of actions.model) {
            if (row.shortcut === label && row.available) {
                root.invokeAction(row.id)
                event.accepted = true
                return
            }
        }
    }
}
