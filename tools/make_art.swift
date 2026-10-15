// Draws the app icon and launch images into packaging/app/. Run: swift tools/make_art.swift
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let out = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "packaging/app")
let space = CGColorSpace(name: CGColorSpace.sRGB)!

func color(_ hex: Int, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: space, components: [CGFloat((hex >> 16) & 255) / 255, CGFloat((hex >> 8) & 255) / 255,
                                            CGFloat(hex & 255) / 255, alpha])!
}

func png(_ name: String, _ w: Int, _ h: Int, _ draw: (CGContext, CGFloat, CGFloat) -> Void) {
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    draw(ctx, CGFloat(w), CGFloat(h))
    let dest = CGImageDestinationCreateWithURL(out.appendingPathComponent(name) as CFURL,
                                               UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
    CGImageDestinationFinalize(dest)
}

// Green tile, white battery outline filled to 80%: a battery that stops short of full.
func icon(_ ctx: CGContext, _ s: CGFloat, _: CGFloat) {
    let grad = CGGradient(colorsSpace: space, colors: [color(0x3DD66B), color(0x1F9A45)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(grad, start: CGPoint(x: 0, y: s), end: CGPoint(x: 0, y: 0), options: [])

    let line = max(1.5, s * 0.05)
    let body = CGRect(x: s * 0.17, y: s * 0.33, width: s * 0.58, height: s * 0.34)
    ctx.setStrokeColor(color(0xFFFFFF))
    ctx.setLineWidth(line)
    ctx.addPath(CGPath(roundedRect: body, cornerWidth: s * 0.075, cornerHeight: s * 0.075, transform: nil))
    ctx.strokePath()

    ctx.setFillColor(color(0xFFFFFF))
    let nub = CGRect(x: body.maxX + line * 0.9, y: s * 0.43, width: s * 0.055, height: s * 0.14)
    ctx.addPath(CGPath(roundedRect: nub, cornerWidth: s * 0.025, cornerHeight: s * 0.025, transform: nil))
    ctx.fillPath()

    let inner = body.insetBy(dx: line * 1.6, dy: line * 1.6)
    let fill = CGRect(x: inner.minX, y: inner.minY, width: inner.width * 0.8, height: inner.height)
    ctx.addPath(CGPath(roundedRect: fill, cornerWidth: s * 0.03, cornerHeight: s * 0.03, transform: nil))
    ctx.fillPath()
}

// The grouped-table background with a navigation bar, so launch blends into the app.
func launch(_ scale: CGFloat) -> (CGContext, CGFloat, CGFloat) -> Void {
    return { ctx, w, h in
        ctx.setFillColor(color(0xEFEFF4))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        ctx.setFillColor(color(0xF7F7F7))
        ctx.fill(CGRect(x: 0, y: h - 64 * scale, width: w, height: 64 * scale))
        ctx.setFillColor(color(0xB2B2B2))
        ctx.fill(CGRect(x: 0, y: h - 64 * scale - max(1, scale / 2), width: w, height: max(1, scale / 2)))
    }
}

try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
for (pt, scales) in [(76.0, [1, 2]), (40.0, [1, 2]), (29.0, [1, 2])] as [(Double, [Int])] {
    for sc in scales {
        let px = Int(pt * Double(sc))
        let name = "AppIcon\(Int(pt))x\(Int(pt))\(sc == 2 ? "@2x" : "")~ipad.png"
        png(name, px, px, icon)
    }
}
for sc in [1, 2] {
    let suffix = sc == 2 ? "@2x" : ""
    png("LaunchImage-Portrait\(suffix)~ipad.png", 768 * sc, 1024 * sc, launch(CGFloat(sc)))
    png("LaunchImage-Landscape\(suffix)~ipad.png", 1024 * sc, 768 * sc, launch(CGFloat(sc)))
}
png("icon-preview.png", 512, 512, icon)
print("art written to \(out.path)")
