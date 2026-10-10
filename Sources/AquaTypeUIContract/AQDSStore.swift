import Darwin
import Foundation

private struct AQDSBuilderManifest: Decodable {
    let implementationSHA256: String

    enum CodingKeys: String, CodingKey {
        case implementationSHA256 = "implementation_sha256"
    }
}

private struct AQDSSourceManifest: Decodable {
    let name: String
    let sha256: String
    let bytes: Int
}

private struct AQDSSectionManifest: Decodable {
    let offset: UInt64
    let bytes: UInt64
    let count: UInt64
    let recordBytes: UInt64

    enum CodingKeys: String, CodingKey {
        case offset, bytes, count
        case recordBytes = "record_bytes"
    }
}

private struct AQDSManifest: Decodable {
    let format: String
    let version: Int
    let endian: String
    let kind: String
    let binaryBytes: Int
    let binarySHA256: String
    let payloadSHA256: String
    let sourceAggregateSHA256: String
    let builder: AQDSBuilderManifest
    let sources: [AQDSSourceManifest]
    let sections: [String: AQDSSectionManifest]
    let limit: Int?

    enum CodingKeys: String, CodingKey {
        case format, version, endian, kind, builder, sources, sections, limit
        case binaryBytes = "binary_bytes"
        case binarySHA256 = "binary_sha256"
        case payloadSHA256 = "payload_sha256"
        case sourceAggregateSHA256 = "source_aggregate_sha256"
    }
}

private final class ReadOnlyMappedFile {
    let url: URL
    let count: Int
    private let descriptor: Int32
    private let pointer: UnsafeMutableRawPointer
    let bytes: UnsafeRawBufferPointer

    init(url: URL) throws {
        self.url = url
        descriptor = open(url.path, O_RDONLY | O_CLOEXEC)
        guard descriptor >= 0 else {
            throw UIContractError.invalidArtifact("cannot open AQDS: \(url.path)")
        }
        var value = stat()
        guard fstat(descriptor, &value) == 0, value.st_size > 0,
              let fileCount = Int(exactly: value.st_size) else {
            close(descriptor)
            throw UIContractError.invalidArtifact("cannot stat AQDS: \(url.path)")
        }
        count = fileCount
        guard let mapped = mmap(nil, count, PROT_READ, MAP_PRIVATE, descriptor, 0),
              mapped != MAP_FAILED else {
            close(descriptor)
            throw UIContractError.invalidArtifact("cannot mmap AQDS: \(url.path)")
        }
        pointer = mapped
        bytes = UnsafeRawBufferPointer(start: mapped, count: count)
    }

    deinit {
        munmap(pointer, count)
        close(descriptor)
    }

    func require(offset: Int, length: Int, label: String) throws -> Range<Int> {
        guard offset >= 0, length >= 0, offset <= count, length <= count - offset else {
            throw UIContractError.invalidArtifact("AQDS \(label) range out of bounds")
        }
        return offset..<(offset + length)
    }

    /// A single byte, through the same bounds check as every other read: an `UnsafeRawBufferPointer` subscript checks
    /// nothing in a release build.
    func u8(_ offset: Int, _ label: String) throws -> UInt8 {
        let range = try require(offset: offset, length: 1, label: label)
        return bytes[range.lowerBound]
    }

    func u16(_ offset: Int, _ label: String) throws -> UInt16 {
        let range = try require(offset: offset, length: 2, label: label)
        return UInt16(bytes[range.lowerBound]) | (UInt16(bytes[range.lowerBound + 1]) << 8)
    }

    func u24(_ offset: Int, _ label: String) throws -> UInt32 {
        let range = try require(offset: offset, length: 3, label: label)
        return UInt32(bytes[range.lowerBound])
            | (UInt32(bytes[range.lowerBound + 1]) << 8)
            | (UInt32(bytes[range.lowerBound + 2]) << 16)
    }

    func u32(_ offset: Int, _ label: String) throws -> UInt32 {
        let range = try require(offset: offset, length: 4, label: label)
        return (0..<4).reduce(UInt32(0)) {
            $0 | (UInt32(bytes[range.lowerBound + $1]) << UInt32($1 * 8))
        }
    }

    func u64(_ offset: Int, _ label: String) throws -> UInt64 {
        let range = try require(offset: offset, length: 8, label: label)
        return (0..<8).reduce(UInt64(0)) {
            $0 | (UInt64(bytes[range.lowerBound + $1]) << UInt64($1 * 8))
        }
    }

    func data(offset: Int, length: Int, label: String) throws -> Data {
        let range = try require(offset: offset, length: length, label: label)
        return Data(bytes[range])
    }

    func utf8(offset: Int, length: Int, label: String) throws -> Data {
        let value = try data(offset: offset, length: length, label: label)
        guard String(data: value, encoding: .utf8) != nil else {
            throw UIContractError.invalidUTF8("AQDS \(label)")
        }
        return value
    }
}

private func aqdsFNV1a64(_ data: Data) -> UInt64 {
    var value: UInt64 = 0xcbf29ce484222325
    for byte in data {
        value ^= UInt64(byte)
        value &*= 0x100000001b3
    }
    return value
}

/// Lexicographic comparison of a query against a blob slice, without a bounds check per byte.
///
/// It is the one read in this file that does not go through `require`, because it runs once per byte of every lookup.
/// That is safe because two checks hold at once: `rawRecord` bounds `offset` and `length` against the blob's own byte
/// count, and the constructor bounds the blob against the mapping.
///
/// `query` is walked as a sequence rather than subscripted.  A `Data` that is a slice of another keeps its parent's
/// index space, so `query[0]` on `parent[5...]` reads the wrong byte or traps - and this is public API reached through
/// `exactRows(raw:)`, which takes whatever `Data` a caller has.
private func aqdsCompare(
    query: Data,
    mapped: ReadOnlyMappedFile,
    blobOffset: Int,
    valueOffset: Int,
    valueLength: Int
) -> Int {
    let common = min(query.count, valueLength)
    for (index, lhs) in zip(0..<common, query) {
        let rhs = mapped.bytes[blobOffset + valueOffset + index]
        if lhs < rhs { return -1 }
        if lhs > rhs { return 1 }
    }
    if query.count < valueLength { return -1 }
    if query.count > valueLength { return 1 }
    return 0
}

private func checkedInt(_ value: UInt64, label: String) throws -> Int {
    guard let output = Int(exactly: value) else {
        throw UIContractError.invalidArtifact("AQDS \(label) exceeds Int")
    }
    return output
}

private func validateAQDSBase(
    binaryURL: URL,
    manifestURL: URL,
    expectedKind: String,
    expectedSHA256: String
) throws -> (ReadOnlyMappedFile, AQDSManifest) {
    let manifest = try JSONDecoder().decode(AQDSManifest.self, from: Data(contentsOf: manifestURL))
    guard manifest.format == "aquatype-dict-compact", manifest.version == 2,
          manifest.endian == "little", manifest.kind == expectedKind,
          manifest.binarySHA256 == expectedSHA256,
          manifest.builder.implementationSHA256
            == "4c611268ba579e8f0d7eff687c8fb5b904e29461a2ad9cfe4947ed24eda20add" else {
        throw UIContractError.invalidArtifact("AQDS manifest identity mismatch")
    }
    guard try HybridDigest.sha256File(binaryURL) == expectedSHA256 else {
        throw UIContractError.invalidArtifact("AQDS binary SHA mismatch")
    }
    let mapped = try ReadOnlyMappedFile(url: binaryURL)
    guard mapped.count == manifest.binaryBytes,
          try mapped.data(offset: 0, length: 8, label: "magic") == Data("AQDICT1\0".utf8),
          try mapped.u32(8, "version") == 2,
          try mapped.u32(12, "endian") == 0x01020304,
          try mapped.u32(20, "header bytes") == 256,
          try mapped.u64(24, "file bytes") == UInt64(mapped.count) else {
        throw UIContractError.invalidArtifact("AQDS binary header mismatch")
    }
    let payload = try mapped.data(offset: 32, length: 32, label: "payload sha").lowercaseHex
    let sources = try mapped.data(offset: 64, length: 32, label: "source sha").lowercaseHex
    guard payload == manifest.payloadSHA256, sources == manifest.sourceAggregateSHA256 else {
        throw UIContractError.invalidArtifact("AQDS header/manifest digest mismatch")
    }
    return (mapped, manifest)
}

public struct AQDSCandidate: Equatable {
    public let textBytes: Data
    public let weight: Double
    public let sourceID: UInt8
    public let sourceName: String
}

public final class AQDSFallbackStore {
    public static let acceptedSHA256 =
        "6fbe62f2359ad4a1c239ac7124adb5c1d135bf584239022c91b7e15342b586ea"

    private let mapped: ReadOnlyMappedFile
    private let manifest: AQDSManifest
    private let rawHashOffset: Int
    private let rawHashSlots: Int
    private let rawIndexOffset: Int
    private let rawCount: Int
    private let rowsOffset: Int
    private let rowCount: Int
    private let rawBlobOffset: Int
    private let rawBlobBytes: Int
    private let textBlobOffset: Int
    private let textBlobBytes: Int

    public let binarySHA256: String

    /// Opens the store after checking the whole file against its pinned SHA-256, then the header and every section's
    /// bounds; anything that does not match fails here rather than during a lookup.
    public init(binaryURL: URL, manifestURL: URL) throws {
        let base = try validateAQDSBase(
            binaryURL: binaryURL,
            manifestURL: manifestURL,
            expectedKind: "fallback",
            expectedSHA256: Self.acceptedSHA256
        )
        mapped = base.0
        manifest = base.1
        guard try mapped.u32(16, "kind") == 1 else {
            throw UIContractError.invalidArtifact("fallback AQDS kind")
        }
        rawIndexOffset = try checkedInt(mapped.u64(96, "raw index offset"), label: "raw index")
        rawCount = try checkedInt(mapped.u64(104, "raw count"), label: "raw count")
        rowsOffset = try checkedInt(mapped.u64(112, "rows offset"), label: "rows")
        rowCount = try checkedInt(mapped.u64(120, "row count"), label: "row count")
        rawBlobOffset = try checkedInt(mapped.u64(144, "raw blob offset"), label: "raw blob")
        rawBlobBytes = try checkedInt(mapped.u64(152, "raw blob bytes"), label: "raw bytes")
        textBlobOffset = try checkedInt(mapped.u64(160, "text blob offset"), label: "text blob")
        textBlobBytes = try checkedInt(mapped.u64(168, "text blob bytes"), label: "text bytes")
        rawHashOffset = try checkedInt(mapped.u64(216, "raw hash offset"), label: "raw hash")
        rawHashSlots = try checkedInt(mapped.u64(224, "raw hash slots"), label: "raw hash slots")
        // Every section has to end inside the mapping; the raw blob in particular is read by `aqdsCompare`, which does
        // not check bounds itself.
        guard rawHashSlots > 0, rawHashSlots & (rawHashSlots - 1) == 0,
              rawHashOffset == 256,
              manifest.sections["raw_hash"]?.offset == UInt64(rawHashOffset),
              manifest.sections["raw_index"]?.offset == UInt64(rawIndexOffset),
              manifest.sections["rows"]?.offset == UInt64(rowsOffset),
              manifest.sections["raw_blob"]?.offset == UInt64(rawBlobOffset),
              manifest.sections["text_blob"]?.offset == UInt64(textBlobOffset),
              rawBlobOffset <= mapped.count, rawBlobBytes <= mapped.count - rawBlobOffset,
              textBlobOffset + textBlobBytes == mapped.count else {
            throw UIContractError.invalidArtifact("fallback AQDS section layout")
        }
        binarySHA256 = Self.acceptedSHA256
    }

    private func rawRecord(_ index: Int) throws -> (offset: Int, length: Int, rowStart: Int, rows: Int) {
        guard index >= 0, index < rawCount else {
            throw UIContractError.invalidArtifact("fallback raw index out of range")
        }
        let base = rawIndexOffset + index * 16
        let offset = Int(try mapped.u32(base, "raw offset"))
        let length = Int(try mapped.u16(base + 4, "raw length"))
        let reserved = try mapped.u16(base + 6, "raw reserved")
        let rowStart = Int(try mapped.u32(base + 8, "raw row start"))
        let rows = Int(try mapped.u32(base + 12, "raw rows"))
        guard reserved == 0, offset <= rawBlobBytes, length <= rawBlobBytes - offset,
              rowStart <= rowCount, rows <= rowCount - rowStart else {
            throw UIContractError.invalidArtifact("corrupt fallback raw record")
        }
        return (offset, length, rowStart, rows)
    }

    private func rawIndex(_ raw: Data) throws -> Int? {
        let hash = aqdsFNV1a64(raw)
        let fingerprint = Int((hash >> 60) & 0xF)
        var slot = Int(hash & UInt64(rawHashSlots - 1))
        for _ in 0..<rawHashSlots {
            let encoded = Int(try mapped.u24(rawHashOffset + slot * 3, "raw hash slot"))
            if encoded == 0 { return nil }
            let indexPlusOne = encoded & 0xF_FFFF
            guard indexPlusOne > 0, indexPlusOne <= rawCount else {
                throw UIContractError.invalidArtifact("corrupt fallback hash index")
            }
            if encoded >> 20 == fingerprint {
                let index = indexPlusOne - 1
                let record = try rawRecord(index)
                if aqdsCompare(
                    query: raw, mapped: mapped, blobOffset: rawBlobOffset,
                    valueOffset: record.offset, valueLength: record.length
                ) == 0 { return index }
            }
            slot = (slot + 1) & (rawHashSlots - 1)
        }
        throw UIContractError.invalidArtifact("fallback raw hash probe exhausted")
    }

    public func exactRows(raw: Data, limit: Int) throws -> [AQDSCandidate] {
        guard limit > 0, let index = try rawIndex(raw) else { return [] }
        let record = try rawRecord(index)
        var output: [AQDSCandidate] = []
        output.reserveCapacity(min(limit, record.rows))
        for rowIndex in record.rowStart..<(record.rowStart + min(limit, record.rows)) {
            let base = rowsOffset + rowIndex * 16
            let offset = Int(try mapped.u32(base, "row text offset"))
            let length = Int(try mapped.u16(base + 4, "row text length"))
            let sourceID = try mapped.u8(base + 6, "row source")
            let reserved = try mapped.u8(base + 7, "row reserved")
            let weight = Double(bitPattern: try mapped.u64(base + 8, "row weight"))
            guard reserved == 0, Int(sourceID) < manifest.sources.count,
                  offset <= textBlobBytes, length <= textBlobBytes - offset,
                  weight.isFinite else {
                throw UIContractError.invalidArtifact("corrupt fallback row")
            }
            output.append(AQDSCandidate(
                textBytes: try mapped.utf8(
                    offset: textBlobOffset + offset, length: length, label: "row text"
                ),
                weight: weight,
                sourceID: sourceID,
                sourceName: manifest.sources[Int(sourceID)].name
            ))
        }
        return output
    }
}
