import QtQuick
import QtMultimedia

// The viewer's one row of video controls: play, time, scrub, and sound, with
// captions and speed only when they mean something. Everything else a player
// could offer lives on the keyboard or in the viewer's menu.
Rectangle {
    id: root

    property var player: null
    property bool fullScreen: false
    property bool hasCaptions: false
    property bool captionsOn: false
    property string captionsLabel: ""
    readonly property bool scrubbing: scrubMouse.pressed
    readonly property bool hovered: hover.hovered

    signal fullScreenToggled()
    signal captionsCycled()
    signal playToggled()

    readonly property real duration: player ? player.duration : 0
    readonly property real position: player ? player.position : 0
    readonly property bool playing: player && player.playbackState === MediaPlayer.PlayingState
    readonly property real rate: player ? player.playbackRate : 1
    readonly property bool wide: width >= 560

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function clock(milliseconds) {
        const total = Math.max(0, Math.round(milliseconds / 1000))
        const hours = Math.floor(total / 3600)
        const minutes = Math.floor((total % 3600) / 60)
        const seconds = total % 60
        const padded = String(seconds).padStart(2, "0")
        return hours > 0 ? hours + ":" + String(minutes).padStart(2, "0") + ":" + padded
                         : minutes + ":" + padded
    }

    function cycleRate() {
        const rates = [1, 1.25, 1.5, 2, 0.5]
        for (let index = 0; index < rates.length; ++index) {
            if (Math.abs(root.player.playbackRate - rates[index]) < 0.01) {
                root.player.playbackRate = rates[(index + 1) % rates.length]
                return
            }
        }
        root.player.playbackRate = 1
    }

    implicitHeight: 46
    radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, height / 2) : 4
    color: root.shade(Theme.background, 0.84)
    border.width: 1
    border.color: root.shade(Theme.foreground, 0.12)

    HoverHandler { id: hover }

    // A press on the bar between its controls stays on the bar rather than
    // reaching the stage, where it would play or pause.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
    }

    IconButton {
        id: playButton
        objectName: "viewerPlayButton"
        anchors.left: parent.left
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        icon: root.playing ? "pause" : "play"
        iconSize: 18
        toolTip: root.playing ? "Pause" : "Play"
        shortcut: "Space"
        onClicked: root.playToggled()
    }

    Text {
        id: elapsed
        anchors.left: playButton.right
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: root.clock(root.position)
        font.family: Theme.fontFamily
        font.pixelSize: 11
        color: Theme.foreground
    }

    Item {
        id: scrub
        objectName: "viewerScrub"
        anchors.left: elapsed.right
        anchors.right: total.left
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        height: 24

        readonly property real fraction: root.duration > 0
                                         ? Math.max(0, Math.min(1, root.position / root.duration)) : 0
        readonly property bool lifted: scrubMouse.containsMouse || scrubMouse.pressed

        function seekTo(x) {
            if (root.player && root.duration > 0) {
                root.player.position = Math.max(0, Math.min(1, x / width)) * root.duration
            }
        }

        Rectangle {
            id: track
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: scrub.lifted ? 5 : 3
            radius: height / 2
            color: root.shade(Theme.foreground, 0.20)
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

        Rectangle {
            width: 12
            height: 12
            radius: 6
            x: scrub.width * scrub.fraction - width / 2
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.brightForeground
            border.width: 2
            border.color: Theme.accent
            opacity: scrub.lifted ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 100 } }
        }

        // The time under the pointer, so a seek lands where it was aimed.
        Rectangle {
            visible: scrubMouse.containsMouse && root.duration > 0
            readonly property real at: Math.max(0, Math.min(scrub.width, scrubMouse.mouseX))
            x: Math.max(-elapsed.width, Math.min(scrub.width - width + total.width,
                                                  at - width / 2))
            y: -height - 10
            width: hoverTime.implicitWidth + 12
            height: hoverTime.implicitHeight + 6
            radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 4) : 3
            color: root.shade(Theme.background, 0.94)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.16)

            Text {
                id: hoverTime
                anchors.centerIn: parent
                text: root.clock(parent.at / Math.max(1, scrub.width) * root.duration)
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.brightForeground
            }
        }

        MouseArea {
            id: scrubMouse
            anchors.fill: parent
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.PointingHandCursor
            onPressed: function (mouse) { scrub.seekTo(mouse.x) }
            onPositionChanged: function (mouse) { if (pressed) scrub.seekTo(mouse.x) }
        }
    }

    Text {
        id: total
        anchors.right: extras.left
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        text: root.clock(root.duration)
        font.family: Theme.fontFamily
        font.pixelSize: 11
        color: Theme.mutedText
    }

    Row {
        id: extras
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        IconButton {
            objectName: "viewerCaptionsButton"
            visible: root.hasCaptions
            anchors.verticalCenter: parent.verticalCenter
            icon: "captions"
            active: root.captionsOn
            toolTip: root.captionsOn ? "Subtitles: " + root.captionsLabel : "Subtitles"
            shortcut: "C"
            onClicked: root.captionsCycled()
        }

        // Shown once the speed is not normal, or when there is room to offer it.
        Rectangle {
            objectName: "viewerSpeedButton"
            visible: Math.abs(root.rate - 1) > 0.01 || root.wide
            anchors.verticalCenter: parent.verticalCenter
            width: speedLabel.implicitWidth + 16
            height: 30
            radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, height / 2) : 3
            color: speedMouse.containsMouse ? root.shade(Theme.foreground, 0.12) : "transparent"

            Text {
                id: speedLabel
                anchors.centerIn: parent
                text: Number(root.rate.toFixed(2)) + "×"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Math.abs(root.rate - 1) > 0.01 ? Font.DemiBold : Font.Normal
                color: Math.abs(root.rate - 1) > 0.01 ? Theme.accent : Theme.foreground
            }
            MouseArea {
                id: speedMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.player) root.cycleRate()
            }
        }

        IconButton {
            id: soundButton
            objectName: "viewerSoundButton"
            anchors.verticalCenter: parent.verticalCenter
            icon: Settings.videoMuted || Settings.videoVolume === 0 ? "volume-muted" : "volume"
            toolTip: Settings.videoMuted ? "Unmute" : "Mute"
            shortcut: "M"
            onClicked: Settings.videoMuted = !Settings.videoMuted
        }

        // Volume, for a window wide enough to give it room. Up and Down and
        // the wheel over the speaker set it everywhere else.
        Item {
            objectName: "viewerVolume"
            visible: root.wide
            anchors.verticalCenter: parent.verticalCenter
            width: 64
            height: 24

            readonly property real level: Settings.videoMuted ? 0 : Settings.videoVolume

            function setFrom(x) {
                Settings.videoVolume = Math.max(0, Math.min(1, x / width))
                if (Settings.videoVolume > 0) {
                    Settings.videoMuted = false
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: 3
                radius: 1.5
                color: root.shade(Theme.foreground, 0.20)

                Rectangle {
                    width: parent.width * parent.parent.level
                    height: parent.height
                    radius: parent.radius
                    color: Theme.foreground
                }
            }
            MouseArea {
                anchors.fill: parent
                preventStealing: true
                cursorShape: Qt.PointingHandCursor
                onPressed: function (mouse) { parent.setFrom(mouse.x) }
                onPositionChanged: function (mouse) { if (pressed) parent.setFrom(mouse.x) }
            }
        }

        IconButton {
            objectName: "viewerTransportFullScreen"
            anchors.verticalCenter: parent.verticalCenter
            icon: root.fullScreen ? "fullscreen-exit" : "fullscreen"
            toolTip: root.fullScreen ? "Exit full screen" : "Full screen"
            shortcut: "F"
            onClicked: root.fullScreenToggled()
        }
    }

    // The wheel over the bar sets the volume, the way it does in most players.
    WheelHandler {
        target: null
        onWheel: function (event) {
            const delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.pixelDelta.y
            Settings.videoVolume = Math.max(0, Math.min(1, Settings.videoVolume + (delta > 0 ? 0.05 : -0.05)))
            if (Settings.videoVolume > 0) {
                Settings.videoMuted = false
            }
            event.accepted = true
        }
    }
}
