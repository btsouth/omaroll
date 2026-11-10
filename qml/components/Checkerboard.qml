import QtQuick

// A checkerboard in the theme's colours, behind a picture with transparency.
// One small tile, drawn by the checker provider and repeated, so a large or
// zoomed picture costs a single texture. The provider answers at once on this
// thread: no image reader is left working when the window closes.
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
        return part(color.r) + part(color.g) + part(color.b)
    }
    readonly property color second: Qt.rgba(base.r + (tint.r - base.r) * tintAmount,
                                            base.g + (tint.g - base.g) * tintAmount,
                                            base.b + (tint.b - base.b) * tintAmount, 1)

    fillMode: Image.Tile
    smooth: false
    asynchronous: false
    cache: true
    source: "image://checker/" + root.hex(root.base) + "/" + root.hex(root.second) + "/" + root.cell
}
