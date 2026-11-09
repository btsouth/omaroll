import QtQuick

Item {
    id: root
    readonly property var hints: [
        ["details", "details"], ["primary", "act"], ["matte", "background"],
        ["trim", "trim"], ["favorite", "favourite"], ["trash", "trash"],
        ["search", "search"], ["folders", "folders"]
    ].map(function(hint) {
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
