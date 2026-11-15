import QtQuick
import QtQuick.Controls.Basic

// Jump to date: the years and months the library shows, each with its count.
// Enter scrolls the grid to it and Shift+Enter shows only that month or year;
// a click scrolls and a Shift+click filters.
Popup {
    id: menu
    // The library window, for its size and spoken status.
    required property var host
    objectName: "dateJumpMenu"
    signal jumpRequested(string key)
    signal filterRequested(string from, string to, string label)

    modal: true
    dim: false
    focus: true
    padding: 6
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    parent: host.contentItem
    width: Math.min(260, host.width - 16)
    height: Math.min(list.contentHeight + topPadding + bottomPadding, host.height - 80)
    margins: 8

    property var rows: []
    // The control that had the keyboard when the list opened. Escape or a
    // click outside gives it back; choosing a row hands it to the grid.
    property Item opener: null
    function takeOpener() {
        const item = menu.opener
        menu.opener = null
        return item
    }
    function shade(base, amount) { return Qt.rgba(base.r, base.g, base.b, amount) }
    function items(count) { return count === 1 ? "1 item" : count + " items" }
    function pad(value) { return String(value).padStart(2, "0") }

    function buildRows() {
        const sections = Captures.sectionRows
        if (sections.rebuildPending()) sections.rebuildNow()
        const out = []
        // A date sort keeps each year's months together.
        let year = null
        for (const month of sections.months()) {
            if (!year || year.year !== month.year) {
                year = {kind: "year", year: month.year, key: month.key, label: String(month.year),
                        count: 0, from: month.year + "-01-01", to: month.year + "-12-31"}
                out.push(year)
            }
            year.count += month.count
            const parts = String(month.key).split("-")
            const last = new Date(Number(parts[0]), Number(parts[1]), 0).getDate()
            out.push({kind: "month", year: month.year, key: month.key, label: month.label,
                      count: month.count, from: month.key + "-01", to: month.key + "-" + menu.pad(last)})
        }
        return out
    }

    // The view's months changed while the list is open: show them as they
    // are now, keeping the highlighted row if it is still there, so a choice
    // never names a month the view no longer has.
    function refreshRows() {
        const next = menu.buildRows()
        if (JSON.stringify(next) === JSON.stringify(menu.rows)) return
        const was = menu.rows[list.currentIndex]
        menu.rows = next
        if (next.length === 0) {
            menu.close()
            return
        }
        let index = -1
        for (let i = 0; was && i < next.length; ++i) {
            if (next[i].kind === was.kind && next[i].key === was.key) { index = i; break }
        }
        list.currentIndex = index >= 0 ? index : 0
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
    }
    Connections {
        target: Captures.sectionRows
        enabled: menu.visible
        function onRebuilt() { menu.refreshRows() }
    }

    // Opens below |item|, on the month at the top of the grid now.
    function popupBelow(item, currentKey) {
        menu.opener = item && item.activeFocus ? item : null
        menu.rows = menu.buildRows()
        if (menu.rows.length === 0) {
            menu.opener = null
            host.say("No dated files in this view")
            return
        }
        const point = item.mapToItem(host.contentItem, 0, item.height + 4)
        menu.x = Math.max(8, Math.min(point.x + item.width - menu.width, host.contentItem.width - menu.width - 8))
        menu.y = point.y
        menu.open()
        let index = 0
        for (let i = 0; i < menu.rows.length; ++i) {
            if (menu.rows[i].kind === "month" && menu.rows[i].key === currentKey) { index = i; break }
        }
        list.currentIndex = index
        list.positionViewAtIndex(index, ListView.Center)
        list.forceActiveFocus()
    }

    function choose(index, filter) {
        const row = menu.rows[index]
        if (!row) return
        menu.opener = null
        menu.close()
        if (filter) menu.filterRequested(row.from, row.to, row.label)
        else menu.jumpRequested(row.key)
    }

    // Opaque whatever the theme's alpha, and drawn in the window.
    popupType: Popup.Item
    background: Rectangle {
        objectName: "dateJumpBackground"
        color: Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 1)
        border.width: 1
        border.color: menu.shade(Theme.foreground, 0.16)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
    }

    contentItem: ListView {
        id: list
        objectName: "dateJumpList"
        clip: true
        model: menu.rows
        implicitHeight: contentHeight
        boundsBehavior: Flickable.StopAtBounds
        highlightMoveDuration: 0
        keyNavigationEnabled: false
        Accessible.role: Accessible.List
        Accessible.name: "Jump to date"
        Accessible.description: "Enter scrolls to it, Shift+Enter shows only it"
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Keys.onPressed: function (event) {
            if (event.key === Qt.Key_Down) {
                currentIndex = Math.min(count - 1, currentIndex + 1)
            } else if (event.key === Qt.Key_Up) {
                currentIndex = Math.max(0, currentIndex - 1)
            } else if (event.key === Qt.Key_Home) {
                currentIndex = 0
            } else if (event.key === Qt.Key_End) {
                currentIndex = count - 1
            } else if (event.key === Qt.Key_PageDown) {
                currentIndex = Math.min(count - 1, currentIndex + 8)
            } else if (event.key === Qt.Key_PageUp) {
                currentIndex = Math.max(0, currentIndex - 8)
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                menu.choose(currentIndex, (event.modifiers & Qt.ShiftModifier) !== 0)
            } else {
                return
            }
            positionViewAtIndex(currentIndex, ListView.Contain)
            event.accepted = true
        }

        delegate: Rectangle {
            id: rowItem
            required property int index
            required property var modelData
            readonly property bool year: modelData.kind === "year"
            objectName: "dateJumpRow_" + modelData.key + (year ? "_year" : "")
            width: list.width
            height: year ? 30 : 28
            radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 4) : 0
            color: ListView.isCurrentItem ? menu.shade(Theme.foreground, 0.09)
                   : mouse.containsMouse ? menu.shade(Theme.foreground, 0.05) : "transparent"
            border.width: ListView.isCurrentItem && list.activeFocus ? 1 : 0
            border.color: Theme.accent
            Accessible.role: Accessible.ListItem
            Accessible.name: modelData.label + ", " + menu.items(modelData.count)
            Accessible.selected: ListView.isCurrentItem
            Accessible.onPressAction: menu.choose(index, false)

            Text {
                anchors.left: parent.left
                anchors.leftMargin: rowItem.year ? 10 : 24
                anchors.right: countText.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: rowItem.modelData.label
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: rowItem.year ? Font.DemiBold : Font.Normal
                color: rowItem.year ? Theme.brightForeground : Theme.foreground
            }
            Text {
                id: countText
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: Number(rowItem.modelData.count).toLocaleString(Qt.locale(), "f", 0)
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }
            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: function (event) {
                    menu.choose(rowItem.index, (event.modifiers & Qt.ShiftModifier) !== 0)
                }
            }
        }
    }
}
