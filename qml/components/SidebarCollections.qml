import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import QtQuick.Effects

// Albums, tags and dates above the folder tree, as one recycling list of
// flattened rows: a framed header per section, then its albums with a cover,
// its tags as an indented tree, or its years, each opening to its months.
//
// A row narrows the library the way Browse does, so it combines with the
// folder, search and other facets; the active row is highlighted, and
// choosing it again clears that facet. Which sections are open is saved.
//
// Section headers stay in reach however long a section is: the header of
// the section being scrolled through sticks to the top, where a click
// collapses it, and the headers of the sections below stick to the bottom,
// where a click goes to them. Ctrl+Up and Ctrl+Down move between headers
// and Ctrl+Left collapses the current section from any of its rows.
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
    // Bumped when albums or tags change, so the rows are read again.
    property int revision: 0
    // Bumped when albums, tags or the library's files change, so the shown
    // rows read their counts and covers again. Neither follows the filter:
    // a search, or choosing a row here, leaves both alone.
    property int detailRevision: 0
    // How many times the list was handed new rows, for tests.
    property int modelResets: 0
    // Whether the keyboard walk and Tab may enter the list: not when it has
    // no room to show a row.
    property bool keyboardReachable: true
    signal renameRequested(string mode, string name)
    signal deleteRequested(string mode, string name)
    // Up from the first row and Down from the last leave the list.
    signal exitUp()
    signal exitDown()

    // From the rows rather than the list's contentHeight, which it estimates
    // from its own height and so would loop back into this.
    function rowHeight(kind) { return kind === "header" ? 36 : kind === "album" ? 42 : 32 }
    readonly property int headerHeight: 36
    // Where each row starts, and one more entry for the end.
    readonly property var offsets: {
        const out = [0]
        let total = 0
        for (const row of rows) {
            total += rowHeight(row.kind)
            out.push(total)
        }
        return out
    }
    readonly property real rowsHeight: offsets[offsets.length - 1]
    // Whole rows only: the list ends on a row's edge rather than cutting
    // the last one it shows through its text.
    function fittedHeight(limit) {
        if (rowsHeight <= limit) return rowsHeight
        let fitted = 0
        for (let i = 1; i < offsets.length && offsets[i] <= limit; ++i) fitted = offsets[i]
        return fitted
    }
    implicitHeight: fittedHeight(maximumHeight)
    visible: rows.length > 0
    // The scroll bar has a gutter of its own beside the rows, so it never
    // crosses a header's frame or the keyboard ring.
    readonly property bool overflowing: rowsHeight > list.height + 0.5
    readonly property real gutter: overflowing ? 10 : 0
    // The keyboard ring shows while the keyboard is in use; a click on a
    // row or header does not leave one behind.
    readonly property bool keyboardShown: !root.Window.window || root.Window.window.keyboardNavigation !== false
    function keyboardUsed() { if (root.Window.window) root.Window.window.keyboardNavigation = true }
    function pointerUsed() { if (root.Window.window) root.Window.window.keyboardNavigation = false }

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

    // Organization can change in bursts while files' keywords are read;
    // one pass after each burst is enough. Not restarted while it runs.
    Timer {
        id: organizationChanges
        interval: 100
        onTriggered: { root.revision++; root.detailRevision++ }
    }
    Timer {
        id: fileChanges
        interval: 100
        onTriggered: root.detailRevision++
    }
    Connections {
        target: Settings
        function onAlbumsChanged() { if (!organizationChanges.running) organizationChanges.start() }
        function onTagsChanged() { if (!organizationChanges.running) organizationChanges.start() }
    }
    Connections {
        target: Captures
        // Files came or went, or a capture date, thumbnail or hidden state
        // changed: a cover may be another file now.
        function onFilesChanged() { if (!fileChanges.running) fileChanges.start() }
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
    // move the current row before it is put back, and rows that read the
    // same are not handed over at all, so their delegates stay.
    property string currentKey: ""
    property bool restoring: false
    property string appliedRows: ""
    function keyOf(row) { return row ? row.kind + ":" + row.name : "" }
    function indexOfKey(key) {
        for (let i = 0; i < rows.length && key !== ""; ++i) {
            if (keyOf(rows[i]) === key) return i
        }
        return -1
    }
    function applyRows() {
        const signature = JSON.stringify(root.rows)
        if (signature === root.appliedRows) return
        root.appliedRows = signature
        const key = root.currentKey
        const before = list.currentIndex
        // The row at the top of the view and how far it is scrolled past,
        // so the view stays where it was.
        const old = list.model
        const topIndex = old && old.length > 0 ? list.indexAt(0, list.contentY) : -1
        const topItem = topIndex >= 0 ? list.itemAtIndex(topIndex) : null
        const topKey = topItem ? root.keyOf(old[topIndex]) : ""
        const topOffset = topItem ? list.contentY - topItem.y : 0
        root.restoring = true
        list.model = root.rows
        const index = root.indexOfKey(key)
        list.currentIndex = index >= 0 ? index : Math.min(before, rows.length - 1)
        const top = root.indexOfKey(topKey)
        if (top >= 0) {
            list.forceLayout()
            list.positionViewAtIndex(top, ListView.Beginning)
            const end = list.originY + Math.max(0, list.contentHeight - list.height)
            list.contentY = Math.max(list.originY, Math.min(end, list.contentY + topOffset))
        }
        root.restoring = false
        if (root.revealPending && index >= 0) {
            root.revealPending = false
            list.forceLayout()
            root.reveal(index)
        }
        // The row asked for is not among these rows, and no rebuild that
        // could bring it is on its way: drop the request, so a later,
        // unrelated rebuild does not jump there.
        if (root.revealPending && index < 0 && !organizationChanges.running) root.revealPending = false
        if (!root.revealPending) root.currentKey = root.keyOf(root.rows[list.currentIndex])
        root.modelResets++
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
        root.reveal(0)
        list.forceActiveFocus()
    }
    function focusLast() {
        list.currentIndex = rows.length - 1
        root.reveal(list.currentIndex)
        list.forceActiveFocus()
    }
    function moveTo(index) {
        if (index < 0 || index >= rows.length) return
        list.currentIndex = index
        root.reveal(index)
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
    // Header rows, in order.
    readonly property var headerIndices: {
        const out = []
        for (let i = 0; i < rows.length; ++i) if (rows[i].kind === "header") out.push(i)
        return out
    }
    // The header of the section a row is in.
    function sectionHeaderOf(index) {
        let found = -1
        for (const header of headerIndices) {
            if (header > index) break
            found = header
        }
        return found
    }
    // Which headers stick, for the list scrolled to top: the header of the
    // section being looked through when its own row is above the view, and
    // the headers below the view, stacked at the bottom.
    function pinnedAt(top) {
        const height = list.height
        const bottom = []
        let edge = top + height
        for (let k = headerIndices.length - 1; k >= 0; --k) {
            const header = headerIndices[k]
            if (offsets[header] + root.headerHeight <= edge + 0.5) break
            bottom.unshift(header)
            edge -= root.headerHeight
        }
        let above = -1
        for (const header of headerIndices) {
            if (offsets[header] < top - 0.5 && bottom.indexOf(header) < 0) above = header
        }
        // Stuck headers never take the room of the last row that would
        // show: in a short pane they give way, the farthest first, then
        // the top one, so a row (even an album's) always has its height.
        const room = Math.max(0, Math.floor((height - root.rowHeight("album")) / root.headerHeight))
        if (above >= 0 && room < 1) above = -1
        while (bottom.length > room - (above >= 0 ? 1 : 0)) bottom.pop()
        // Pushed up by the next header as it arrives.
        let shift = 0
        if (above >= 0) {
            const next = headerIndices[headerIndices.indexOf(above) + 1]
            if (next !== undefined) shift = Math.min(0, offsets[next] - top - root.headerHeight)
        }
        return {top: above, shift: shift, bottom: bottom,
                topCover: above >= 0 ? root.headerHeight + shift : 0,
                bottomCover: bottom.length * root.headerHeight}
    }
    // How far the view is scrolled, measured from a row on screen: the
    // list's own origin is an estimate while rows above are unseen.
    function exactViewTop() {
        const at = list.indexAt(1, list.contentY + 1)
        const item = at >= 0 ? list.itemAtIndex(at) : null
        return item && at < root.offsets.length ? root.offsets[at] + list.contentY - item.y
                                                : list.contentY - list.originY
    }
    readonly property real viewTop: { void list.contentY; void list.originY; void list.count; return exactViewTop() }
    readonly property var pinned: { void list.height; return pinnedAt(root.viewTop) }
    // Scrolls just enough that a row is in view and clear of the stuck
    // headers.
    function reveal(index) {
        if (index < 0 || index >= rows.length) return
        list.positionViewAtIndex(index, ListView.Contain)
        const rowTop = offsets[index]
        const rowBottom = offsets[index + 1]
        const extent = Math.max(0, rowsHeight - list.height)
        const start = root.exactViewTop()
        let top = start
        for (let pass = 0; pass < 2; ++pass) {
            const pins = pinnedAt(top)
            const cover = pins.top === index ? 0 : pins.topCover
            if (rowTop < top + cover) top = rows[index].kind === "header" ? rowTop : rowTop - root.headerHeight
            const after = pinnedAt(top)
            const below = after.bottom.indexOf(index) >= 0 ? 0 : after.bottomCover
            if (rowBottom > top + list.height - below) top = rowBottom - list.height + below
            top = Math.max(0, Math.min(extent, top))
        }
        if (Math.abs(top - start) > 0.5) list.contentY += top - start
    }
    // A stuck header was clicked: collapse the section it heads when it is
    // the one at the top, or go to it when it is below.
    function pinnedHeaderClicked(index) {
        const row = rows[index]
        if (!row) return
        const section = row.section
        const atTop = root.pinned.top === index
        if (atTop) root.setOpen(section, false)
        else if (!row.open) root.setOpen(section, true)
        const at = root.indexOfKey("header:" + section)
        list.currentIndex = at
        list.forceActiveFocus()
        list.forceLayout()
        if (atTop) root.reveal(at)
        else list.positionViewAtIndex(at, ListView.Beginning)
    }
    function stepSection(index, step) {
        const header = root.sectionHeaderOf(index)
        const at = root.headerIndices.indexOf(header)
        if (step < 0 && header >= 0 && header < index) return header
        const next = root.headerIndices[at + step]
        return next === undefined ? -1 : next
    }
    // Focus a row by kind and name once it is there, as after a rename,
    // whose new row comes with the next rebuild.
    function focusRow(kind, name) {
        root.currentKey = kind + ":" + name
        root.revealPending = true
        // Under a collapsed section, open it once to reach the row.
        const section = kind === "album" ? "albums" : kind === "tag" ? "tags" : "dates"
        if (root.indexOfKey(root.currentKey) < 0 && !root.isOpen(section)) root.setOpen(section, true)
        const index = root.indexOfKey(root.currentKey)
        if (index >= 0) {
            list.currentIndex = index
            root.revealPending = false
            list.forceLayout()
            root.reveal(index)
        } else if (!organizationChanges.running) {
            // Not there, and no rebuild is coming to bring it.
            root.revealPending = false
            root.currentKey = root.keyOf(root.rows[list.currentIndex])
        }
        list.forceActiveFocus()
    }
    property bool revealPending: false

    function openMenu(index, item, x, y) {
        const row = rows[index]
        if (!row || (row.kind !== "album" && row.kind !== "tag")) return
        list.currentIndex = index
        rowMenu.kind = row.kind
        rowMenu.name = row.name
        rowMenu.popup(item, x, y)
    }

    // The rows show only between the stuck headers, so the headers need no
    // backing of their own and look the same as the headers in the list:
    // nothing scrolls under them to show through.
    Item {
        id: rowsClip
        objectName: "collectionRowsClip"
        x: 0
        width: root.width
        y: Math.max(0, root.pinned.topCover)
        height: Math.max(0, root.height - y - root.pinned.bottomCover)
        clip: true
        ListView {
            id: list
            objectName: "sidebarCollections"
            x: 0
            y: -rowsClip.y
            width: root.width
            height: root.height
            clip: true
            Accessible.description: "Ctrl+Up and Ctrl+Down go to the previous or next section, Ctrl+Left collapses this one"
            focus: true
            activeFocusOnTab: root.keyboardReachable
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            currentIndex: -1
            keyNavigationEnabled: false
            reuseItems: true
            Accessible.role: Accessible.List
            Accessible.name: "Collections"
            onCurrentIndexChanged: if (!root.restoring) root.currentKey = root.keyOf(root.rows[currentIndex])
            onActiveFocusChanged: if (activeFocus && currentIndex < 0 && count > 0) currentIndex = 0
            // Beside the stuck headers too, not cut by them.
            ScrollBar.vertical: ScrollBar {
                objectName: "sidebarCollectionsScrollBar"
                parent: root
                x: root.width - width
                y: 0
                height: root.height
                policy: root.overflowing ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                width: 6
                Accessible.ignored: !root.overflowing
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
                root.keyboardUsed()
                const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
                if (ctrl && (event.key === Qt.Key_Down || event.key === Qt.Key_Up)) {
                    const target = root.stepSection(index, event.key === Qt.Key_Down ? 1 : -1)
                    if (target >= 0) root.moveTo(target)
                    event.accepted = true
                    return
                }
                if (ctrl && event.key === Qt.Key_Left) {
                    const header = root.sectionHeaderOf(index)
                    if (header >= 0) {
                        const section = root.rows[header].section
                        root.setOpen(section, false)
                        root.moveTo(root.indexOfKey("header:" + section))
                    }
                    event.accepted = true
                    return
                }
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
                readonly property bool current: ListView.isCurrentItem && list.activeFocus && root.keyboardShown
                readonly property bool active: { void Captures.albumFilter; void Captures.tagFilter
                                                 void Captures.dateFrom; void Captures.dateTo
                                                 return root.isActive(modelData) }
                width: list.width - root.gutter
                height: root.rowHeight(kind)
                // The list hands the keyboard to its current row, so the row
                // itself is what a screen reader reads, not a nameless filler.
                Accessible.role: kind === "header" ? Accessible.Heading : Accessible.ListItem
                Accessible.name: {
                    const row = modelData
                    if (kind === "header") return row.label + ", " + row.count
                    const counted = count >= 0 ? ", " + root.items(count) : ""
                    switch (kind) {
                    case "album": return "Album " + row.name + counted
                    case "tag": return "Tag " + row.name + counted
                    default: return row.label + counted
                    }
                }
                Accessible.description: kind === "header"
                                        ? (modelData.open ? "Expanded. Enter collapses it" : "Collapsed. Enter expands it")
                                        : kind === "tag" && modelData.fromFile ? "From the files' keywords" : ""
                Accessible.focusable: true
                Accessible.focused: ListView.isCurrentItem && list.activeFocus
                Accessible.selectable: kind !== "header"
                Accessible.selected: active
                // A header and a year open, as a tree item would.
                Accessible.expandable: kind === "header" || kind === "year"
                Accessible.expanded: (kind === "header" || kind === "year") && modelData.open === true
                Accessible.onPressAction: root.activate(index)
                // Albums read their paths once per change, for the count and cover.
                readonly property var albumPaths: {
                    void root.detailRevision
                    return kind === "album" ? Settings.albumPaths(modelData.name) : []
                }
                readonly property int count: {
                    void root.detailRevision
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
                    Accessible.ignored: true
                    onToggled: {
                        root.pointerUsed()
                        // The row first, so taking the keyboard does not
                        // scroll the list to its top row.
                        list.currentIndex = rowItem.index
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
                    Accessible.ignored: true

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
                            void root.detailRevision
                            return rowItem.kind === "album" ? Captures.coverFor(rowItem.albumPaths) : ({})
                        }
                        readonly property real dpr: Screen.devicePixelRatio
                        // Rounded like the grid's thumbnails: clip alone only
                        // cuts to the square.
                        Rectangle {
                            id: coverMask
                            anchors.fill: parent
                            radius: coverFrame.radius
                            visible: false
                            layer.enabled: coverFrame.radius > 0
                            layer.smooth: true
                        }
                        Image {
                            objectName: "albumCover"
                            anchors.fill: parent
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            sourceSize: Qt.size(32, 32)
                            layer.enabled: coverFrame.radius > 0
                            layer.smooth: true
                            layer.effect: MultiEffect {
                                maskEnabled: true
                                maskSource: coverMask
                                maskThresholdMin: 0.5
                                maskSpreadAtMin: 1.0
                            }
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
                            root.pointerUsed()
                            list.currentIndex = rowItem.index
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
    }

    // Stuck headers, drawn over the list on the sidebar's own surface.
    component PinnedHeader: Item {
        id: pin
        property int rowIndex: -1
        // Only a header row: while rows are handed over the index can
        // briefly name another kind.
        readonly property var row: rowIndex >= 0 && root.rows[rowIndex] && root.rows[rowIndex].kind === "header"
                                   ? root.rows[rowIndex] : null
        width: list.width - root.gutter
        height: root.headerHeight
        visible: row !== null
        SidebarHeader {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 2
            height: 28
            label: pin.row ? pin.row.label : ""
            count: pin.row && pin.row.count !== undefined ? pin.row.count : -1
            expanded: !!pin.row && pin.row.open === true
            keyboardCurrent: false
            // The real header row is what the keyboard and screen readers
            // reach; this is a shortcut to it for the pointer.
            Accessible.ignored: true
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: { root.pointerUsed(); root.pinnedHeaderClicked(pin.rowIndex) }
        }
    }
    Item {
        id: pins
        anchors.fill: parent
        clip: true
        PinnedHeader {
            objectName: "pinnedTopHeader"
            rowIndex: root.pinned.top
            y: root.pinned.shift
        }
        Repeater {
            model: root.pinned.bottom
            PinnedHeader {
                required property int modelData
                required property int index
                objectName: "pinnedBottomHeader"
                rowIndex: modelData
                y: pins.height - (root.pinned.bottom.length - index) * root.headerHeight
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
        // Drawn in the window as an item and opaque whatever the theme's
        // alpha: as a separate popup window it was see-through on the
        // desktop, with the sidebar showing behind its rows.
        popupType: Popup.Item
        background: Rectangle {
            objectName: "sidebarCollectionMenuBackground"
            implicitWidth: 200
            color: Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 1)
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
