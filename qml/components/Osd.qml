pragma Singleton
import QtQuick

// Colours for controls drawn over a picture or a video. They stay dark in
// every theme, the way mpv, Loupe and Celluloid keep their OSD: a light
// translucent bar over a bright frame washes out both. Only the accent comes
// from the theme, lifted until it reads against the panel.
QtObject {
    id: root

    // The panel behind controls, and the scrim at the top of the viewer.
    readonly property color panel: Qt.rgba(0.07, 0.075, 0.086, 0.84)
    readonly property color panelSolid: Qt.rgba(0.07, 0.075, 0.086, 0.96)
    // The surround behind a picture or video in a light theme, a little darker
    // than the panels so the toolbar still reads as a raised surface.
    readonly property color backdrop: Qt.rgba(0.055, 0.058, 0.066, 1)
    readonly property color border: Qt.rgba(1, 1, 1, 0.14)
    readonly property color scrimTop: Qt.rgba(0, 0, 0, 0.74)
    readonly property color scrimMiddle: Qt.rgba(0, 0, 0, 0.42)

    readonly property color foreground: "#eef0f3"
    readonly property color brightForeground: "#ffffff"
    readonly property color mutedText: "#bfc2c9"
    readonly property color track: Qt.rgba(1, 1, 1, 0.42)
    readonly property color hover: Qt.rgba(1, 1, 1, 0.14)
    readonly property color divider: Qt.rgba(1, 1, 1, 0.18)

    // The panel over a white frame, the worst case for light text on it.
    readonly property color worstBacking: root.over(root.panel, Qt.rgba(1, 1, 1, 1))
    readonly property color accent: root.legible(Theme.accent, root.worstBacking, 4.5)

    function over(top, bottom) {
        const a = top.a
        return Qt.rgba(top.r * a + bottom.r * (1 - a), top.g * a + bottom.g * (1 - a),
                       top.b * a + bottom.b * (1 - a), 1)
    }

    function channel(value) {
        return value <= 0.04045 ? value / 12.92 : Math.pow((value + 0.055) / 1.055, 2.4)
    }

    function luminance(colour) {
        return 0.2126 * root.channel(colour.r) + 0.7152 * root.channel(colour.g)
               + 0.0722 * root.channel(colour.b)
    }

    // WCAG contrast ratio, 1 to 21.
    function contrast(first, second) {
        const a = root.luminance(first)
        const b = root.luminance(second)
        return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05)
    }

    // The colour, mixed toward white until it reaches the ratio against the
    // backing. A theme accent that is already bright enough is unchanged.
    function legible(colour, backing, ratio) {
        let mixed = Qt.rgba(colour.r, colour.g, colour.b, 1)
        for (let step = 0; step < 20 && root.contrast(mixed, backing) < ratio; ++step) {
            mixed = Qt.rgba(mixed.r + (1 - mixed.r) * 0.12, mixed.g + (1 - mixed.g) * 0.12,
                            mixed.b + (1 - mixed.b) * 0.12, 1)
        }
        return mixed
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }
}
