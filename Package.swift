// swift-tools-version: 5.9
// The input method app (third_party/squirrel, built with xcodebuild) spawns AquaTypeEngine, which speaks the app's JSONL
// contract (AquaTypeUIContract) on top of the decoder's C API (AquaTypeDecoderBridge -> decoder/native/libaquadecoder.dylib
// + MLX).
import Foundation
import PackageDescription

let packageDirectory = URL(fileURLWithPath: #filePath).deletingLastPathComponent().path
let aqLibDir = "\(packageDirectory)/decoder/native"
let aqDylib = "\(aqLibDir)/libaquadecoder.dylib"
// Where MLX's library is found when the engine is run from .build during development (the app bundle carries its own
// copy): AQUATYPE_MLX_LIB, or the mlx package in the project's .venv, whichever Python version that is.
let venvLib = "\(packageDirectory)/.venv/lib"
let mlxLibDir = ProcessInfo.processInfo.environment["AQUATYPE_MLX_LIB"]
    ?? (try? FileManager.default.contentsOfDirectory(atPath: venvLib))?.filter { $0.hasPrefix("python") }.sorted().last
        .map { "\(venvLib)/\($0)/site-packages/mlx/lib" }
    ?? "\(venvLib)/python3/site-packages/mlx/lib"

let package = Package(
    name: "AquaType",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .executable(name: "AquaTypeEngine", targets: ["AquaTypeEngine"]),
        .library(name: "AquaTypeUIContract", targets: ["AquaTypeUIContract"]),
    ],
    targets: [
        .target(
            name: "AquaTypeUIContract",
            path: "Sources/AquaTypeUIContract",
            linkerSettings: [.linkedFramework("CryptoKit")]
        ),
        .target(
            name: "AquaTypeDecoderBridge",
            path: "Sources/AquaTypeDecoderBridge",
            publicHeadersPath: "include",
            linkerSettings: [
                .unsafeFlags([aqDylib, "-Xlinker", "-rpath", "-Xlinker", aqLibDir, "-Xlinker", "-rpath", "-Xlinker", mlxLibDir])
            ]
        ),
        .executableTarget(
            name: "AquaTypeEngine",
            dependencies: ["AquaTypeUIContract", "AquaTypeDecoderBridge"],
            path: "Sources/AquaTypeEngine"
        ),
    ]
)
