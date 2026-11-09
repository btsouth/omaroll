pragma Singleton
import QtQuick

// Stable tokens identify existing handler branches. Change the physical keys
// here; help, toolbar hints and keyboard dispatch derive from these rows.
QtObject {
    id: root
    property var activeTip: null
    readonly property var rows: Settings.shortcutRows
    function sequence(context, id) {
        for (const row of rows)
            if (row.context === context && row.id === id)
                return row.keys;
        return [];
    }
    function display(keys) {
        if (keys.length === 6 && keys[0] === "Alt+0" && keys[5] === "Alt+5") return "Alt+0 to Alt+5";
        if (keys.length === 8 && keys[0] === "1" && keys[7] === "8") return "1 to 8";
        return keys.map(function (key) {
            return key.replace("Return", "Enter").replace("Backtab", "Shift+Tab").replace("PgUp", "Page Up").replace("PgDown", "Page Down");
        }).filter(function (key, index, all) {
            return all.indexOf(key) === index;
        }).join(" / ");
    }
    function code(sequence) {
        let key = sequence.substring(sequence.lastIndexOf("+") + 1);
        if (sequence.endsWith("++") || sequence === "+")
            key = "Plus";
        const names = {
            "0": "0",
            "1": "1",
            "2": "2",
            "3": "3",
            "4": "4",
            "5": "5",
            "-": "Minus",
            "_": "Underscore",
            "=": "Equal",
            ",": "Comma",
            ".": "Period",
            "[": "BracketLeft",
            "]": "BracketRight",
            "?": "Question",
            "/": "Slash",
            "Esc": "Escape",
            "Del": "Delete",
            "PgUp": "PageUp",
            "PgDown": "PageDown"
        };
        return Qt["Key_" + (names[key] || key)];
    }
    function keyMap(context) {
        let result = {};
        for (const row of rows) {
            if (row.context !== context)
                continue;
            for (let i = 0; i < row.keys.length; ++i) {
                const key = row.keys[i];
                const physical = code(key);
                let token = row.tokens[i];
                if (!token) {
                    token = key.substring(key.lastIndexOf("+") + 1);
                    const names = {
                        "+": "Plus",
                        "": "Plus",
                        "=": "Equal",
                        "-": "Minus",
                        "_": "Underscore",
                        ",": "Comma",
                        ".": "Period",
                        "[": "BracketLeft",
                        "]": "BracketRight",
                        "?": "Question",
                        "Esc": "Escape",
                        "Del": "Delete",
                        "PgUp": "PageUp",
                        "PgDown": "PageDown"
                    };
                    token = names[token] || token;
                }
                result[token] = physical;
            }
        }
        return result;
    }
    readonly property var previewKeyCodes: keyMap("preview")
    readonly property var quickKeys: keyMap("quick")
    readonly property var libraryKeys: keyMap("library")
    readonly property var quickHints: {
        let hints = {};
        for (const row of rows)
            if (row.context === "quick")
                // Menus show one key, as they did; Shift+Del beside
                // "Move to Trash" would suggest the wrong action.
                hints[row.id] = display([row.keys[0]]);
        hints["subtitles-earlier"] = sequence("quick", "subtitle-offset")[0];
        hints["subtitles-later"] = sequence("quick", "subtitle-offset")[1];
        return hints;
    }
    readonly property var previewKeys: {
        let keys = {};
        for (const row of rows)
            if (row.context === "preview")
                keys[row.id] = {
                    key: code(row.keys[0]),
                    label: display(row.keys)
                };
        return keys;
    }
    // Key caps for help: one list per alternative, one cap per key, with
    // the sets help would otherwise spell out key by key collapsed:
    // "Shift+Left / Shift+Right / Shift+Up / Shift+Down" is Shift + Arrows.
    function caps(keys, literal) {
        if (literal)
            return keys.map(function (key) { return [key]; });
        if (keys.length === 6 && keys[0] === "Alt+0" && keys[5] === "Alt+5")
            return [["Alt", "0–5"]];
        if (keys.length === 8 && keys[0] === "1" && keys[7] === "8")
            return [["1–8"]];
        const glyphs = {
            "Left": "←", "Right": "→", "Up": "↑", "Down": "↓",
            "Return": "Enter", "Esc": "Esc", "Del": "Delete", "Backtab": "Shift+Tab",
            "PgUp": "Page Up", "PgDown": "Page Down"
        };
        function split(key) {
            if (key === "+") return ["+"];
            const parts = key.endsWith("++") ? key.slice(0, -2).split("+").concat(["+"]) : key.split("+");
            const base = parts[parts.length - 1];
            // Only a glyph can stand for more than one key ("Shift+Tab");
            // the base may itself be "+" (Ctrl++).
            if (glyphs[base] === undefined) return parts;
            return parts.slice(0, -1).concat(glyphs[base].split("+"));
        }
        const all = keys.map(split);
        // A modifier, or none, with all four arrows.
        if (all.length === 4) {
            const mods = all[0].slice(0, -1).join("+");
            const bases = all.map(function (key) { return key[key.length - 1]; }).sort().join("");
            const sameMods = all.every(function (key) { return key.slice(0, -1).join("+") === mods; });
            if (sameMods && bases === ["←", "→", "↑", "↓"].sort().join(""))
                return [all[0].slice(0, -1).concat(["Arrows"])];
        }
        const seen = [];
        return all.filter(function (key) {
            const text = key.join("+");
            if (seen.indexOf(text) >= 0) return false;
            seen.push(text);
            return true;
        });
    }
    function groups(context, video, animated, audio, sidecar, filter) {
        let entries = rows.filter(function (row) {
            // The library help also teaches the search syntax.
            const inContext = row.context === context
                              || (context === "library" && row.context === "search");
            return inContext && (row.flag === "" || (row.flag === "still" && !video) || (row.flag === "video" && video) || (row.flag === "motion" && (video || animated)) || (row.flag === "audio" && audio) || (row.flag === "sidecar" && sidecar));
        }).map(function (row) {
            return {
                keys: display(row.keys),
                caps: caps(row.keys, row.context === "search"),
                label: row.label,
                group: row.group
            };
        });
        let groups = [];
        const needle = filter.toLowerCase().trim();
        for (const row of entries) {
            if ((row.keys + " " + row.label + " " + row.group).toLowerCase().indexOf(needle) < 0)
                continue;
            let group = groups.find(function (group) {
                return group.title === row.group;
            });
            if (!group) {
                group = {
                    title: row.group,
                    rows: []
                };
                groups.push(group);
            }
            group.rows.push(row);
        }
        return groups;
    }
}
