import QtQuick
import QtQuick.Controls.Basic

// Albums and tags for the checked or right-clicked files: remove from the
// current album or tag, add to an existing one, or name a new one.
Menu {
    id: menu
    // The library window, for its colors and spoken status.
    required property var host
    required property Item nameSheet
    // The grid whose checked files the actions apply to.
    required property var grid
    property var targetPaths: []
    objectName: "albumActionMenu"
    // Modal, undimmed: the press that closes the menu is consumed here
    // rather than also landing on the tile or pill under it.
    modal: true
    dim: false

    background: Rectangle {
        implicitWidth: 220
        color: host.shade(Theme.background, 0.97)
        border.width: 1
        border.color: host.shade(Theme.foreground, 0.16)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
    }

    MenuItem {
        id: removeFromAlbumRow
        text: "Remove from " + Captures.albumFilter
        Accessible.name: text
        visible: Captures.albumFilter !== ""
        height: visible ? 30 : 0
        contentItem: Text {
            text: "Remove from " + Captures.albumFilter
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.foreground
            verticalAlignment: Text.AlignVCenter
            leftPadding: 12
        }
        background: Rectangle {
            color: removeFromAlbumRow.hovered
                   ? host.shade(Theme.foreground, 0.08) : "transparent"
        }
        onTriggered: {
            const paths = menu.targetPaths
            Settings.removeFromAlbum(Captures.albumFilter, paths)
            grid.clearChecked()
            host.say("Removed " + paths.length + (paths.length === 1 ? " item" : " items"))
        }
    }

    Repeater {
        model: Settings.albumNames

        MenuItem {
            id: addAlbumRow
            text: "Add to " + addAlbumRow.modelData
            Accessible.name: text
            required property string modelData
            height: 30
            contentItem: Text {
                text: "Add to " + addAlbumRow.modelData
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.foreground
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
            }
            background: Rectangle {
                color: addAlbumRow.hovered
                       ? host.shade(Theme.foreground, 0.08) : "transparent"
            }
            onTriggered: {
                const paths = menu.targetPaths
                const changed = Settings.addToAlbum(addAlbumRow.modelData, paths)
                host.say(changed ? "Added to " + addAlbumRow.modelData
                                 : "Already in " + addAlbumRow.modelData)
            }
        }
    }

    MenuItem {
        id: newAlbumRow
        text: "+ New album"
        Accessible.name: text
        height: 30
        contentItem: Text {
            text: "+ New album"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.accent
            verticalAlignment: Text.AlignVCenter
            leftPadding: 12
        }
        background: Rectangle {
            color: newAlbumRow.hovered
                   ? host.shade(Theme.foreground, 0.08) : "transparent"
        }
        onTriggered: nameSheet.open(menu.targetPaths)
    }

    MenuSeparator {
        visible: Settings.tagNames.length > 0 || Captures.tagFilter !== ""
    }

    MenuItem {
        id: removeTagRow
        text: "Remove tag " + Captures.tagFilter
        Accessible.name: text
        visible: Captures.tagFilter !== ""
        height: visible ? 30 : 0
        contentItem: Text {
            text: "Remove tag " + Captures.tagFilter
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.foreground
            verticalAlignment: Text.AlignVCenter
            leftPadding: 12
        }
        background: Rectangle {
            color: removeTagRow.hovered
                   ? host.shade(Theme.foreground, 0.08) : "transparent"
        }
        onTriggered: {
            const paths = menu.targetPaths
            Settings.removeTag(Captures.tagFilter, paths)
            grid.clearChecked()
            host.say("Removed tag from " + paths.length
                     + (paths.length === 1 ? " item" : " items"))
        }
    }

    Repeater {
        model: Settings.tagNames

        MenuItem {
            id: addTagRow
            text: "Tag as " + addTagRow.modelData
            Accessible.name: text
            required property string modelData
            height: 30
            contentItem: Text {
                text: "Tag as " + addTagRow.modelData
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.foreground
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
            }
            background: Rectangle {
                color: addTagRow.hovered
                       ? host.shade(Theme.foreground, 0.08) : "transparent"
            }
            onTriggered: {
                const paths = menu.targetPaths
                const changed = Settings.addTag(addTagRow.modelData, paths)
                host.say(changed ? "Tagged as " + addTagRow.modelData
                                 : "Already tagged as " + addTagRow.modelData)
            }
        }
    }

    MenuItem {
        id: newTagRow
        text: "+ New tag"
        Accessible.name: text
        height: 30
        contentItem: Text {
            text: "+ New tag"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.accent
            verticalAlignment: Text.AlignVCenter
            leftPadding: 12
        }
        background: Rectangle {
            color: newTagRow.hovered
                   ? host.shade(Theme.foreground, 0.08) : "transparent"
        }
        onTriggered: nameSheet.open(menu.targetPaths, "tag")
    }

}
