import QtQuick
import QtQuick.Controls.Basic

// The viewer's details, on request: what the file is, when it was made, how
// it was rated, and what the camera or encoder recorded. Hidden until asked
// for, so the picture keeps the whole window.
Rectangle {
    id: root

    property string fileName: ""
    property string folder: ""
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

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    implicitWidth: 300
    implicitHeight: Math.min(content.implicitHeight + 32, parent ? parent.height - 32 : 600)
    radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
    color: root.shade(Theme.background, 0.92)
    border.width: 1
    border.color: root.shade(Theme.foreground, 0.12)

    // Clicks and the wheel stay on the card rather than reaching the picture.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: function (wheel) { wheel.accepted = true }
    }

    Flickable {
        anchors.fill: parent
        anchors.margins: 16
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
                color: Theme.brightForeground
            }

            Text {
                width: parent.width
                visible: text !== ""
                text: [root.technical, root.sizeLabel].filter(function (part) {
                    return part !== ""
                }).join("  ·  ")
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }

            Text {
                width: parent.width
                visible: root.dateLabel !== ""
                text: root.dateLabel
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
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
                            color: index < root.rating ? Theme.yellow : Theme.mutedText
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
                color: Theme.mutedText
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
                    color: Theme.foreground
                }
            }

            Item { width: 1; height: 4 }

            // Where the file lives. A click shows it in the file manager.
            Text {
                objectName: "viewerInfoFolder"
                width: parent.width
                text: root.folder
                elide: Text.ElideMiddle
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.underline: folderMouse.containsMouse
                color: folderMouse.containsMouse ? Theme.accent : Theme.mutedText

                MouseArea {
                    id: folderMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.folderRequested()
                }
                ToolTip {
                    visible: folderMouse.containsMouse
                    text: "Show in files"
                    delay: 500
                }
            }
        }
    }
}
