import QtQuick

// A few keys for what can be done right now. Without a selection they are
// about the highlighted tile and finding things; with one, the keys that act
// on the whole selection. Hints are in order of use and drop whole from the
// end when the footer is short of room, never cut mid-hint.
Item {
    id: root
    property int selectedCount: 0
    readonly property var hintKeys: selectedCount === 0
        ? [["details", "details"], ["primary", "act"], ["select", "select"],
           ["search", "search"], ["folders", "folders"]]
        : selectedCount === 1
          ? [["favorite", "favourite"], ["trash", "trash"], ["escape", "clear"],
             ["copy", "copy"], ["send", "send"]]
          : [["favorite", "favourite"], ["trash", "trash"], ["escape", "clear"],
             ["select-all", "all"], ["copy", "copy"]]
    readonly property var hints: hintKeys.map(function(hint) {
        return ShortcutCatalog.display([ShortcutCatalog.sequence("library", hint[0])[0]]) + " " + hint[1]
    })
    Row {
        id: row
        spacing: 16
        Repeater {
            model: root.hints
            Text {
                required property string modelData
                required property int index
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
                text: modelData
                visible: {
                    let used = implicitWidth;
                    for (let i = 0; i < index; ++i)
                        used += row.children[i].implicitWidth + 16;
                    return used <= root.width;
                }
            }
        }
    }
}
