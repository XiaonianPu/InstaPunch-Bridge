// swift-tools-version:5.9
import PackageDescription

// Mac test relay: same protocol v1 framing as the firmware (main/frame.c is
// symlinked into BridgeFrame); downstream classic SPP devices go through
// IOBluetooth RFCOMM instead of Bluedroid. Build the .app with
// ./build-app.sh (Bluetooth needs the Info.plist usage string).
let package = Package(
    name: "BridgeMac",
    platforms: [.macOS(.v14)],
    targets: [
        .target(name: "BridgeFrame", path: "Sources/BridgeFrame"),
        .executableTarget(
            name: "BridgeMac",
            dependencies: ["BridgeFrame"],
            path: "Sources/BridgeMac",
            linkerSettings: [.linkedFramework("CoreBluetooth"), .linkedFramework("IOBluetooth")]
        ),
    ],
    swiftLanguageVersions: [.v5]
)
