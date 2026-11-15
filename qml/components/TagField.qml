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
        const found = Settings.matchTags(typed, root.present, 8)
        const lower = typed.toLowerCase()
        const exists = Settings.tagNames.some(function (name) { return name.toLowerCase() === lower })
        const next = found.map(function (name) { return {name: name, create: false} })
        if (!exists) next.push({name: typed, create: true})
        root.rows = next
        root.highlighted = next.length > 0 ? 0 : -1
        // Upward when the space below would not hold the list.
        const window = root.Window.window
        const bottom = root.mapToItem(null, 0, root.height).y
        suggestions.upward = window !== null && bottom + suggestions.implicitHeight + 8 > window.height
                             && bottom - root.height > suggestions.implicitHeight + 8
    }

    function add(name) {
        if (root.paths.length === 0) return
        const typed = root.normalized(name)
        if (typed === "") return
        const before = Settings.latestUndoId
        const result = Settings.createTagFor(typed, root.paths)
        if (result.name === "") {
            root.tagged(Settings.organizationError || "Use a shorter tag name", 0)
            return
        }
        input.text = ""
        const count = result.count
        const items = root.paths.length === 1 ? "" : " on " + count + (count === 1 ? " item" : " items")
        const message = result.created ? "Created tag " + result.name + items
                      : count === 0 ? "Already tagged " + result.name
                      : "Tagged " + result.name + items
        root.tagged(message, Settings.latestUndoId !== before ? Settings.latestUndoId : 0)
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
        maximumLength: 120
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
        property bool upward: false
        visible: root.rows.length > 0 && input.activeFocus
        // The field keeps the keyboard; the list only shows and takes clicks.
        focus: false
        modal: false
        closePolicy: Popup.NoAutoClose
        x: 0
        y: upward ? -height - 4 : root.height + 4
        width: root.width
        padding: 4
        implicitHeight: list.contentHeight + topPadding + bottomPadding
        background: Rectangle {
            color: root.osd ? Osd.panelSolid : Theme.background
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
            border.width: 1
            border.color: root.osd ? Osd.border : root.shade(Theme.foreground, 0.16)
        }

        contentItem: ListView {
            id: list
            objectName: "tagSuggestionList"
            implicitHeight: contentHeight
            interactive: false
            model: root.rows
            currentIndex: root.highlighted
            Accessible.role: Accessible.List
            Accessible.name: "Tag suggestions"

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property bool current: index === root.highlighted
                width: ListView.view.width
                height: 28
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
