import QtQuick
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
    readonly property int slideshowInterval: 4000

    signal actionTriggered(string id)
    signal navigateRequested(int direction)
    signal fullScreenRequested(bool enabled)

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function open() {
        playbackError = ""
        if (!visible) {
            showInfo = true
            slideshowRunning = false
        }
        resetImageView()
        visible = true
        forceActiveFocus()
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
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            preventStealing: true
            onClicked: root.close()
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
            acceptedButtons: Qt.LeftButton | Qt.RightButton
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
                          + root.stamp + root.path
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
                onClicked: root.requestNavigation(-1)
            }

            PillButton {
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                visible: root.canNavigate
                label: "→"
                floating: true
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
                        label: root.fullScreen || stage.width < 520
                               ? (root.slideshowRunning ? "Ⅱ" : "▶")
                               : (root.slideshowRunning ? "Pause slideshow" : "Start slideshow")
                        toolTip: root.fullScreen || stage.width < 520
                                 ? (root.slideshowRunning ? "Pause slideshow" : "Start slideshow")
                                 : ""
                        active: root.slideshowRunning
                        onClicked: root.setSlideshow(!root.slideshowRunning)
                    }
                    PillButton {
                        label: root.fullScreen || stage.width < 520 ? "i"
                                               : (root.showInfo ? "Hide details" : "Show details")
                        toolTip: root.fullScreen || stage.width < 520
                                 ? (root.showInfo ? "Hide details" : "Show details") : ""
                        active: root.showInfo
                        onClicked: root.showInfo = !root.showInfo
                    }
                    PillButton {
                        visible: root.isVideo || root.slideshowRunning
                        label: root.fullScreen ? "↙" : (stage.width < 520 ? "⛶" : "Fullscreen")
                        toolTip: root.fullScreen ? "Exit fullscreen"
                                                 : (stage.width < 520 ? "Fullscreen" : "")
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
                    active: root.imageZoom === 1 && root.imageRotation === 0
                    onClicked: root.resetImageView()
                }
                PillButton {
                    label: "−"
                    onClicked: root.adjustImageZoom(1 / 1.25)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: stage.width >= 520
                    width: 42
                    horizontalAlignment: Text.AlignHCenter
                    text: Math.round(root.imageZoom * 100) + "%"
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }
                PillButton {
                    label: "+"
                    onClicked: root.adjustImageZoom(1.25)
                }
                PillButton {
                    label: stage.width < 520 ? "↻" : "Rotate"
                    toolTip: stage.width < 520 ? "Rotate" : ""
                    onClicked: root.rotateImage()
                }
                PillButton {
                    label: root.fullScreen ? "↙" : (stage.width < 520 ? "⛶" : "Full")
                    toolTip: root.fullScreen ? "Exit fullscreen"
                                             : (stage.width < 520 ? "Fullscreen" : "")
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
                    anchors.right: clockLabel.left
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
            }

            // Actions, from the registry
            ListView {
                id: actions
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.topMargin: 96
                anchors.bottomMargin: 14
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                clip: true
                spacing: 1

                model: root.visible ? Registry.actionsFor(root.isVideo) : []

                delegate: Item {
                    id: row
                    required property var modelData
                    width: actions.width
                    height: 30

                    readonly property bool usable: modelData.available
                    readonly property bool primary: modelData.id === (root.isVideo
                                                       ? Settings.videoPrimaryAction
                                                       : Settings.imagePrimaryAction)

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
                        color: rowHover.hovered && row.usable
                               ? root.shade(Theme.foreground, 0.09)
                               : "transparent"
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

                    TapHandler {
                        enabled: row.usable
                        onSingleTapped: root.actionTriggered(row.modelData.id)
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
        if (event.key === Qt.Key_F5) {
            root.setSlideshow(!root.slideshowRunning)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_F11) {
            root.setFullScreen(!root.fullScreen)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_I) {
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
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
            root.requestNavigation(event.key === Qt.Key_Left ? -1 : 1)
            event.accepted = true
            return
        }
        if (!root.isVideo && (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal)) {
            root.adjustImageZoom(1.25)
            event.accepted = true
            return
        }
        if (!root.isVideo && event.key === Qt.Key_Minus) {
            root.adjustImageZoom(1 / 1.25)
            event.accepted = true
            return
        }
        if (!root.isVideo && event.key === Qt.Key_0) {
            root.resetImageView()
            event.accepted = true
            return
        }
        if (!root.isVideo && event.key === Qt.Key_R) {
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
