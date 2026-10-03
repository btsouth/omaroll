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
    property bool videoPausedForRender: false
    property int visibilityBeforeFullScreen: Window.Windowed
    readonly property bool fullScreen: root.visibility === Window.FullScreen
    property string status: ""
    property string videoError: ""
    property string loadedMediaPath: ""
    property string loadedMediaVersion: ""
    property bool reloadingVideo: false
    readonly property bool imageError: stillLoader.item !== null
                                       && stillLoader.item.status === Image.Error
    readonly property string playbackError: Session.isVideo ? root.videoError
        : root.imageError ? (Session.isAnimated ? "Could not display this animation"
                                                : "Could not display this picture")
        : ""

    // A still is fitted until it is zoomed; viewScale is then the logical
    // size of one source pixel. Zero means "fit".
    property real viewScale: 0
    property int viewRotation: 0
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
    // disagree on shape, as a preview cropped differently would.
    readonly property bool rawMeasured: Session.isRaw && root.loadedSize.width > 0
        && Session.rawSize.width > 0 && Session.rawSize.height > 0
        && Math.abs(Session.rawSize.width / Session.rawSize.height
                    / (root.loadedSize.width / root.loadedSize.height) - 1) < 0.02
    readonly property real sourceWidth: root.rawMeasured ? Session.rawSize.width
                                                         : root.loadedSize.width
    readonly property real sourceHeight: root.rawMeasured ? Session.rawSize.height
                                                          : root.loadedSize.height
    // Once that preview would be drawn larger than its own pixels, the raw is
    // demosaiced in the background and laid over it. A preview the size of
    // the raw, as most cameras write, never needs it.
    readonly property bool rawDetailWanted: root.rawMeasured && root.imageReady
        && root.loadedSize.width < Session.rawSize.width * 0.9
        && root.effectiveScale * root.dpr * Session.rawSize.width > root.loadedSize.width * 1.05
    // The file whose full decode last arrived, kept so zooming back out and
    // in again does not drop it and decode it twice.
    property string rawDetailPath: ""
    readonly property bool stillReady: stillLoader.item !== null
                                       && (stillLoader.item.status === Image.Ready
                                           || stillLoader.item.status === Image.Error)
    property bool animationPlaying: true

    // Video, created on the first recording so browsing pictures never starts
    // the multimedia backend.
    readonly property var player: playerLoader.item
    readonly property bool playing: player !== null
                                    && player.playbackState === MediaPlayer.PlayingState
    property bool resumeAvailable: false
    property real resumePosition: 0
    property bool resumePending: false
    property var subtitleFiles: []
    property string externalSubtitle: ""
    property int subtitleChoice: 0
    readonly property string subtitleText: externalSubtitle !== "" && player
        ? Subtitles.textAt(externalSubtitle, Math.round(player.position))
        : (videoOutput.videoSink ? videoOutput.videoSink.subtitleText : "")

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
    readonly property real actualScale: 1 / root.dpr
    readonly property real effectiveScale: root.viewScale > 0 ? root.viewScale : root.fitScale
    readonly property real displayWidth: root.rotatedWidth * root.effectiveScale
    readonly property real displayHeight: root.rotatedHeight * root.effectiveScale
    readonly property int zoomPercent: Math.round(root.effectiveScale * root.dpr * 100)
    readonly property bool imageReady: root.visible && !Session.isVideo && root.stillReady
                                       && root.sourceWidth > 0

    readonly property bool chromeShown: root.chromePinned || root.pointerActive || root.infoOpen
                                        || actionMenu.visible || confirm.visible || editorChooser.visible
                                        || header.hovered || transport.hovered
                                        || previousButton.hovered || nextButton.hovered
                                        || toolbar.hovered || filmstrip.hovered
                                        || transport.scrubbing
                                        || (Session.isVideo && !root.playing && !root.slideshowRunning)
    readonly property bool pointerHidden: !root.chromeShown
                                          && (root.playing || root.fullScreen || root.slideshowRunning)

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function say(message) {
        root.status = message
        statusTimer.restart()
    }

    // Called by the opt-in startup trace, before scene synchronization.
    function startupReadiness() {
        if (root.imageReady) return 2
        const poster = Session.isVideo && videoPoster.visible && videoPoster.status === Image.Ready
        return (Session.isVideo && root.player && root.player.hasVideo ? 2 : 0)
            | (poster ? 8 : 0)
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

    function resetView() {
        zoomAnimation.stop()
        root.viewScale = 0
        root.viewRotation = 0
        still.contentX = 0
        still.contentY = 0
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
        root.zoomTo(root.fitScale)
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
        if (Math.abs(root.viewScale - root.fitScale) <= root.fitScale * 0.001) {
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
        root.zoomTo(root.actualScale, x, y)
    }

    // The toolbar's 1:1 / Fit button.
    function toggleFit() {
        if (root.viewScale > 0) {
            root.fitToWindow()
        } else {
            root.showActualSize()
        }
    }

    function rotate(clockwise) {
        if (Session.isVideo) {
            return
        }
        root.viewRotation = (root.viewRotation + (clockwise ? 90 : 270)) % 360
        zoomAnimation.stop()
        root.viewScale = 0
    }

    function step(direction) {
        slideshowTimer.stop()
        if (!Session.step(direction)) {
            root.say("This is the only file here")
        }
    }

    // The slideshow waits for each picture to be on screen before timing it.
    function armSlideshow() {
        if (root.slideshowRunning && !actionMenu.visible && !Session.isVideo && root.stillReady) {
            slideshowTimer.restart()
        }
    }
    onStillReadyChanged: root.armSlideshow()

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
        if (!root.player) {
            return
        }
        const duration = root.player.duration
        const maximum = duration > 0 ? duration - 1 : Math.max(0, milliseconds)
        root.resumeAvailable = false
        root.resumePending = false
        root.player.position = Math.max(0, Math.min(maximum, milliseconds))
    }

    function seek(milliseconds) {
        if (root.player && root.player.duration > 0) {
            root.seekTo(root.player.position + milliseconds)
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

    // Off, then each embedded track, then each sidecar file.
    function subtitleChoices() {
        const choices = [{kind: "off", label: "Off", index: -1, path: ""}]
        if (root.player) {
            const tracks = root.player.subtitleTracks
            for (let i = 0; i < tracks.length; ++i) {
                let label = "Track " + (i + 1)
                try {
                    const language = tracks[i].stringValue(QMediaMetaData.Language)
                    if (language && language.length > 0) {
                        label = language.toUpperCase()
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
        root.player.activeSubtitleTrack = choice.kind === "embedded" ? choice.index : -1
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

    // The next file for the slideshow: any other one when shuffling, and
    // never a video when the settings leave them out.
    function advanceSlideshow() {
        if (actionMenu.visible) return
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
            for (let attempt = 0; attempt < total; ++attempt) {
                Session.step(1)
                if (Settings.slideshowVideos || !Session.isVideo) {
                    return
                }
            }
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
        root.say(Settings.isFavorite(Session.path) ? "Added to favourites" : "Removed from favourites")
    }

    function rate(stars) {
        Settings.setRating([Session.path], stars)
        root.say(stars > 0 ? "Rated " + "★".repeat(stars) : "Rating cleared")
    }

    function requestTrash() {
        if (Session.path === "") {
            return
        }
        confirm.path = Session.path
        confirm.detail = Session.path
        confirm.open()
    }

    // The keys a viewer is expected to have, and what the menu shows beside
    // each entry. They are this window's own: F is full screen here, as in
    // every player, rather than the library's Open containing folder.
    readonly property var viewerShortcuts: ({
        library: "Enter", copy: "Y", "open-with": "Ctrl+O", annotate: "A", develop: "D", "choose-editor": "Shift+D", send: "S", trim: "T", frame: "G",
        play: "P", rotate: "R", slideshow: "F5", fullscreen: "F", info: "I", filmstrip: "B",
        favorite: "V", trash: "Del"
    })

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
        if (!video) {
            entries.push({id: "rotate", label: "Rotate"})
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

    // Renders hold a known state so the screenshot is the same every time.
    function openViewForRender(view) {
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
            if (Session.isVideo) {
                root.startPlayerSoon()
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

    // Creating the multimedia backend is synchronous. Let a frame containing
    // the poster finish submission, then start the player on the next GUI
    // turn. A missing or slow thumbnail must not prevent playback.
    property int playerStartGeneration: 0
    Component.onCompleted: Session.watchVideoStartup(root)
    function startPlayerSoon() {
        if (!playerLoader.active) {
            ++root.playerStartGeneration
            playerStart.restart()
        }
    }
    function queuePlayerStart() {
        playerStart.stop()
        const generation = root.playerStartGeneration
        Qt.callLater(function () {
            if (generation === root.playerStartGeneration) root.startPlayer()
        })
    }
    function startPlayer() {
        playerStart.stop()
        if (Session.isVideo && !playerLoader.active) {
            Settings.prepareVideoPlayback()
            playerLoader.active = true
        }
    }
    function videoStartupReadiness() {
        return playerStart.running && root.visible && Session.isVideo
            && videoPoster.status === Image.Ready ? root.playerStartGeneration : 0
    }
    function startPlayerAfterPoster(generation) {
        if (playerStart.running && generation === root.playerStartGeneration) {
            root.queuePlayerStart()
        }
    }
    Timer {
        id: playerStart
        interval: 250
        onTriggered: root.queuePlayerStart()
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
        root.slideshowRunning = false
        slideshowTimer.stop()
        if (root.player) {
            root.player.stop()
        }
        root.infoOpen = false
        confirm.close()
        actionMenu.close()
        Session.clear()
    }

    Connections {
        target: Settings
        function onMarksChanged() { root.marksVersion++ }
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
            interactive: root.viewScale > 0
                         && (contentWidth > width + 0.5 || contentHeight > height + 0.5)
            contentWidth: Math.max(width, root.displayWidth)
            contentHeight: Math.max(height, root.displayHeight)

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

                // The full decode of a camera raw, over its preview.
                Image {
                    objectName: "viewerRawDetail"
                    anchors.fill: parent
                    source: root.visible && (root.rawDetailWanted
                                             || root.rawDetailPath === Session.path)
                            ? Session.rawUrl : ""
                    asynchronous: true
                    smooth: parent.smooth
                    mipmap: true
                    fillMode: Image.Stretch
                    opacity: status === Image.Ready ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 140 } }
                    onStatusChanged: {
                        if (status === Image.Ready) {
                            root.rawDetailPath = Session.path
                        }
                    }
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
            visible: Session.isVideo && !(root.player && root.player.hasVideo)
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            // The thumbnail provider applies the ratio from its URL once.
            sourceSize: Qt.size(Math.round(width), Math.round(height))
            source: root.visible && Session.isVideo && Session.path !== ""
                    ? "image://thumbs/" + root.dpr + "@40~" + Session.stamp
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
                    source: root.visible && Session.isVideo && !root.reloadingVideo ? Session.url : ""
                    videoOutput: videoSurface.item
                    audioOutput: AudioOutput {
                        volume: Settings.videoVolume
                        muted: Settings.videoMuted || !root.audible
                    }
                    onSourceChanged: {
                        root.videoError = ""
                        if (source.toString() !== "") play()
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
                        if (root.videoPausedForRender && mediaPlayer.position >= 1000
                                && mediaPlayer.playbackState === MediaPlayer.PlayingState) {
                            mediaPlayer.pause()
                        }
                    }
                    // Remember the spot on pause; the timer covers long
                    // uninterrupted playback. Media keys follow the video
                    // played last.
                    onPlaybackStateChanged: {
                        if (playbackState === MediaPlayer.PlayingState) {
                            Mpris.track(mediaPlayer)
                        }
                        if (playbackState === MediaPlayer.PausedState && duration > 0
                                && position >= 5000 && position < duration - 3000) {
                            Settings.setVideoPosition(Session.path, Math.round(position))
                        }
                    }
                    onMediaStatusChanged: {
                        if (mediaStatus === MediaPlayer.LoadedMedia && root.resumePending
                                && !root.slideshowRunning) {
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
                    Settings.setVideoPosition(Session.path, Math.round(player.position))
                }
            }
        }

        Item {
            id: videoOutput
            anchors.fill: parent
            visible: Session.isVideo && root.player !== null && root.player.hasVideo
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

            // Mouse side buttons use the viewer's existing file order. Keep
            // their scope here so normal clicks, menus and video controls can
            // still take their own input above this layer.
            MouseArea {
                objectName: "viewerSideButtonNavigation"
                anchors.fill: parent
                acceptedButtons: Qt.BackButton | Qt.ForwardButton
                enabled: Session.count > 1 && !actionMenu.visible && !confirm.visible
                         && !editorChooser.visible
                onClicked: function (mouse) {
                    root.step(mouse.button === Qt.BackButton ? -1 : 1)
                }
            }
        }

        // A file that will not open says so in the middle of the window.
        Item {
            anchors.centerIn: parent
            width: errorColumn.implicitWidth
            height: errorColumn.implicitHeight
            visible: root.playbackError !== ""

            // A press on the prompt stays here rather than also reaching the
            // tap layer underneath, where it would play or pause.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
            }

            Column {
                id: errorColumn
                spacing: 12

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.playbackError
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Theme.red
                }
                PillButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: Session.isVideo && Registry.available("play")
                    label: "Open in mpv"
                    onClicked: Registry.run("play", Session.path)
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
                    active: true
                    onClicked: root.resumeVideo()
                }
                PillButton {
                    label: "Start over"
                    floating: true
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
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutQuad } }

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

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0.0; color: root.shade(Theme.surfaceBackground, 0.82) }
                    GradientStop { position: 1.0; color: root.shade(Theme.surfaceBackground, 0.0) }
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
                    color: Theme.brightForeground
                }

                Text {
                    id: counterText
                    objectName: "viewerCounter"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: Session.count > 1
                    text: (Session.index + 1) + " / " + Session.count
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
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
                    icon: "info"
                    toolTip: root.infoOpen ? "Hide details" : "Details"
                    shortcut: "I"
                    active: root.infoOpen
                    onClicked: root.infoOpen = !root.infoOpen
                }
                IconButton {
                    objectName: "viewerLibraryButton"
                    icon: "grid"
                    toolTip: "Open in library"
                    shortcut: "Enter"
                    onClicked: Session.openInLibrary()
                }
                IconButton {
                    id: moreButton
                    objectName: "viewerMoreButton"
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
            shortcut: Session.isVideo ? "Page Up" : "Left"
            onClicked: root.step(-1)
        }
        IconButton {
            id: nextButton
            objectName: "viewerNext"
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
            shortcut: Session.isVideo ? "Page Down" : "Right"
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
            fitted: root.viewScale === 0
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
        color: root.shade(Theme.background, 0.86)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.12)
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
            text: root.viewScale > 0 ? root.zoomPercent + "%" : "Fit  ·  " + root.zoomPercent + "%"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.brightForeground
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
        width: Math.min(toastText.implicitWidth + 28, parent.width - 48)
        height: toastText.implicitHeight + 14
        radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, height / 2) : 3
        color: root.shade(Theme.background, 0.90)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.14)
        opacity: root.status !== "" ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutQuad } }

        Text {
            id: toastText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width - 28)
            elide: Text.ElideRight
            text: root.status
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Theme.accent
        }
    }

    ViewerInfoPanel {
        id: infoPanel
        objectName: "viewerInfoPanel"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 58
        anchors.rightMargin: 14
        visible: root.infoOpen
        fileName: Session.fileName
        folder: Session.folder
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
        onRateRequested: function (stars) { root.rate(stars) }
        onFavoriteToggled: root.toggleFavorite()
        onFolderRequested: root.perform("files")
    }

    Menu {
        id: actionMenu
        objectName: "viewerMenu"
        // Modal, undimmed: the press that closes the menu is consumed here
        // rather than also landing on the picture under it.
        modal: true
        dim: false
        readonly property var entries: root.menuEntries()
        onVisibleChanged: {
            if (visible) {
                slideshowTimer.stop()
            } else if (root.slideshowRunning && Session.isVideo && !Settings.slideshowVideos
                       && root.loadedMediaPath === Session.path) {
                root.advanceSlideshow()
            } else if (root.slideshowRunning && Session.isVideo
                       && root.loadedMediaPath === Session.path
                       && (root.videoError !== "" || (root.player
                           && root.player.mediaStatus === MediaPlayer.EndOfMedia))) {
                // A clip may finish or fail while its menu holds the target.
                slideshowTimer.restart()
            } else {
                root.armSlideshow()
            }
        }
        onClosed: keys.forceActiveFocus()

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

    EditorChooser {
        id: editorChooser
        objectName: "editorChooser"
    }

    ConfirmSheet {
        id: confirm
        objectName: "viewerConfirm"
        property string path: ""
        title: "Move this item to Trash?"
        confirmLabel: "Move to Trash"
        onAccepted: {
            if (confirm.path !== "" && Actions.moveToTrash(confirm.path)) {
                Session.forget(confirm.path)
                root.say("Moved to Trash")
            }
            confirm.path = ""
        }
        onVisibleChanged: if (!visible) keys.forceActiveFocus()
    }

    // The keyboard. One handler rather than window shortcuts: shifted pairs
    // were ambiguous as Shortcuts on a real keyboard, and a single place keeps
    // the medium-specific meanings readable.
    Item {
        id: keys
        objectName: "viewerKeys"
        focus: true

        Keys.onPressed: function (event) {
            if (editorChooser.visible) {
                event.accepted = false
                return
            }
            const control = (event.modifiers & Qt.ControlModifier) !== 0
            const alt = (event.modifiers & Qt.AltModifier) !== 0
            const shift = (event.modifiers & Qt.ShiftModifier) !== 0
            const video = Session.isVideo
            let handled = true

            if (alt && event.key >= Qt.Key_0 && event.key <= Qt.Key_5) {
                root.rate(event.key - Qt.Key_0)
            } else if (control && event.key === Qt.Key_Z) {
                if (Settings.canUndo()) {
                    Settings.undo()
                    root.say("Undone")
                }
            } else if (control && event.key === Qt.Key_C) {
                root.perform("copy")
            } else if (control && event.key === Qt.Key_O) {
                root.perform("open-with")
            } else if (control && event.key === Qt.Key_W) {
                root.close()
            } else if (control && event.key === Qt.Key_Q) {
                Qt.quit()
            } else if (control || alt) {
                handled = false
            } else {
                switch (event.key) {
                // Tab would walk focus into the details card and leave the
                // viewer's keys behind.
                case Qt.Key_Tab:
                case Qt.Key_Backtab:
                    break
                case Qt.Key_Escape:
                    if (root.slideshowRunning) root.setSlideshow(false)
                    else if (root.fullScreen) root.setFullScreen(false)
                    else if (root.infoOpen) root.infoOpen = false
                    else root.close()
                    break
                case Qt.Key_Left:
                case Qt.Key_Right: {
                    const direction = event.key === Qt.Key_Left ? -1 : 1
                    if (video) root.seek(direction * 5000)
                    else root.step(direction)
                    break
                }
                case Qt.Key_PageUp:
                case Qt.Key_PageDown:
                    root.step(event.key === Qt.Key_PageUp ? -1 : 1)
                    break
                case Qt.Key_Home:
                case Qt.Key_End:
                    if (video && root.player) {
                        root.seekTo(event.key === Qt.Key_Home ? 0 : root.player.duration)
                    } else {
                        Session.jump(event.key === Qt.Key_Home ? 0 : Session.count - 1)
                    }
                    break
                case Qt.Key_Space:
                case Qt.Key_K:
                    if (video || Session.isAnimated) root.togglePlayback()
                    else handled = false
                    break
                case Qt.Key_J:
                case Qt.Key_L:
                    if (video) root.seek(event.key === Qt.Key_J ? -10000 : 10000)
                    else handled = false
                    break
                case Qt.Key_Up:
                case Qt.Key_Down:
                    if (video) root.adjustVolume(event.key === Qt.Key_Up ? 0.05 : -0.05)
                    else handled = false
                    break
                case Qt.Key_M:
                    if (video) {
                        Settings.videoMuted = !Settings.videoMuted
                        root.say(Settings.videoMuted ? "Muted" : "Sound on")
                    } else {
                        handled = false
                    }
                    break
                case Qt.Key_C:
                    if (video) root.cycleSubtitles()
                    else handled = false
                    break
                case Qt.Key_BracketLeft:
                case Qt.Key_BracketRight:
                    if (video && root.player) {
                        root.player.playbackRate = Math.max(0.25, Math.min(4,
                            root.player.playbackRate + (event.key === Qt.Key_BracketLeft ? -0.25 : 0.25)))
                        root.say("Speed " + Number(root.player.playbackRate.toFixed(2)) + "×")
                    } else {
                        handled = false
                    }
                    break
                case Qt.Key_Backspace:
                    if (video && root.player) {
                        root.player.playbackRate = 1
                        root.say("Normal speed")
                    } else {
                        handled = false
                    }
                    break
                case Qt.Key_Plus:
                case Qt.Key_Equal:
                    root.zoomBy(1.25)
                    break
                case Qt.Key_Minus:
                case Qt.Key_Underscore:
                    root.zoomBy(1 / 1.25)
                    break
                case Qt.Key_0:
                    if (!video) root.fitToWindow()
                    break
                case Qt.Key_1:
                    if (!video) root.toggleFit()
                    break
                case Qt.Key_R:
                    root.rotate(!shift)
                    break
                case Qt.Key_F:
                case Qt.Key_F11:
                    root.setFullScreen(!root.fullScreen)
                    break
                case Qt.Key_F5:
                    root.setSlideshow(!root.slideshowRunning)
                    break
                case Qt.Key_I:
                    root.infoOpen = !root.infoOpen
                    break
                case Qt.Key_Return:
                case Qt.Key_Enter:
                    Session.openInLibrary()
                    break
                case Qt.Key_Delete:
                    root.requestTrash()
                    break
                case Qt.Key_V:
                    root.toggleFavorite()
                    break
                case Qt.Key_Y:
                    root.perform("copy")
                    break
                case Qt.Key_S:
                    root.perform("send")
                    break
                case Qt.Key_D:
                case Qt.Key_O:
                    if (Session.isRaw) root.perform(event.modifiers & Qt.ShiftModifier ? "choose-editor" : "develop")
                    else handled = false
                    break
                case Qt.Key_A:
                    if (!video) root.perform("annotate")
                    else handled = false
                    break
                case Qt.Key_B:
                    if (Session.count > 1) root.toggleFilmstrip()
                    else handled = false
                    break
                case Qt.Key_T:
                    if (video) root.perform("trim")
                    else handled = false
                    break
                case Qt.Key_G:
                    if (video) root.perform("frame")
                    else handled = false
                    break
                case Qt.Key_P:
                    if (video) root.perform("play")
                    else handled = false
                    break
                case Qt.Key_Menu:
                    actionMenu.popup(stage, stage.width / 2, stage.height / 2)
                    break
                default:
                    handled = false
                }
            }
            event.accepted = handled
        }
    }
}
