pragma Singleton
import QtQuick

// Stable tokens identify existing handler branches. Change the physical keys
// here; help, toolbar hints and keyboard dispatch derive from these rows.
QtObject {
    id: root
    property var activeTip: null
    readonly property var rows: [
        {"context": "quick", "id": "previous", "keys": ["Left", "Right"], "label": "Previous / next file", "group": "Navigation", "flag": "still", "tokens": ["Left", "Right"]},
        {"context": "quick", "id": "step", "keys": ["PgUp", "PgDown"], "label": "Previous / next file", "group": "Navigation", "flag": "", "tokens": ["PageUp", "PageDown"]},
        {"context": "quick", "id": "ends", "keys": ["Home", "End"], "label": "First / last file, or start / end of video", "group": "Navigation", "flag": "", "tokens": ["Home", "End"]},
        {"context": "quick", "id": "library", "keys": ["Return", "Enter"], "label": "Open in library", "group": "Navigation", "flag": "", "tokens": ["Return", "Enter"]},
        {"context": "quick", "id": "filmstrip", "keys": ["B"], "label": "Show / hide filmstrip", "group": "Navigation", "flag": "", "tokens": []},
        {"context": "quick", "id": "slideshow", "keys": ["F5"], "label": "Start / stop slideshow", "group": "Navigation", "flag": "", "tokens": []},
        {"context": "quick", "id": "escape", "keys": ["Esc"], "label": "Stop slideshow, leave full screen, close details or viewer", "group": "Navigation", "flag": "", "tokens": ["Escape"]},
        {"context": "quick", "id": "focus", "keys": ["Tab", "Backtab"], "label": "Keep viewer keyboard focus", "group": "Navigation", "flag": "", "tokens": ["Tab", "Backtab"]},
        {"context": "quick", "id": "zoom-in", "keys": ["+", "="], "label": "Zoom in", "group": "Zoom", "flag": "", "tokens": ["Plus", "Equal"]},
        {"context": "quick", "id": "zoom-out", "keys": ["-", "_"], "label": "Zoom out", "group": "Zoom", "flag": "", "tokens": ["Minus", "Underscore"]},
        {"context": "quick", "id": "fit", "keys": ["0"], "label": "Fit", "group": "Zoom", "flag": "still", "tokens": []},
        {"context": "quick", "id": "actual", "keys": ["1"], "label": "Toggle actual size / fit", "group": "Zoom", "flag": "still", "tokens": []},
        {"context": "quick", "id": "fit-width", "keys": ["W"], "label": "Fit width from the top", "group": "Zoom", "flag": "still", "tokens": []},
        {"context": "quick", "id": "fill", "keys": ["Shift+W"], "label": "Fill / fit window", "group": "Zoom", "flag": "still", "tokens": []},
        {"context": "quick", "id": "pan", "keys": ["Shift+Left", "Shift+Right", "Shift+Up", "Shift+Down"], "label": "Pan picture", "group": "Zoom", "flag": "still", "tokens": []},
        {"context": "quick", "id": "rotate", "keys": ["R", "Shift+R"], "label": "Rotate right / left", "group": "Zoom", "flag": "", "tokens": []},
        {"context": "quick", "id": "playback", "keys": ["Space", "K"], "label": "Play / pause video or animation", "group": "Playback", "flag": "motion", "tokens": []},
        {"context": "quick", "id": "seek", "keys": ["Left", "Right"], "label": "Seek five seconds", "group": "Playback", "flag": "video", "tokens": []},
        {"context": "quick", "id": "seek-ten", "keys": ["J", "L"], "label": "Seek ten seconds", "group": "Playback", "flag": "video", "tokens": []},
        {"context": "quick", "id": "step-frame", "keys": [",", "."], "label": "Previous / next frame", "group": "Playback", "flag": "video", "tokens": ["Comma", "Period"]},
        {"context": "quick", "id": "video-loop", "keys": ["Shift+L"], "label": "Toggle video loop", "group": "Playback", "flag": "video", "tokens": []},
        {"context": "quick", "id": "volume", "keys": ["Up", "Down"], "label": "Volume up / down", "group": "Playback", "flag": "video", "tokens": []},
        {"context": "quick", "id": "mute", "keys": ["M"], "label": "Mute / unmute", "group": "Playback", "flag": "video", "tokens": []},
        {"context": "quick", "id": "speed", "keys": ["[", "]"], "label": "Decrease / increase speed", "group": "Playback", "flag": "video", "tokens": ["BracketLeft", "BracketRight"]},
        {"context": "quick", "id": "normal-speed", "keys": ["Backspace"], "label": "Normal speed", "group": "Playback", "flag": "video", "tokens": []},
        {"context": "quick", "id": "audio-track", "keys": ["Shift+A"], "label": "Cycle audio tracks", "group": "Playback", "flag": "audio", "tokens": []},
        {"context": "quick", "id": "subtitles", "keys": ["C"], "label": "Cycle subtitles", "group": "Playback", "flag": "video", "tokens": []},
        {"context": "quick", "id": "subtitle-offset", "keys": ["Z", "X"], "label": "Sidecar subtitles earlier / later by 0.5 seconds", "group": "Playback", "flag": "sidecar", "tokens": []},
        {"context": "quick", "id": "trim", "keys": ["T"], "label": "Trim video", "group": "Playback", "flag": "video", "tokens": []},
        {"context": "quick", "id": "frame", "keys": ["G"], "label": "Save video frame", "group": "Playback", "flag": "video", "tokens": []},
        {"context": "quick", "id": "play", "keys": ["P"], "label": "Open in mpv", "group": "Playback", "flag": "video", "tokens": []},
        {"context": "quick", "id": "fullscreen", "keys": ["F", "F11"], "label": "Toggle full screen", "group": "Actions", "flag": "", "tokens": []},
        {"context": "quick", "id": "info", "keys": ["I"], "label": "Show / hide details", "group": "Actions", "flag": "", "tokens": []},
        {"context": "quick", "id": "copy", "keys": ["Y", "Ctrl+C"], "label": "Copy image or file", "group": "Actions", "flag": "", "tokens": []},
        {"context": "quick", "id": "copy-path", "keys": ["Ctrl+Shift+C"], "label": "Copy path", "group": "Actions", "flag": "", "tokens": []},
        {"context": "quick", "id": "open-with", "keys": ["Ctrl+O"], "label": "Choose another application", "group": "Actions", "flag": "", "tokens": []},
        {"context": "quick", "id": "menu", "keys": ["Menu"], "label": "Show action menu", "group": "Actions", "flag": "", "tokens": []},
        {"context": "quick", "id": "send", "keys": ["S"], "label": "Send", "group": "Actions", "flag": "", "tokens": []},
        {"context": "quick", "id": "annotate", "keys": ["A"], "label": "Annotate picture", "group": "Actions", "flag": "still", "tokens": []},
        {"context": "quick", "id": "develop", "keys": ["D", "O"], "label": "Open RAW editor", "group": "Actions", "flag": "still", "tokens": []},
        {"context": "quick", "id": "choose-editor", "keys": ["Shift+D", "Shift+O"], "label": "Choose RAW editor", "group": "Actions", "flag": "still", "tokens": []},
        {"context": "quick", "id": "favorite", "keys": ["V"], "label": "Toggle favourite", "group": "Actions", "flag": "", "tokens": []},
        {"context": "quick", "id": "rating", "keys": ["Alt+0", "Alt+1", "Alt+2", "Alt+3", "Alt+4", "Alt+5"], "label": "Clear / set rating", "group": "Actions", "flag": "", "tokens": []},
        {"context": "quick", "id": "undo", "keys": ["Ctrl+Z"], "label": "Undo", "group": "Actions", "flag": "", "tokens": []},
        {"context": "quick", "id": "trash", "keys": ["Del", "Shift+Del"], "label": "Trash / delete permanently", "group": "Actions", "flag": "", "tokens": ["Delete", "Delete"]},
        {"context": "quick", "id": "close", "keys": ["Ctrl+W", "Ctrl+Q"], "label": "Close viewer / quit", "group": "Actions", "flag": "", "tokens": []},
        {"context": "quick", "id": "help", "keys": ["?", "F1"], "label": "Show / hide help", "group": "Actions", "flag": "", "tokens": ["Question", "F1"]},
        {"context": "library", "id": "develop", "keys": ["D", "O"], "label": "Develop RAW", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "choose-editor", "keys": ["Shift+D", "Shift+O"], "label": "Choose RAW editor", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "undo", "keys": ["Ctrl+Z"], "label": "Undo", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "delete", "keys": ["Shift+Del"], "label": "Delete permanently", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "refresh", "keys": ["R"], "label": "Refresh", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "search", "keys": ["/"], "label": "Search", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "select-all", "keys": ["Ctrl+A"], "label": "Select all", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "folders", "keys": ["F9"], "label": "Toggle folders", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "larger", "keys": ["Ctrl++", "Ctrl+="], "label": "Larger tiles", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "smaller", "keys": ["Ctrl+-"], "label": "Smaller tiles", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "reset", "keys": ["Ctrl+0"], "label": "Reset tile size", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "escape", "keys": ["Esc"], "label": "Close sheet, clear selection or close window", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "copy-path", "keys": ["Ctrl+Shift+C"], "label": "Copy path", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "help", "keys": ["?", "F1"], "label": "Help", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "quit", "keys": ["Ctrl+Q"], "label": "Quit", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "details", "keys": ["Return", "Enter"], "label": "Open details", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "primary", "keys": ["Space"], "label": "Primary action", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "select", "keys": ["X"], "label": "Select tile", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "left", "keys": ["Left", "H"], "label": "Move left", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "down", "keys": ["Down", "J"], "label": "Move down", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "up", "keys": ["Up", "K"], "label": "Move up", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "right", "keys": ["Right", "L"], "label": "Move right", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "ends", "keys": ["Home", "End"], "label": "First / last tile", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "section-next", "keys": ["Tab"], "label": "Next section", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "section-prev", "keys": ["Backtab"], "label": "Previous section", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "sections", "keys": ["1", "2", "3", "4", "5", "6", "7", "8"], "label": "Choose section", "group": "Library", "flag": "", "tokens": []},
        {"context": "library", "id": "rating", "keys": ["Alt+0", "Alt+1", "Alt+2", "Alt+3", "Alt+4", "Alt+5"], "label": "Clear / set rating", "group": "Library", "flag": "", "tokens": []},
        {"context": "preview", "id": "previous", "keys": ["Left"], "label": "Previous file", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "next", "keys": ["Right"], "label": "Next file", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "slideshow", "keys": ["F5"], "label": "Slideshow", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "info", "keys": ["I"], "label": "Details", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "fullscreen", "keys": ["F11"], "label": "Full screen", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "fit", "keys": ["0"], "label": "Fit picture / PDF", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "actual", "keys": ["1"], "label": "Actual picture size", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "zoomOut", "keys": ["-"], "label": "Zoom out", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "zoomIn", "keys": ["+", "="], "label": "Zoom in", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "rotate", "keys": ["R"], "label": "Rotate picture", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "flipHorizontal", "keys": ["Shift+H"], "label": "Flip picture horizontally", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "flipVertical", "keys": ["Shift+V"], "label": "Flip picture vertically", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "caption", "keys": ["Alt+C"], "label": "Edit caption", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "copy-path", "keys": ["Ctrl+Shift+C"], "label": "Copy path", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "focus", "keys": ["Tab", "Backtab"], "label": "Actions / preview focus", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "escape", "keys": ["Esc"], "label": "Close preview", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "primary", "keys": ["Space"], "label": "Play / pause or primary action", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "pages", "keys": ["PgUp", "PgDown"], "label": "Previous / next PDF page", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "seek", "keys": ["J", "L"], "label": "Seek five seconds", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "frames", "keys": [",", "."], "label": "Previous / next frame", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "mute", "keys": ["M"], "label": "Mute", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "volume", "keys": ["Up", "Down", "0", "9"], "label": "Volume up / down", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "speed", "keys": ["[", "]"], "label": "Playback speed", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "normal", "keys": ["Backspace"], "label": "Normal speed", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "ends", "keys": ["Home", "End"], "label": "Start / end of video", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "help", "keys": ["?", "F1"], "label": "Help", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "delete", "keys": ["Shift+Del"], "label": "Delete permanently", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "develop", "keys": ["D", "O"], "label": "Develop RAW", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "choose-editor", "keys": ["Shift+D", "Shift+O"], "label": "Choose RAW editor", "group": "Preview", "flag": "", "tokens": []},
        {"context": "preview", "id": "selection-copy", "keys": ["Ctrl+C"], "label": "Copy selected PDF text", "group": "Preview", "flag": "", "tokens": []}
    ]
    function sequence(context, id) {
        for (const row of rows) if (row.context === context && row.id === id) return row.keys
        return []
    }
    function display(keys) {
        return keys.map(function(key) {
            return key.replace("Return", "Enter").replace("Backtab", "Shift+Tab")
                .replace("PgUp", "Page Up").replace("PgDown", "Page Down")
        }).filter(function(key, index, all) { return all.indexOf(key) === index }).join(" / ")
    }
    function code(sequence) {
        let key = sequence.substring(sequence.lastIndexOf("+") + 1)
        if (sequence.endsWith("++") || sequence === "+") key = "Plus"
        const names = {"0": "0", "1": "1", "2": "2", "3": "3", "4": "4", "5": "5",
            "-": "Minus", "_": "Underscore", "=": "Equal", ",": "Comma", ".": "Period",
            "[": "BracketLeft", "]": "BracketRight", "?": "Question", "/": "Slash",
            "Esc": "Escape", "Del": "Delete", "PgUp": "PageUp", "PgDown": "PageDown"}
        return Qt["Key_" + (names[key] || key)]
    }
    function keyMap(context) {
        let result = {}
        for (const row of rows) {
            if (row.context !== context) continue
            for (let i = 0; i < row.keys.length; ++i) {
                const key = row.keys[i]
                const physical = code(key)
                let token = row.tokens[i]
                if (!token) {
                    token = key.substring(key.lastIndexOf("+") + 1)
                    const names = {"+": "Plus", "": "Plus", "=": "Equal", "-": "Minus", "_": "Underscore", ",": "Comma", ".": "Period", "[": "BracketLeft", "]": "BracketRight", "?": "Question", "Esc": "Escape", "Del": "Delete", "PgUp": "PageUp", "PgDown": "PageDown"}
                    token = names[token] || token
                }
                result[token] = physical
            }
        }
        return result
    }
    readonly property var quickKeys: keyMap("quick")
    readonly property var libraryKeys: keyMap("library")
    readonly property var quickHints: {
        let hints = {}
        for (const row of rows) if (row.context === "quick") hints[row.id] = display(row.keys)
        return hints
    }
    readonly property var previewKeys: {
        let keys = {}
        for (const row of rows) if (row.context === "preview") keys[row.id] = {key: code(row.keys[0]), label: display(row.keys)}
        return keys
    }
    function groups(context, video, animated, audio, sidecar, filter) {
        let entries = rows.filter(function(row) {
            return row.context === context && (row.flag === "" || (row.flag === "still" && !video)
                || (row.flag === "video" && video) || (row.flag === "motion" && (video || animated))
                || (row.flag === "audio" && audio) || (row.flag === "sidecar" && sidecar))
        }).map(function(row) { return {keys: display(row.keys), label: row.label, group: row.group} })
        // The existing action registry owns the installed-tool action bindings.
        // Use its data directly, so menu hints, dispatch and help stay in sync.
        if (context !== "quick") {
            const ids = ["annotate", "send", "trim", "play", "frame", "background", "matte", "corrections", "correctionsbatch", "compare", "ocr", "export", "copy", "files", "open-with", "rename", "favorite", "hide", "trash"]
            for (const id of ids) {
                const key = Registry.shortcutFor(id)
                if (key !== "") entries.push({keys: key, label: id.replace(/-/g, " "), group: "Actions"})
            }
        }
        let groups = []
        const needle = filter.toLowerCase().trim()
        for (const row of entries) {
            if ((row.keys + " " + row.label + " " + row.group).toLowerCase().indexOf(needle) < 0) continue
            let group = groups.find(function(group) { return group.title === row.group })
            if (!group) { group = {title: row.group, rows: []}; groups.push(group) }
            group.rows.push(row)
        }
        return groups
    }
}
