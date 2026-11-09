import QtQuick

// One chip per active Browse facet (folder, album, tag, date, camera, lens,
// rating, review set or saved view), each with its own clear control. The
// facets combine, so clearing one leaves the others in place.
Flow {
    id: root

    spacing: 6

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function folderName(path) {
        const parts = path.split("/").filter(function (part) { return part !== "" })
        return parts.length > 0 ? parts[parts.length - 1] : path
    }

    function isoToDate(value) {
        const parts = value.split("-")
        return new Date(Number(parts[0]), Number(parts[1]) - 1, Number(parts[2]))
    }

    function dateLabel() {
        if (Captures.modifiedAfter !== "") {
            return "Changed since " + Qt.formatDateTime(new Date(Captures.modifiedAfter),
                                                         Qt.locale().dateTimeFormat(Locale.ShortFormat))
        }
        const prefix = Captures.dateField === 1 ? "Modified " : ""
        const from = Captures.dateFrom
        const to = Captures.dateTo
        if (from !== "" && from === to) {
            return prefix + Qt.formatDate(isoToDate(from), "d MMMM yyyy")
        }
        if (from !== "" && to !== "") {
            const first = isoToDate(from)
            const last = isoToDate(to)
            const monthEnd = new Date(first.getFullYear(), first.getMonth() + 1, 0)
            if (first.getDate() === 1 && last.getTime() === monthEnd.getTime()) {
                return prefix + Qt.formatDate(first, "MMMM yyyy")
            }
            return prefix + Qt.formatDate(first, "d MMM yyyy") + " – " + Qt.formatDate(last, "d MMM yyyy")
        }
        if (from !== "") return prefix + "From " + Qt.formatDate(isoToDate(from), "d MMM yyyy")
        return prefix + "Until " + Qt.formatDate(isoToDate(to), "d MMM yyyy")
    }

    // The header hides the search field in a narrow window with a
    // selection; a typed query then shows here so the filter stays visible.
    property bool showSearch: false
    readonly property int facetCount: chips.length - (chips.length > 0 && chips[0].key === "search" ? 1 : 0)

    readonly property var chips: {
        const list = []
        if (showSearch && Captures.searchText !== "")
            list.push({key: "search", kind: "Search", label: Captures.searchText})
        if (Captures.smartCollectionFilter !== "")
            list.push({key: "view", kind: "Saved view", label: Captures.smartCollectionFilter})
        if (Captures.duplicatesOnly)
            list.push({key: "duplicates", kind: "Review", label: "Exact duplicates"})
        if (Captures.similarOnly)
            list.push({key: "similar", kind: "Review", label: "Similar pictures"})
        if (Captures.folderFilter !== "")
            list.push({key: "folder", kind: "Folder", label: folderName(Captures.folderFilter),
                       tip: Captures.folderFilter})
        if (Captures.albumFilter !== "")
            list.push({key: "album", kind: "Album", label: Captures.albumFilter})
        if (Captures.tagFilter !== "")
            list.push({key: "tag", kind: "Tag", label: "#" + Captures.tagFilter})
        if (Captures.dateFrom !== "" || Captures.dateTo !== "" || Captures.modifiedAfter !== "")
            list.push({key: "date", kind: "Date", label: dateLabel()})
        if (Captures.cameraFilter !== "")
            list.push({key: "camera", kind: "Camera", label: Captures.cameraFilter})
        if (Captures.lensFilter !== "")
            list.push({key: "lens", kind: "Lens", label: Captures.lensFilter})
        if (Captures.minimumRating > 0)
            list.push({key: "rating", kind: "Rating", label: "★".repeat(Captures.minimumRating) + " or more"})
        return list
    }

    // One filter pass in C++, rather than one per facet.
    function clearAll() { Captures.clearFacets(false) }

    function clear(key) {
        // A saved view set everything it shows, search included.
        if (key === "view") Captures.clearFacets(true)
        else if (key === "search") Captures.searchText = ""
        else if (key === "duplicates") Captures.duplicatesOnly = false
        else if (key === "similar") Captures.similarOnly = false
        else if (key === "folder") Captures.folderFilter = ""
        else if (key === "album") Captures.setAlbumFilter("", [])
        else if (key === "tag") Captures.setTagFilter("", [])
        else if (key === "date") Captures.clearDateRange()
        else if (key === "camera") Captures.cameraFilter = ""
        else if (key === "lens") Captures.lensFilter = ""
        else if (key === "rating") Captures.minimumRating = 0
    }

    Repeater {
        model: root.chips

        Rectangle {
            id: chip
            required property var modelData
            objectName: "filterChip-" + modelData.key
            height: 24
            width: Math.min(root.width, chipRow.implicitWidth + 4)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
            color: root.shade(Theme.accent, 0.14)
            border.width: 1
            border.color: root.shade(Theme.accent, 0.45)

            Row {
                id: chipRow
                anchors.verticalCenter: parent.verticalCenter
                x: 8
                spacing: 6

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: chip.modelData.kind
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    color: Theme.mutedText
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, Math.max(40, root.width - 120))
                    text: chip.modelData.label
                    elide: Text.ElideMiddle
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    color: Theme.brightForeground
                }

                // The one clear control for this facet.
                Rectangle {
                    id: clearButton
                    objectName: "filterChipClear-" + chip.modelData.key
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 18
                    radius: width / 2
                    activeFocusOnTab: true
                    color: clearHover.hovered || activeFocus
                           ? root.shade(Theme.foreground, 0.16) : "transparent"
                    border.width: activeFocus ? 1 : 0
                    border.color: Theme.accent
                    Accessible.role: Accessible.Button
                    Accessible.name: "Clear " + chip.modelData.kind.toLowerCase() + " " + chip.modelData.label
                    Accessible.onPressAction: root.clear(chip.modelData.key)
                    Keys.onReturnPressed: root.clear(chip.modelData.key)
                    Keys.onEnterPressed: root.clear(chip.modelData.key)
                    Keys.onSpacePressed: root.clear(chip.modelData.key)

                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        color: Theme.foreground
                    }
                    HoverHandler { id: clearHover }
                    TapHandler { onTapped: root.clear(chip.modelData.key) }
                    ThemedToolTip {
                        requested: clearHover.hovered
                        text: "Clear " + chip.modelData.kind.toLowerCase()
                    }
                }
            }

            HoverHandler { id: chipHover }
            ThemedToolTip {
                requested: chipHover.hovered && !clearHover.hovered && chip.modelData.tip !== undefined
                text: chip.modelData.tip !== undefined ? chip.modelData.tip : ""
            }
        }
    }

    // With two or more, one more control clears them together.
    Text {
        id: clearAllButton
        objectName: "filterChipsClearAll"
        visible: root.chips.length >= 2
        height: 24
        verticalAlignment: Text.AlignVCenter
        leftPadding: 4
        rightPadding: 4
        text: "Clear all"
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.underline: clearAllHover.hovered || activeFocus
        color: Theme.accent
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Clear all filters"
        Accessible.onPressAction: root.clearAll()
        Keys.onReturnPressed: root.clearAll()
        Keys.onEnterPressed: root.clearAll()
        Keys.onSpacePressed: root.clearAll()
        HoverHandler { id: clearAllHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.clearAll() }

        Rectangle {
            objectName: "filterChipsClearAllFocus"
            anchors.fill: parent
            anchors.topMargin: 2
            anchors.bottomMargin: 2
            visible: clearAllButton.activeFocus
            color: "transparent"
            radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 3) : 2
            border.width: 1
            border.color: Theme.accent
        }
    }
}
