import QtQuick
import QtQuick.Controls.Basic

// The files beside this one, as a strip of thumbnails above the controls, so
// the rest of the folder can be seen and reached without stepping through it.
// The open file stays in the middle; a click opens another. With keyboard
// focus, Left and Right move a ring along the strip and Enter opens the file
// under it.
Rectangle {
    id: root

    property int count: 0
    property int currentIndex: -1
    property real devicePixelRatio: 1
    readonly property bool hovered: hover.hovered
    readonly property int tileWidth: 72
    readonly property int tileHeight: 48
    readonly property int spacing: 4

    signal chosen(int index)

    // The tile under the keyboard ring, while the strip has focus.
    property int focusIndex: -1
    readonly property bool keyboardFocused: list.activeFocus

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    implicitWidth: root.count * (root.tileWidth + root.spacing) - root.spacing + 12
    implicitHeight: root.tileHeight + 12
    radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 10) : 4
    color: root.shade(Theme.background, 0.84)
    border.width: 1
    border.color: root.shade(Theme.foreground, 0.12)

    HoverHandler { id: hover }

    // A press between the tiles stays on the strip.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
    }

    ListView {
        id: list
        objectName: "viewerFilmstripList"
        anchors.fill: parent
        anchors.margins: 6
        orientation: ListView.Horizontal
        spacing: root.spacing
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: root.visible ? root.count : 0
        currentIndex: root.currentIndex
        // Keeps the open file centred, gliding rather than jumping as the
        // arrows move through the folder.
        highlightRangeMode: ListView.ApplyRange
        preferredHighlightBegin: (width - root.tileWidth) / 2
        preferredHighlightEnd: (width + root.tileWidth) / 2
        highlightMoveDuration: 160
        // A long folder builds only the tiles in view, plus a few either side.
        cacheBuffer: root.tileWidth * 4
        activeFocusOnTab: true
        keyNavigationEnabled: false
        Accessible.role: Accessible.List
        Accessible.name: "Files in this folder"
        onActiveFocusChanged: {
            if (activeFocus) {
                root.focusIndex = root.currentIndex
            } else {
                root.focusIndex = -1
            }
        }
        function moveRing(index) {
            root.focusIndex = Math.max(0, Math.min(root.count - 1, index))
            list.positionViewAtIndex(root.focusIndex, ListView.Contain)
        }
        Keys.onPressed: function (event) {
            if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier)) return
            switch (event.key) {
            case Qt.Key_Left:
            case Qt.Key_Right:
                list.moveRing(root.focusIndex + (event.key === Qt.Key_Left ? -1 : 1))
                break
            case Qt.Key_Home:
            case Qt.Key_End:
                list.moveRing(event.key === Qt.Key_Home ? 0 : root.count - 1)
                break
            case Qt.Key_Return:
            case Qt.Key_Enter:
            case Qt.Key_Space:
                if (root.focusIndex >= 0) root.chosen(root.focusIndex)
                break
            default:
                return
            }
            event.accepted = true
        }

        delegate: Item {
            id: tile
            required property int index
            readonly property bool current: tile.index === root.currentIndex
            readonly property bool ringed: tile.index === root.focusIndex && root.keyboardFocused
            readonly property string fileName: {
                const revision = Session.sequenceRevision;
                return Session.fileNameAt(tile.index);
            }
            readonly property bool video: {
                const revision = Session.sequenceRevision;
                return Session.isVideoAt(tile.index);
            }
            readonly property bool broken: picture.status === Image.Error
            objectName: "viewerFilmstripTile" + tile.index
            width: root.tileWidth
            height: root.tileHeight
            Accessible.role: Accessible.ListItem
            Accessible.name: tile.fileName + (tile.video ? ", video" : "")
                             + (tile.broken ? ", cannot be shown" : "")
            Accessible.selectable: true
            Accessible.selected: tile.current
            Accessible.focusable: true
            Accessible.focused: tile.ringed
            Accessible.onPressAction: root.chosen(tile.index)

            Rectangle {
                anchors.fill: parent
                radius: 3
                color: root.shade(Theme.foreground, 0.08)
            }

            Image {
                id: picture
                objectName: "viewerFilmstripPicture" + tile.index
                anchors.fill: parent
                anchors.margins: 2
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                smooth: true
                // The provider applies the ratio from the URL once.
                sourceSize: Qt.size(Math.round(width), Math.round(height))
                source: {
                    const revision = Session.sequenceRevision;
                    return Session.thumbnailUrl(tile.index, root.devicePixelRatio);
                }
                opacity: tile.current || tile.ringed || tap.hovered ? 1 : 0.62
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }

            // A file that cannot be shown is marked rather than left blank.
            Column {
                objectName: "viewerFilmstripBroken" + tile.index
                anchors.centerIn: parent
                visible: tile.broken
                spacing: 1
                Icon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    name: "warning"
                    size: 16
                    color: Theme.red
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Can't show"
                    font.family: Theme.fontFamily
                    font.pixelSize: 9
                    color: Theme.mutedText
                }
            }

            Text {
                anchors.centerIn: parent
                visible: tile.video && !tile.broken
                text: "▶"
                font.pixelSize: 13
                color: "#ffffff"
                style: Text.Outline
                styleColor: Qt.rgba(0, 0, 0, 0.6)
            }

            Rectangle {
                id: rawBadge
                objectName: "viewerFilmstripRawBadge" + tile.index
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 4
                readonly property string label: {
                    const revision = Session.sequenceRevision;
                    return Session.companionPathAt(tile.index).length > 0
                        ? "RAW+JPG" : Session.rawFormatAt(tile.index);
                }
                visible: label.length > 0
                width: rawBadgeText.implicitWidth + 6
                height: rawBadgeText.implicitHeight + 2
                radius: 2
                color: Qt.rgba(0, 0, 0, 0.78)

                Text {
                    id: rawBadgeText
                    anchors.centerIn: parent
                    text: rawBadge.label
                    font.pixelSize: 9
                    font.bold: true
                    color: "#ffffff"
                }
            }

            Rectangle {
                anchors.fill: parent
                radius: 3
                color: "transparent"
                border.width: 2
                border.color: Theme.accent
                visible: tile.current
            }
            // The keyboard ring, outside the current file's border.
            Rectangle {
                anchors.fill: parent
                anchors.margins: -2
                radius: 4
                color: "transparent"
                border.width: 2
                border.color: Theme.brightForeground
                visible: tile.ringed
            }

            ThemedToolTip {
                requested: tap.hovered
                keyboardRequested: tile.ringed
                text: tile.fileName
                delay: tile.ringed ? 0 : 500
            }

            HoverHandler {
                id: tap
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: root.chosen(tile.index)
            }
        }
    }
}
