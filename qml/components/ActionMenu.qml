import QtQuick
import QtQuick.Controls.Basic

Menu {
    id: root
    property var entries: []
    function grouped(rows) {
        const out = []; let group = ""
        const groups = ["File", "Edit and finish", "Tools and sharing", "Organize"]
        const ordered = rows.map(function(row, index) { return {row: row, index: index} })
            .sort(function(a, b) {
                const groupOrder = groups.indexOf(a.row.group) - groups.indexOf(b.row.group)
                return groupOrder || a.index - b.index
            }).map(function(entry) { return entry.row })
        for (const row of ordered) {
            if (group !== "" && row.group !== group) out.push({separator: true})
            out.push(row); group = row.group
        }
        return out
    }
    property string entryPrefix: "contextAction_"
    signal triggered(string actionId)
    // Modal, undimmed: the press that closes the menu is consumed here
    // rather than also landing on the picture under it.
    modal: true
    dim: false
    function shade(base, alpha) { return Qt.rgba(base.r, base.g, base.b, alpha) }

    background: Rectangle {
        implicitWidth: 232
        color: root.shade(Theme.background, 0.97)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.16)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
    }

    Repeater {
        model: root.entries

        MenuItem {
            id: entry
            required property var modelData
            readonly property bool separator: modelData.separator === true
            objectName: separator ? "" : root.entryPrefix + modelData.id
            enabled: !separator && modelData.available !== false
            height: separator ? 9 : 30
            opacity: enabled || separator ? 1 : 0.55
            contentItem: Item {
                Rectangle {
                    visible: entry.separator
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 1
                    color: root.shade(Theme.foreground, 0.12)
                }
                Text {
                    visible: !entry.separator
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: shortcutText.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.separator ? "" : entry.modelData.label + (entry.modelData.available === false && entry.modelData.hint ? " · needs " + entry.modelData.hint : "")
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: entry.modelData.id === "trash" ? Theme.red
                           : entry.highlighted ? Theme.brightForeground : Theme.foreground
                }
                Text {
                    id: shortcutText
                    visible: !entry.separator
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.separator ? ""
                          : (entry.modelData.shortcut || "")
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    color: Theme.mutedText
                }
            }
            background: Rectangle {
                color: entry.highlighted && !entry.separator
                       ? root.shade(Theme.foreground, 0.09) : "transparent"
                radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 3) : 0
            }
            onTriggered: root.triggered(entry.modelData.id)
        }
    }
}

