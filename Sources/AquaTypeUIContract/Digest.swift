import CryptoKit
import Foundation

/// Lowercase hexadecimal, in one place, by table lookup rather than a format string per byte.
private func lowercaseHexadecimal<S: Sequence>(_ bytes: S) -> String where S.Element == UInt8 {
    let digits: [UInt8] = Array("0123456789abcdef".utf8)
    var out = [UInt8]()
    out.reserveCapacity(64)
    for byte in bytes {
        out.append(digits[Int(byte >> 4)])
        out.append(digits[Int(byte & 0xF)])
    }
    return String(decoding: out, as: UTF8.self)
}

public enum HybridDigest {
    public static func sha256Hex(_ data: Data) -> String {
        lowercaseHexadecimal(SHA256.hash(data: data))
    }

    public static func sha256File(_ url: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var hasher = SHA256()
        // A pool per chunk: each 1 MB chunk is an autoreleased NSData, and the engine is a child process without a run
        // loop, so without one the whole 73 MB dictionary store would stay in memory for the life of the process.
        while true {
            let done: Bool = try autoreleasepool {
                let data = try handle.read(upToCount: 1 << 20) ?? Data()
                if data.isEmpty { return true }
                hasher.update(data: data)
                return false
            }
            if done { break }
        }
        return lowercaseHexadecimal(hasher.finalize())
    }
}

public extension Data {
    var lowercaseHex: String {
        lowercaseHexadecimal(self)
    }
}
