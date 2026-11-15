import QtQuick
import QtQuick.Controls.Basic

// Types a tag onto files. Suggestions come from the library's tags, matched at
// the start of any word, ignoring case and accents. Up and Down move, Tab
// takes the highlighted name into the field, Enter adds it (or creates the
// typed tag), a comma adds and keeps typing, and Escape clears the field,
// then leaves it.
FocusScope {
    id: root

    property var paths: []
    // Over a picture or video: dark in every theme (see Osd).
    property bool osd: false
    // Tags the files already carry, left out of the suggestions.
    property var present: []
    readonly property alias text: input.text
    readonly property bool editing: input.activeFocus
    property var rows: []
    property int highlighted: -1
    // What happened, and the undo step that takes it back (0 for none).
    signal tagged(string message, int undoId)
    // The tag the files now carry, after an add that was saved.
    signal added(string name)
    // Ctrl+Z with nothing typed: the app's undo, not the field's.
    signal undoRequested()

    readonly property color textColour: root.osd ? Osd.foreground : Theme.foreground
    readonly property color mutedColour: root.osd ? Osd.mutedText : Theme.mutedText
    readonly property color accentColour: root.osd ? Osd.accent : Theme.accent

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function focusField() {
        input.forceActiveFocus()
    }

    // The name as AppSettings would store it: no empty levels or extra spaces.
    function normalized(name) {
        return name.split("/").map(function (part) { return part.trim().replace(/\s+/g, " ") })
            .filter(function (part) { return part !== "" }).join("/")
    }

    function refresh() {
        const typed = root.normalized(input.text)
        if (typed === "" || !input.activeFocus) {
            root.rows = []
            root.highlighted = -1
            return
        }
        // One lookup each: reading Settings.tagNames here copied every tag
        // on every key, which froze typing with a few hundred tags.
        const found = Settings.matchTags(typed, root.present, 8)
        const exists = Settings.existingTag(typed) !== ""
        const next = found.map(function (name) { return {name: name, create: false} })
        if (!exists) next.push({name: typed, create: true})
        root.rows = next
        root.highlighted = next.length > 0 ? 0 : -1
        root.placeSuggestions()
    }

    // Below the field while at least three rows fit there, scrolling the
    // rest; above only when they do not, so the list covers the chips over
    // the field only when it has to.
    function placeSuggestions() {
        const window = root.Window.window
        if (window === null) return
        const top = root.mapToItem(null, 0, 0).y
        const below = window.height - (top + root.height) - 4 - 8
        const above = top - 4 - 8
        const wanted = root.rows.length * suggestions.rowHeight + suggestions.padding * 2
        const least = Math.min(wanted, 3 * suggestions.rowHeight + suggestions.padding * 2)
        suggestions.upward = below < least && above > below
        suggestions.room = Math.max(0, suggestions.upward ? above : below)
    }

    function add(name) {
        if (root.paths.length === 0) return
        const typed = root.normalized(name)
        if (typed === "") return
        const before = Settings.latestUndoId
        const result = Settings.createTagFor(typed, root.paths)
        const undoId = Settings.latestUndoId !== before ? Settings.latestUndoId : 0
        if (result.name === "" || Settings.organizationError !== "") {
            // Not saved: the typed name stays, and the line says why.
            root.tagged(Settings.organizationError || "Use a shorter tag name", undoId)
            return
        }
        input.text = ""
        const count = result.count
        const items = root.paths.length === 1 ? "" : " on " + count + (count === 1 ? " item" : " items")
        const message = result.created ? "Created tag " + result.name + items
                      : count === 0 ? "Already tagged " + result.name
                      : "Tagged " + result.name + items
        root.tagged(message, undoId)
        root.added(result.name)
    }

    function accept() {
        if (root.highlighted >= 0 && root.highlighted < root.rows.length) {
            root.add(root.rows[root.highlighted].name)
        } else if (input.text.trim() !== "") {
            root.add(input.text)
        }
    }

    // Clears what was typed. False when there was nothing to clear, so a
    // second Escape can leave the field.
    function clearTyped() {
        if (input.text === "" && root.rows.length === 0) return false
        input.text = ""
        root.refresh()
        return true
    }

    implicitWidth: 200
    implicitHeight: 30

    // What was typed belongs to the files it was typed for.
    onPathsChanged: {
        input.text = ""
        root.refresh()
    }

    // Arrow, Home, End and page keys the field did not use (Left at the
    // start, Up with no suggestions) stop here: further up they would move
    // to another file with the typed text still waiting.
    Keys.onPressed: function (event) {
        if (event.modifiers & Qt.AltModifier) return
        if ([Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End,
             Qt.Key_PageUp, Qt.Key_PageDown].indexOf(event.key) >= 0) {
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 4) : 3
        color: input.activeFocus ? root.shade(root.textColour, 0.10)
               : hover.hovered ? root.shade(root.textColour, 0.05) : "transparent"
        border.width: 1
        border.color: input.activeFocus ? root.shade(root.accentColour, 0.6)
                                        : root.shade(root.textColour, 0.16)
    }

    TextInput {
        id: input
        objectName: "tagFieldInput"
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        verticalAlignment: TextInput.AlignVCenter
        clip: true
        focus: true
        activeFocusOnTab: true
        selectByMouse: true
        // Tag names keep 80 characters (TagNames::normalized).
        maximumLength: 80
        font.family: Theme.fontFamily
        font.pixelSize: 13
        color: root.textColour
        selectionColor: root.shade(root.accentColour, 0.5)
        selectedTextColor: root.osd ? Osd.brightForeground : Theme.brightForeground
        Accessible.role: Accessible.EditableText
        Accessible.name: "Add tag"
        Accessible.description: {
            const row = root.highlighted >= 0 ? root.rows[root.highlighted] : undefined
            return row ? (row.create ? "Create tag " : "Suggestion ") + row.name : ""
        }
        onTextChanged: root.refresh()
        onActiveFocusChanged: root.refresh()

        Keys.onPressed: function (event) {
            if (event.key === Qt.Key_Z && (event.modifiers & Qt.ControlModifier)
                    && !(event.modifiers & (Qt.ShiftModifier | Qt.AltModifier)) && input.text === "") {
                root.undoRequested()
                event.accepted = true
                return
            }
            const modified = event.modifiers & (Qt.ControlModifier | Qt.AltModifier)
            if (modified) return
            if ((event.key === Qt.Key_Down || event.key === Qt.Key_Up) && root.rows.length > 0) {
                const step = event.key === Qt.Key_Down ? 1 : -1
                root.highlighted = Math.max(0, Math.min(root.rows.length - 1, root.highlighted + step))
                event.accepted = true
            } else if (event.key === Qt.Key_Tab && !(event.modifiers & Qt.ShiftModifier)
                       && root.highlighted >= 0 && root.highlighted < root.rows.length
                       && !root.rows[root.highlighted].create
                       && input.text !== root.rows[root.highlighted].name) {
                input.text = root.rows[root.highlighted].name
                input.cursorPosition = input.text.length
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                root.accept()
                event.accepted = true
            } else if (event.key === Qt.Key_Comma) {
                root.accept()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape) {
                // Not taken when empty: the window's Escape then leaves.
                event.accepted = root.clearTyped()
            }
        }
    }

    Text {
        anchors.fill: input
        verticalAlignment: Text.AlignVCenter
        visible: input.text === ""
        text: "Add tag"
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 13
        color: root.mutedColour
    }

    HoverHandler {
        id: hover
        cursorShape: Qt.IBeamCursor
    }

    Popup {
        id: suggestions
        objectName: "tagSuggestions"
        // Drawn inside the window: as a window of its own the compositor
        // can make it translucent, with the panel reading through.
        popupType: Popup.Item
        property bool upward: false
        // The space the list may take on its side of the field.
        property real room: 0
        readonly property int rowHeight: 28
        visible: root.rows.length > 0 && input.activeFocus
        // The field keeps the keyboard; the list only shows and takes clicks.
        focus: false
        modal: false
        closePolicy: Popup.NoAutoClose
        // Kept inside the window: the room was measured for it.
        margins: 0
        x: 0
        y: upward ? -height - 4 : root.height + 4
        width: root.width
        padding: 4
        // Whole rows only, so no half-cut row shows at the edge.
        height: Math.min(list.contentHeight,
                         Math.max(1, Math.floor((room - topPadding - bottomPadding) / rowHeight)) * rowHeight)
                + topPadding + bottomPadding
        background: Rectangle {
            // Opaque: text under the list must not read through it.
            color: root.osd ? Qt.rgba(Osd.panelSolid.r, Osd.panelSolid.g, Osd.panelSolid.b, 1)
                            : Theme.background
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
            border.width: 1
            border.color: root.osd ? Osd.border : root.shade(Theme.foreground, 0.16)
        }

        contentItem: ListView {
            id: list
            objectName: "tagSuggestionList"
            implicitHeight: contentHeight
            clip: true
            interactive: false
            boundsBehavior: Flickable.StopAtBounds
            model: root.rows
            currentIndex: root.highlighted
            onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
            Accessible.role: Accessible.List
            Accessible.name: "Tag suggestions"

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property bool current: index === root.highlighted
                width: ListView.view.width
                height: suggestions.rowHeight
                radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 3) : 0
                color: row.current || rowMouse.containsMouse ? root.shade(root.textColour, 0.10) : "transparent"
                Accessible.role: Accessible.ListItem
                Accessible.name: row.modelData.create ? "Create tag " + row.modelData.name : row.modelData.name
                Accessible.selected: row.current

                Text {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    text: row.modelData.create ? "Create tag “" + row.modelData.name + "”"
                                               : row.modelData.name
                    elide: Text.ElideMiddle
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: row.modelData.create ? root.accentColour : root.textColour
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.add(row.modelData.name)
                        input.forceActiveFocus()
                    }
                }
            }
        }
    }
}
