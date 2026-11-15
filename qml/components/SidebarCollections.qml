import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic

// Albums, tags and dates above the folder tree, as one recycling list of
// flattened rows: a framed header per section, then its albums with a cover,
// its tags as an indented tree, or its years, each opening to its months.
//
// A row narrows the library the way Browse does, so it combines with the
// folder, search and other facets; the active row is highlighted, and
// choosing it again clears that facet. Which sections are open is saved.
FocusScope {
    id: root

    // The tallest the list may grow; the folder tree keeps the rest.
    property real maximumHeight: 0
    readonly property bool menuOpen: rowMenu.visible
    readonly property alias listView: list
    readonly property alias currentIndex: list.currentIndex
    // Years opened to their months, by year. Not saved: a year shows its
    // months only while it is being looked through.
    property var openYears: ({})
    // Bumped when albums, tags or the library change, so counts and covers
    // are read again.
    property int revision: 0
    signal renameRequested(string mode, string name)
    signal deleteRequested(string mode, string name)
    // Up from the first row and Down from the last leave the list.
    signal exitUp()
    signal exitDown()

    implicitHeight: Math.min(list.contentHeight, maximumHeight)
    visible: rows.length > 0

    function shade(base, amount) { return Qt.rgba(base.r, base.g, base.b, amount) }
    function items(count) { return count === 1 ? "1 item" : count + " items" }
    function isOpen(section) {
        return Settings.sidebarSections.indexOf(section) >= 0
    }
    function setOpen(section, open) {
        const sections = Settings.sidebarSections.filter(function (name) { return name !== section })
        if (open) sections.push(section)
        Settings.sidebarSections = sections
    }
    function setYearOpen(year, open) {
        const years = Object.assign({}, root.openYears)
        if (open) years[year] = true
        else delete years[year]
        root.openYears = years
    }
    function isoDay(year, month, day) {
        return year + "-" + String(month).padStart(2, "0") + "-" + String(day).padStart(2, "0")
    }

    Connections {
        target: Settings
        function onAlbumsChanged() { root.revision++ }
        function onTagsChanged() { root.revision++ }
    }
    Connections {
        target: Captures
        function onCountChanged() { root.revision++ }
    }

    // Years from Captures.dateBuckets, newest first, each with its months.
    readonly property var years: {
        const byYear = []
        for (const month of Captures.dateBuckets) {
            const year = Number(String(month.key).slice(0, 4))
            if (byYear.length === 0 || byYear[byYear.length - 1].year !== year)
                byYear.push({year: year, count: 0, months: []})
            const entry = byYear[byYear.length - 1]
            entry.count += month.count
            entry.months.push(month)
        }
        return byYear
    }

    readonly property var rows: {
        void root.revision
        const out = []
        const albums = Settings.albumNames
        if (albums.length > 0) {
            const open = root.isOpen("albums")
            out.push({kind: "header", section: "albums", name: "albums", label: "Albums",
                      count: albums.length, open: open, depth: 0})
            if (open) for (const name of albums)
                out.push({kind: "album", name: name, label: name, depth: 0})
        }
        const tags = Settings.tagOutline()
        if (tags.length > 0) {
            const open = root.isOpen("tags")
            out.push({kind: "header", section: "tags", name: "tags", label: "Tags",
                      count: tags.length, open: open, depth: 0})
            if (open) for (const tag of tags) {
                const parts = String(tag.name).split("/")
                out.push({kind: "tag", name: tag.name, label: parts[parts.length - 1],
                          depth: parts.length - 1, fromFile: tag.fromFile === true})
            }
        }
        if (root.years.length > 0) {
            const open = root.isOpen("dates")
            out.push({kind: "header", section: "dates", name: "dates", label: "Dates",
                      count: root.years.length, open: open, depth: 0})
            if (open) for (const year of root.years) {
                const yearOpen = root.openYears[year.year] === true
                out.push({kind: "year", name: String(year.year), label: String(year.year),
                          count: year.count, open: yearOpen, depth: 0,
                          from: root.isoDay(year.year, 1, 1), to: root.isoDay(year.year, 12, 31)})
                if (yearOpen) for (const month of year.months) {
                    out.push({kind: "month", name: month.key, label: month.label,
                              shortLabel: Qt.locale().monthName(Number(String(month.key).slice(5, 7)) - 1,
                                                                Locale.LongFormat),
                              count: month.count, depth: 1, from: month.from, to: month.to})
                }
            }
        }
        return out
    }

    // The list keeps its place by row identity across rebuilds. The model
    // is handed over here rather than bound, so the reset it causes cannot
    // move the current row before it is put back.
    property string currentKey: ""
    property bool restoring: false
    function keyOf(row) { return row ? row.kind + ":" + row.name : "" }
    function applyRows() {
        const key = root.currentKey
        const before = list.currentIndex
        root.restoring = true
        list.model = root.rows
        let index = -1
        for (let i = 0; i < rows.length && key !== ""; ++i) {
            if (keyOf(rows[i]) === key) { index = i; break }
        }
        list.currentIndex = index >= 0 ? index : Math.min(before, rows.length - 1)
        root.restoring = false
        root.currentKey = root.keyOf(root.rows[list.currentIndex])
    }
    onRowsChanged: applyRows()
    Component.onCompleted: applyRows()

    function isActive(row) {
        if (!row) return false
        switch (row.kind) {
        case "album": return Captures.albumFilter === row.name
        case "tag": return Captures.tagFilter === row.name
        case "year":
        case "month":
            return Captures.modifiedAfter === "" && Captures.dateField === 0
                && Captures.dateFrom === row.from && Captures.dateTo === row.to
        }
        return false
    }

    // The same calls as Browse. Choosing the active row again clears it.
    function activate(index) {
        const row = rows[index]
        if (!row) return
        list.currentIndex = index
        const active = root.isActive(row)
        switch (row.kind) {
        case "header":
            root.setOpen(row.section, !row.open)
            break
        case "album":
            if (active) Captures.setAlbumFilter("", [])
            else Captures.setAlbumFilter(row.name, Settings.albumPaths(row.name))
            break
        case "tag":
            if (active) Captures.setTagFilter("", [])
            else Captures.setTagFilter(row.name, Settings.tagPaths(row.name))
            break
        case "year":
        case "month":
            if (active) Captures.clearDateRange()
            else Captures.setDateRange(row.from, row.to, 0)
            break
        }
    }

    function focusFirst() {
        list.currentIndex = 0
        list.positionViewAtIndex(0, ListView.Contain)
        list.forceActiveFocus()
    }
    function focusLast() {
        list.currentIndex = rows.length - 1
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        list.forceActiveFocus()
    }
    function moveTo(index) {
        if (index < 0 || index >= rows.length) return
        list.currentIndex = index
        list.positionViewAtIndex(index, ListView.Contain)
    }
    // The row a child sits under: a tag's parent tag, a month's year, or
    // the section header.
    function parentIndex(index) {
        const row = rows[index]
        if (!row || row.kind === "header") return -1
        for (let i = index - 1; i >= 0; --i) {
            const above = rows[i]
            if (above.kind === "header") return i
            if (row.kind === "month" && above.kind === "year") return i
            if (row.kind === "tag" && above.kind === "tag" && above.depth < row.depth) return i
        }
        return -1
    }
    function openMenu(index, item, x, y) {
        const row = rows[index]
        if (!row || (row.kind !== "album" && row.kind !== "tag")) return
        list.currentIndex = index
        rowMenu.kind = row.kind
        rowMenu.name = row.name
        rowMenu.popup(item, x, y)
    }

    ListView {
        id: list
        objectName: "sidebarCollections"
        anchors.fill: parent
        clip: true
        focus: true
        activeFocusOnTab: true
        boundsBehavior: Flickable.StopAtBounds
        highlightMoveDuration: 0
        currentIndex: -1
        keyNavigationEnabled: false
        reuseItems: true
        Accessible.role: Accessible.List
        Accessible.name: "Collections"
        onCurrentIndexChanged: if (!root.restoring) root.currentKey = root.keyOf(root.rows[currentIndex])
        onActiveFocusChanged: if (activeFocus && currentIndex < 0 && count > 0) currentIndex = 0
        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
            contentItem: Rectangle {
                visible: parent.size < 1
                implicitWidth: 4
                radius: width / 2
                color: root.shade(Theme.foreground, 0.32)
            }
            background: Rectangle { color: "transparent" }
        }

        Keys.onPressed: function (event) {
            const index = currentIndex < 0 ? 0 : currentIndex
            const row = root.rows[index]
            if (!row) return
            if (event.key === Qt.Key_Down) {
                if (index + 1 < root.rows.length) root.moveTo(index + 1)
                else root.exitDown()
            } else if (event.key === Qt.Key_Up) {
                if (index > 0) root.moveTo(index - 1)
                else root.exitUp()
            } else if (event.key === Qt.Key_Home) {
                root.moveTo(0)
            } else if (event.key === Qt.Key_End) {
                root.moveTo(root.rows.length - 1)
            } else if (event.key === Qt.Key_Right) {
                if (row.kind === "header" && !row.open) root.setOpen(row.section, true)
                else if (row.kind === "year" && !row.open) root.setYearOpen(row.name, true)
                else if ((row.kind === "header" || row.kind === "year")
                         && index + 1 < root.rows.length && root.parentIndex(index + 1) === index)
                    root.moveTo(index + 1)
            } else if (event.key === Qt.Key_Left) {
                if (row.kind === "header" && row.open) root.setOpen(row.section, false)
                else if (row.kind === "year" && row.open) root.setYearOpen(row.name, false)
                else root.moveTo(root.parentIndex(index))
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                       || event.key === Qt.Key_Space) {
                root.activate(index)
            } else if (event.key === Qt.Key_Menu
                       || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                const item = itemAtIndex(index)
                if (item) root.openMenu(index, item, 8, item.height)
            } else {
                return
            }
            event.accepted = true
        }

        delegate: Item {
            id: rowItem
            required property int index
            required property var modelData
            readonly property string kind: modelData.kind
            readonly property bool current: ListView.isCurrentItem && list.activeFocus
            readonly property bool active: { void Captures.albumFilter; void Captures.tagFilter
                                             void Captures.dateFrom; void Captures.dateTo
                                             return root.isActive(modelData) }
            width: list.width
            height: kind === "header" ? 36 : kind === "album" ? 42 : 32
            // Albums read their paths once per rebuild, for the count and cover.
            readonly property var albumPaths: {
                void root.revision
                return kind === "album" ? Settings.albumPaths(modelData.name) : []
            }
            readonly property int count: {
                void root.revision
                return kind === "album" ? albumPaths.length
                     : kind === "tag" ? Settings.tagItemCount(modelData.name)
                     : modelData.count !== undefined ? modelData.count : -1
            }

            SidebarHeader {
                visible: rowItem.kind === "header"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 2
                height: 28
                label: rowItem.modelData.label
                count: rowItem.kind === "header" ? rowItem.modelData.count : -1
                expanded: rowItem.modelData.open === true
                keyboardCurrent: rowItem.current
                Accessible.ignored: !visible
                onToggled: {
                    list.forceActiveFocus()
                    root.activate(rowItem.index)
                }
            }

            Rectangle {
                id: entry
                objectName: "collectionRow"
                visible: rowItem.kind !== "header"
                anchors.fill: parent
                anchors.topMargin: 1
                anchors.bottomMargin: 1
                radius: Math.min(6, Theme.cornerRadius)
                color: rowItem.active ? root.shade(Theme.accent, 0.18)
                       : mouse.containsMouse ? root.shade(Theme.foreground, 0.07) : "transparent"
                border.width: rowItem.current ? 1 : 0
                border.color: Theme.accent
                Accessible.ignored: !visible
                Accessible.role: Accessible.ListItem
                Accessible.name: {
                    const row = rowItem.modelData
                    const counted = rowItem.count >= 0 ? ", " + root.items(rowItem.count) : ""
                    switch (rowItem.kind) {
                    case "album": return "Album " + row.name + counted
                    case "tag": return "Tag " + row.name + counted
                    default: return row.label + counted
                    }
                }
                Accessible.description: rowItem.kind === "tag" && rowItem.modelData.fromFile
                                        ? "From the files' keywords" : ""
                Accessible.selectable: true
                Accessible.selected: rowItem.active
                Accessible.onPressAction: root.activate(rowItem.index)

                readonly property real indent: Math.min(rowItem.modelData.depth * 14, width / 2)

                Text {
                    id: chevron
                    x: 6 + entry.indent
                    anchors.verticalCenter: parent.verticalCenter
                    width: rowItem.kind === "year" ? 16 : 4
                    text: rowItem.kind === "year" ? (rowItem.modelData.open ? "▾" : "▸") : ""
                    color: Theme.mutedText
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                }
                Rectangle {
                    id: coverFrame
                    visible: rowItem.kind === "album"
                    anchors.left: chevron.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: visible ? 32 : 0
                    height: 32
                    radius: Math.min(4, Theme.cornerRadius)
                    color: root.shade(Theme.foreground, 0.08)
                    clip: true
                    readonly property var cover: {
                        void root.revision
                        return rowItem.kind === "album" ? Captures.coverFor(rowItem.albumPaths) : ({})
                    }
                    readonly property real dpr: Screen.devicePixelRatio
                    Image {
                        objectName: "albumCover"
                        anchors.fill: parent
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize: Qt.size(32, 32)
                        readonly property var cover: coverFrame.cover
                        source: cover && cover.path
                                ? "image://thumbs/" + coverFrame.dpr + (cover.isVideo ? "@20" : "")
                                  + "~" + encodeURIComponent(cover.thumbnailVersion || String(cover.stamp))
                                  + encodeURIComponent(cover.path)
                                : ""
                    }
                }
                Icon {
                    id: fileMark
                    visible: rowItem.kind === "tag" && rowItem.modelData.fromFile === true
                    anchors.left: coverFrame.right
                    anchors.leftMargin: visible ? 4 : 0
                    anchors.verticalCenter: parent.verticalCenter
                    width: visible ? 12 : 0
                    size: 12
                    name: "file"
                    color: Theme.mutedText
                }
                Text {
                    anchors.left: fileMark.right
                    anchors.leftMargin: 6
                    anchors.right: countText.left
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: rowItem.kind === "month" ? rowItem.modelData.shortLabel : rowItem.modelData.label
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Theme.foreground
                }
                Text {
                    id: countText
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: rowItem.count >= 0 ? Number(rowItem.count).toLocaleString(Qt.locale(), "f", 0) : ""
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }
                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: function (event) {
                        list.forceActiveFocus()
                        if (event.button === Qt.RightButton) {
                            root.openMenu(rowItem.index, entry, event.x, event.y)
                        } else if (rowItem.kind === "year" && event.x < chevron.x + chevron.width) {
                            list.currentIndex = rowItem.index
                            root.setYearOpen(rowItem.modelData.name, !rowItem.modelData.open)
                        } else {
                            root.activate(rowItem.index)
                        }
                    }
                }
                ThemedToolTip {
                    requested: mouse.containsMouse && rowItem.kind === "tag"
                    text: rowItem.modelData.fromFile
                          ? rowItem.modelData.name + " · from the files' keywords"
                          : String(rowItem.modelData.name)
                }
            }
        }
    }

    Menu {
        id: rowMenu
        objectName: "sidebarCollectionMenu"
        property string kind: ""
        property string name: ""
        modal: true
        dim: false
        background: Rectangle {
            implicitWidth: 200
            color: Theme.background
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.16)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        }
        delegate: MenuItem {
            id: menuItem
            height: 30
            Accessible.name: menuItem.text
            contentItem: Text {
                text: menuItem.text
                color: menuItem.highlighted ? Theme.brightForeground : Theme.foreground
                font.family: Theme.fontFamily
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
                rightPadding: 12
                elide: Text.ElideRight
            }
            background: Rectangle {
                color: menuItem.highlighted ? root.shade(Theme.foreground, 0.09) : "transparent"
                radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 3) : 0
            }
        }
        Action {
            text: rowMenu.kind === "album" ? "Rename album…" : "Rename tag…"
            onTriggered: {
                const kind = rowMenu.kind
                const name = rowMenu.name
                Qt.callLater(function () { root.renameRequested(kind, name) })
            }
        }
        Action {
            text: rowMenu.kind === "album" ? "Delete album" : "Delete tag"
            onTriggered: root.deleteRequested(rowMenu.kind, rowMenu.name)
        }
        onClosed: if (root.visible) list.forceActiveFocus()
    }
}
