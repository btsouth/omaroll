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
    property string kindLabel: ""
    property string dayLabel: ""
    property string timeLabel: ""
    property string sizeLabel: ""
    property bool isVideo: false
    property double stamp: 0
    property bool favorite: false
    property int kind: 0
    property string playbackError: ""
    property bool canNavigate: false
    property real imageZoom: 1.0
    property int imageRotation: 0
    property real imageSourceWidth: 0
    property real imageSourceHeight: 0
    property bool stillReady: false
    property bool fullScreen: false
    property bool showInfo: true
    property bool slideshowRunning: false
    property bool slideshowPausedForRender: false
    property bool qrDetected: false
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
    readonly property string durationLabel: root.isVideo && player.duration > 0
                                             ? root.formatDuration(player.duration) : ""
    readonly property real playbackPosition: player.position
    readonly property string technicalLabel: root.dimensionsLabel !== "" && root.durationLabel !== ""
                                              ? root.dimensionsLabel + "  ·  " + root.durationLabel
                                              : (root.dimensionsLabel !== "" ? root.dimensionsLabel
                                                                             : root.durationLabel)
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
        zoomOut: { key: Qt.Key_Minus, label: "−" },
        zoomIn: { key: Qt.Key_Plus, label: "+" },
        rotate: { key: Qt.Key_R, label: "R" }
    })
    property bool actionNavigationActive: false
    property string focusedActionId: ""
    onQrDetectedChanged: {
        if (actionNavigationActive) {
            Qt.callLater(restoreFocus)
        }
    }

    signal actionTriggered(string id)
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
        const rows = Registry.actionsFor(root.isVideo)
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
        const keepActionFocus = visible && actionNavigationActive
        const previousActionId = focusedActionId
        playbackError = ""
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
        Qt.callLater(stillViewport.centerContent)
    }

    function adjustImageZoom(factor) {
        imageZoom = Math.max(1, Math.min(4, imageZoom * factor))
        Qt.callLater(stillViewport.centerContent)
    }

    function rotateImage() {
        imageRotation = (imageRotation + 90) % 360
        imageZoom = 1.0
        Qt.callLater(stillViewport.centerContent)
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
        imageSourceWidth = 0
        imageSourceHeight = 0
        MediaInfo.inspect(path, isVideo)
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
                    model: ["Fit", "−", "+", "Rotate", "Full"]
                    PillButton { label: modelData }
                }
                Item { width: 42; height: 1 }
            }
            Row {
                id: compactImageMeasure
                visible: false
                spacing: 6
                Repeater {
                    model: ["Fit", "−", "+", "↻", "⛶"]
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
                visible: root.isVideo && !player.hasVideo
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

                    Loader {
                        id: stillLoader
                        anchors.centerIn: parent
                        width: stillViewport.sourceWidth * stillViewport.fittedScale
                               * root.imageZoom
                        height: stillViewport.sourceHeight * stillViewport.fittedScale
                                * root.imageZoom
                        rotation: root.imageRotation
                        readonly property string suffix: root.fileName.toLowerCase()
                        sourceComponent: suffix.endsWith(".gif") || suffix.endsWith(".webp")
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
                id: animatedStill
                AnimatedImage {
                    source: root.visible && !root.isVideo && root.path !== ""
                            ? Library.fileUrl(root.path) : ""
                    asynchronous: true
                    autoTransform: true
                    cache: false
                    playing: root.visible
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

            // A recording plays in place, muted, the moment the preview opens.
            // Enough to tell two clips apart or find the moment to trim; the
            // full player with sound is one keystroke away in mpv.
            MediaPlayer {
                id: player
                source: root.visible && root.isVideo && root.path !== ""
                        ? Library.fileUrl(root.path) : ""
                videoOutput: output
                audioOutput: AudioOutput { id: audio; muted: true }
                loops: root.slideshowRunning ? 1 : MediaPlayer.Infinite
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
                onMediaStatusChanged: {
                    if (mediaStatus === MediaPlayer.EndOfMedia && root.slideshowRunning) {
                        root.requestNavigation(1)
                    }
                }
            }

            // Decoding a muted preview while the window is minimised is wasted
            // battery; resume when it comes back.
            readonly property bool windowShown: Window.visibility !== Window.Minimized
                                                && Window.visibility !== Window.Hidden
            onWindowShownChanged: {
                if (!root.isVideo || !root.visible) {
                    return
                }
                if (windowShown) {
                    player.play()
                } else {
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

            VideoOutput {
                id: output
                anchors.fill: parent
                anchors.margins: 16
                anchors.bottomMargin: 56
                fillMode: VideoOutput.PreserveAspectFit
                visible: root.isVideo && player.hasVideo
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
                    onClicked: root.resetImageView()
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
                    text: Math.round(root.imageZoom * 100) + "%"
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

            // Transport: play/pause, a scrub bar, the clock, sound.
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

                // Play, clock and sound at their natural widths beside a scrub
                // bar that is still worth dragging.
                readonly property int scrubMinimum: 120
                readonly property real minimumWidth: playButton.implicitWidth + 14 + scrubMinimum
                                                     + 14 + clockLabel.implicitWidth + 12
                                                     + soundButton.implicitWidth

                function clock(ms) {
                    const total = Math.max(0, Math.round(ms / 1000))
                    const minutes = Math.floor(total / 60)
                    const seconds = total % 60
                    return minutes + ":" + (seconds < 10 ? "0" : "") + seconds
                }

                PillButton {
                    id: playButton
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    label: player.playbackState === MediaPlayer.PlayingState ? "❚❚" : "▶"
                    onClicked: player.playbackState === MediaPlayer.PlayingState
                               ? player.pause() : player.play()
                }

                Item {
                    id: scrub
                    anchors.left: playButton.right
                    anchors.right: clockLabel.visible ? clockLabel.left : soundButton.left
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height

                    readonly property real fraction: player.duration > 0
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
                        if (player.duration > 0) {
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
                    anchors.right: soundButton.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: transport.clock(player.position) + " / " + transport.clock(player.duration)
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }

                PillButton {
                    id: soundButton
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    label: audio.muted ? "Muted" : "Sound"
                    active: !audio.muted
                    onClicked: audio.muted = !audio.muted
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
                visible: root.isVideo && root.slideshowRunning && player.duration > 0
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
                    width: parent.width * Math.max(0, Math.min(1, player.position / player.duration))
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
                    readonly property bool primary: modelData.id === (root.isVideo
                                                       ? Settings.videoPrimaryAction
                                                       : Settings.imagePrimaryAction)
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
            root.actionTriggered(root.isVideo ? Settings.videoPrimaryAction
                                              : Settings.imagePrimaryAction)
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
        if (!root.isVideo && event.key === root.viewerShortcuts.rotate.key) {
            root.rotateImage()
            event.accepted = true
            return
        }
        if (root.isVideo && (event.key === Qt.Key_J || event.key === Qt.Key_L)) {
            const step = event.key === Qt.Key_J ? -5000 : 5000
            player.position = Math.max(0, Math.min(player.duration, player.position + step))
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
