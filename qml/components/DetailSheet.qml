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
    property string fileName: ""
    property string selectionLabel: ""
    property string kindLabel: ""
    property string dayLabel: ""
    property string timeLabel: ""
    property string sizeLabel: ""
    property bool isVideo: false
    // Keep playback state after first use, but do not initialize the multimedia
    // backend just to browse pictures. Loader creation is synchronous.
    readonly property var player: playerLoader.item
    readonly property var audio: player ? player.audioOutput : null
    onIsVideoChanged: if (isVideo) playerLoader.active = true
    property bool isDocument: false
    property int pdfPage: 1
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
    property bool showInfo: true
    property bool slideshowRunning: false
    property bool slideshowPausedForRender: false
    property bool videoPausedForRender: false
    property bool qrDetected: false
    property bool resumeVideoAfterRestore: false
    // The window's status line, repeated here: the footer sits under the
    // backdrop, so a result said there while the viewer is open goes unread.
    property string status: ""
    readonly property int slideshowInterval: 4000
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
    readonly property real playbackVolume: audio ? audio.volume : 0.8
    readonly property bool playbackMuted: audio ? audio.muted : false
    readonly property int playbackLoops: player ? player.loops : 1
    readonly property bool isAnimatedImage: !root.isVideo && !root.isDocument
                                             && (root.fileName.toLowerCase().endsWith(".gif")
                                                 || root.fileName.toLowerCase().endsWith(".webp"))
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
        const rows = Registry.actionsForKind(root.isVideo, root.isDocument)
        return rows.filter(function (row) { return row.id !== "qr" || root.qrDetected })
    }

    function focusAction(index) {
        if (index < 0 || index >= actions.count || !actions.model[index].available) {
            return false
        }
        if (!showInfo) {
            showInfo = true
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
        const keepActionFocus = visible && actionNavigationActive
        const previousActionId = focusedActionId
        playbackError = ""
        pdfPage = 1
        if (!visible) {
            showInfo = true
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

    function rotateImage() {
        imageRotation = (imageRotation + 90) % 360
        imageZoom = 1.0
        Qt.callLater(stillViewport.centerContent)
    }

    function toggleVideoPlayback() {
        if (player.playbackState === MediaPlayer.PlayingState) {
            player.pause()
        } else {
            player.play()
        }
    }

    function seekVideo(milliseconds) {
        player.position = Math.max(0, Math.min(player.duration, player.position + milliseconds))
    }

    function adjustVolume(amount) {
        audio.volume = Math.max(0, Math.min(1, audio.volume + amount))
        if (audio.volume > 0) {
            audio.muted = false
        }
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

    function cycleSubtitleTrack() {
        const count = player.subtitleTracks.length
        if (count === 0) {
            return
        }
        player.activeSubtitleTrack = player.activeSubtitleTrack + 1 >= count
                                     ? -1 : player.activeSubtitleTrack + 1
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
        slideshowRunning = enabled
        if (enabled) {
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
        setSlideshow(false)
        setFullScreen(false)
        actionNavigationActive = false
        focusedActionId = ""
        actions.currentIndex = -1
        visible = false
    }

    function dismiss() {
        if (slideshowRunning) {
            setSlideshow(false)
            setFullScreen(false)
            showInfo = true
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
        if (!showInfo && actionNavigationActive) {
            focusPreview()
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

        // Preview
        Rectangle {
            id: stage
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: sidebar.left
            anchors.margins: 1
            color: root.shade(Theme.darkerBackground, 0.85)

            // Margins around the bottom row: 16 either side, 12 between the
            // groups, 10 of padding inside the slideshow group's panel.
            readonly property int rowChrome: 16 + 12 + 10 + 16

            // Nothing here reads compactControls, which keeps it loop-free.
            Row {
                id: wideTopMeasure
                visible: false
                spacing: 6
                Repeater {
                    model: root.isVideo ? ["Start slideshow", "Hide details", "Fullscreen"]
                                        : ["Start slideshow", "Hide details"]
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

            // A recording shows its thumbnail until the first decoded frame.
            Image {
                id: videoPoster
                anchors.fill: parent
                anchors.margins: 16
                anchors.bottomMargin: 56
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true
                mipmap: true
                visible: root.isVideo && !(player && player.hasVideo)
                sourceSize: Qt.size(Math.round(stage.width * Screen.devicePixelRatio),
                                    Math.round(stage.height * Screen.devicePixelRatio))
                source: root.path === "" ? ""
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
                visible: !root.isVideo
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
                        readonly property string suffix: root.fileName.toLowerCase()
                        sourceComponent: root.isDocument ? pdfStill
                                         : suffix.endsWith(".gif") || suffix.endsWith(".webp")
                                         ? animatedStill : staticStill
                    }
                }

                WheelHandler {
                    target: null
                    onWheel: function (event) {
                        const delta = event.angleDelta.y !== 0
                                      ? event.angleDelta.y : event.pixelDelta.y
                        root.adjustImageZoom(delta > 0 ? 1.2 : 1 / 1.2)
                        event.accepted = true
                    }
                }

                TapHandler {
                    onDoubleTapped: root.imageZoom > 1
                                    ? root.resetImageView() : root.adjustImageZoom(2)
                }
            }

            Component {
                id: staticStill
                Image {
                    source: root.visible && !root.isVideo && root.path !== ""
                            ? Library.fileUrl(root.path) : ""
                    asynchronous: true
                    autoTransform: true
                    smooth: true
                    mipmap: true
                    fillMode: Image.Stretch
                    onStatusChanged: {
                        if (status === Image.Ready) {
                            root.imageSourceWidth = sourceSize.width
                            root.imageSourceHeight = sourceSize.height
                            root.stillReady = true
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
                            root.imageSourceWidth = sourceSize.width
                            root.imageSourceHeight = sourceSize.height
                            root.stillReady = true
                        } else if (status === Image.Error) {
                            root.playbackError = PdfInfo.available
                                                 ? "Could not display this PDF page"
                                                 : "PDF support needs Poppler"
                            root.stillReady = true
                        }
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
                        source: root.visible && root.isVideo && root.path !== ""
                                ? Library.fileUrl(root.path) : ""
                        videoOutput: videoSurface.item
                        audioOutput: AudioOutput {
                            volume: 0.8
                            muted: false
                        }
                        loops: 1
                        onSourceChanged: {
                            if (source.toString() !== "") {
                                play()
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
                        onMediaStatusChanged: {
                            if (mediaStatus === MediaPlayer.EndOfMedia && root.slideshowRunning) {
                                root.requestNavigation(1)
                            }
                        }
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
                anchors.bottomMargin: 56
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
                anchors.horizontalCenter: output.horizontalCenter
                anchors.bottom: output.bottom
                anchors.bottomMargin: 24
                width: Math.max(0, output.width - 80)
                visible: root.isVideo && output.videoSink
                         && output.videoSink.subtitleText !== ""
                text: output.videoSink ? output.videoSink.subtitleText : ""
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

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 62
                spacing: 8
                visible: root.isDocument

                PillButton {
                    label: "Previous page"
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
                    label: "Next page"
                    enabled: PdfInfo.pageCount > 0 && root.pdfPage < PdfInfo.pageCount
                    onClicked: if (root.pdfPage < PdfInfo.pageCount) {
                        root.stillReady = false
                        root.pdfPage++
                    }
                }
            }

            Rectangle {
                id: topControlsPanel
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.bottomMargin: 11
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
                        label: root.compactControls ? "i"
                                               : (root.showInfo ? "Hide details" : "Show details")
                        toolTip: root.showInfo ? "Hide details" : "Show details"
                        shortcut: root.viewerShortcuts.info.label
                        active: root.showInfo
                        onClicked: root.showInfo = !root.showInfo
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
                    toolTip: "Actual size"
                    shortcut: root.viewerShortcuts.actual.label
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

            // Transport: play/pause, scrub, clock, tracks, speed, and sound.
            Item {
                id: transport
                visible: root.isVideo && !root.slideshowRunning
                anchors.left: parent.left
                anchors.right: topControlsPanel.left
                anchors.bottom: parent.bottom
                anchors.leftMargin: 16
                anchors.rightMargin: 12
                anchors.bottomMargin: 16
                height: 28

                // Keep the scrub useful at the minimum window width. Track
                // selectors appear only when the file has alternatives.
                readonly property int scrubMinimum: 120
                readonly property real minimumWidth: playButton.implicitWidth + 14 + scrubMinimum
                                                     + 14 + clockLabel.implicitWidth + 12
                                                     + mediaControls.implicitWidth

                function clock(ms) {
                    const total = Math.max(0, Math.round(ms / 1000))
                    const minutes = Math.floor(total / 60)
                    const seconds = total % 60
                    return minutes + ":" + (seconds < 10 ? "0" : "") + seconds
                }

                PillButton {
                    id: playButton
                    objectName: "videoPlayButton"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    label: root.playbackState === MediaPlayer.PlayingState ? "❚❚" : "▶"
                    toolTip: root.playbackState === MediaPlayer.PlayingState ? "Pause" : "Play"
                    shortcut: "Space"
                    onClicked: root.toggleVideoPlayback()
                }

                Item {
                    id: scrub
                    anchors.left: playButton.right
                    anchors.right: clockLabel.visible ? clockLabel.left : mediaControls.left
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
                    // The clock goes first when the transport is short of room.
                    visible: transport.width >= transport.minimumWidth
                    anchors.right: mediaControls.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: transport.clock(root.playbackPosition) + " / " + transport.clock(player ? player.duration : 0)
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }

                Row {
                    id: mediaControls
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    PillButton {
                        visible: player && player.audioTracks.length > 1 && transport.width >= 520
                        label: "Audio " + (player ? player.activeAudioTrack + 1 : 1)
                        toolTip: "Switch audio track"
                        onClicked: root.cycleAudioTrack()
                    }
                    PillButton {
                        visible: player && player.subtitleTracks.length > 0 && transport.width >= 440
                        objectName: "subtitleButton"
                        label: "CC"
                        toolTip: player && player.activeSubtitleTrack >= 0
                                 ? "Turn subtitles off" : "Choose subtitles"
                        active: player && player.activeSubtitleTrack >= 0
                        onClicked: root.cycleSubtitleTrack()
                    }
                    PillButton {
                        id: speedButton
                        objectName: "playbackSpeedButton"
                        label: Number(root.playbackRate.toFixed(2)) + "×"
                        toolTip: "Playback speed"
                        shortcut: "[  ]"
                        active: Math.abs(root.playbackRate - 1) > 0.01
                        onClicked: root.cyclePlaybackRate()
                    }
                    PillButton {
                        id: soundButton
                        objectName: "videoSoundButton"
                        label: root.playbackMuted ? "Muted" : Math.round(root.playbackVolume * 100) + "%"
                        toolTip: root.playbackMuted ? "Unmute" : "Mute"
                        shortcut: "M"
                        active: !root.playbackMuted
                        onClicked: audio.muted = !audio.muted
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

        // Sidebar
        Item {
            id: sidebar
            visible: root.showInfo
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: root.showInfo ? (panel.width < 700 ? 220 : 280) : 0

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            Column {
                id: metadataColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 18
                spacing: 4

                Text {
                    objectName: "viewerSelectionLabel"
                    width: parent.width
                    visible: root.selectionLabel !== ""
                    text: root.selectionLabel
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    text: (root.favorite ? "★ " : "") + root.fileName
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: Theme.brightForeground
                    elide: Text.ElideMiddle
                }

                Text {
                    width: parent.width
                    text: root.kindLabel + "  ·  " + root.sizeLabel
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }

                Text {
                    width: parent.width
                    text: root.dayLabel + "  ·  " + root.timeLabel
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }

                Text {
                    objectName: "mediaMetadata"
                    width: parent.width
                    visible: root.technicalLabel !== ""
                    text: root.technicalLabel
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }

                Text {
                    width: parent.width
                    visible: MediaInfo.path === root.path && MediaInfo.loading
                    text: "Reading details…"
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }

                // Rating. Click a star to set it, or the lit star again to
                // clear. Alt+1 to Alt+5 and Alt+0 do the same from the keyboard.
                Row {
                    objectName: "viewerRating"
                    spacing: 2

                    Repeater {
                        model: 5

                        Text {
                            required property int index
                            readonly property bool lit: index < root.rating
                            text: "★"
                            font.pixelSize: 15
                            color: lit ? Theme.yellow : root.shade(Theme.foreground, 0.28)
                            Accessible.role: Accessible.Button
                            Accessible.name: (index + 1) + (index === 0 ? " star" : " stars")

                            TapHandler {
                                onTapped: root.rateRequested(index + 1 === root.rating
                                                             ? 0 : index + 1)
                            }
                        }
                    }
                }

                // Caption. Enter or leaving the field saves; Escape puts the
                // saved text back and returns to the picture.
                Rectangle {
                    width: parent.width
                    height: 26
                    radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
                    color: root.shade(Theme.foreground, captionField.activeFocus ? 0.10 : 0.05)
                    border.width: 1
                    border.color: captionField.activeFocus
                                  ? root.shade(Theme.accent, 0.6)
                                  : root.shade(Theme.foreground, 0.12)

                    TextInput {
                        id: captionField
                        objectName: "viewerCaption"
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        activeFocusOnTab: false
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: Theme.foreground
                        selectionColor: root.shade(Theme.accent, 0.5)
                        selectedTextColor: Theme.brightForeground
                        // Typing breaks a plain binding, so follow the saved
                        // caption explicitly when the file or its marks change.
                        readonly property string saved: root.caption
                        onSavedChanged: text = saved
                        Component.onCompleted: text = saved
                        onEditingFinished: {
                            if (text.trim() !== root.caption) {
                                root.captionEdited(text.trim())
                            }
                        }
                        Keys.onEscapePressed: {
                            text = root.caption
                            root.focusPreview()
                        }
                        Keys.onReturnPressed: root.focusPreview()
                        Keys.onEnterPressed: root.focusPreview()
                    }

                    Text {
                        anchors.fill: captionField
                        verticalAlignment: Text.AlignVCenter
                        visible: captionField.text === "" && !captionField.activeFocus
                        text: "Add a caption"
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: root.shade(Theme.foreground, 0.35)
                    }

                    TapHandler {
                        onTapped: captionField.forceActiveFocus()
                    }
                }

                Repeater {
                    model: MediaInfo.path === root.path ? MediaInfo.lines : []
                    Text {
                        required property string modelData
                        width: metadataColumn.width
                        text: modelData
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: Theme.mutedText
                    }
                }
            }

            Text {
                id: actionsHeading
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.top: metadataColumn.bottom
                anchors.topMargin: 14
                text: "ACTIONS"
                font.family: Theme.fontFamily
                font.pixelSize: 9
                font.weight: Font.DemiBold
                color: root.shade(Theme.foreground, 0.38)
            }

            // Actions, from the registry
            ListView {
                id: actions
                objectName: "viewerActions"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: actionsHeading.bottom
                anchors.bottom: parent.bottom
                anchors.topMargin: 4
                anchors.bottomMargin: 14
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                clip: true
                spacing: 1
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                model: root.visible ? root.visibleActions() : []

                delegate: FocusScope {
                    id: row
                    required property int index
                    required property var modelData
                    objectName: "viewerAction_" + modelData.id
                    width: actions.width
                    height: 30
                    activeFocusOnTab: false

                    readonly property bool usable: modelData.available
                    readonly property bool primary: modelData.id ===
                                                    Registry.primaryActionForKind(
                                                        root.isVideo, root.isDocument)
                    readonly property string shortcut: modelData.shortcut
                    readonly property string toolTipText: modelData.shortcut !== ""
                                                           ? modelData.label + "  ·  "
                                                             + modelData.shortcut
                                                           : ""

                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.label
                    Accessible.description: modelData.shortcut !== ""
                                            ? "Shortcut " + modelData.shortcut : ""
                    Accessible.ignored: !row.usable
                    Accessible.onPressAction: if (row.usable) root.actionTriggered(row.modelData.id)

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
                        text: row.modelData.label
                              + (row.usable ? "" : "  ·  needs " + row.modelData.hint)
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: row.primary ? Font.DemiBold : Font.Normal
                        color: !row.usable
                               ? root.shade(Theme.foreground, 0.32)
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
                        color: root.shade(Theme.foreground, 0.35)
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
                            root.actionTriggered(row.modelData.id)
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
                            root.actionTriggered(row.modelData.id)
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
            return "Del"
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

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Backtab
                || (event.key === Qt.Key_Tab && (event.modifiers & Qt.ShiftModifier))) {
            root.focusPreview()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Tab) {
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
        if (event.key === Qt.Key_Escape) {
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
            root.showActualImageSize()
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
            audio.muted = !audio.muted
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
                root.actionTriggered(row.id)
                event.accepted = true
                return
            }
        }
    }
}
