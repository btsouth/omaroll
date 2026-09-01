import QtQuick

// Settings exist to change defaults, never to make the app work. Everything
// here has a working value on a fresh install, which is why this window is
// small and why nothing in it is required reading.
Item {
    id: root

    signal rescanRequested()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function open() {
        visible = true
        forceActiveFocus()
    }

    function close() { visible = false }

    visible: false
    anchors.fill: parent
    focus: visible

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.6)
        TapHandler { onSingleTapped: root.close() }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(520, root.width - 60)
        height: column.implicitHeight + 44
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        TapHandler { onSingleTapped: {} }

        Column {
            id: column
            anchors.centerIn: parent
            width: parent.width - 44
            spacing: 14

            Text {
                text: "Settings"
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.brightForeground
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: "Omaroll reads the folders Omarchy already writes captures to. "
                      + "It never moves, renames or changes a file it finds."
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }

            Rectangle {
                width: parent.width
                height: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            // Downloads
            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - toggleDownloads.width - 12
                    spacing: 2

                    Text {
                        text: "Include Downloads"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Media files in your Downloads folder, top level only."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: toggleDownloads
                    anchors.verticalCenter: parent.verticalCenter
                    label: Settings.scanDownloads ? "On" : "Off"
                    active: Settings.scanDownloads
                    onClicked: Settings.scanDownloads = !Settings.scanDownloads
                }
            }

            // Recursion depth
            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - depthButton.width - 12
                    spacing: 2

                    Text {
                        text: "Folder depth"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "How far to look inside Pictures and Videos. Screenshots and "
                              + "recordings are always read from the top level, where Omarchy "
                              + "writes them."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: depthButton
                    anchors.verticalCenter: parent.verticalCenter
                    label: Settings.recursionDepth + " deep"
                    onClicked: Settings.recursionDepth =
                               Settings.recursionDepth >= 6 ? 1 : Settings.recursionDepth + 1
                }
            }

            // Hidden
            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - hiddenButton.width - 12
                    spacing: 2

                    Text {
                        text: "Show hidden captures"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Hiding is a \"don't show me this again\" mark. It never touches "
                              + "the file."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: hiddenButton
                    anchors.verticalCenter: parent.verticalCenter
                    label: Captures.showHidden ? "Shown" : "Hidden"
                    active: Captures.showHidden
                    onClicked: Captures.showHidden = !Captures.showHidden
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            Row {
                anchors.right: parent.right
                spacing: 8

                PillButton {
                    label: "Rescan now"
                    onClicked: {
                        root.rescanRequested()
                        root.close()
                    }
                }

                PillButton {
                    label: "Done"
                    active: true
                    onClicked: root.close()
                }
            }
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            root.close()
            event.accepted = true
        }
    }
}
