import QtQuick
import QtQuick.Controls.Basic

// The viewer's details, on request: what the file is, when it was made, how
// it was rated, and what the camera or encoder recorded. Hidden until asked
// for, so the picture keeps the whole window. It sits over the picture, so it
// is dark in every theme like the toolbar and scrims (see Osd).
Rectangle {
    id: root

    property string fileName: ""
    property string folder: ""
    property string filePath: ""
    property string resolvedPath: ""
    property string technical: ""
    property string sizeLabel: ""
    property string dateLabel: ""
    property int rating: 0
    property bool favorite: false
    property var lines: []
    property bool loading: false

    signal rateRequested(int stars)
    signal favoriteToggled()
    signal folderRequested()
    signal copyPathRequested()
    signal copyNameRequested()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    implicitWidth: 300
    implicitHeight: Math.min(content.implicitHeight + copyActions.implicitHeight + 40,
                             parent ? parent.height - 32 : 600)
    radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
    color: Osd.panelSolid
    border.width: 1
    border.color: Osd.border

    // Clicks and the wheel stay on the card rather than reaching the picture.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: function (wheel) { wheel.accepted = true }
    }

    Flickable {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: copyActions.top
        anchors.margins: 16
        anchors.bottomMargin: 8
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content
            width: parent.width
            spacing: 6

            Text {
                objectName: "viewerInfoName"
                width: parent.width
                text: root.fileName
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 3
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: Osd.brightForeground
            }

            Text {
                width: parent.width
                visible: text !== ""
                text: [root.technical, root.sizeLabel].filter(function (part) {
                    return part !== ""
                }).join("  ·  ")
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Osd.mutedText
            }

            Text {
                width: parent.width
                visible: root.dateLabel !== ""
                text: root.dateLabel
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Osd.mutedText
            }

            Item { width: 1; height: 4 }

            // Rating and favourite. Clicking the lit star clears the rating.
            Row {
                spacing: 10

                Row {
                    objectName: "viewerInfoRating"
                    spacing: 1
                    anchors.verticalCenter: parent.verticalCenter

                    Repeater {
                        model: 5

                        Icon {
                            required property int index
                            name: index < root.rating ? "star-filled" : "star"
                            size: 18
                            color: index < root.rating ? Theme.yellow : Osd.mutedText
                            Accessible.role: Accessible.Button
                            Accessible.name: (index + 1) + (index === 0 ? " star" : " stars")

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.rateRequested(parent.index + 1 === root.rating
                                                              ? 0 : parent.index + 1)
                            }
                        }
                    }
                }

                PillButton {
                    objectName: "viewerInfoFavourite"
                    anchors.verticalCenter: parent.verticalCenter
                    osd: true
                    label: root.favorite ? "Favourite" : "Add to favourites"
                    active: root.favorite
                    shortcut: "V"
                    onClicked: root.favoriteToggled()
                }
            }

            Item { width: 1; height: 4 }

            Text {
                width: parent.width
                visible: root.loading
                text: "Reading details…"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Osd.mutedText
            }

            Repeater {
                model: root.lines
                Text {
                    required property string modelData
                    width: content.width
                    text: modelData
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Osd.foreground
                }
            }

            Item { width: 1; height: 4 }

            // Where the file lives, once: the name is the title above, so
            // this is its folder, shortened in the middle so both ends stay.
            // The whole path is in the tip and Copy path below; a click shows
            // the file in the file manager.
            Text {
                objectName: "viewerInfoFolder"
                width: parent.width
                text: "In " + root.folder
                elide: Text.ElideMiddle
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.underline: folderMouse.containsMouse || activeFocus
                color: folderMouse.containsMouse || activeFocus ? Osd.accent : Osd.mutedText
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: "Open folder " + root.folder
                Accessible.description: root.filePath
                Accessible.onPressAction: root.folderRequested()
                Keys.onReturnPressed: root.folderRequested()
                Keys.onEnterPressed: root.folderRequested()
                Keys.onSpacePressed: root.folderRequested()

                Rectangle {
                    objectName: "viewerInfoFolderFocus"
                    anchors.fill: parent
                    anchors.margins: -2
                    visible: parent.activeFocus
                    color: "transparent"
                    radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 3) : 2
                    border.width: 1
                    border.color: Osd.accent
                }

                MouseArea {
                    id: folderMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.folderRequested()
                }
                ThemedToolTip {
                    requested: folderMouse.containsMouse
                    keyboardRequested: parent.activeFocus && parent.Window.window
                                       && parent.Window.window.keyboardNavigation === true
                    text: root.filePath + "\nOpen containing folder"
                    delay: 500
                }
            }
            Text {
                objectName: "viewerInfoTarget"
                width: parent.width
                visible: root.resolvedPath !== ""
                text: "Target: " + root.resolvedPath
                elide: Text.ElideMiddle
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Osd.mutedText
                Accessible.role: Accessible.StaticText
                Accessible.name: "Target: " + root.resolvedPath

                HoverHandler { id: targetHover }
                ThemedToolTip {
                    requested: targetHover.hovered && parent.truncated
                    text: root.resolvedPath
                    delay: 500
                }
            }
        }
    }

    // Keep copy actions reachable while long paths and metadata scroll above.
    Flow {
        id: copyActions
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        spacing: 6

        PillButton {
            objectName: "viewerInfoCopyPath"
            osd: true
            label: "Copy path"
            onClicked: root.copyPathRequested()
        }
        PillButton {
            objectName: "viewerInfoCopyName"
            osd: true
            label: "Copy file name"
            onClicked: root.copyNameRequested()
        }
    }
}
