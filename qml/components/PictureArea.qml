import QtQuick

// The part of a very large JPEG on screen, decoded at the screen's resolution
// once the view settles, with a quarter of the view as margin to pan into.
// The reduced copy underneath shows meanwhile, and the last area stays until
// the next one arrives. A child of the picture's item, whose own pixels are
// the upright picture's at pictureScale; mapping through it also undoes any
// rotation or flip the view applies.
Image {
    id: area

    // What the picture is seen through, usually a Flickable or a clipping cell.
    required property Item viewport
    property bool wanted: false
    // Logical size of one picture pixel.
    property real pictureScale: 1
    property real devicePixelRatio: 1
    property size fullSize: Qt.size(0, 0)
    property string path: ""
    // function (rect, size): the URL of that area of the picture.
    property var urlFor: null
    // Anything else that moves the view, such as a pan offset or rotation.
    property var viewState: []

    // In the upright picture's own pixels: what is shown, and what is loading,
    // with the scale it was asked for at.
    property rect shown: Qt.rect(0, 0, 0, 0)
    property rect pending: Qt.rect(0, 0, 0, 0)
    property real pendingScale: 0
    property string pendingPath: ""

    x: shown.x * pictureScale
    y: shown.y * pictureScale
    width: shown.width * pictureScale
    height: shown.height * pictureScale
    asynchronous: true
    retainWhileLoading: true
    cache: false
    fillMode: Image.Stretch
    // Not on status: that is Loading again while the next area decodes, and
    // the last one should stay meanwhile.
    visible: wanted && shown.width > 0 && pendingPath === path
    onStatusChanged: {
        if (status === Image.Ready) {
            shown = pending
        } else if (status === Image.Error) {
            // Ask again the next time the view moves.
            pendingScale = 0
        }
    }

    readonly property var view: [wanted, path, pictureScale, viewport.width, viewport.height,
                                 viewport.contentX, viewport.contentY, viewState]
    onViewChanged: {
        if (wanted) {
            settle.restart()
        } else {
            settle.stop()
            source = ""
            shown = Qt.rect(0, 0, 0, 0)
            pendingScale = 0
            pendingPath = ""
        }
    }

    Timer {
        id: settle
        interval: 120
        onTriggered: area.request()
    }

    function request() {
        const picture = area.parent
        const scale = area.pictureScale
        if (!wanted || scale <= 0 || !picture || !urlFor) return
        // The viewport's corners in the picture's pixels.
        let left = Infinity, top = Infinity, right = -Infinity, bottom = -Infinity
        for (const corner of [[0, 0], [viewport.width, 0], [0, viewport.height],
                              [viewport.width, viewport.height]]) {
            const at = picture.mapFromItem(viewport, corner[0], corner[1])
            left = Math.min(left, at.x / scale)
            right = Math.max(right, at.x / scale)
            top = Math.min(top, at.y / scale)
            bottom = Math.max(bottom, at.y / scale)
        }
        const clamp = (v, high) => Math.max(0, Math.min(high, v))
        const fullWidth = area.fullSize.width
        const fullHeight = area.fullSize.height
        left = clamp(left, fullWidth)
        right = clamp(right, fullWidth)
        top = clamp(top, fullHeight)
        bottom = clamp(bottom, fullHeight)
        if (right <= left || bottom <= top) return
        // Already loaded or loading at about this scale, or at the picture's
        // own pixels, which zooming further in cannot sharpen.
        const ownPixels = pendingScale * devicePixelRatio >= 1 && scale * devicePixelRatio >= 1
        if (pendingPath === path && pendingScale > 0
                && (ownPixels || (scale <= pendingScale * 1.01 && scale >= pendingScale * 0.6))
                && left >= pending.x && top >= pending.y
                && right <= pending.x + pending.width
                && bottom <= pending.y + pending.height) {
            return
        }
        const marginX = (right - left) / 4
        const marginY = (bottom - top) / 4
        const x = Math.floor(clamp(left - marginX, fullWidth))
        const y = Math.floor(clamp(top - marginY, fullHeight))
        const rect = Qt.rect(x, y,
                             Math.ceil(clamp(right + marginX, fullWidth)) - x,
                             Math.ceil(clamp(bottom + marginY, fullHeight)) - y)
        // Never more pixels than the picture has there.
        const pixels = Math.min(1, scale * devicePixelRatio)
        const size = Qt.size(Math.max(1, Math.round(rect.width * pixels)),
                             Math.max(1, Math.round(rect.height * pixels)))
        pending = rect
        pendingScale = scale
        pendingPath = path
        source = urlFor(rect, size)
    }
}
