import QtQuick

// Which machine on the tailnet gets the file.
//
// omarchy-tailscale-send does the sending and its own notification; it only
// needs a machine name first. This asks the daemon for the machines that can
// take a Taildrop right now and hands the chosen one over.
Item {
    id: root

    property var paths: []
    property int current: 0
    signal sent(string machine, int count)

    function open(files) {
        paths = files === undefined ? [] : files
        current = 0
        Tailscale.refresh()
        visible = true
        forceActiveFocus()
    }

    function close() { visible = false }

    function choose(index) {
        const peer = Tailscale.peers[index]
        if (peer === undefined) {
            return
        }
        root.close()
        Registry.runBatchWith("tailscale", { "machine": peer.machine }, root.paths)
        root.sent(peer.name, root.paths.length)
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    visible: false
    anchors.fill: parent
    focus: visible

    // Own wheel input for the whole modal so it cannot scroll the library
    // underneath, as Settings does.
    WheelHandler {
        target: null
        enabled: root.visible
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (event) { event.accepted = true }
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.55)
        TapHandler { onSingleTapped: root.close() }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(420, root.width - 60)
        height: column.implicitHeight + 40
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        TapHandler { onSingleTapped: {} }

        Column {
            id: column
            anchors.centerIn: parent
            width: parent.width - 40
            spacing: 8

            Text {
                width: parent.width
                text: "Send to a machine"
                font.family: Theme.fontFamily
                font.pixelSize: 14
                font.weight: Font.DemiBold
                color: Theme.brightForeground
            }

            Text {
                width: parent.width
                text: root.paths.length === 1
                      ? root.paths[0].substring(root.paths[0].lastIndexOf("/") + 1)
                      : root.paths.length + " files"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.mutedText
                elide: Text.ElideMiddle
            }

            Item { width: 1; height: 4 }

            // Why there is nothing to pick, while there is nothing to pick.
            Text {
                width: parent.width
                visible: Tailscale.peers.length === 0
                text: Tailscale.busy ? "Asking Tailscale…" : Tailscale.error
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Tailscale.busy ? Theme.mutedText : Theme.red
                wrapMode: Text.WordWrap
            }

            Column {
                width: parent.width
                spacing: 2

                Repeater {
                    model: Tailscale.peers

                    Rectangle {
                        id: peerRow
                        required property int index
                        required property var modelData
                        width: parent.width
                        height: 34
                        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
                        color: root.current === index
                               ? root.shade(Theme.accent, 0.16)
                               : (peerHover.hovered ? root.shade(Theme.foreground, 0.08)
                                                    : "transparent")
                        border.width: root.current === index ? 1 : 0
                        border.color: root.shade(Theme.accent, 0.6)
                        Behavior on color { ColorAnimation { duration: 120 } }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.right: osLabel.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: peerRow.modelData.name
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            color: root.current === peerRow.index ? Theme.accent : Theme.foreground
                        }

                        Text {
                            id: osLabel
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: peerRow.modelData.os
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            color: Theme.mutedText
                        }

                        HoverHandler {
                            id: peerHover
                            cursorShape: Qt.PointingHandCursor
                            onHoveredChanged: if (hovered) root.current = peerRow.index
                        }
                        TapHandler { onSingleTapped: root.choose(peerRow.index) }
                    }
                }
            }

            Item { width: 1; height: 8 }

            Row {
                anchors.right: parent.right
                spacing: 8

                PillButton {
                    visible: !Tailscale.busy && Tailscale.peers.length === 0
                    label: "Retry"
                    onClicked: Tailscale.refresh()
                }
                PillButton {
                    label: "Cancel"
                    onClicked: root.close()
                }
                PillButton {
                    visible: Tailscale.peers.length > 0
                    label: "Send"
                    active: true
                    onClicked: root.choose(root.current)
                }
            }
        }
    }

    Keys.onPressed: function (event) {
        switch (event.key) {
        case Qt.Key_Escape:
            root.close()
            event.accepted = true
            break
        case Qt.Key_Return:
        case Qt.Key_Enter:
            root.choose(root.current)
            event.accepted = true
            break
        case Qt.Key_Down:
        case Qt.Key_J:
            if (Tailscale.peers.length > 0) {
                root.current = (root.current + 1) % Tailscale.peers.length
            }
            event.accepted = true
            break
        case Qt.Key_Up:
        case Qt.Key_K:
            if (Tailscale.peers.length > 0) {
                root.current = (root.current + Tailscale.peers.length - 1) % Tailscale.peers.length
            }
            event.accepted = true
            break
        }
    }
}
