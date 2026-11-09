import QtQuick
import QtQuick.Controls.Basic
import QtMultimedia
import Omaroll

// The quick viewer: what a picture or a video opens into from the file
// manager. The media fills the window and the controls stay out of the way
// until the pointer moves. Organising, matting, correcting and the rest of the
// workbench belong to the library, one key away.
ApplicationWindow {
    id: root
    readonly property bool tooltipsBlocked: root.blockingActionOpen
    property bool keyboardNavigation: false
    TapHandler { acceptedButtons: Qt.AllButtons; onPressedChanged: if (pressed) root.keyboardNavigation = false }


    width: 1180
    height: 780
    // Small enough for a small picture's window, which keeps its shape.
    minimumWidth: 320
    minimumHeight: 240
    // Shown from C++ once the first file is set and the window is sized.
    visible: false
    // The file, then the app. Qt appends the app name to any title that does
    // not already end with it, so this is also the whole title a compositor
    // sees. The library's is "Omaroll" alone, so a rule matching the whole
    // title against ".* · Omaroll" floats this window and leaves the library
    // tiled.
    // A viewer opened beside another maps under the plain title so that rule
    // leaves it tiled, then takes its own; see HyprlandPlacement.
    title: Session.fileName !== "" && !root.mapTiled ? Session.fileName + " · Omaroll" : "Omaroll"
    readonly property var presentation: videoSurface.item
        ? Settings.videoPresentation(videoSurface.item) : null
    property bool mapTiled: false
    // With several viewers open, messages that belong to no viewer in
    // particular appear only in the one used last.
    property bool frontmost: true
    // Only one viewer's video is heard: the one used last. The others keep
    // playing silently, and the saved mute setting is left alone.
    property bool audible: true
    // The window paints nothing; the canvas below carries the theme's alpha,
    // as the library's does, and the picture itself is always drawn opaque.
    color: "transparent"

    // How long the pointer may rest before the controls fade.
    property int chromeTimeout: 2200
    property bool pointerActive: false
    // Renders and tests hold the controls up regardless of the pointer.
    property bool chromePinned: false
    property bool infoOpen: false
    property bool slideshowRunning: false
    readonly property bool blockingActionOpen: actionMenu.visible || confirm.visible
                                              || permanentConfirm.visible || editorChooser.visible || shortcutHelp.visible
    onBlockingActionOpenChanged: {
        if (root.blockingActionOpen) slideshowTimer.stop()
        else Qt.callLater(root.resumeSlideshow)
    }
    property bool videoPausedForRender: false
    property int visibilityBeforeFullScreen: Window.Windowed
    readonly property bool fullScreen: root.visibility === Window.FullScreen
    property string status: ""
    property string videoError: ""
    property string loadedMediaPath: ""
    property string loadedMediaVersion: ""
    property bool reloadingVideo: false
    readonly property bool imageError: Session.pictureError !== ""
                                       || (stillLoader.item !== null
                                           && stillLoader.item.status === Image.Error)
    readonly property string playbackError: Session.isVideo ? root.videoError
        : root.imageError ? (Session.isAnimated ? "Could not display this animation"
                             : Session.pictureError !== "" ? Session.pictureError
                             : "Could not display this picture")
        : ""
    // Why, once the session has read the file: a title naming the cause and
    // a sentence about it. Until then the plain message above.
    readonly property bool failureKnown: !Session.isVideo && Session.pictureFailure.title !== undefined
    readonly property string errorTitle: root.failureKnown ? Session.pictureFailure.title
                                                           : root.playbackError
    readonly property string errorDetail: root.failureKnown ? Session.pictureFailure.detail : ""
    readonly property string errorCause: root.failureKnown ? Session.pictureFailure.cause : ""
    // Each failed file is looked at once, including a replacement at the
    // same name.
    readonly property string failedPicture: root.imageError && !Session.isVideo
                                            ? Session.path + "\n" + Session.contentVersion : ""
    onFailedPictureChanged: if (root.failedPicture !== "") Session.diagnosePicture()

    // A still is fitted until it is zoomed; viewScale is then the logical
    // size of one source pixel. Zero means "fit".
    property string fitMode: "fit"
    property real viewScale: 0
    property int viewRotation: 0
    // Mirrored for viewing, across the screen after the turn, so Shift+H
    // always swaps left and right as shown. At most one is on: both at once
    // are a half turn, kept as one. Reset with the turn on the next file.
    property bool viewFlipHorizontal: false
    property bool viewFlipVertical: false
    readonly property bool viewTransformed: root.viewRotation !== 0 || root.viewFlipHorizontal
                                            || root.viewFlipVertical
    readonly property string viewCopyKind: root.viewRotation !== 0
                                           && (root.viewFlipHorizontal || root.viewFlipVertical)
                                           ? "rotated and flipped"
                                           : root.viewRotation !== 0 ? "rotated" : "flipped"
    // Short enough for the menu's width.
    readonly property string viewCopyLabel: root.viewRotation !== 0 ? "Save rotated copy"
                                                                     : "Save flipped copy"
    // What the view shows can be saved as a copy: a still the editor writes,
    // so not a camera raw or an animation, which would lose its frames.
    readonly property bool canSaveViewCopy: root.viewTransformed && !Session.isVideo
                                            && !Session.isRaw && !Session.isAnimated
                                            && !root.imageError && Session.path !== ""
    property int viewCopyJob: 0
    property int viewCopySerial: -1
    // The file the copy is of, which the window may have stepped away from
    // by the time it is saved.
    property string viewCopySource: ""
    property string viewCopyDescription: ""
    readonly property bool savingViewCopy: root.viewCopyJob > 0
    // Read straight off the loaded image, so a picture that arrives from the
    // cache in the same turn as the file change can never be sized as the
    // previous one.
    readonly property size loadedSize: stillLoader.item !== null
                                       && stillLoader.item.status === Image.Ready
                                       ? Qt.size(stillLoader.item.implicitWidth,
                                                 stillLoader.item.implicitHeight)
                                       : Qt.size(0, 0)
    // A camera raw shows the camera's preview, which can be smaller than the
    // raw. Measured by the raw instead, actual size and the details mean the
    // raw's pixels and the preview stretches over them, unless the two
    // disagree on shape, as a preview cropped differently would. A very large
    // picture is shown the same way, from a copy the size of the screen.
    readonly property size measuredSize: Session.isRaw ? Session.rawSize : Session.fullSize
    readonly property bool measured: root.loadedSize.width > 0
        && root.measuredSize.width > 0 && root.measuredSize.height > 0
        && Math.abs(root.measuredSize.width / root.measuredSize.height
                    / (root.loadedSize.width / root.loadedSize.height) - 1) < 0.02
    readonly property real sourceWidth: root.measured ? root.measuredSize.width
                                                      : root.loadedSize.width
    readonly property real sourceHeight: root.measured ? root.measuredSize.height
                                                       : root.loadedSize.height
    // Once that preview would be drawn larger than its own pixels, the raw is
    // demosaiced in the background and laid over it. A preview the size of
    // the raw, as most cameras write, never needs it. A large picture loads
    // its full resolution the same way when there is memory for it.
    readonly property url detailSource: Session.isRaw ? Session.rawUrl : Session.detailUrl
    readonly property bool enlargedPastCopy: root.measured && root.imageReady
        && root.loadedSize.width < root.measuredSize.width * 0.9
        && root.effectiveScale * root.dpr * root.measuredSize.width > root.loadedSize.width * 1.05
    readonly property bool detailWanted: root.enlargedPastCopy && root.detailSource.toString() !== ""
    // Zoomed past the copy with no full resolution to load: say why, once.
    readonly property bool detailMissing: root.enlargedPastCopy && Session.reduced
                                          && !Session.areas && Session.detailUrl.toString() === ""
    onDetailMissingChanged: {
        if (root.detailMissing && Session.pictureNote !== "") root.say(Session.pictureNote)
    }
    // The file whose full decode last arrived, kept so zooming back out and
    // in again does not drop it and decode it twice.
    property string detailPath: ""
    readonly property bool stillReady: Session.pictureError !== ""
                                       || (stillLoader.item !== null
                                           && (stillLoader.item.status === Image.Ready
                                               || stillLoader.item.status === Image.Error))
    property bool animationPlaying: true

    // Video, created on the first recording so browsing pictures never starts
    // the multimedia backend.
    readonly property var player: playerLoader.item
    // What the player opens. Since Qt 6.12 the player stops on every write to
    // its source, even of the same file, so it only follows this property,
    // which changes when the file does. A binding on the player itself is
    // rewritten whenever the session notifies, which cancels autoplay.
    readonly property url videoSource: root.visible && Session.isVideo && !root.reloadingVideo
                                       ? Session.url : ""
    readonly property bool playing: player !== null
                                    && player.playbackState === MediaPlayer.PlayingState
    property bool resumeAvailable: false
    property real resumePosition: 0
    property bool resumePending: false
    property bool loopVideo: false
    property var subtitleFiles: []
    property string externalSubtitle: ""
    property int subtitleChoice: 0
    property int subtitleOffsetMs: 0
    readonly property bool hasAudioChoices: root.player && root.player.audioTracks.length > 1
    readonly property string subtitleText: {
        void Subtitles.revision
        return externalSubtitle !== "" && player
        ? Subtitles.textAt(externalSubtitle, Math.round(player.position) - root.subtitleOffsetMs)
        : (root.presentation ? root.presentation.subtitleText : "")
    }

    readonly property bool favorite: { void root.marksVersion; return Settings.isFavorite(Session.path) }
    readonly property int rating: { void root.marksVersion; return Settings.rating(Session.path) }
    property int marksVersion: 0

    readonly property real dpr: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
    readonly property bool sideways: root.viewRotation === 90 || root.viewRotation === 270
    readonly property real rotatedWidth: root.sideways ? root.sourceHeight : root.sourceWidth
    readonly property real rotatedHeight: root.sideways ? root.sourceWidth : root.sourceHeight
    // Fitted to the window, scaling a small picture up the way imv and mpv do,
    // but never past four source pixels per screen pixel, where an icon would
    // turn to mush. Actual size is one key away.
    readonly property real fitScale: root.rotatedWidth > 0 && root.rotatedHeight > 0
        ? Math.min(stage.width / root.rotatedWidth, stage.height / root.rotatedHeight, 4 / root.dpr)
        : 0
    readonly property real fittedScale: root.fitMode === "width"
        ? Math.min(stage.width / Math.max(1, root.rotatedWidth), 4 / root.dpr)
        : root.fitMode === "fill" && root.rotatedWidth > 0 && root.rotatedHeight > 0
        ? Math.min(Math.max(stage.width / root.rotatedWidth, stage.height / root.rotatedHeight),
                   4 / root.dpr)
        : (root.fitMode === "shrink" || !Settings.enlargeSmallPictures)
          ? Math.min(root.fitScale, root.actualScale) : root.fitScale
    readonly property real actualScale: 1 / root.dpr
    readonly property real effectiveScale: root.viewScale > 0 ? root.viewScale : root.fittedScale
    readonly property real displayWidth: root.rotatedWidth * root.effectiveScale
    readonly property real displayHeight: root.rotatedHeight * root.effectiveScale
    readonly property int zoomPercent: Math.round(root.effectiveScale * root.dpr * 100)
    readonly property bool imageReady: root.visible && !Session.isVideo && root.stillReady
                                       && root.sourceWidth > 0

    // Keyboard focus is on one of the controls, reached with Tab, rather than
    // on the picture. The controls stay up while it is there.
    // Remembered while the window is inactive, when nothing has active focus,
    // so the focused control is not hidden from under the keyboard.
    property Item keyboardFocusItem: null
    onActiveFocusItemChanged: {
        if (root.activeFocusItem !== null) root.keyboardFocusItem = root.activeFocusItem
        Qt.callLater(root.reclaimFocus)
    }
    readonly property bool controlsFocused: root.keyboardFocusItem !== null
                                            && root.keyboardFocusItem !== keys
                                            && root.focusInside(root.keyboardFocusItem)
    function focusInside(item) {
        for (let at = item; at; at = at.parent) {
            if (at === chrome || at === errorState || at === infoPanel) return true
        }
        return false
    }
    // Tab and Shift+Tab: through the visible controls in order, then back to
    // the picture, where every shortcut works.
    function moveFocus(forward) {
        root.revealChrome()
        const from = root.activeFocusItem !== null ? root.activeFocusItem : keys
        let next = from.nextItemInFocusChain(forward)
        for (let guard = 0; next && next !== keys && !root.focusInside(next) && guard < 64; ++guard) {
            next = next.nextItemInFocusChain(forward)
        }
        (next ? next : keys).forceActiveFocus(Qt.TabFocusReason)
    }
    // A focused control that is hidden or switched off would keep Space and
    // Enter, or leave the keyboard on the window itself where no key works.
    // The picture takes it back. Popups keep their own focus.
    function reclaimFocus() {
        if (!root.visible || !root.active) return
        const at = root.activeFocusItem
        if (at === keys) return
        let lost = at === null || !at.visible || !at.enabled
        for (let up = keys.parent; up && !lost; up = up.parent) {
            if (up === at) lost = true
        }
        if (lost) keys.forceActiveFocus()
    }
    Connections {
        target: root.activeFocusItem
        ignoreUnknownSignals: true
        function onVisibleChanged() { Qt.callLater(root.reclaimFocus) }
        function onEnabledChanged() { Qt.callLater(root.reclaimFocus) }
    }

    readonly property bool chromeShown: root.chromePinned || root.pointerActive || root.infoOpen
                                        || root.controlsFocused
                                        || shortcutHelp.visible
                                        || actionMenu.visible || confirm.visible || permanentConfirm.visible || editorChooser.visible
                                        || header.hovered || transport.hovered
                                        || previousButton.hovered || nextButton.hovered
                                        || toolbar.hovered || filmstrip.hovered
                                        || transport.scrubbing
                                        || (Session.isVideo && !root.playing && !root.slideshowRunning)
    readonly property bool pointerHidden: !root.chromeShown
                                          && (root.playing || root.fullScreen || root.slideshowRunning)

    readonly property bool choosingEditor: editorChooser.visible
    function showShortcutHelp() { shortcutHelp.open() }
    function showActionMenu() { actionMenu.popup(stage, stage.width / 2, stage.height / 2) }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    // A pale surround around the picture, where its own edges can vanish.
    readonly property bool lightSurround: Osd.luminance(Theme.surfaceBackground) > 0.4

    function say(message) {
        root.status = message
        root.undoId = 0
        statusTimer.interval = 2600
        statusTimer.restart()
    }

    // A result that can be taken back: the toast offers Undo for a while,
    // as long as nothing newer has joined the undo history.
    property int undoId: 0
    function sayUndoable(message, id) {
        root.say(message)
        root.undoId = id
        if (id > 0) {
            statusTimer.interval = 6000
            statusTimer.restart()
        }
    }

    // Ctrl+Z and the toast's Undo: whatever happened last, marks or Trash.
    function undo() {
        if (!Settings.canUndo()) return
        const message = Settings.undo()
        if (message !== "") root.say(message)
    }

    // Called by the opt-in startup trace, before scene synchronization.
    function startupReadiness() {
        if (root.imageReady) return 2
        const poster = Session.isVideo && videoPoster.visible && videoPoster.status === Image.Ready
        return (Session.isVideo && root.player && root.player.hasVideo ? 2 : 0)
            | (poster ? 8 : 0) | (Session.isVideo && videoOutput.visible ? 16 : 0)
    }

    function revealChrome() {
        root.pointerActive = true
        chromeTimer.restart()
    }

    function setFullScreen(enabled) {
        if (enabled === root.fullScreen) {
            return
        }
        if (enabled) {
            root.visibilityBeforeFullScreen = root.visibility
            root.visibility = Window.FullScreen
        } else {
            root.visibility = root.visibilityBeforeFullScreen === Window.FullScreen
                              ? Window.Windowed : root.visibilityBeforeFullScreen
        }
    }

    // Fill stays on for this window until asked again, so stepping through a
    // folder in a tile keeps covering it.
    function resetView() {
        zoomAnimation.stop()
        root.fitMode = root.fitMode === "fill" ? "fill" : "fit"
        root.viewScale = 0
        root.viewRotation = 0
        root.viewFlipHorizontal = false
        root.viewFlipVertical = false
        still.contentX = 0
        still.contentY = 0
        root.keepFillCentred()
    }

    // Zoom eases toward a goal rather than jumping. Each wheel notch, key,
    // button or double click moves the goal, and the picture follows it over
    // a short curve around the same point, so whatever is under the pointer
    // stays under it. Without a point, around the centre. A pinch follows the
    // fingers directly instead.
    property real zoomGoal: 0
    property point zoomAnchor: Qt.point(0, 0)
    property real easedScale: 0
    readonly property bool zooming: zoomAnimation.running

    function zoomTo(target, x, y, instant) {
        if (Session.isVideo || root.fitScale <= 0) {
            return
        }
        // Out as far as the fitted size, or the real size when a small
        // picture is scaled up to fit; in to 32 pixels per source pixel.
        const minimum = Math.min(root.fitScale, root.actualScale)
        const maximum = Math.max(root.fitScale * 2, 32 / root.dpr)
        root.zoomGoal = Math.max(minimum, Math.min(maximum, target))
        root.zoomAnchor = Qt.point(x === undefined ? still.width / 2 : x,
                                   y === undefined ? still.height / 2 : y)
        zoomBadge.flash()
        zoomAnimation.stop()
        if (instant === true) {
            root.applyScale(root.zoomGoal, root.zoomAnchor.x, root.zoomAnchor.y)
            root.settleZoom()
            return
        }
        zoomAnimation.from = root.effectiveScale
        zoomAnimation.to = root.zoomGoal
        zoomAnimation.start()
    }

    // Notches that arrive while the picture is still moving add to the goal,
    // not to wherever the picture happens to be on the way.
    function zoomBy(factor, x, y, instant) {
        const base = zoomAnimation.running ? root.zoomGoal : root.effectiveScale
        root.zoomTo(base * factor, x, y, instant)
    }

    function fitToWindow() {
        if (Session.isVideo || !root.imageReady) return
        root.viewScale = root.effectiveScale
        root.fitMode = "fit"
        root.zoomTo(root.fittedScale)
    }

    function setFitMode(mode) {
        if (Session.isVideo || !root.imageReady) return
        zoomAnimation.stop()
        root.fitMode = mode
        root.viewScale = 0
        still.contentX = 0
        still.contentY = 0
        zoomBadge.flash()
    }

    // Fill covers the window, cropping the edges that do not fit, centred.
    // Asking again goes back to fitting the whole picture.
    function toggleFill() {
        if (Session.isVideo || !root.imageReady) return
        if (root.fitMode === "fill" && root.viewScale === 0) {
            root.setFitMode("fit")
            return
        }
        root.setFitMode("fill")
        root.centreView()
    }

    function keepFillCentred() {
        if (root.fitMode === "fill" && root.viewScale === 0) Qt.callLater(root.centreView)
    }

    function centreView() {
        still.contentX = Math.max(0, (still.contentWidth - still.width) / 2)
        still.contentY = Math.max(0, (still.contentHeight - still.height) / 2)
    }

    function panImage(dx, dy) {
        if (Session.isVideo || root.effectiveScale <= root.fitScale * 1.001) return
        still.cancelFlick()
        still.contentX = Math.max(0, Math.min(still.contentWidth - still.width, still.contentX + dx))
        still.contentY = Math.max(0, Math.min(still.contentHeight - still.height, still.contentY + dy))
    }

    function copyPath(nameOnly) {
        const path = Session.deletionPath
        const text = nameOnly ? path.slice(path.lastIndexOf("/") + 1) : path
        if (text === "") return
        root.say(Actions.copyPlainText(text) ? (nameOnly ? "File name copied" : "Path copied")
                                            : "Could not copy text")
    }

    // One step of a zoom: the new scale, with the point under the anchor kept
    // where it was.
    function applyScale(scale, px, py) {
        const before = root.effectiveScale
        if (before <= 0) {
            return
        }
        const boxX = (still.contentX + px - frame.x) / before
        const boxY = (still.contentY + py - frame.y) / before
        root.viewScale = scale
        still.contentX = Math.max(0, Math.min(still.contentWidth - still.width,
                                              frame.x + boxX * scale - px))
        still.contentY = Math.max(0, Math.min(still.contentHeight - still.height,
                                              frame.y + boxY * scale - py))
    }

    // Arriving back at the fitted size goes back to following the window.
    function settleZoom() {
        if (Math.abs(root.viewScale - root.fittedScale) <= root.fittedScale * 0.001) {
            root.viewScale = 0
        }
    }

    NumberAnimation {
        id: zoomAnimation
        target: root
        property: "easedScale"
        duration: 180
        easing.type: Easing.OutCubic
        onFinished: root.settleZoom()
    }
    onEasedScaleChanged: {
        if (zoomAnimation.running) {
            root.applyScale(root.easedScale, root.zoomAnchor.x, root.zoomAnchor.y)
        }
    }

    function showActualSize(x, y) {
        if (Session.isVideo || !root.imageReady) return
        root.viewScale = root.effectiveScale
        root.fitMode = "fit"
        root.zoomTo(root.actualScale, x, y)
    }

    function toggleActualSize() {
        if (root.viewScale > 0
                && Math.abs(root.viewScale - root.actualScale) <= root.actualScale * 0.001) {
            root.fitToWindow()
        } else {
            root.showActualSize()
        }
    }

    // The toolbar's 1:1 / Fit button.
    function toggleFit() {
        if (root.viewScale > 0 || root.fitMode !== "fit") {
            root.fitToWindow()
        } else {
            root.showActualSize()
        }
    }

    function rotate(clockwise) {
        if (Session.isVideo || root.imageError) {
            return
        }
        // Under one mirror the picture turns the other way, so a clockwise
        // press still turns what is on screen clockwise.
        const mirrored = root.viewFlipHorizontal !== root.viewFlipVertical
        root.viewRotation = (root.viewRotation + (clockwise !== mirrored ? 90 : 270)) % 360
        zoomAnimation.stop()
        root.viewScale = 0
    }

    // Mirror the view, keeping the same part of a zoomed picture on screen.
    function flip(horizontal) {
        if (Session.isVideo || root.imageError) {
            return
        }
        still.cancelFlick()
        if (horizontal) {
            root.viewFlipHorizontal = !root.viewFlipHorizontal
            still.contentX = Math.max(0, still.contentWidth - still.width - still.contentX)
        } else {
            root.viewFlipVertical = !root.viewFlipVertical
            still.contentY = Math.max(0, still.contentHeight - still.height - still.contentY)
        }
        if (root.viewFlipHorizontal && root.viewFlipVertical) {
            root.viewFlipHorizontal = false
            root.viewFlipVertical = false
            root.viewRotation = (root.viewRotation + 180) % 360
        }
    }

    // A copy of what the view shows, beside the original, which stays as it
    // is. The copy then opens here, next to it.
    function saveViewCopy() {
        if (root.savingViewCopy) return
        if (!root.canSaveViewCopy) {
            if (!Session.isVideo && !root.viewTransformed) root.say("Rotate or flip the picture first")
            else if (Session.isRaw) root.say("Camera raws are not corrected here")
            else if (Session.isAnimated) root.say("Animations are not saved turned here")
            return
        }
        root.viewCopySerial = Session.openSerial()
        root.viewCopySource = Session.path
        root.viewCopyDescription = root.viewCopyKind
        root.viewCopyJob = ImageEdit.nextJobId()
        // A link's copy goes beside the link, in the folder being browsed.
        ImageEdit.saveViewCopy(Session.path, root.viewRotation / 90, root.viewFlipHorizontal,
                               root.viewFlipVertical, root.viewCopyJob, Session.deletionPath)
    }

    function fileNameOf(path) {
        return path.substring(path.lastIndexOf("/") + 1)
    }

    Connections {
        target: ImageEdit
        function onViewCopySaved(outputPath, note, jobId) {
            if (jobId !== root.viewCopyJob) return
            root.viewCopyJob = 0
            const shown = Session.showSaved(outputPath, root.viewCopySource, root.viewCopySerial)
            const saved = "Saved a " + root.viewCopyDescription + " copy"
            root.say((shown === "shown" ? saved + " beside the original"
                      : shown === "listed" ? saved + " of " + root.fileNameOf(root.viewCopySource)
                                             + " beside it"
                      : saved + " to " + outputPath)
                     + (note !== "" ? ". " + note : ""))
            if (note !== "") {
                statusTimer.interval = 5000
                statusTimer.restart()
            }
        }
        function onFailed(message, jobId) {
            if (jobId !== root.viewCopyJob) return
            root.viewCopyJob = 0
            root.say(message)
        }
    }

    // The file on screen, dragged into another app as the file itself: the
    // same data a library tile carries. Ctrl+drag on the picture or a plain
    // drag on the title starts it, so a plain drag still pans.
    readonly property var dragMimeData: Session.path !== "" ? Actions.fileDragData([Session.path]) : ({})
    property bool draggingOut: false
    function startFileDrag(handler) {
        if (Session.path === "" || root.draggingOut) return
        root.draggingOut = true
        const source = Session.isVideo ? videoOutput : frame
        const edge = 160
        const wide = source.width >= source.height
        const size = wide ? Qt.size(edge, Math.max(1, Math.round(edge * source.height / Math.max(1, source.width))))
                          : Qt.size(Math.max(1, Math.round(edge * source.width / Math.max(1, source.height))), edge)
        const begin = function (url) {
            // The grab is asynchronous; a release before it lands is no drag.
            if (!handler.active) {
                root.draggingOut = false
                return
            }
            if (url !== undefined) fileDrag.Drag.imageSource = url
            fileDrag.Drag.hotSpot = Qt.point(size.width / 2, size.height / 2)
            fileDrag.Drag.active = true
        }
        if (!source.visible || source.width < 1 || source.height < 1
                || !source.grabToImage(function (result) { begin(result.url) }, size)) {
            begin(undefined)
        }
    }
    function finishFileDrag() {
        root.draggingOut = false
        // The release that ended the drag went to the drop target, so the
        // handlers would otherwise stay active and miss the next drag.
        for (const handler of [stageFileDrag, titleFileDrag]) {
            handler.enabled = false
            handler.enabled = true
        }
    }

    // Files dropped here open here, as from the file manager, with a note
    // naming any skipped; each folder, and a document, goes to the library.
    function openDropped(urls) {
        const result = Session.openDropped(urls)
        if (result.error !== undefined) {
            root.say(result.error)
            return
        }
        if (result.handOff === true) Actions.openFiles(result.paths)
        for (const folder of result.folders) Actions.openFiles([folder])
        if (result.note !== "") root.say(result.note)
    }

    function step(direction) {
        slideshowTimer.stop()
        if (root.slideshowRunning) {
            root.stepSlideshow(direction)
            return
        }
        if (!Session.step(direction)) {
            root.say("This is the only file here")
        }
    }

    // The slideshow waits for each picture to be on screen before timing it.
    function armSlideshow() {
        if (root.slideshowRunning && !root.blockingActionOpen && !Session.isVideo && root.stillReady) {
            slideshowTimer.restart()
        }
    }
    onStillReadyChanged: root.armSlideshow()

    function playVideo() {
        if (!root.player) return
        if (root.resumeAvailable) root.resumeVideo()
        else root.player.play()
    }

    function togglePlayback() {
        if (Session.isVideo && root.player) {
            if (root.resumeAvailable) {
                root.resumeVideo()
            } else if (root.playing) {
                root.player.pause()
            } else {
                root.player.play()
            }
        } else if (Session.isAnimated) {
            root.animationPlaying = !root.animationPlaying
        }
    }

    function seekTo(milliseconds) {
        root.seekTarget(milliseconds, true)
    }

    function seekTarget(milliseconds, roundBoundary) {
        if (!root.player) {
            return
        }
        const duration = root.player.duration
        const maximum = duration > 0 ? duration - 1 : Math.max(0, milliseconds)
        root.resumeAvailable = false
        root.resumePending = false
        root.player.launchTarget = -1
        transport.noteSeek()
        root.player.position = Math.max(0, Math.min(maximum, roundBoundary
            ? Settings.videoSeekTarget(milliseconds, root.player.metaData.value(MediaMetaData.VideoFrameRate))
            : Math.round(milliseconds)))
    }

    // One frame back or forward, paused, as in mpv. Positions are whole
    // milliseconds, so aim for the middle of the frame rather than its start.
    function stepFrame(direction) {
        if (!root.player || root.player.duration <= 0) return
        if (root.playing) root.player.pause()
        const rate = root.player.metaData.value(MediaMetaData.VideoFrameRate)
        const frame = rate > 0 ? 1000 / rate : 1000 / 30
        const current = Math.floor(root.player.position / frame)
        root.seekTo(Math.round((Math.max(0, current + direction) + 0.5) * frame))
    }

    function seek(milliseconds) {
        if (root.player && root.player.duration > 0) {
            root.seekTarget(root.player.position + milliseconds, false)
        }
    }

    function adjustVolume(amount) {
        Settings.videoVolume = Math.max(0, Math.min(1, Settings.videoVolume + amount))
        if (Settings.videoVolume > 0) {
            Settings.videoMuted = false
        }
        root.say(Settings.videoMuted ? "Muted" : "Volume " + Math.round(Settings.videoVolume * 100) + "%")
    }

    function resumeVideo() {
        if (!root.player) {
            return
        }
        root.seekTo(root.resumePosition)
        root.player.play()
    }

    function restartVideo() {
        if (!root.player) {
            return
        }
        root.seekTo(0)
        Settings.clearVideoPosition(Session.path)
        root.player.play()
    }

    function toggleVideoLoop() {
        if (!Session.isVideo) return
        root.loopVideo = !root.loopVideo
        root.say(root.loopVideo ? "Video loop on" : "Video loop off")
    }

    function setPlaybackRate(rate) {
        if (!Session.isVideo || !root.player) return
        root.player.playbackRate = Math.max(0.25, Math.min(4, rate))
        if (Settings.rememberPlaybackSpeed) {
            Settings.videoPlaybackRate = root.player.playbackRate
        }
        root.say(root.player.playbackRate === 1 ? "Normal speed"
                 : "Speed " + Number(root.player.playbackRate.toFixed(2)) + "×")
    }

    function cycleAudioTrack() {
        if (!Session.isVideo || !root.hasAudioChoices) return
        const index = (root.player.activeAudioTrack + 1) % root.player.audioTracks.length
        root.player.activeAudioTrack = index
        if (root.presentation) root.presentation.trackChanged()
        let label = "Track " + (index + 1)
        try {
            const track = root.player.audioTracks[index]
            const title = track.stringValue(MediaMetaData.Title)
            const language = track.stringValue(MediaMetaData.Language)
            if (title) label += ": " + title
            else if (language) label += ": " + language
        } catch (error) {
            // A value without stringValue keeps the ordinal.
        }
        root.say("Audio: " + label)
    }

    function adjustSubtitleOffset(amount) {
        if (!Session.isVideo || root.externalSubtitle === "") return
        root.subtitleOffsetMs = Math.max(-10000, Math.min(10000, root.subtitleOffsetMs + amount))
        root.say("Subtitle delay " + (root.subtitleOffsetMs > 0 ? "+" : "")
                 + (root.subtitleOffsetMs / 1000).toFixed(1) + " s")
    }

    // Off, then each embedded track, then each sidecar file.
    function subtitleChoices() {
        const choices = [{kind: "off", label: "Off", index: -1, path: ""}]
        if (root.player) {
            const tracks = root.player.subtitleTracks
            for (let i = 0; i < tracks.length; ++i) {
                let label = "Track " + (i + 1)
                try {
                    const language = tracks[i].stringValue(MediaMetaData.Language)
                    if (language && language.length > 0) {
                        label = language
                    }
                } catch (error) {
                    // A value without stringValue keeps the ordinal.
                }
                choices.push({kind: "embedded", label: label, index: i, path: ""})
            }
        }
        for (let i = 0; i < root.subtitleFiles.length; ++i) {
            choices.push({kind: "external", label: Subtitles.label(root.subtitleFiles[i]),
                          index: -1, path: root.subtitleFiles[i]})
        }
        return choices
    }

    function cycleSubtitles() {
        const choices = root.subtitleChoices()
        if (choices.length <= 1) {
            root.say("No subtitles for this video")
            return
        }
        root.subtitleChoice = (root.subtitleChoice + 1) % choices.length
        const choice = choices[root.subtitleChoice]
        const subtitleTrack = choice.kind === "embedded" ? choice.index : -1
        const trackChanged = root.player.activeSubtitleTrack !== subtitleTrack
        root.player.activeSubtitleTrack = subtitleTrack
        if (trackChanged && root.presentation) root.presentation.trackChanged()
        root.externalSubtitle = choice.kind === "external" ? choice.path : ""
        root.say(choice.kind === "off" ? "Subtitles off" : "Subtitles: " + choice.label)
    }

    function setSlideshow(enabled) {
        if (enabled === root.slideshowRunning) {
            return
        }
        if (enabled && Session.count < 2) {
            root.say("Nothing else here to show")
            return
        }
        root.slideshowRunning = enabled
        if (enabled) {
            root.infoOpen = false
            root.pointerActive = false
            root.setFullScreen(true)
            if (Session.isVideo && !Settings.slideshowVideos) {
                root.advanceSlideshow()
            } else if (Session.isVideo && root.player) {
                root.seekTo(0)
                root.player.play()
            } else {
                root.armSlideshow()
            }
        } else {
            slideshowTimer.stop()
            root.setFullScreen(false)
        }
    }

    function resumeSlideshow() {
        if (!root.slideshowRunning || root.blockingActionOpen) return
        if (Session.isVideo && root.loadedMediaPath === Session.path) {
            if (!Settings.slideshowVideos) {
                root.advanceSlideshow()
            } else if (root.videoError !== "" || (root.player
                       && root.player.mediaStatus === MediaPlayer.EndOfMedia)) {
                slideshowTimer.restart()
            }
        } else {
            root.armSlideshow()
        }
    }

    function stepSlideshow(direction) {
        const total = Session.count
        for (let attempt = 1; attempt <= total; ++attempt) {
            const index = (Session.index + (direction < 0 ? -attempt : attempt) + total) % total
            if (!Settings.slideshowVideos && Session.isVideoAt(index)) continue
            if (index === Session.index) root.armSlideshow()
            else Session.jump(index)
            return
        }
        root.setSlideshow(false)
        root.say("No pictures here for a slideshow")
    }

    // The next file for the slideshow: any other one when shuffling, and
    // never a video when the settings leave them out.
    function advanceSlideshow() {
        if (root.blockingActionOpen) return
        const total = Session.count
        if (Settings.slideshowShuffle && total > 1) {
            const eligible = []
            for (let index = 0; index < total; ++index) {
                if (index === Session.index) {
                    continue
                }
                if (!Settings.slideshowVideos && Session.neighbourIsVideo(index - Session.index)) {
                    continue
                }
                eligible.push(index)
            }
            if (eligible.length > 0) {
                Session.jump(eligible[Math.floor(Math.random() * eligible.length)])
                return
            }
            // A sole eligible picture is where the ordered path would cycle
            // back; keep it on screen rather than reporting that none exist.
            if (!Session.isVideo || Settings.slideshowVideos) {
                slideshowTimer.restart()
                return
            }
        } else {
            root.stepSlideshow(1)
            return
        }
        root.setSlideshow(false)
        root.say("No pictures here for a slideshow")
    }

    function toggleFilmstrip() {
        Settings.viewerFilmstrip = !Settings.viewerFilmstrip
        root.revealChrome()
        root.say(Settings.viewerFilmstrip ? "Filmstrip shown" : "Filmstrip hidden")
    }

    function toggleFavorite() {
        Settings.toggleFavorite(Session.path)
        root.say(Settings.organizationError || (Settings.isFavorite(Session.path)
                 ? "Added to favourites" : "Removed from favourites"))
    }

    function rate(stars) {
        Settings.setRating([Session.path], stars)
        root.say(Settings.organizationError || (stars > 0
                 ? "Rated " + "★".repeat(stars) : "Rating cleared"))
    }

    function requestTrash() {
        if (Session.path === "") {
            return
        }
        if (confirm.visible || permanentConfirm.visible || editorChooser.visible) return
        // Bind the prompt to this file as it is now. A file swapped in under
        // the same name, or a symlink pointed elsewhere, while the prompt is
        // up is refused rather than trashed in its place.
        const target = Actions.captureFileAction(Session.deletionPath, Session.path)
        if (target.path === undefined) return
        target.viewedPath = Session.path
        confirm.target = target
        confirm.index = Session.index
        confirm.serial = Session.openSerial()
        confirm.path = target.path
        confirm.viewedPath = Session.path
        confirm.detail = confirm.path
        confirm.open()
    }

    function requestPermanentDelete() {
        if (Session.path === "" || confirm.visible || permanentConfirm.visible || actionMenu.visible
                || editorChooser.visible) return
        const entry = Session.deletionPath
        const viewedPath = Session.path
        const target = Actions.capturePermanentDelete(entry, viewedPath)
        if (target.parent === undefined) return
        if (!Settings.confirmPermanentDelete) {
            root.deletePermanently(target, viewedPath)
            return
        }
        permanentConfirm.path = entry
        permanentConfirm.target = target
        permanentConfirm.viewedPath = viewedPath
        permanentConfirm.detail = "This skips Trash and cannot be undone.\n\n" + entry
        permanentConfirm.open()
    }

    function deletePermanently(target, viewedPath) {
        if (Actions.deleteCapturedPermanently(target)) {
            Session.forget(viewedPath)
            root.say("Permanently deleted")
        }
    }

    // The keys a viewer is expected to have, and what the menu shows beside
    // each entry. They are this window's own: F is full screen here, as in
    // every player, rather than the library's Open containing folder.
    readonly property var viewerShortcuts: ShortcutCatalog.quickHints

    // Handed-off actions, in the order the menu offers them. Only what is
    // installed and suits the medium is shown.
    readonly property var stillActions: ["develop", "choose-editor", "copy", "open-with", "omaframe", "annotate", "edit", "background", "send", "print", "files"]
    readonly property var videoActions: ["frame", "trim", "copy", "open-with", "play", "send", "files"]

    function menuEntries() {
        void root.marksVersion
        const video = Session.isVideo
        const entries = [{id: "library", label: "Open in library"}, {separator: true}]
        if (Session.companionPath !== "") {
            entries.push({id: "companion", label: Session.isRaw ? "View JPEG companion" : "View RAW companion"})
        }
        const wanted = video ? root.videoActions : root.stillActions
        const rows = Registry.actionsForKind(video, false, Session.path)
        let group = ""
        for (const row of rows) {
            const id = row.id
            if (wanted.indexOf(id) < 0) continue
            if (group !== "" && row.group !== group) entries.push({separator: true})
            group = row.group
            let label = row.label
            if (id === "copy" && video) label = "Copy file"
            if (id === "play") label = "Open in mpv"
            entries.push({id: id, label: label, available: row.available, hint: row.hint})
        }
        entries.push({separator: true})
        if (video) {
            entries.push({id: "video-loop", label: "Loop video: " + (root.loopVideo ? "On" : "Off")})
            if (root.hasAudioChoices) {
                entries.push({id: "audio-track", label: "Cycle audio track"})
            }
            if (root.externalSubtitle !== "") {
                entries.push({id: "subtitles-earlier", label: "Subtitles earlier"})
                entries.push({id: "subtitles-later", label: "Subtitles later"})
            }
        }
        entries.push({id: "copy-path", label: "Copy path"})
        entries.push({id: "copy-name", label: "Copy file name"})
        entries.push({separator: true})
        if (!video) {
            // Nothing to fit or turn when the picture could not be shown.
            const viewable = !root.imageError
            entries.push({id: "fit", label: "Fit", available: viewable})
            entries.push({id: "fit-width", label: "Fit width", available: viewable})
            entries.push({id: "fill", label: "Fill window", available: viewable})
            entries.push({id: "actual", label: "Actual size", available: viewable})
            entries.push({id: "fit-shrink", label: "Fit without enlarging", available: viewable})
            entries.push({id: "rotate", label: "Rotate", available: viewable})
            entries.push({id: "flip-horizontal", label: "Flip horizontally", available: viewable})
            entries.push({id: "flip-vertical", label: "Flip vertically", available: viewable})
            if (root.canSaveViewCopy) {
                entries.push({id: "save-view-copy", label: root.viewCopyLabel,
                              available: !root.savingViewCopy})
            }
        }
        if (Session.count > 1) {
            entries.push({id: "slideshow",
                          label: root.slideshowRunning ? "Stop slideshow" : "Slideshow"})
        }
        if (Session.count > 1) {
            entries.push({id: "filmstrip",
                          label: Settings.viewerFilmstrip ? "Hide filmstrip" : "Show filmstrip"})
        }
        entries.push({id: "fullscreen", label: root.fullScreen ? "Exit full screen" : "Full screen"})
        entries.push({id: "info", label: root.infoOpen ? "Hide details" : "Details"})
        entries.push({id: "help", label: "Keyboard shortcuts"})
        entries.push({separator: true})
        entries.push({id: "favorite",
                      label: root.favorite ? "Remove from favourites" : "Add to favourites"})
        entries.push({id: "trash", label: "Move to Trash"})
        return entries
    }

    function performMenuAction(id) {
        root.perform(id)
        actionMenu.close()
    }

    function perform(id) {
        const path = Session.path
        if (path === "") {
            return
        }
        switch (id) {
        case "video-loop":
            root.toggleVideoLoop()
            return
        case "audio-track":
            root.cycleAudioTrack()
            return
        case "subtitles-earlier":
        case "subtitles-later":
            root.adjustSubtitleOffset(id === "subtitles-earlier" ? -500 : 500)
            return
        case "copy-path":
        case "copy-name":
            root.copyPath(id === "copy-name")
            return
        case "fit":
            root.fitToWindow()
            return
        case "fill":
            root.toggleFill()
            return
        case "fit-width":
        case "fit-shrink":
            root.setFitMode(id === "fit-width" ? "width" : "shrink")
            return
        case "actual":
            root.showActualSize()
            return
        case "help":
            shortcutHelp.open()
            return
        case "companion":
            Session.switchCompanion()
            return
        case "develop":
        case "choose-editor":
            if (Session.isRaw) editorChooser.request(path, id === "choose-editor")
            return
        case "library":
            Session.openInLibrary()
            return
        case "rotate":
            root.rotate(true)
            return
        case "flip-horizontal":
        case "flip-vertical":
            root.flip(id === "flip-horizontal")
            return
        case "save-view-copy":
            root.saveViewCopy()
            return
        case "slideshow":
            root.setSlideshow(!root.slideshowRunning)
            return
        case "fullscreen":
            root.setFullScreen(!root.fullScreen)
            return
        case "filmstrip":
            root.toggleFilmstrip()
            return
        case "info":
            root.infoOpen = !root.infoOpen
            return
        case "favorite":
            root.toggleFavorite()
            return
        case "trash":
            root.requestTrash()
            return
        case "play":
            Registry.runBatchWith("play", {"seek": (Math.max(0, root.player ? root.player.position : 0) / 1000).toFixed(3)}, [path])
            return
        case "frame": {
            if (!Session.isVideo || !root.player) {
                return
            }
            const position = Math.max(0, root.player.position)
            Registry.runBatchWith("frame", {"seek": (position / 1000).toFixed(3),
                                            "frame": root.frameStamp(position)}, [path])
            return
        }
        default:
            if (!Registry.appliesToKind(id, Session.isVideo, false, path)) {
                root.say(Session.isRaw && Registry.appliesToKind(id, false, false)
                         ? "That one cannot open camera raws"
                         : Session.isVideo ? "That one is for pictures" : "That one is for videos")
                return
            }
            Registry.run(id, path)
        }
    }

    function frameStamp(milliseconds) {
        const value = Math.max(0, Math.floor(milliseconds))
        const pad = function (number, width) { return String(number).padStart(width, "0") }
        const hours = Math.floor(value / 3600000)
        return (hours > 0 ? pad(hours, 2) + "h" : "")
               + pad(Math.floor((value % 3600000) / 60000), 2) + "m"
               + pad(Math.floor((value % 60000) / 1000), 2) + "s" + pad(value % 1000, 3)
    }

    // Asked by --render before it grabs. A video render waits for the frame
    // it holds, a second in, rather than the poster; one that never decodes
    // times out and writes nothing.
    property string renderView: ""
    function renderReady() {
        if (root.renderView !== "viewer-video") return true
        const player = root.player
        return root.videoError === "" && !!player && !!root.presentation && root.presentation.ready
               && player.position >= 1000 && player.playbackState !== MediaPlayer.PlayingState
    }

    // Renders hold a known state so the screenshot is the same every time.
    function openViewForRender(view) {
        root.renderView = view
        root.chromePinned = true
        if (view === "viewer-video") {
            // Back to the start, then held on a frame a second in.
            root.videoPausedForRender = true
            root.resumeAvailable = false
            if (root.player) {
                root.seekTo(0)
                root.player.play()
            }
        } else if (view === "viewer-info") {
            root.infoOpen = true
        } else if (view === "viewer-help") {
            shortcutHelp.open()
        } else if (view === "viewer-menu") {
            actionMenu.popup(moreButton, moreButton.width - actionMenu.implicitWidth,
                             moreButton.height + 6)
        }
    }

    // A new file: forget the previous one's zoom, rotation and playback.
    Connections {
        target: Session
        function onCurrentChanged() {
            const samePath = root.loadedMediaPath === Session.path
            if (samePath && root.loadedMediaVersion === Session.contentVersion) {
                return
            }
            actionMenu.close()
            root.loadedMediaPath = Session.path
            root.loadedMediaVersion = Session.contentVersion
            if (samePath && Session.isVideo && root.player) {
                // The local URL stays the same after a save. Clear the player
                // source for one turn so it opens the replacement file.
                root.reloadingVideo = true
                Qt.callLater(function () { root.reloadingVideo = false })
            }
            root.animationPlaying = true
            root.resetView()
            root.resumeAvailable = false
            root.resumePosition = 0
            root.resumePending = true
            root.externalSubtitle = ""
            root.subtitleChoice = 0
            root.subtitleOffsetMs = 0
            if (Session.isVideo) {
                Qt.callLater(root.startPlayer)
                root.subtitleFiles = Subtitles.files(Session.path)
            } else {
                root.subtitleFiles = []
            }
            if (root.infoOpen && Session.path !== "") {
                MediaInfo.inspect(Session.path, Session.isVideo)
            }
            // A cached picture is ready before this handler runs, so its
            // readiness never changes; arm once the bindings have settled.
            Qt.callLater(root.armSlideshow)
        }
        function onEmptied() {
            root.close()
        }
    }

    // The backend is created on the first video and kept for later ones.
    // Creating it takes about a frame, so it starts on the next turn, after the
    // window is loaded, and the poster and first decoded frame load side by side.
    function startPlayer() {
        if (Session.isVideo && !playerLoader.active) {
            Settings.prepareVideoPlayback()
            playerLoader.active = true
        }
    }

    onInfoOpenChanged: {
        if (root.infoOpen && Session.path !== "" && MediaInfo.path !== Session.path) {
            MediaInfo.inspect(Session.path, Session.isVideo)
        }
    }

    onVisibleChanged: {
        if (root.visible) {
            keys.forceActiveFocus()
            root.revealChrome()
        }
    }

    // A closed viewer forgets its file: the next open must not flash the last
    // picture, and a hidden window must not keep playing sound.
    onClosing: {
        root.fitMode = "fit"
        root.loopVideo = false
        root.slideshowRunning = false
        slideshowTimer.stop()
        if (root.player) {
            root.player.stop()
        }
        root.infoOpen = false
        root.undoId = 0
        confirm.close()
        permanentConfirm.close()
        actionMenu.close()
        shortcutHelp.close()
        Session.clear()
    }

    Connections {
        target: Settings
        function onMarksChanged() { root.marksVersion++ }
        // Undo put back a file this window moved to Trash: show it again
        // where it was.
        function onTrashRestored(records, origin) {
            if (origin !== Session) return
            for (const record of records) {
                Session.restore(record.viewedPath !== undefined ? record.viewedPath : record.path,
                                record.path, record.index !== undefined ? record.index : 0,
                                record.openSerial !== undefined ? record.openSerial : -1)
            }
        }
        function onOrganizationErrorChanged() {
            if (root.visible && root.frontmost && Settings.organizationError !== "")
                root.say(Settings.organizationError)
        }
    }

    Connections {
        target: Actions
        function onFailed(message) { if (root.visible && root.frontmost) root.say(message) }
        function onReported(message) { if (root.visible && root.frontmost) root.say(message) }
    }

    Timer {
        id: chromeTimer
        interval: root.chromeTimeout
        onTriggered: root.pointerActive = false
    }

    Timer {
        id: statusTimer
        interval: 2600
        onTriggered: root.status = ""
    }

    Timer {
        id: slideshowTimer
        interval: Settings.slideshowIntervalSeconds * 1000
        onTriggered: if (root.slideshowRunning && root.visible) root.advanceSlideshow()
    }

    // The canvas: the theme's translucent surface, like the library. Full
    // screen there is nothing behind it worth showing, so it is solid there.
    Rectangle {
        anchors.fill: parent
        visible: root.fullScreen
        color: Theme.surfaceBackground
    }
    Chrome {
        anchors.fill: parent
    }

    Item {
        id: stage
        objectName: "viewerStage"
        anchors.fill: parent

        // Stills. Fitted on open, zoomed around the pointer, panned once
        // larger than the window, rotated for viewing without touching the file.
        Flickable {
            id: still
            objectName: "viewerStill"
            anchors.fill: parent
            visible: !Session.isVideo
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            // A drag follows the pointer; a flick glides a little and stops,
            // rather than sailing down a long screenshot.
            flickDeceleration: 4000
            maximumFlickVelocity: 4000
            interactive: root.effectiveScale > root.fitScale * 1.001
                         && (contentWidth > width + 0.5 || contentHeight > height + 0.5)
            contentWidth: Math.max(width, root.displayWidth)
            contentHeight: Math.max(height, root.displayHeight)
            // A filled picture stays centred as its tile or picture changes size.
            onWidthChanged: root.keepFillCentred()
            onHeightChanged: root.keepFillCentred()
            onContentWidthChanged: root.keepFillCentred()
            onContentHeightChanged: root.keepFillCentred()

            // A double click zooms to the real size and back; a picture
            // already shown at its real size zooms to twice that instead. Being
            // inside the Flickable, it hands the press over once it becomes a
            // drag, so a zoomed picture still pans.
            TapHandler {
                acceptedButtons: Qt.LeftButton
                onDoubleTapped: function (eventPoint) {
                    if (root.viewScale > 0) {
                        root.fitToWindow()
                        return
                    }
                    const at = stage.mapFromItem(null, eventPoint.scenePosition)
                    const actual = Math.abs(root.fitScale - root.actualScale)
                                   > root.actualScale * 0.01
                    root.zoomTo(actual ? root.actualScale : root.fitScale * 2, at.x, at.y)
                }
            }

            Item {
                id: frame
                objectName: "viewerFrame"
                width: root.displayWidth
                height: root.displayHeight
                x: (still.contentWidth - width) / 2
                y: (still.contentHeight - height) / 2
                // Across the screen, after the picture inside has turned.
                transform: Scale {
                    origin.x: frame.width / 2
                    origin.y: frame.height / 2
                    xScale: root.viewFlipHorizontal ? -1 : 1
                    yScale: root.viewFlipVertical ? -1 : 1
                }

                // On a light theme's pale surround, a hairline and a soft
                // shadow keep a white picture's edges visible.
                Rectangle {
                    objectName: "viewerPictureEdge"
                    anchors.centerIn: parent
                    width: stillLoader.width + 2
                    height: stillLoader.height + 2
                    rotation: root.viewRotation
                    visible: root.stillReady && !root.imageError && root.lightSurround
                    color: "transparent"
                    border.width: 1
                    border.color: Qt.rgba(0, 0, 0, 0.16)

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        z: -1
                        radius: 3
                        color: "transparent"
                        border.width: 3
                        border.color: Qt.rgba(0, 0, 0, 0.05)
                    }
                }

                // The picture's own area is solid, so a transparent PNG shows
                // the theme's surface rather than the desktop through it.
                Rectangle {
                    objectName: "viewerPictureBacking"
                    anchors.centerIn: parent
                    width: stillLoader.width
                    height: stillLoader.height
                    rotation: root.viewRotation
                    visible: root.stillReady && !root.imageError
                    color: Theme.surfaceBackground
                }

                Loader {
                    id: stillLoader
                    objectName: "viewerStillLoader"
                    anchors.centerIn: parent
                    width: root.sourceWidth * root.effectiveScale
                    height: root.sourceHeight * root.effectiveScale
                    rotation: root.viewRotation
                    sourceComponent: Session.isAnimated ? animatedStill : staticStill
                    opacity: 1

                    NumberAnimation on opacity {
                        id: fadeIn
                        from: 0
                        to: 1
                        duration: 140
                        easing.type: Easing.OutQuad
                        running: false
                    }
                }
            }
        }

        Component {
            id: staticStill
            Image {
                objectName: "viewerImage"
                source: root.visible && !Session.isVideo ? Session.imageUrl : ""
                asynchronous: true
                // Wide-gamut pictures are converted to the screen's sRGB.
                // The preloaded neighbours match, so they share its cache.
                colorSpace.namedColorSpace: ColorSpace.SRgb
                autoTransform: true
                // Smooth while fitted, even when a small picture is scaled up;
                // crisp pixels once zoomed well past their real size.
                smooth: root.viewScale === 0 || root.effectiveScale * root.dpr < 2
                mipmap: true
                fillMode: Image.Stretch
                // A preloaded neighbour is simply there; one that had to be
                // read fades in rather than popping.
                property bool waited: false
                onStatusChanged: {
                    if (status === Image.Loading) {
                        waited = true
                    } else if (status === Image.Ready && waited) {
                        fadeIn.restart()
                    }
                    if (status !== Image.Loading) {
                        waited = false
                    }
                }

                // The full decode of a camera raw, over its preview, or a
                // large picture's full resolution over its reduced copy.
                // That one only shows while zoomed in: zoomed out, the
                // mipmapped copy looks better than a full-size texture
                // shrunk without mipmaps, which it would cost too much to
                // make.
                Image {
                    objectName: "viewerDetail"
                    anchors.fill: parent
                    source: root.visible && (root.detailWanted
                                             || root.detailPath === Session.path)
                            ? root.detailSource : ""
                    asynchronous: true
                    colorSpace.namedColorSpace: ColorSpace.SRgb
                    autoTransform: !Session.isRaw
                    smooth: parent.smooth
                    mipmap: Session.isRaw
                    fillMode: Image.Stretch
                    opacity: status === Image.Ready && (Session.isRaw || root.detailWanted) ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 140 } }
                    onStatusChanged: {
                        if (status === Image.Ready) {
                            root.detailPath = Session.path
                        }
                    }
                }

                // A very large JPEG zoomed past its copy: only the part on
                // screen, at the screen's resolution.
                PictureArea {
                    objectName: "viewerArea"
                    viewport: still
                    wanted: root.visible && Session.areas && root.enlargedPastCopy
                    pictureScale: root.effectiveScale
                    devicePixelRatio: root.dpr
                    fullSize: Qt.size(root.sourceWidth, root.sourceHeight)
                    path: Session.path
                    urlFor: (rect, size) => Session.areaUrl(rect, size)
                    viewState: [root.viewRotation, root.viewFlipHorizontal, root.viewFlipVertical]
                    smooth: parent.smooth
                }
            }
        }

        Component {
            id: animatedStill
            AnimatedImage {
                objectName: "viewerAnimation"
                source: root.visible && !Session.isVideo ? Session.imageUrl : ""
                asynchronous: true
                autoTransform: true
                cache: false
                playing: root.visible && root.animationPlaying
                smooth: root.viewScale === 0 || root.effectiveScale * root.dpr < 2
                fillMode: Image.Stretch
            }
        }

        // Original-resolution neighbours admitted by the session's shared
        // decoded-pixel budget. Their URLs match the displayed Image's cache.
        Repeater {
            model: [1, -1]
            Image {
                required property int modelData
                objectName: "viewerPrefetch" + modelData
                visible: false
                asynchronous: true
                colorSpace.namedColorSpace: ColorSpace.SRgb
                autoTransform: true
                source: root.visible ? (modelData === 1 ? Session.nextPreloadUrl
                                                       : Session.previousPreloadUrl) : ""
            }
        }

        // A recording shows its thumbnail until the first frame is decoded.
        Image {
            id: videoPoster
            objectName: "viewerVideoPoster"
            anchors.fill: parent
            visible: Session.isVideo && !(root.presentation && root.presentation.ready)
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            // The thumbnail provider applies the ratio from its URL once.
            sourceSize: Qt.size(Math.round(width), Math.round(height))
            source: root.visible && Session.isVideo && Session.path !== ""
                    ? "image://thumbs/" + root.dpr + "@40~" + Session.thumbnailVersion
                      + encodeURIComponent(Session.path)
                    : ""
        }

        Loader {
            id: playerLoader
            active: false
            sourceComponent: Component {
                MediaPlayer {
                    id: mediaPlayer
                    objectName: "viewerPlayer"
                    property string resumeIdentity: ""
                    // A --start time stays wanted until playback is seen at
                    // or past it, so a seek the backend loses is sent again.
                    property real launchTarget: -1
                    property int launchRetries: 0
                    function applyLaunchStart() {
                        if (Session.pendingStartPosition(Session.path) < 0) return false
                        root.resumePending = false
                        root.resumeAvailable = false
                        if (seekable && duration > 0 && (mediaStatus === MediaPlayer.LoadedMedia
                                || mediaStatus === MediaPlayer.BufferingMedia || mediaStatus === MediaPlayer.BufferedMedia)) {
                            launchTarget = Math.min(duration, Session.takeStartPosition(Session.path))
                            launchRetries = 0
                            // A seek like any other: seek previews hold off.
                            transport.noteSeek()
                            position = launchTarget
                            play()
                        }
                        return true
                    }
                    function confirmLaunchStart() {
                        if (launchTarget < 0 || playbackState !== MediaPlayer.PlayingState) return
                        if (position >= launchTarget - 500 || launchRetries >= 2) {
                            launchTarget = -1
                            return
                        }
                        ++launchRetries
                        transport.noteSeek()
                        position = launchTarget
                    }
                    onSeekableChanged: applyLaunchStart()
                    onDurationChanged: applyLaunchStart()
                    property Connections launchConnection: Connections {
                        target: Session
                        function onStartPositionChanged() { mediaPlayer.applyLaunchStart() }
                    }
                    source: root.videoSource
                    videoOutput: root.presentation
                    // Lets the presentation keep the picture at the position
                    // after an early pause or a track change.
                    property Binding presentationPlayer: Binding {
                        target: root.presentation
                        property: "player"
                        value: mediaPlayer
                        when: root.presentation !== null
                    }
                    audioOutput: AudioOutput {
                        volume: Settings.videoVolume
                        muted: Settings.videoMuted || !root.audible
                    }
                    onSourceChanged: {
                        if (root.presentation) root.presentation.reset()
                        launchTarget = -1
                        root.videoError = ""
                        if (source.toString() !== "") {
                            playbackRate = Settings.rememberPlaybackSpeed ? Settings.videoPlaybackRate : 1
                            mediaPlayer.resumeIdentity = Settings.videoIdentity(Session.path)
                            Mpris.track(mediaPlayer, root)
                            play()
                        } else {
                            Mpris.release(mediaPlayer)
                        }
                    }
                    // A clip the backend cannot decode says so rather than
                    // sitting behind a dead play button. A slideshow moves on.
                    onErrorOccurred: function (error, errorString) {
                        root.videoError = errorString !== "" ? errorString
                                                             : "Could not play this file"
                        if (root.slideshowRunning) {
                            slideshowTimer.restart()
                        }
                    }
                    onPositionChanged: {
                        confirmLaunchStart()
                        if (root.videoPausedForRender && mediaPlayer.position >= 1000
                                && mediaPlayer.playbackState === MediaPlayer.PlayingState) {
                            mediaPlayer.pause()
                        }
                    }
                    // Remember the spot on pause; the timer covers long
                    // uninterrupted playback.
                    onPlaybackStateChanged: {
                        if (playbackState === MediaPlayer.PlayingState && root.resumeAvailable) {
                            pause()
                            return
                        }
                        if (playbackState === MediaPlayer.PausedState && duration > 0
                                && position >= 5000 && position < duration - 3000) {
                            Settings.setVideoPosition(Session.path, Math.round(position), mediaPlayer.resumeIdentity)
                        }
                    }
                    onMediaStatusChanged: {
                        const explicitStart = applyLaunchStart()
                        if (mediaStatus === MediaPlayer.LoadedMedia && root.resumePending
                                && !root.slideshowRunning && !explicitStart) {
                            root.resumePending = false
                            const saved = Settings.videoPosition(Session.path)
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
                            root.advanceSlideshow()
                            return
                        }
                        if (root.loopVideo) {
                            root.restartVideo()
                            return
                        }
                        // Hold the first frame rather than a blank stage, so
                        // the play button reads as replay.
                        pause()
                        position = 0
                        Settings.clearVideoPosition(Session.path)
                        root.resumeAvailable = false
                    }
                }
            }
        }

        Timer {
            interval: 5000
            repeat: true
            running: root.visible && Session.isVideo && root.playing
            onTriggered: {
                const player = root.player
                if (player.duration > 0 && player.position >= 5000
                        && player.position < player.duration - 3000) {
                    Settings.setVideoPosition(Session.path, Math.round(player.position), player.resumeIdentity)
                }
            }
        }

        Item {
            id: videoOutput
            anchors.fill: parent
            visible: Session.isVideo && root.presentation && root.presentation.hasFrame
                     && (root.presentation.ready || videoPoster.status !== Image.Ready)
            readonly property QtObject videoSink: videoSurface.item ? videoSurface.item.videoSink : null

            Loader {
                id: videoSurface
                anchors.fill: parent
                active: playerLoader.active
                sourceComponent: VideoOutput {
                    objectName: "viewerVideoOutput"
                    fillMode: VideoOutput.PreserveAspectFit
                }
            }
        }

        Text {
            objectName: "viewerSubtitles"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: root.chromeShown && Session.isVideo
                                  ? transport.height + 40
                                    + (filmstrip.visible ? filmstrip.height + 10 : 0)
                                  : 36
            width: Math.max(0, parent.width - 120)
            visible: Session.isVideo && root.subtitleText !== ""
            text: root.subtitleText
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            font.family: Theme.fontFamily
            font.pixelSize: Math.max(16, Math.min(30, parent.height / 24))
            font.weight: Font.DemiBold
            color: "white"
            style: Text.Outline
            styleColor: "black"
            Behavior on anchors.bottomMargin { NumberAnimation { duration: 180 } }
        }

        // Gestures sit above the media without taking it over: the tap and
        // wheel handlers only watch, so a drag still pans a zoomed picture,
        // and the controls above accept their own clicks first.
        Item {
            id: gestures
            objectName: "viewerGestures"
            anchors.fill: parent

            // Pointer movement over the window brings the controls back.
            // Qt Quick repeats the hover every frame while anything on screen
            // changes, a playing video included, so only a real move counts.
            HoverHandler {
                property point last: Qt.point(-1, -1)
                // A zoomed picture can be dragged, and the hand says so.
                cursorShape: root.pointerHidden ? Qt.BlankCursor
                             : still.interactive ? (still.dragging ? Qt.ClosedHandCursor
                                                                   : Qt.OpenHandCursor)
                             : Qt.ArrowCursor
                onPointChanged: {
                    const at = point.position
                    if (Math.abs(at.x - last.x) >= 1 || Math.abs(at.y - last.y) >= 1) {
                        last = at
                        root.revealChrome()
                    }
                }
            }

            // On a video a click plays or pauses and a double click goes full
            // screen. Pictures take their clicks inside the pan area below:
            // a handler up here takes the press for itself, and a drag on a
            // zoomed picture would never reach it.
            TapHandler {
                acceptedButtons: Qt.LeftButton
                enabled: Session.isVideo
                onSingleTapped: root.togglePlayback()
                onDoubleTapped: root.setFullScreen(!root.fullScreen)
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: function (eventPoint) {
                    const at = stage.mapFromItem(null, eventPoint.scenePosition)
                    actionMenu.popup(stage, at.x, at.y)
                }
            }

            // The wheel zooms a picture; a touchpad scroll pans one that is
            // zoomed in, and a pinch zooms.
            WheelHandler {
                target: null
                enabled: !Session.isVideo
                // The default takes mice only, and Qt reports some wheels on
                // Wayland as a touchpad, which then never reached this.
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: function (event) {
                    const at = still.mapFromItem(null, point.scenePosition)
                    // Told apart by what the events do, not by the device's
                    // label: Qt can call a wheel a touchpad, and a wheel can
                    // carry pixel deltas. A touchpad swipe comes in phases
                    // with pixel deltas; a wheel notch has no phase.
                    const touchpad = event.phase !== Qt.NoScrollPhase
                                     && (event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0)
                    if (touchpad && !(event.modifiers & Qt.ControlModifier)) {
                        if (root.viewScale > 0) {
                            still.contentX = Math.max(0, Math.min(still.contentWidth - still.width,
                                                                  still.contentX - event.pixelDelta.x))
                            still.contentY = Math.max(0, Math.min(still.contentHeight - still.height,
                                                                  still.contentY - event.pixelDelta.y))
                        }
                        event.accepted = true
                        return
                    }
                    const delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.pixelDelta.y
                    if (delta !== 0) {
                        root.zoomBy(Math.pow(1.15, delta / 120), at.x, at.y)
                    }
                    event.accepted = true
                }
            }

            PinchHandler {
                target: null
                enabled: !Session.isVideo
                property real last: 1
                onActiveChanged: last = 1
                onActiveScaleChanged: {
                    if (!active || last <= 0) {
                        return
                    }
                    const at = still.mapFromItem(null, centroid.scenePosition)
                    root.zoomBy(activeScale / last, at.x, at.y, true)
                    last = activeScale
                }
            }

            // Ctrl+drag takes the file itself into another app. Taken over
            // from the pan, which a plain drag keeps.
            DragHandler {
                id: stageFileDrag
                objectName: "viewerStageFileDrag"
                target: null
                acceptedButtons: Qt.LeftButton
                acceptedModifiers: Qt.ControlModifier
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                grabPermissions: PointerHandler.CanTakeOverFromItems
                                 | PointerHandler.CanTakeOverFromHandlersOfDifferentType
                                 | PointerHandler.ApprovesTakeOverByHandlersOfSameType
                onActiveChanged: if (active) root.startFileDrag(stageFileDrag)
            }

            // Mouse side buttons use the viewer's existing file order. Keep
            // their scope here so normal clicks, menus and video controls can
            // still take their own input above this layer.
            MouseArea {
                objectName: "viewerSideButtonNavigation"
                anchors.fill: parent
                acceptedButtons: Qt.BackButton | Qt.ForwardButton
                enabled: Session.count > 1 && !actionMenu.visible && !confirm.visible && !permanentConfirm.visible
                         && !editorChooser.visible
                onClicked: function (mouse) {
                    root.step(mouse.button === Qt.BackButton ? -1 : 1)
                }
            }
        }

        // A file that will not open says why in the middle of the window,
        // with what can still be done with it.
        Item {
            id: errorState
            objectName: "viewerErrorState"
            anchors.centerIn: parent
            width: errorColumn.width
            height: errorColumn.implicitHeight
            visible: root.playbackError !== ""
            // Tab reaches these buttons too; every other key is the viewer's.
            Keys.forwardTo: [keys]

            // A press on the prompt stays here rather than also reaching the
            // tap layer underneath, where it would play or pause.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
            }

            Column {
                id: errorColumn
                width: Math.min(440, stage.width - 48)
                spacing: 10

                Icon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: !Session.isVideo
                    name: "warning"
                    size: 30
                    color: Theme.red
                }
                Text {
                    objectName: "viewerErrorTitle"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: root.errorTitle
                    font.family: Theme.fontFamily
                    font.pixelSize: Session.isVideo ? 13 : 15
                    font.weight: Session.isVideo ? Font.Normal : Font.DemiBold
                    color: Session.isVideo ? Theme.red : Theme.brightForeground
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
                Text {
                    objectName: "viewerErrorDetail"
                    width: parent.width
                    visible: text !== ""
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: root.errorDetail
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Theme.mutedText
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
                Item { width: 1; height: 2 }
                // Side by side where they fit, stacked in a narrow window.
                Grid {
                    id: errorActions
                    anchors.horizontalCenter: parent.horizontalCenter
                    columns: errorColumn.width >= 400 ? 4 : 1
                    columnSpacing: 8
                    rowSpacing: 8
                    // Nothing is left to act on once the file has gone.
                    readonly property bool present: root.errorCause !== "missing"

                    PillButton {
                        objectName: "viewerErrorPlay"
                        visible: Session.isVideo && Registry.available("play")
                        label: "Open in mpv"
                        onClicked: Registry.run("play", Session.path)
                    }
                    PillButton {
                        objectName: "viewerErrorOpenWith"
                        visible: errorActions.present && Registry.available("open-with")
                        label: "Open with…"
                        shortcut: "Ctrl+O"
                        onClicked: root.perform("open-with")
                    }
                    PillButton {
                        objectName: "viewerErrorShowInFolder"
                        visible: errorActions.present && Registry.available("files")
                        label: "Show in folder"
                        onClicked: root.perform("files")
                    }
                    PillButton {
                        objectName: "viewerErrorTrash"
                        visible: errorActions.present
                        label: "Move to Trash"
                        shortcut: "Del"
                        onClicked: root.requestTrash()
                    }
                }
            }
        }

        // A saved spot is offered rather than jumped to.
        Item {
            objectName: "viewerResumePrompt"
            anchors.centerIn: parent
            width: resumeRow.implicitWidth
            height: resumeRow.implicitHeight
            visible: Session.isVideo && root.resumeAvailable && !root.slideshowRunning

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
            }

            Row {
                id: resumeRow
                spacing: 8

                PillButton {
                    objectName: "viewerResumeButton"
                    label: "Resume from " + transport.clock(root.resumePosition)
                    floating: true
                    osd: true
                    active: true
                    onClicked: root.resumeVideo()
                }
                PillButton {
                    label: "Start over"
                    floating: true
                    osd: true
                    onClicked: root.restartVideo()
                }
            }
        }
    }

    // Everything below fades with the pointer.
    Item {
        id: chrome
        anchors.fill: parent
        opacity: root.chromeShown ? 1 : 0
        // At once when asked, so Tab can land on a control in the same turn.
        visible: opacity > 0 || root.chromeShown
        Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutQuad } }
        // A focused control still hears the viewer's shortcuts.
        Keys.forwardTo: [keys]

        // Header: the file and where it sits in the folder, then the three
        // ways out of the picture.
        Item {
            id: header
            objectName: "viewerHeader"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 64
            readonly property bool hovered: headerHover.hovered

            HoverHandler { id: headerHover }

            // A dark scrim in every theme: a light one bleaches the top of
            // the frame. Strong enough behind the name for light text over a
            // white picture, gone by the bottom edge.
            Rectangle {
                objectName: "viewerHeaderScrim"
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Osd.scrimTop }
                    GradientStop { position: 0.55; color: Osd.scrimMiddle }
                    GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0) }
                }
            }

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.right: headerButtons.left
                anchors.rightMargin: 12
                anchors.top: parent.top
                anchors.topMargin: 14
                spacing: 10

                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.favorite
                    name: "star-filled"
                    size: 13
                    color: Theme.yellow
                }

                Text {
                    id: titleText
                    objectName: "viewerTitle"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, parent.width - counterText.implicitWidth - 30)
                    text: Session.fileName
                    elide: Text.ElideMiddle
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: Osd.brightForeground
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                    Accessible.description: "Drag the name, or Ctrl+drag the picture, to copy the file into another app"

                    // The name is a handle for the file: drag it into a file
                    // manager, a browser or a chat.
                    HoverHandler {
                        cursorShape: titleFileDrag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                    }
                    DragHandler {
                        id: titleFileDrag
                        objectName: "viewerTitleFileDrag"
                        target: null
                        acceptedButtons: Qt.LeftButton
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        onActiveChanged: if (active) root.startFileDrag(titleFileDrag)
                    }
                }

                Text {
                    id: counterText
                    objectName: "viewerCounter"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: Session.count > 1
                    text: (Session.index + 1) + " / " + Session.count
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Osd.foreground
                }
            }

            Row {
                id: headerButtons
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.top: parent.top
                anchors.topMargin: 6
                spacing: 2

                IconButton {
                    objectName: "viewerInfoButton"
                    osd: true
                    icon: "info"
                    toolTip: root.infoOpen ? "Hide details" : "Details"
                    shortcut: root.viewerShortcuts.info
                    active: root.infoOpen
                    onClicked: root.infoOpen = !root.infoOpen
                }
                IconButton {
                    objectName: "viewerLibraryButton"
                    osd: true
                    icon: "grid"
                    toolTip: "Open in library"
                    shortcut: root.viewerShortcuts.library
                    onClicked: Session.openInLibrary()
                }
                IconButton {
                    id: moreButton
                    objectName: "viewerMoreButton"
                    osd: true
                    icon: "more"
                    toolTip: "More"
                    active: actionMenu.visible
                    onClicked: actionMenu.visible
                               ? actionMenu.close()
                               : actionMenu.popup(moreButton,
                                                  moreButton.width - actionMenu.implicitWidth,
                                                  moreButton.height + 6)
                }
            }
        }

        IconButton {
            id: previousButton
            objectName: "viewerPrevious"
            osd: true
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            // Pictures step from the toolbar; a video's bar has no room for it.
            visible: Session.isVideo && Session.count > 1
            raised: true
            implicitWidth: 40
            implicitHeight: 40
            icon: "chevron-left"
            iconSize: 20
            toolTip: "Previous"
            shortcut: ShortcutCatalog.display([ShortcutCatalog.sequence("quick", Session.isVideo ? "step" : "previous")[0]])
            onClicked: root.step(-1)
        }
        IconButton {
            id: nextButton
            objectName: "viewerNext"
            osd: true
            anchors.right: parent.right
            // Clear of the details card while it is open.
            anchors.rightMargin: root.infoOpen ? infoPanel.width + 28 : 14
            anchors.verticalCenter: parent.verticalCenter
            visible: Session.isVideo && Session.count > 1
            raised: true
            implicitWidth: 40
            implicitHeight: 40
            icon: "chevron-right"
            iconSize: 20
            toolTip: "Next"
            shortcut: ShortcutCatalog.display([ShortcutCatalog.sequence("quick", Session.isVideo ? "step" : "previous")[1]])
            onClicked: root.step(1)
        }

        ViewerToolbar {
            id: toolbar
            objectName: "viewerToolbar"
            visible: !Session.isVideo
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            compact: parent.width < 460
            viewControls: !root.imageError
            fitted: root.viewScale === 0 && root.fitMode === "fit"
            canStep: Session.count > 1
            slideshowRunning: root.slideshowRunning
            onZoomOut: root.zoomBy(1 / 1.25)
            onZoomIn: root.zoomBy(1.25)
            onToggleFit: root.toggleFit()
            onPrevious: root.step(-1)
            onNext: root.step(1)
            onSlideshow: root.setSlideshow(!root.slideshowRunning)
            onRotateLeft: root.rotate(false)
            onRotateRight: root.rotate(true)
            flipControls: parent.width >= 600
            flippedHorizontally: root.viewFlipHorizontal
            flippedVertically: root.viewFlipVertical
            canSaveCopy: root.canSaveViewCopy && !root.savingViewCopy
            saveCopyLabel: root.viewCopyLabel
            onFlipHorizontal: root.flip(true)
            onFlipVertical: root.flip(false)
            onSaveCopy: root.saveViewCopy()
            onTrash: root.requestTrash()
        }

        ViewerFilmstrip {
            id: filmstrip
            objectName: "viewerFilmstrip"
            // Hidden on a window too short to keep the picture in view, and
            // during a slideshow, which is about the one picture.
            visible: Settings.viewerFilmstrip && Session.count > 1 && !root.slideshowRunning
                     && parent.height >= 360
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 26 + (Session.isVideo ? transport.height : toolbar.height)
            width: Math.min(implicitWidth, parent.width - 32, 760)
            count: Session.count
            currentIndex: Session.index
            devicePixelRatio: root.dpr
            onChosen: function (index) {
                slideshowTimer.stop()
                Session.jump(index)
            }
        }

        ViewerTransport {
            id: transport
            objectName: "viewerTransport"
            visible: Session.isVideo && !root.slideshowRunning
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            width: Math.min(880, parent.width - 32)
            player: root.player
            source: Session.isVideo ? Session.path : ""
            previewsWanted: Session.isVideo && !root.slideshowRunning
            frameShown: !!root.presentation && root.presentation.ready
            devicePixelRatio: root.dpr
            fullScreen: root.fullScreen
            hasCaptions: (root.player && root.player.subtitleTracks.length > 0)
                         || root.subtitleFiles.length > 0
            captionsOn: root.subtitleChoice > 0
            captionsLabel: {
                const choices = root.subtitleChoices()
                return root.subtitleChoice < choices.length ? choices[root.subtitleChoice].label : ""
            }
            onPlayToggled: root.togglePlayback()
            onFullScreenToggled: root.setFullScreen(!root.fullScreen)
            onCaptionsCycled: root.cycleSubtitles()
            onRateRequested: function (rate) { root.setPlaybackRate(rate) }
            onSeekRequested: function (milliseconds) {
                root.seekTo(milliseconds)
            }
        }
    }

    // Zoom level, said briefly while it changes.
    Rectangle {
        id: zoomBadge
        objectName: "viewerZoomBadge"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        // Above the toolbar, which is up while the zoom is changing.
        anchors.bottomMargin: (toolbar.visible ? toolbar.height + 26 : 22)
                              + (filmstrip.visible && root.chromeShown ? filmstrip.height + 10 : 0)
        width: zoomText.implicitWidth + 20
        height: zoomText.implicitHeight + 10
        radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, height / 2) : 3
        color: Osd.panel
        border.width: 1
        border.color: Osd.border
        opacity: 0
        visible: opacity > 0

        function flash() {
            zoomBadge.opacity = 1
            zoomBadgeTimer.restart()
        }

        Behavior on opacity { NumberAnimation { duration: 180 } }

        Text {
            id: zoomText
            anchors.centerIn: parent
            text: root.viewScale > 0 ? root.zoomPercent + "%"
                  : (root.fitMode === "width" ? "Fit width" : root.fitMode === "fill" ? "Fill"
                     : root.fitMode === "shrink" ? "Fit without enlarging" : "Fit")
                    + "  ·  " + root.zoomPercent + "%"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Osd.brightForeground
        }
        Timer {
            id: zoomBadgeTimer
            interval: 1100
            onTriggered: zoomBadge.opacity = 0
        }
    }

    // Results, said where the eye already is.
    Rectangle {
        id: toast
        objectName: "viewerStatus"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 58
        readonly property bool undoable: root.undoId > 0 && root.undoId === Settings.latestUndoId
        width: Math.min(toastText.implicitWidth + 28
                        + (undoable ? undoButton.implicitWidth + 14 : 0), parent.width - 48)
        height: toastText.implicitHeight + 14
        radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, height / 2) : 3
        color: Osd.panelSolid
        border.width: 1
        border.color: Osd.border
        opacity: root.status !== "" ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutQuad } }

        Text {
            id: toastText
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, parent.width - 28
                            - (toast.undoable ? undoButton.implicitWidth + 14 : 0))
            elide: Text.ElideRight
            text: root.status
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Osd.accent
        }

        Text {
            id: undoButton
            objectName: "viewerUndoButton"
            anchors.left: toastText.right
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            visible: toast.undoable
            text: "Undo"
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.bold: true
            font.underline: undoArea.containsMouse
            color: Osd.foreground
            Accessible.role: Accessible.Button
            Accessible.name: "Undo"
            Accessible.description: "Shortcut Ctrl+Z"
            Accessible.onPressAction: root.undo()

            MouseArea {
                id: undoArea
                anchors.fill: parent
                anchors.margins: -6
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.undo()
            }
        }
    }

    // A picture that shows but is cut off says so while it is open, since
    // the grey part looks like the picture itself.
    Rectangle {
        id: pictureWarning
        objectName: "viewerPictureWarning"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 58 + (toast.visible ? toast.height + 8 : 0)
        width: Math.min(warningText.implicitWidth + 46, parent.width - 48)
        height: warningText.height + 12
        radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, height / 2) : 3
        color: Osd.panelSolid
        border.width: 1
        border.color: root.shade(Theme.yellow, 0.5)
        visible: !Session.isVideo && Session.pictureWarning !== "" && !root.imageError
        Accessible.role: Accessible.AlertMessage
        Accessible.name: "Cut off. " + Session.pictureWarning

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "warning"
                size: 14
                color: Theme.yellow
            }
            Text {
                id: warningText
                anchors.verticalCenter: parent.verticalCenter
                // Wrapped rather than cut short in a narrow window.
                width: pictureWarning.width - 46
                text: Session.pictureWarning
                wrapMode: Text.WordWrap
                maximumLineCount: 4
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Osd.foreground
            }
        }
    }

    // Carries the outgoing drag; startFileDrag() sets its picture.
    Item {
        id: fileDrag
        objectName: "viewerFileDrag"
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction
        Drag.proposedAction: Qt.CopyAction
        Drag.mimeData: root.dragMimeData
        Drag.onDragFinished: root.finishFileDrag()
    }

    DropArea {
        id: dropTarget
        objectName: "viewerDropArea"
        anchors.fill: parent
        keys: ["text/uri-list"]
        enabled: !root.blockingActionOpen
        // The file this window is dragging out is not dropped back on it.
        onEntered: function (drag) {
            if (root.draggingOut || !drag.hasUrls) drag.accepted = false
            else drag.accept(Qt.CopyAction)
        }
        onDropped: function (drop) {
            if (root.draggingOut || !drop.hasUrls) return
            drop.accept(Qt.CopyAction)
            root.openDropped(drop.urls)
        }
    }
    DropHighlight {
        objectName: "viewerDropHighlight"
        shown: dropTarget.containsDrag && !root.draggingOut
        label: "Drop to open here"
    }

    ViewerInfoPanel {
        id: infoPanel
        objectName: "viewerInfoPanel"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 58
        anchors.rightMargin: 14
        height: Math.min(implicitHeight, Math.max(0, parent.height - y - 16))
        visible: root.infoOpen
        fileName: Session.fileName
        folder: Session.folder
        filePath: Session.deletionPath
        resolvedPath: Session.path !== Session.deletionPath ? Session.path : ""
        technical: {
            const parts = []
            if (Session.isVideo) {
                if (root.player && root.player.duration > 0) {
                    parts.push(transport.clock(root.player.duration))
                }
            } else if (root.sourceWidth > 0) {
                parts.push(Math.round(root.sourceWidth) + " × " + Math.round(root.sourceHeight))
            }
            return parts.join("  ·  ")
        }
        sizeLabel: Session.sizeLabel
        dateLabel: Session.dateLabel
        rating: root.rating
        favorite: root.favorite
        lines: MediaInfo.path === Session.path ? MediaInfo.lines : []
        loading: MediaInfo.path === Session.path && MediaInfo.loading
        Keys.forwardTo: [keys]
        onRateRequested: function (stars) { root.rate(stars) }
        onFavoriteToggled: root.toggleFavorite()
        onFolderRequested: root.perform("files")
        onCopyPathRequested: root.copyPath(false)
        onCopyNameRequested: root.copyPath(true)
    }

    Menu {
        id: actionMenu
        objectName: "viewerMenu"
        // Modal, undimmed: the press that closes the menu is consumed here
        // rather than also landing on the picture under it.
        modal: true
        dim: false
        readonly property var entries: root.menuEntries()
        onClosed: if (!shortcutHelp.visible) keys.forceActiveFocus()
        // Sized from the rows themselves. The stock list measures its own
        // contentHeight, which changes with the height a tall menu is held to
        // and loops; the rows' heights are known (see the delegate below).
        readonly property real entriesHeight: {
            let total = 0
            for (const entry of entries) {
                total += entry.separator === true ? 9
                         : entry.available === false && entry.hint ? 46 : 30
            }
            return total
        }
        contentItem: ListView {
            implicitHeight: actionMenu.entriesHeight
            model: actionMenu.contentModel
            interactive: Window.window
                         ? implicitHeight + actionMenu.topPadding + actionMenu.bottomPadding > actionMenu.height
                         : false
            clip: true
            currentIndex: actionMenu.currentIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }

        background: Rectangle {
            implicitWidth: 232
            color: root.shade(Theme.background, 0.97)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.16)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        }

        Repeater {
            model: actionMenu.entries

            MenuItem {
                id: entry
                required property var modelData
                readonly property bool separator: modelData.separator === true
                objectName: separator ? "" : "viewerMenu_" + modelData.id
                // The label is drawn below; this is what a screen reader
                // says. Separators are not items at all to it.
                text: separator ? "" : modelData.label
                Accessible.name: separator ? "" : modelData.label
                                 + (modelData.available === false && modelData.hint
                                    ? ", needs " + modelData.hint : "")
                Accessible.description: separator || !root.viewerShortcuts[modelData.id] ? ""
                                        : "Shortcut " + root.viewerShortcuts[modelData.id]
                // Controls marks every MenuItem accessible again when it sets
                // the role, which undoes ignored; a separator role is what
                // screen readers skip.
                Accessible.role: separator ? Accessible.Separator : Accessible.MenuItem
                Accessible.ignored: separator
                enabled: !separator && modelData.available !== false
                height: separator ? 9 : modelData.available === false && modelData.hint ? 46 : 30
                opacity: enabled || separator ? 1 : 0.55
                contentItem: Item {
                    Rectangle {
                        visible: entry.separator
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 1
                        color: root.shade(Theme.foreground, 0.12)
                    }
                    Text {
                        visible: !entry.separator
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.right: shortcutText.left
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: entry.modelData.available === false && entry.modelData.hint ? -7 : 0
                        text: entry.separator ? "" : entry.modelData.label
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: entry.modelData.id === "trash" ? Theme.red
                               : entry.highlighted ? Theme.brightForeground : Theme.foreground
                    }
                    Text {
                        visible: !entry.separator && entry.modelData.available === false && !!entry.modelData.hint
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 5
                        text: "Needs " + (entry.modelData.hint || "")
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                    Text {
                        id: shortcutText
                        visible: !entry.separator
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.separator ? ""
                              : (root.viewerShortcuts[entry.modelData.id] || "")
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }
                background: Rectangle {
                    color: entry.highlighted && !entry.separator
                           ? root.shade(Theme.foreground, 0.09) : "transparent"
                    radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 3) : 0
                }
                onTriggered: root.performMenuAction(entry.modelData.id)
            }
        }
    }

    ShortcutHelp {
        id: shortcutHelp
        objectName: "viewerShortcutHelp"
        video: Session.isVideo
        animated: Session.isAnimated
        audioChoices: root.hasAudioChoices
        sidecarSubtitles: root.externalSubtitle !== ""
        onClosed: keys.forceActiveFocus()
    }

    EditorChooser {
        id: editorChooser
        objectName: "editorChooser"
        onClosed: keys.forceActiveFocus()
    }

    ConfirmSheet {
        id: confirm
        objectName: "viewerConfirm"
        property var target: ({})
        property int index: 0
        property int serial: 0
        property string path: ""
        property string viewedPath: ""
        title: "Move this item to Trash?"
        confirmLabel: "Move to Trash"
        onAccepted: {
            const target = confirm.target
            const index = confirm.index
            const serial = confirm.serial
            confirm.target = ({})
            confirm.path = ""
            if (target.path === undefined) return
            // A refusal is said by Actions.failed; the viewer stays on the
            // file now at that name.
            const record = Actions.trashCaptured(target)
            if (record.path === undefined) return
            record.index = index
            record.openSerial = serial
            Session.forget(target.viewedPath)
            root.sayUndoable("Moved to Trash", Settings.pushTrashUndo([record], Session))
        }
        onVisibleChanged: if (!visible) keys.forceActiveFocus()
    }

    ConfirmSheet {
        id: permanentConfirm
        objectName: "viewerPermanentConfirm"
        property var target: ({})
        property string path: ""
        property string viewedPath: ""
        title: "Permanently delete this file?"
        confirmLabel: "Delete permanently"
        onAccepted: {
            const target = permanentConfirm.target
            const viewedPath = permanentConfirm.viewedPath
            permanentConfirm.path = ""
            permanentConfirm.target = ({})
            root.deletePermanently(target, viewedPath)
        }
        onVisibleChanged: if (!visible) keys.forceActiveFocus()
    }

    // The keyboard. One handler rather than window shortcuts: shifted pairs
    // were ambiguous as Shortcuts on a real keyboard, and a single place keeps
    // the medium-specific meanings readable.
    ViewerKeys {
        id: keys
        objectName: "viewerKeys"
        focus: true
        // The picture's place in the Tab order, after the last control.
        activeFocusOnTab: true
        viewer: root
        Accessible.role: Accessible.Graphic
        Accessible.name: Session.fileName !== "" ? Session.fileName : "Omaroll"
        Accessible.description: Session.count > 1 ? (Session.index + 1) + " of " + Session.count : ""
    }
}
