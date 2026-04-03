/*
 * Copyright (c) 2026 Alpine on iOS contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

import SwiftUI
import MetalKit

struct DisplayContainerView: View {
    @State private var scale: CGFloat = 1.0
    @State private var lastScale: CGFloat = 1.0
    @State private var offset: CGSize = .zero
    @State private var lastOffset: CGSize = .zero

    var body: some View {
        ZStack {
            Color.black.edgesIgnoringSafeArea(.all)

            DisplayView()
                .scaleEffect(scale)
                .offset(offset)
                .gesture(
                    MagnificationGesture()
                        .onChanged { value in
                            scale = max(0.5, min(3.0, lastScale * value))
                        }
                        .onEnded { _ in
                            lastScale = scale
                        }
                )
                .simultaneousGesture(
                    DragGesture(minimumDistance: 20)
                        .onChanged { value in
                            offset = CGSize(
                                width: lastOffset.width + value.translation.width,
                                height: lastOffset.height + value.translation.height
                            )
                        }
                        .onEnded { _ in
                            lastOffset = offset
                        }
                )

            /* Virtual cursor indicator */
            VStack {
                Spacer()
                HStack {
                    Text("Pinch to zoom, drag to pan")
                        .font(.caption)
                        .foregroundColor(.white.opacity(0.5))
                        .padding(8)
                        .background(Color.black.opacity(0.5))
                        .cornerRadius(8)
                    Spacer()
                }
                .padding()
            }
        }
    }
}

struct DisplayView: UIViewRepresentable {

    func makeUIView(context: Context) -> MTKView {
        let view = MTKView()
        guard let device = MTLCreateSystemDefaultDevice() else {
            return view
        }
        view.device = device
        view.delegate = context.coordinator
        view.preferredFramesPerSecond = 60
        view.colorPixelFormat = .bgra8Unorm
        view.enableSetNeedsDisplay = false
        view.isPaused = false
        view.isMultipleTouchEnabled = true
        context.coordinator.setup(device: device)

        /* Add touch handler for mouse input */
        let tap = UITapGestureRecognizer(target: context.coordinator,
                                          action: #selector(Coordinator.handleTap(_:)))
        view.addGestureRecognizer(tap)

        let pan = UIPanGestureRecognizer(target: context.coordinator,
                                          action: #selector(Coordinator.handlePan(_:)))
        pan.maximumNumberOfTouches = 1
        view.addGestureRecognizer(pan)

        return view
    }

    func updateUIView(_ uiView: MTKView, context: Context) {}

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    class Coordinator: NSObject, MTKViewDelegate {
        private var device: MTLDevice?
        private var commandQueue: MTLCommandQueue?
        private var texture: MTLTexture?
        private var lastWidth: UInt32 = 0
        private var lastHeight: UInt32 = 0

        func setup(device: MTLDevice) {
            self.device = device
            self.commandQueue = device.makeCommandQueue()
        }

        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

        func draw(in view: MTKView) {
            guard let pixels = fb_get_pixels() else { return }
            var width: UInt32 = 0
            var height: UInt32 = 0
            fb_get_size(&width, &height)
            if width == 0 || height == 0 { return }

            guard let device = self.device,
                  let commandQueue = self.commandQueue,
                  let drawable = view.currentDrawable,
                  let descriptor = view.currentRenderPassDescriptor
            else { return }

            /* Recreate texture if dimensions changed. */
            if width != lastWidth || height != lastHeight {
                let desc = MTLTextureDescriptor.texture2DDescriptor(
                    pixelFormat: .bgra8Unorm,
                    width: Int(width),
                    height: Int(height),
                    mipmapped: false)
                desc.usage = [.shaderRead]
                texture = device.makeTexture(descriptor: desc)
                lastWidth = width
                lastHeight = height
            }

            guard let texture = self.texture else { return }

            /* Upload pixels to texture. */
            let stride = Int(width) * 4
            let region = MTLRegionMake2D(0, 0, Int(width), Int(height))
            texture.replace(
                region: region,
                mipmapLevel: 0,
                withBytes: pixels,
                bytesPerRow: stride)

            /* Blit texture to drawable. */
            guard let commandBuffer = commandQueue.makeCommandBuffer(),
                  let blitEncoder = commandBuffer.makeBlitCommandEncoder()
            else { return }

            let srcSize = MTLSizeMake(Int(width), Int(height), 1)
            let dstSize = MTLSizeMake(
                drawable.texture.width,
                drawable.texture.height, 1)
            let srcOrigin = MTLOriginMake(0, 0, 0)
            let dstOrigin = MTLOriginMake(0, 0, 0)

            /* Scale by copying; use the smaller of src/dst. */
            let copyWidth = min(srcSize.width, dstSize.width)
            let copyHeight = min(srcSize.height, dstSize.height)
            let copySize = MTLSizeMake(copyWidth, copyHeight, 1)

            blitEncoder.copy(
                from: texture,
                sourceSlice: 0,
                sourceLevel: 0,
                sourceOrigin: srcOrigin,
                sourceSize: copySize,
                to: drawable.texture,
                destinationSlice: 0,
                destinationLevel: 0,
                destinationOrigin: dstOrigin)

            blitEncoder.endEncoding()
            commandBuffer.present(drawable)
            commandBuffer.commit()
        }

        /* Touch-to-mouse mapping */

        @objc func handleTap(_ gesture: UITapGestureRecognizer) {
            guard let view = gesture.view else { return }
            let loc = gesture.location(in: view)
            let mapped = mapToFramebuffer(point: loc, viewSize: view.bounds.size)
            input_send_touch(Int32(mapped.x), Int32(mapped.y), 1)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                input_send_touch(Int32(mapped.x), Int32(mapped.y), 0)
            }
        }

        @objc func handlePan(_ gesture: UIPanGestureRecognizer) {
            guard let view = gesture.view else { return }
            let loc = gesture.location(in: view)
            let mapped = mapToFramebuffer(point: loc, viewSize: view.bounds.size)

            switch gesture.state {
            case .began:
                input_send_touch(Int32(mapped.x), Int32(mapped.y), 1)
            case .changed:
                input_send_touch(Int32(mapped.x), Int32(mapped.y), 1)
            case .ended, .cancelled:
                input_send_touch(Int32(mapped.x), Int32(mapped.y), 0)
            default:
                break
            }
        }

        private func mapToFramebuffer(point: CGPoint, viewSize: CGSize) -> CGPoint {
            var w: UInt32 = 0, h: UInt32 = 0
            fb_get_size(&w, &h)
            if w == 0 || h == 0 { return .zero }
            let x = point.x / viewSize.width * CGFloat(w)
            let y = point.y / viewSize.height * CGFloat(h)
            return CGPoint(x: max(0, min(x, CGFloat(w - 1))),
                           y: max(0, min(y, CGFloat(h - 1))))
        }
    }
}
