import QtQuick
import QtQuick.Controls.Basic

// One row of the viewer's menu: an action with its key, a missing tool with
// the package it needs, a separator, or the way into a submenu.
MenuItem {
    id: entry

    // {id, label, available, hint, checked} or {separator: true}. A submenu's
    // row has none of it and takes its name from the submenu. A row with
    // |checked| is a choice, ticked when it is the one in use.
    property var row: ({})
    readonly property bool separator: row.separator === true
    readonly property bool needsTool: row.available === false && !!row.hint
    readonly property string shortcutText: separator || !row.id
                                           ? "" : (ShortcutCatalog.quickHints[row.id] || "")

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    objectName: separator ? "" : subMenu ? "viewerMenu_group-" + (subMenu.groupId || "")
                                         : "viewerMenu_" + row.id
    // The label is drawn below; this is what a screen reader says.
    // Separators are not items at all to it.
    text: separator ? "" : subMenu ? subMenu.title : (row.label || "")
    Accessible.name: separator ? "" : text + (needsTool ? ", needs " + row.hint : "")
    Accessible.description: entry.shortcutText !== "" ? "Shortcut " + entry.shortcutText : ""
    // Controls marks every MenuItem accessible again when it sets the role,
    // which undoes ignored; a separator role is what screen readers skip.
    Accessible.role: separator ? Accessible.Separator : Accessible.MenuItem
    Accessible.ignored: separator
    enabled: !separator && row.available !== false
    // Drawn from the row rather than checkable, which would untick a choice
    // picked again.
    readonly property bool choice: row.checked !== undefined
    readonly property bool chosen: row.checked === true
    Accessible.checkable: choice
    Accessible.checked: chosen
    height: separator ? 9 : needsTool ? 46 : 30
    opacity: enabled || separator ? 1 : 0.55
    arrow: null
    indicator: null

    contentItem: Item {
        Rectangle {
            visible: entry.separator
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 1
            color: entry.shade(Theme.foreground, 0.12)
        }
        Icon {
            id: tick
            objectName: entry.objectName + "Tick"
            visible: entry.chosen
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            name: "check"
            size: 14
            color: entry.highlighted ? Theme.brightForeground : Theme.accent
        }
        Text {
            visible: !entry.separator
            anchors.left: parent.left
            anchors.leftMargin: entry.choice ? 28 : 12
            anchors.right: shortcutLabel.visible ? shortcutLabel.left
                           : submenuArrow.visible ? submenuArrow.left : parent.right
            anchors.rightMargin: shortcutLabel.visible || submenuArrow.visible ? 8 : 12
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: entry.needsTool ? -7 : 0
            text: entry.text
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: entry.row.id === "trash" ? Theme.red
                   : entry.highlighted ? Theme.brightForeground : Theme.foreground
        }
        Text {
            visible: !entry.separator && entry.needsTool
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 5
            text: "Needs " + (entry.row.hint || "")
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.mutedText
        }
        Text {
            id: shortcutLabel
            visible: !entry.separator && entry.shortcutText !== ""
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: entry.shortcutText
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.mutedText
        }
        Icon {
            id: submenuArrow
            visible: !!entry.subMenu
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            name: "chevron-right"
            size: 14
            color: entry.highlighted ? Theme.brightForeground : Theme.mutedText
        }
    }
    background: Rectangle {
        color: entry.highlighted && !entry.separator ? entry.shade(Theme.foreground, 0.09) : "transparent"
        radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 3) : 0
    }
}
