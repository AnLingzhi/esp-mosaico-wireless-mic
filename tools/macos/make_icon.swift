// SPDX-License-Identifier: Apache-2.0
import AppKit

// Code-native vector artwork, rendered at each macOS icon resolution.
let output = URL(fileURLWithPath: CommandLine.arguments[1])
let iconset = output.appendingPathComponent("Mosaico.iconset")
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for size in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let pixels = size * scale
        let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels,
            pixelsHigh: pixels, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
            isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
        let transform = NSAffineTransform()
        transform.scale(by: CGFloat(pixels) / 1024)
        transform.concat()
        let tile = NSBezierPath(roundedRect: NSRect(x: 80, y: 80, width: 864, height: 864),
                                xRadius: 190, yRadius: 190)
        NSGradient(starting: NSColor(srgbRed: 0.34, green: 0.52, blue: 0.65, alpha: 1),
                   ending: NSColor(srgbRed: 0.13, green: 0.25, blue: 0.34, alpha: 1))!
            .draw(in: tile, angle: -70)
        let cream = NSColor(srgbRed: 1, green: 0.97, blue: 0.9, alpha: 1)
        cream.setFill()
        NSBezierPath(roundedRect: NSRect(x: 408, y: 420, width: 208, height: 330),
                     xRadius: 104, yRadius: 104).fill()
        cream.setStroke()
        let cradle = NSBezierPath()
        cradle.lineWidth = 35
        cradle.lineCapStyle = .round
        cradle.move(to: NSPoint(x: 340, y: 520))
        cradle.line(to: NSPoint(x: 340, y: 440))
        cradle.curve(to: NSPoint(x: 684, y: 440), controlPoint1: NSPoint(x: 340, y: 225),
                     controlPoint2: NSPoint(x: 684, y: 225))
        cradle.line(to: NSPoint(x: 684, y: 520))
        cradle.move(to: NSPoint(x: 512, y: 280))
        cradle.line(to: NSPoint(x: 512, y: 220))
        cradle.move(to: NSPoint(x: 430, y: 220))
        cradle.line(to: NSPoint(x: 594, y: 220))
        cradle.stroke()
        NSColor(srgbRed: 0.91, green: 0.55, blue: 0.45, alpha: 1).setFill()
        NSBezierPath(ovalIn: NSRect(x: 724, y: 712, width: 100, height: 100)).fill()
        NSGraphicsContext.restoreGraphicsState()
        let suffix = scale == 2 ? "@2x" : ""
        try bitmap.representation(using: .png, properties: [:])!.write(
            to: iconset.appendingPathComponent("icon_\(size)x\(size)\(suffix).png"))
    }
}
