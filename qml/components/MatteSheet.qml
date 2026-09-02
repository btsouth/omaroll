import QtQuick

// The matte picker: six finished backgrounds and the raw capture, composed from
// the image's own dominant hue.
//
// No editor in the flow. Choosing between finished results replaces tweaking
// one, which is the whole idea and the reason this is the only feature omaroll
// builds rather than delegates.
Item {
    id: root

    property string path: ""
    property string fileName: ""
    property int selected: 0
    property int aspect: 0
    property int paddingPercent: 7

    signal saved(string outputPath)

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function open() {
        selected = 0
        visible = true
        forceActiveFocus()
    }

    function close() { visible = false }

    function save() {
        if (preview.status === Image.Error) {
            return
        }
        Matte.composeAndSave(root.path, root.selected, root.aspect, root.paddingPercent / 100.0)
        root.close()
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

    readonly property var matteNames: Matte.matteNames()
    readonly property var aspectNames: Matte.aspectNames()

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.68)
        // A MouseArea rather than a TapHandler: a TapHandler only takes a
        // passive grab, so a tap on a control inside the card also arrived
        // here and closed the sheet. Every button is accepted so a right
        // click cannot fall through to a tile behind the scrim.
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
        anchors.centerIn: parent
        width: Math.min(1040, root.width - 60)
        height: Math.min(760, root.height - 60)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.97)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        // Clicks inside the card must not reach the scrim behind it.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
        }

        // Header
        Item {
            id: head
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 54

            Text {
                id: title
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                text: "Make it postable"
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.brightForeground
            }

            Text {
                anchors.left: title.right
                anchors.leftMargin: 16
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideMiddle
                text: root.fileName
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }
        }

        // Large preview of the current choice
        Rectangle {
            id: stage
            anchors.top: head.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: strip.top
            anchors.margins: 16
            color: root.shade(Theme.darkerBackground, 0.6)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3

            Image {
                id: preview
                anchors.fill: parent
                anchors.margins: 10
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true
                mipmap: true
                sourceSize: Qt.size(Math.round(stage.width * Screen.devicePixelRatio),
                                    Math.round(stage.height * Screen.devicePixelRatio))
                source: root.path === "" ? "" :
                        "image://matte/" + root.selected + "." + root.aspect + "."
                        + root.paddingPercent + root.path
                opacity: status === Image.Ready ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 160 } }
            }

            Text {
                anchors.centerIn: parent
                visible: preview.status !== Image.Ready
                text: preview.status === Image.Error ? "Could not read this file" : "Composing…"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: preview.status === Image.Error ? Theme.red : Theme.mutedText
            }
        }

        // Contact strip of every matte
        ListView {
            id: strip
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: controls.top
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            anchors.bottomMargin: 12
            height: 96
            orientation: ListView.Horizontal
            spacing: 8
            clip: true

            model: root.matteNames

            delegate: Item {
                id: chip
                required property int index
                required property string modelData
                width: 128
                height: strip.height

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
                    color: root.shade(Theme.darkerBackground, 0.5)
                    border.width: root.selected === chip.index ? 2 : 1
                    border.color: root.selected === chip.index
                                  ? Theme.accent
                                  : root.shade(Theme.foreground, chipHover.hovered ? 0.24 : 0.12)
                    clip: true

                    Behavior on border.color { ColorAnimation { duration: 130 } }

                    Image {
                        anchors.fill: parent
                        anchors.margins: 2
                        anchors.bottomMargin: 18
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        smooth: true
                        sourceSize: Qt.size(248, 150)
                        source: root.path === "" ? "" :
                                "image://matte/" + chip.index + "." + root.aspect + "."
                                + root.paddingPercent + root.path
                    }

                    Text {
                        anchors.bottom: parent.bottom
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottomMargin: 4
                        text: chip.modelData
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: root.selected === chip.index ? Theme.accent : Theme.mutedText
                    }

                    HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onSingleTapped: root.selected = chip.index }
                }
            }
        }

        // Controls
        Item {
            id: controls
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 56

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                Repeater {
                    model: root.aspectNames

                    PillButton {
                        required property int index
                        required property string modelData
                        label: modelData
                        active: root.aspect === index
                        onClicked: root.aspect = index
                    }
                }

                Item { width: 10; height: 1 }

                PillButton {
                    label: "Padding " + root.paddingPercent + "%"
                    onClicked: {
                        // Cycles the three that matter rather than exposing a
                        // slider for a value nobody tunes precisely.
                        const stops = [0, 4, 7, 12, 18]
                        const next = stops.indexOf(root.paddingPercent) + 1
                        root.paddingPercent = stops[next % stops.length]
                    }
                }
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                PillButton {
                    label: "Cancel"
                    onClicked: root.close()
                }

                PillButton {
                    label: "Copy and save"
                    active: preview.status !== Image.Error
                    onClicked: root.save()
                }
            }
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            root.close()
            event.accepted = true
        } else if (event.key === Qt.Key_Left) {
            root.selected = (root.selected + root.matteNames.length - 1) % root.matteNames.length
            event.accepted = true
        } else if (event.key === Qt.Key_Right) {
            root.selected = (root.selected + 1) % root.matteNames.length
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.save()
            event.accepted = true
        } else if (event.key >= Qt.Key_1 && event.key <= Qt.Key_7) {
            root.selected = event.key - Qt.Key_1
            event.accepted = true
        }
    }
}
