import QtQuick
import QtQuick.Controls.Basic

// Pick one RAW developer once, or show the chooser on every Develop request.
// Editors owns persistence and launching; this sheet only gathers the choice.
Popup {
    id: root

    property string selectedId: ""
    property string customDraft: ""
    property string path: ""
    property bool requestedHere: false

    // The shared preference service emits synchronously. Only the window
    // making this request opens a dialog; each dialog keeps its own file.
    function request(path, choose) {
        root.requestedHere = true
        try {
            if (choose) Editors.showChooser(path)
            else Editors.requestOpen(path)
        } finally {
            root.requestedHere = false
        }
    }
    readonly property bool hasAvailable: {
        const rows = Editors.candidates
        for (let index = 0; index < rows.length; ++index) {
            if (rows[index].available) {
                return true
            }
        }
        return false
    }
    readonly property string hint: Editors.message !== ""
                                      ? Editors.message
                                      : (root.hasAvailable ? ""
                                                           : "No RAW editor is installed. Install darktable or RawTherapee, or set a custom command.")

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function fileName(path) {
        return path.substring(path.lastIndexOf("/") + 1)
    }

    function pickInitial() {
        const rows = Editors.candidates
        for (let index = 0; index < rows.length; ++index) {
            if (rows[index].preferred && rows[index].available) {
                root.selectedId = rows[index].id
                return
            }
        }
        for (let index = 0; index < rows.length; ++index) {
            if (rows[index].available) {
                root.selectedId = rows[index].id
                return
            }
        }
        root.selectedId = ""
    }

    // Called by Editors.chooseRequested and by windows that want to force the
    // chooser. The equality check keeps showChooser from signalling back here.
    function openFor(path) {
        root.path = path
        root.open()
    }

    function openSelected() {
        if (root.selectedId === "") {
            return
        }
        if (root.selectedId === "custom") {
            Editors.customCommand = root.customDraft
        }
        if (Editors.launch(root.selectedId, root.path)) {
            root.close()
        }
    }

    width: Math.min(500, parent.width - 60)
    height: Math.min(parent.height - 60, contentItem.implicitHeight + 44)
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    padding: 22
    modal: true
    focus: true
    dim: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)
    }

    contentItem: Flickable {
        id: scroller
        objectName: "editorChooserScroll"

        implicitWidth: root.availableWidth
        implicitHeight: Math.min(root.parent.height - 104, content.implicitHeight)
        width: implicitWidth
        height: implicitHeight
        contentWidth: width
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Keys.onPressed: function (event) {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                root.openSelected()
                event.accepted = true
            }
        }

        ScrollBar.vertical: ScrollBar {
            id: verticalScrollBar
            policy: ScrollBar.AsNeeded
        }

        Column {
            id: content

            width: scroller.width - (verticalScrollBar.visible
                                     ? verticalScrollBar.width + 8 : 0)
            spacing: 9

            Text {
                width: parent.width
                text: "Open RAW in an editor"
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.brightForeground
            }

            Text {
                width: parent.width
                text: root.fileName(root.path)
                elide: Text.ElideMiddle
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }

            Column {
                width: parent.width
                spacing: 2

                Repeater {
                    model: Editors.candidates

                    Rectangle {
                        id: candidateRow
                        required property int index
                        required property var modelData

                        width: parent.width
                        height: 38
                        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
                        color: root.selectedId === candidateRow.modelData.id
                               ? root.shade(Theme.accent, 0.16)
                               : (candidateHover.hovered
                                  ? root.shade(Theme.foreground, 0.08) : "transparent")
                        border.width: root.selectedId === candidateRow.modelData.id ? 1 : 0
                        border.color: root.shade(Theme.accent, 0.60)
                        opacity: candidateRow.modelData.available ? 1 : 0.52

                        Text {
                            id: candidateLabel
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.right: candidateState.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: candidateRow.modelData.label
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            color: root.selectedId === candidateRow.modelData.id
                                   ? Theme.brightForeground : Theme.foreground
                        }

                        Text {
                            id: candidateState
                            anchors.right: favoriteButton.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: candidateRow.modelData.available
                                  ? ""
                                  : (candidateRow.modelData.id === "custom"
                                     ? (root.customDraft.trim() === ""
                                        ? "Not configured" : "Not available")
                                     : "Not installed")
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            color: Theme.mutedText
                        }

                        IconButton {
                            id: favoriteButton
                            anchors.right: parent.right
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            width: 30
                            height: 30
                            icon: candidateRow.modelData.preferred ? "star-filled" : "star"
                            active: candidateRow.modelData.preferred
                            enabled: candidateRow.modelData.available
                            toolTip: candidateRow.modelData.preferred
                                     ? "Default editor" : "Use by default"
                            onClicked: {
                                if (Editors.setPreferred(candidateRow.modelData.id)) {
                                    root.selectedId = candidateRow.modelData.id
                                }
                            }
                        }

                        HoverHandler {
                            id: candidateHover
                            enabled: candidateRow.modelData.available
                                     || candidateRow.modelData.id === "custom"
                            cursorShape: Qt.PointingHandCursor
                        }

                        TapHandler {
                            enabled: candidateRow.modelData.available
                                     || candidateRow.modelData.id === "custom"
                            onSingleTapped: root.selectedId = candidateRow.modelData.id
                        }
                    }
                }
            }

            Text {
                width: parent.width
                visible: root.hint !== ""
                text: root.hint
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.yellow
            }

            Rectangle {
                width: parent.width
                height: 36
                radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
                color: root.shade(Theme.foreground, 0.06)
                border.width: 1
                border.color: customInput.activeFocus
                              ? root.shade(Theme.accent, 0.65)
                              : root.shade(Theme.foreground, 0.16)

                TextInput {
                    id: customInput
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    text: root.customDraft
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.foreground
                    selectionColor: root.shade(Theme.accent, 0.5)
                    selectedTextColor: Theme.brightForeground
                    onTextChanged: {
                        if (root.customDraft !== text) {
                            root.customDraft = text
                        }
                        Editors.customCommand = text
                    }
                    Keys.onReturnPressed: root.openSelected()
                    Keys.onEnterPressed: root.openSelected()
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    visible: customInput.text === ""
                    text: "Custom command, for example rawtherapee {path}"
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }
            }

            Text {
                width: parent.width
                text: "Use {path} where the file should go; without it, the path is appended."
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 10
                color: Theme.mutedText
            }

            Row {
                width: parent.width
                spacing: 8

                Item {
                    id: alwaysAsk
                    width: 168
                    height: 24

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 7

                        Rectangle {
                            width: 15
                            height: 15
                            anchors.verticalCenter: parent.verticalCenter
                            radius: Theme.cornerRadius > 0
                                    ? Math.min(3, Theme.cornerRadius) : 2
                            color: Editors.alwaysChoose
                                   ? root.shade(Theme.accent, 0.72)
                                   : root.shade(Theme.foreground, 0.06)
                            border.width: 1
                            border.color: Editors.alwaysChoose
                                          ? Theme.accent
                                          : root.shade(Theme.foreground, 0.25)

                            Text {
                                anchors.centerIn: parent
                                visible: Editors.alwaysChoose
                                text: "✓"
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                color: Theme.brightForeground
                            }
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Always ask"
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                            color: Theme.foreground
                        }
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }

                    TapHandler {
                        onSingleTapped: Editors.alwaysChoose = !Editors.alwaysChoose
                    }
                }

                Item {
                    width: Math.max(0, parent.width - alwaysAsk.width
                                    - cancelButton.width - openButton.width - parent.spacing * 3)
                    height: 1
                }

                PillButton {
                    id: cancelButton
                    objectName: "editorChooserCancel"
                    anchors.verticalCenter: parent.verticalCenter
                    label: "Cancel"
                    onClicked: root.close()
                }

                PillButton {
                    id: openButton
                    objectName: "editorChooserOpen"
                    anchors.verticalCenter: parent.verticalCenter
                    label: "Open"
                    active: true
                    enabled: root.selectedId !== ""
                    onClicked: root.openSelected()
                }
            }
        }
    }

    onOpened: {
        root.customDraft = Editors.customCommand
        root.pickInitial()
        if (root.selectedId === "") {
            customInput.forceActiveFocus()
        }
    }

    Connections {
        target: Editors

        function onChooseRequested() {
            if (root.requestedHere) root.openFor(Editors.requestedPath)
        }
    }
}
