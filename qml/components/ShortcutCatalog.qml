pragma Singleton
import QtQuick

QtObject {
    id: root
    property var activeTip: null
    property bool video: false
    readonly property var quickGroups: [
        {title: "Navigation", rows: [
            {keys: "← / →", label: "Previous / next file", still: true},
            {keys: "Page Up / Page Down", label: "Previous / next file"},
            {keys: "Home / End", label: video ? "Start / end of video" : "First / last file"},
            {keys: "Enter", label: "Open in library"},
            {keys: "B", label: "Show / hide filmstrip"},
            {keys: "F5", label: "Start / stop slideshow"},
            {keys: "Esc", label: "Close help, then stop slideshow, leave full screen, close details or viewer"}]},
        {title: "Zoom", still: true, rows: [
            {keys: "+ / −", label: "Zoom in / out"},
            {keys: "0", label: "Fit"},
            {keys: "1 / double click", label: "Toggle actual size / fit"},
            {keys: "W", label: "Fit width, starting at the top"},
            {keys: "Shift+W", label: "Fill the window, or fit again"},
            {keys: "Shift + arrows", label: "Pan a zoomed picture"},
            {keys: "R / Shift+R", label: "Rotate right / left"}]},
        {title: "Video", video: true, rows: [
            {keys: "Space / K", label: "Play / pause"},
            {keys: "← / →", label: "Seek five seconds"},
            {keys: "J / L", label: "Seek ten seconds"},
            {keys: ", / .", label: "Step one frame back / forward"},
            {keys: "Shift+L", label: "Toggle video loop"},
            {keys: "↑ / ↓", label: "Volume up / down"},
            {keys: "M", label: "Mute / unmute"},
            {keys: "[ / ]", label: "Decrease / increase speed"},
            {keys: "Backspace", label: "Normal speed"},
            {keys: "Shift+A", label: "Cycle audio tracks", audio: true},
            {keys: "C", label: "Cycle subtitles"},
            {keys: "Z / X", label: "Sidecar subtitles earlier / later by 0.5 seconds", sidecar: true},
            {keys: "T / G / P", label: "Trim / save frame / open in mpv"}]},
        {title: "Actions", rows: [
            {keys: "Space / K", label: "Play / pause animation", animation: true},
            {keys: "F / F11", label: "Toggle full screen"},
            {keys: "I", label: "Show / hide details"},
            {keys: "Y / Ctrl+C", label: video ? "Copy file" : "Copy image"},
            {keys: "Ctrl+Shift+C", label: "Copy path"},
            {keys: "Ctrl+O", label: "Choose another application"},
            {keys: "Menu", label: "Show the action menu"},
            {keys: "S", label: "Send"},
            {keys: "A", label: "Annotate", still: true},
            {keys: "D / Shift+D", label: "Open RAW editor / choose editor", still: true},
            {keys: "V", label: "Toggle favourite"},
            {keys: "Alt+1 to Alt+5 / Alt+0", label: "Rate / clear rating"},
            {keys: "Ctrl+Z", label: "Undo"},
            {keys: "Del / Shift+Del", label: "Trash / delete permanently"},
            {keys: "Ctrl+W / Ctrl+Q", label: "Close viewer / quit Omaroll"},
            {keys: "? / F1", label: "Show / hide shortcuts"}]}
    ]
    readonly property var libraryGroups: [
    {
        "title": "Navigation",
        "rows": [
            {
                "keys": "Enter",
                "label": "Open details"
            },
            {
                "keys": "Space",
                "label": "Primary action"
            },
            {
                "keys": "Left / Right / Up / Down / H / J / K / L",
                "label": "Move through the grid"
            },
            {
                "keys": "Home / End",
                "label": "First / last tile"
            },
            {
                "keys": "Tab / Shift+Tab",
                "label": "Next / previous section"
            },
            {
                "keys": "1 to 8",
                "label": "Choose section"
            },
            {
                "keys": "/",
                "label": "Search"
            },
            {
                "keys": "F9",
                "label": "Toggle folders"
            },
            {
                "keys": "R",
                "label": "Refresh"
            }
        ]
    },
    {
        "title": "Organization",
        "rows": [
            {
                "keys": "X",
                "label": "Select tile"
            },
            {
                "keys": "Ctrl+A",
                "label": "Select all"
            },
            {
                "keys": "Alt+0 to Alt+5",
                "label": "Clear / set rating"
            },
            {
                "keys": "Ctrl+Z",
                "label": "Undo"
            },
            {
                "keys": "Shift+Del",
                "label": "Delete permanently"
            }
        ]
    },
    {
        "title": "Window",
        "rows": [
            {
                "keys": "Ctrl++ / Ctrl+= / Ctrl+-",
                "label": "Change tile size"
            },
            {
                "keys": "Ctrl+0",
                "label": "Reset tile size"
            },
            {
                "keys": "Esc",
                "label": "Close sheet, clear selection or close window"
            },
            {
                "keys": "Ctrl+Q",
                "label": "Quit"
            },
            {
                "keys": "? / F1",
                "label": "Help"
            }
        ]
    }
]
    readonly property var previewGroups: [
    {
        "title": "Navigation",
        "rows": [
            {
                "keys": "Left / Right",
                "label": "Previous / next file"
            },
            {
                "keys": "Tab / Shift+Tab",
                "label": "Actions / preview focus"
            },
            {
                "keys": "Esc",
                "label": "Close preview"
            },
            {
                "keys": "Alt+C",
                "label": "Edit caption"
            },
            {
                "keys": "? / F1",
                "label": "Help"
            }
        ]
    },
    {
        "title": "View",
        "rows": [
            {
                "keys": "F5",
                "label": "Slideshow"
            },
            {
                "keys": "F11",
                "label": "Full screen"
            },
            {
                "keys": "I",
                "label": "Details"
            },
            {
                "keys": "+ / = / -",
                "label": "Zoom in / out"
            },
            {
                "keys": "0 / 1",
                "label": "Fit / actual size (pictures)"
            },
            {
                "keys": "R",
                "label": "Rotate picture"
            },
            {
                "keys": "Shift+H / Shift+V",
                "label": "Flip picture"
            },
            {
                "keys": "Page Up / Page Down",
                "label": "Previous / next PDF page"
            }
        ]
    },
    {
        "title": "Playback",
        "rows": [
            {
                "keys": "Space",
                "label": "Play / pause or primary action"
            },
            {
                "keys": "J / L",
                "label": "Seek five seconds"
            },
            {
                "keys": ", / .",
                "label": "Previous / next frame"
            },
            {
                "keys": "M",
                "label": "Mute"
            },
            {
                "keys": "Up / Down / 0 / 9",
                "label": "Volume up / down"
            },
            {
                "keys": "[ / ]",
                "label": "Playback speed"
            },
            {
                "keys": "Backspace",
                "label": "Normal speed"
            },
            {
                "keys": "Home / End",
                "label": "Start / end"
            }
        ]
    }
]
    function keyLabel(code) {
        const names = {16777234: "←", 16777236: "→", 16777235: "↑", 16777237: "↓"}
        if (names[code]) return names[code]
        if (code >= 32 && code <= 126) return String.fromCharCode(code)
        for (const name in quickKeys) if (quickKeys[name] === code) return name
        return ""
    }
    function remapQuickLabel(label) {
        return label.replace(/\b[A-Z0-9]\b/g, function(token) {
            return quickKeys[token] === undefined ? token : keyLabel(quickKeys[token])
        })
    }
    function groups(context, video, animated, audio, sidecar, filter) {
        const source = context === "quick" ? quickGroups : context === "preview" ? previewGroups : libraryGroups
        let result = source.map(function(group) {
            return {title: group.title, rows: group.rows.filter(function(row) {
                return (!row.still || !video) && (!row.video || video)
                    && (!row.animation || animated) && (!row.audio || audio)
                    && (!row.sidecar || sidecar)
            })}
        })
        if (context !== "quick") {
            const ids = ["annotate", "send", "develop", "trim", "play", "frame", "background", "matte", "corrections", "correctionsbatch", "compare", "ocr", "export", "choose-editor", "copy", "files", "open-with", "rename", "favorite", "hide", "trash"]
            result.push({title: "Actions", rows: ids.map(function(id) {
                return {keys: Registry.shortcutFor(id), label: id.replace(/-/g, " ")}
            }).filter(function(row) { return row.keys !== "" })})
            result.push({title: "Clipboard and editors", rows: [
                {keys: "Ctrl+Shift+C", label: "Copy path"},
                {keys: "D / O", label: "Develop RAW"},
                {keys: "Shift+D / Shift+O", label: "Choose RAW editor"}]})
        }
        const needle = filter.toLowerCase().trim()
        return result.map(function(group) {
            return {title: group.title, rows: group.rows.map(function(row) {
                return {keys: context === "quick" ? remapQuickLabel(row.keys) : row.keys, label: row.label}
            }).filter(function(row) {
                return (group.title + " " + row.keys + " " + row.label).toLowerCase().indexOf(needle) >= 0
            })}
        }).filter(function(group) { return group.rows.length > 0 })
    }
    readonly property var quickHints: ({
        library: "Enter", copy: "Y", "open-with": "Ctrl+O", annotate: "A", develop: "D", "choose-editor": "Shift+D", send: "S", trim: "T", frame: "G",
        play: "P", rotate: "R", slideshow: "F5", fullscreen: "F", info: "I", filmstrip: "B",
        "video-loop": "Shift+L", "audio-track": "Shift+A",
        "subtitles-earlier": "Z", "subtitles-later": "X",
        favorite: "V", trash: "Del", "copy-path": "Ctrl+Shift+C", "fit-width": "W", fill: "Shift+W", help: "? / F1", fit: "0", actual: "1"
    })
    readonly property var previewKeys: ({
        previous: { key: Qt.Key_Left, label: "Left" },
        next: { key: Qt.Key_Right, label: "Right" },
        slideshow: { key: Qt.Key_F5, label: "F5" },
        info: { key: Qt.Key_I, label: "I" },
        fullscreen: { key: Qt.Key_F11, label: "F11" },
        fit: { key: Qt.Key_0, label: "0" },
        actual: { key: Qt.Key_1, label: "1" },
        zoomOut: { key: Qt.Key_Minus, label: "−" },
        zoomIn: { key: Qt.Key_Plus, label: "+" },
        rotate: { key: Qt.Key_R, label: "R" },
        flipHorizontal: { key: Qt.Key_H, label: "Shift+H" },
        flipVertical: { key: Qt.Key_V, label: "Shift+V" }
    })

    readonly property var quickKeys: ({
        0: Qt.Key_0,
        1: Qt.Key_1,
        5: Qt.Key_5,
        A: Qt.Key_A,
        B: Qt.Key_B,
        Backspace: Qt.Key_Backspace,
        Backtab: Qt.Key_Backtab,
        BracketLeft: Qt.Key_BracketLeft,
        BracketRight: Qt.Key_BracketRight,
        C: Qt.Key_C,
        Comma: Qt.Key_Comma,
        D: Qt.Key_D,
        Delete: Qt.Key_Delete,
        Down: Qt.Key_Down,
        End: Qt.Key_End,
        Enter: Qt.Key_Enter,
        Equal: Qt.Key_Equal,
        Escape: Qt.Key_Escape,
        F: Qt.Key_F,
        F1: Qt.Key_F1,
        F11: Qt.Key_F11,
        F5: Qt.Key_F5,
        G: Qt.Key_G,
        Home: Qt.Key_Home,
        I: Qt.Key_I,
        J: Qt.Key_J,
        K: Qt.Key_K,
        L: Qt.Key_L,
        Left: Qt.Key_Left,
        M: Qt.Key_M,
        Menu: Qt.Key_Menu,
        Minus: Qt.Key_Minus,
        O: Qt.Key_O,
        P: Qt.Key_P,
        PageDown: Qt.Key_PageDown,
        PageUp: Qt.Key_PageUp,
        Period: Qt.Key_Period,
        Plus: Qt.Key_Plus,
        Q: Qt.Key_Q,
        Question: Qt.Key_Question,
        R: Qt.Key_R,
        Return: Qt.Key_Return,
        Right: Qt.Key_Right,
        S: Qt.Key_S,
        Space: Qt.Key_Space,
        T: Qt.Key_T,
        Tab: Qt.Key_Tab,
        Underscore: Qt.Key_Underscore,
        Up: Qt.Key_Up,
        V: Qt.Key_V,
        W: Qt.Key_W,
        X: Qt.Key_X,
        Y: Qt.Key_Y,
        Z: Qt.Key_Z
    })
    readonly property var libraryKeys: ({
        Backtab: Qt.Key_Backtab,
        Delete: Qt.Key_Delete,
        End: Qt.Key_End,
        Enter: Qt.Key_Enter,
        H: Qt.Key_H,
        Home: Qt.Key_Home,
        J: Qt.Key_J,
        K: Qt.Key_K,
        L: Qt.Key_L,
        Return: Qt.Key_Return,
        Space: Qt.Key_Space,
        Tab: Qt.Key_Tab,
        X: Qt.Key_X
    })
    readonly property var libraryBindings: ({
        binding0: ["D", "O"],
        binding1: ["Shift+D", "Shift+O"],
        binding2: ["Ctrl+Z"],
        binding3: ["Shift+Del"],
        binding4: ["R"],
        binding5: ["/"],
        binding6: ["Ctrl+A"],
        binding7: ["F9"],
        binding8: ["Ctrl++", "Ctrl+="],
        binding9: ["Ctrl+-"],
        binding10: ["Ctrl+0"],
        binding11: ["Escape"],
        binding12: ["Ctrl+Shift+C"]
    })
}
