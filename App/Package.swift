// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "HDMIVolume",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "HDMIVolumeCore", targets: ["HDMIVolumeCore"]),
        .executable(name: "HDMIVolume", targets: ["HDMIVolumeApp"])
    ],
    targets: [
        .target(
            name: "HDMIVolumeCore",
            path: "Sources/HDMIVolumeCore"
        ),
        .executableTarget(
            name: "HDMIVolumeApp",
            dependencies: ["HDMIVolumeCore"],
            path: "Sources/HDMIVolumeApp"
        ),
        .testTarget(
            name: "HDMIVolumeTests",
            dependencies: ["HDMIVolumeCore"],
            path: "Tests/HDMIVolumeTests"
        )
    ]
)
