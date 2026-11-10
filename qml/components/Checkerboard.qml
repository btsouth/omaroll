import QtQuick

// A checkerboard in the theme's colours, behind a picture with transparency.
// One small tile, drawn as an SVG with the two colours and repeated, so a
// large or zoomed picture costs a single texture.
Image {
    id: root

    // The two squares: the surface, and the surface tinted toward the text.
    property color base: Theme.surfaceBackground
    property color tint: Theme.foreground
    property real tintAmount: 0.12
    // Each square's side, in logical pixels.
    property int cell: 8

    function hex(color) {
        const part = function (value) {
            return ("0" + Math.round(Math.max(0, Math.min(1, value)) * 255).toString(16)).slice(-2)
        }
        return "#" + part(color.r) + part(color.g) + part(color.b)
    }
    readonly property color second: Qt.rgba(base.r + (tint.r - base.r) * tintAmount,
                                            base.g + (tint.g - base.g) * tintAmount,
                                            base.b + (tint.b - base.b) * tintAmount, 1)

    fillMode: Image.Tile
    smooth: false
    asynchronous: false
    cache: true
    source: {
        const size = root.cell * 2
        const svg = "<svg xmlns='http://www.w3.org/2000/svg' width='" + size + "' height='" + size + "'>"
                  + "<rect width='" + size + "' height='" + size + "' fill='" + root.hex(root.base) + "'/>"
                  + "<rect width='" + root.cell + "' height='" + root.cell + "' fill='" + root.hex(root.second) + "'/>"
                  + "<rect x='" + root.cell + "' y='" + root.cell + "' width='" + root.cell + "' height='"
                  + root.cell + "' fill='" + root.hex(root.second) + "'/></svg>"
        return "data:image/svg+xml," + encodeURIComponent(svg)
    }
}
