import Foundation

/// Where a candidate came from.  These strings are the wire format: the engine writes them into every response's
/// `provenance` and the shell reads them back to decide what may appear and what gets the 「記憶」 note.
public enum UIFallbackCandidateSource: String, Codable, CaseIterable {
    case nativeAQDS = "G_NATIVE_AQDS"           // dictionary words for the leading syllables
    case decoderMemory = "G_DECODER_MEMORY"     // personal memory: what the user committed before for exactly these keys
    case decoderModel = "G_DECODER_MODEL"       // the model's candidates
    case charTable = "G_CHAR_TABLE"             // char_to_keys: the characters of the first syllable
}

public struct UIFallbackRequest: Codable, Equatable {
    public static let schemaID = "aquatype-ui-fallback-request/v1"
    public static let maximumCommittedContextUTF8Bytes = 768
    public static let maximumCommittedContextUTF16Units = 256
    // The contract's bound, not the product's.  The limit a user meets is the shell's (190 keys, where a keystroke is
    // refused with a beep); this one only has to stay far enough above it never to be the binding constraint.
    public static let maximumRawKeysUTF8Bytes = 256

    public let schema: String
    public let clientID: String
    public let clientGeneration: UInt64
    public let rawKeys: String
    public let rawIdentity: String
    public let committedContextUTF8: String
    public let contextIdentity: String
    public let maxCandidates: Int

    enum CodingKeys: String, CodingKey {
        case schema
        case clientID = "client_id"
        case clientGeneration = "client_generation"
        case rawKeys = "raw_keys"
        case rawIdentity = "raw_identity"
        case committedContextUTF8 = "committed_context_utf8"
        case contextIdentity = "context_identity"
        case maxCandidates = "max_candidates"
    }

    public init(
        clientID: String,
        clientGeneration: UInt64,
        rawKeys: String,
        rawIdentity: String? = nil,
        committedContextUTF8: String = "",
        contextIdentity: String? = nil,
        maxCandidates: Int = 9
    ) {
        schema = Self.schemaID
        self.clientID = clientID
        self.clientGeneration = clientGeneration
        self.rawKeys = rawKeys
        self.rawIdentity = rawIdentity ?? Self.identity(for: rawKeys)
        self.committedContextUTF8 = committedContextUTF8
        self.contextIdentity = contextIdentity ?? Self.contextIdentity(for: committedContextUTF8)
        self.maxCandidates = maxCandidates
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        schema = try container.decode(String.self, forKey: .schema)
        clientID = try container.decode(String.self, forKey: .clientID)
        clientGeneration = try container.decode(UInt64.self, forKey: .clientGeneration)
        rawKeys = try container.decode(String.self, forKey: .rawKeys)
        rawIdentity = try container.decode(String.self, forKey: .rawIdentity)
        maxCandidates = try container.decode(Int.self, forKey: .maxCandidates)
        if container.contains(.committedContextUTF8) {
            committedContextUTF8 = try container.decode(
                String.self, forKey: .committedContextUTF8
            )
            contextIdentity = try container.decode(String.self, forKey: .contextIdentity)
        } else {
            guard !container.contains(.contextIdentity) else {
                throw DecodingError.dataCorruptedError(
                    forKey: .contextIdentity,
                    in: container,
                    debugDescription: "context identity requires an explicit context field"
                )
            }
            // A request without a context field has no context: it means the same as an empty one, identity
            // included.
            committedContextUTF8 = ""
            contextIdentity = Self.contextIdentity(for: "")
        }
    }

    /// Whether a scalar is a Han character.  The character table ships every reading in Unicode, extension planes
    /// included, so a test limited to 0x3400...0x9FFF would drop every extension-B character on the way to the panel,
    /// and refuse to remember one if it were committed.
    public static func isHan(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.value {
        case 0x3400...0x4DBF,      // Ext A
             0x4E00...0x9FFF,      // URO
             0xF900...0xFAFF,      // compatibility ideographs
             0x20000...0x2A6DF,    // Ext B
             0x2A700...0x2EBEF,    // Ext C, D, E, F
             0x2F800...0x2FA1F,    // compatibility supplement
             0x30000...0x323AF:    // Ext G, H
            return true
        default:
            return false
        }
    }

    public static func identity(for rawKeys: String) -> String {
        Data(rawKeys.utf8).base64EncodedString()
    }

    public static func contextIdentity(for committedContextUTF8: String) -> String {
        HybridDigest.sha256Hex(Data(committedContextUTF8.utf8))
    }

    public func validate() throws {
        // The same client-id rule the commit envelope applies, not a looser one: the engine keys its per-client state
        // by the id a request carries, and a client whose requests passed but whose commits were refused would type
        // normally and never be remembered.  The shell's id is a lowercased UUID and passes both.
        guard schema == Self.schemaID,
              !clientID.isEmpty, clientID.utf8.count <= 128,
              clientID.utf8.allSatisfy({ (0x21...0x7E).contains($0) }),
              clientGeneration > 0,
              !rawKeys.isEmpty, rawKeys.utf8.count <= Self.maximumRawKeysUTF8Bytes,
              rawKeys.utf8.allSatisfy({ (0x20...0x7E).contains($0) }),    // the keys a commit accepts; no NUL to cut it
              rawIdentity == Self.identity(for: rawKeys),
              committedContextUTF8.utf8.count <= Self.maximumCommittedContextUTF8Bytes,
              committedContextUTF8.utf16.count <= Self.maximumCommittedContextUTF16Units,
              contextIdentity == Self.contextIdentity(for: committedContextUTF8),
              // At most 64: the input method asks for 63, everything its panel can show.
              (1...64).contains(maxCandidates) else {
            throw UIContractError.invalidArgument("invalid UI fallback request envelope")
        }
    }
}

public struct UIFallbackCandidate: Codable, Equatable {
    public let textUTF8: String
    public let provenance: [UIFallbackCandidateSource]
    /// How many of the request's raw keys this candidate accounts for.  nil means all of them, which is what a
    /// whole-sentence candidate does.  A shorter value is what makes committing a piece at a time possible: the shell
    /// commits the text, removes that many keys from the composition and asks again for the rest.  Without it every
    /// candidate consumed the whole input, so a sentence the model got wrong could only be retyped.
    public let consumedKeys: Int?

    enum CodingKeys: String, CodingKey {
        case textUTF8 = "text_utf8"
        case provenance
        case consumedKeys = "consumed_keys"
    }

    public init(textUTF8: String, provenance: [UIFallbackCandidateSource], consumedKeys: Int? = nil) {
        self.textUTF8 = textUTF8
        self.provenance = provenance
        self.consumedKeys = consumedKeys
    }
}

public struct UIFallbackResponse: Codable, Equatable {
    public static let schemaID = "aquatype-ui-fallback-response/v1"
    public static let visiblePathID = "DETERMINISTIC_RIME_AQDS_PERSONAL_V1"
    public static let decoderVisiblePathID = "DECODER_V1"      // the decoder served this response

    public let schema: String
    public let clientID: String
    public let clientGeneration: UInt64
    public let rawKeys: String
    public let rawIdentity: String
    public let contextIdentity: String
    public let visiblePath: String
    public let candidates: [UIFallbackCandidate]

    enum CodingKeys: String, CodingKey {
        case schema
        case clientID = "client_id"
        case clientGeneration = "client_generation"
        case rawKeys = "raw_keys"
        case rawIdentity = "raw_identity"
        case contextIdentity = "context_identity"
        case visiblePath = "visible_path"
        case candidates
    }

    public init(request: UIFallbackRequest, candidates: [UIFallbackCandidate], visiblePath: String = UIFallbackResponse.visiblePathID) {
        schema = Self.schemaID
        clientID = request.clientID
        clientGeneration = request.clientGeneration
        rawKeys = request.rawKeys
        rawIdentity = request.rawIdentity
        contextIdentity = request.contextIdentity
        self.visiblePath = visiblePath
        self.candidates = candidates
    }
}

public struct UIFallbackCommitEvent: Codable, Equatable {
    public static let schemaID = "aquatype-ui-personal-commit/v1"
    public static let maximumTextUTF8Bytes = 256
    public static let maximumUnixTimestamp: Int64 = 4_102_444_800
    public static let countDelta = 1

    public let schema: String
    public let clientID: String
    public let eventID: String
    public let clientGeneration: UInt64
    public let rawKeys: String
    public let rawIdentity: String
    public let selectedText: String
    public let timestampUnixSeconds: Int64
    public let countDelta: Int

    enum CodingKeys: String, CodingKey {
        case schema
        case clientID = "client_id"
        case eventID = "event_id"
        case clientGeneration = "client_generation"
        case rawKeys = "raw_keys"
        case rawIdentity = "raw_identity"
        case selectedText = "selected_text"
        case timestampUnixSeconds = "timestamp_unix_seconds"
        case countDelta = "count_delta"
    }

    public init(
        clientID: String,
        eventID: String,
        clientGeneration: UInt64,
        rawKeys: String,
        rawIdentity: String? = nil,
        selectedText: String,
        timestampUnixSeconds: Int64,
        countDelta: Int = Self.countDelta
    ) {
        schema = Self.schemaID
        self.clientID = clientID
        self.eventID = eventID
        self.clientGeneration = clientGeneration
        self.rawKeys = rawKeys
        self.rawIdentity = rawIdentity ?? UIFallbackRequest.identity(for: rawKeys)
        self.selectedText = selectedText
        self.timestampUnixSeconds = timestampUnixSeconds
        self.countDelta = countDelta
    }

    public func validate() throws {
        let validClientID = !clientID.isEmpty && clientID.utf8.count <= 128
            && clientID.utf8.allSatisfy { (0x21...0x7E).contains($0) }
        let validEventID = !eventID.isEmpty && eventID.utf8.count <= 128
            && eventID.utf8.allSatisfy { (0x21...0x7E).contains($0) }
        let validRaw = !rawKeys.isEmpty && rawKeys.utf8.count <= UIFallbackRequest.maximumRawKeysUTF8Bytes
            && rawKeys.utf8.allSatisfy { (0x20...0x7E).contains($0) }
        let validText = !selectedText.isEmpty
            && selectedText.utf8.count <= Self.maximumTextUTF8Bytes
            && !selectedText.unicodeScalars.contains {
                CharacterSet.controlCharacters.contains($0)
            }
            && selectedText.unicodeScalars.contains(where: UIFallbackRequest.isHan)
        guard schema == Self.schemaID,
              validClientID,
              validEventID,
              clientGeneration > 0,
              validRaw,
              rawIdentity == UIFallbackRequest.identity(for: rawKeys),
              validText,
              (0...Self.maximumUnixTimestamp).contains(timestampUnixSeconds),
              countDelta == Self.countDelta else {
            throw UIContractError.invalidArgument("invalid UI personal commit envelope")
        }
    }
}

public enum UIFallbackCommitStatus: String, Codable, Equatable {
    case recorded = "RECORDED"
}

public struct UIFallbackCommitAck: Codable, Equatable {
    public static let schemaID = "aquatype-ui-personal-commit-ack/v1"

    public let schema: String
    public let clientID: String
    public let eventID: String
    public let rawIdentity: String
    public let status: UIFallbackCommitStatus
    public let effectiveCount: Double

    enum CodingKeys: String, CodingKey {
        case schema
        case clientID = "client_id"
        case eventID = "event_id"
        case rawIdentity = "raw_identity"
        case status
        case effectiveCount = "effective_count"
    }

    public init(
        event: UIFallbackCommitEvent,
        status: UIFallbackCommitStatus,
        effectiveCount: Double
    ) {
        schema = Self.schemaID
        clientID = event.clientID
        eventID = event.eventID
        rawIdentity = event.rawIdentity
        self.status = status
        self.effectiveCount = effectiveCount
    }
}

/// What the engine sends when it cannot answer a line.
///
/// It exists because the alternative is silence, and silence from a request/response channel is indistinguishable
/// from a slow reply: the host keeps a callback alive for a response that is never coming, and the visible symptom is
/// not an error message but a candidate panel that never appears.  Every rejected request or commit whose client,
/// generation and keys can still be read gets one of these, keyed by them, so the host can fail that request and say
/// why.  A line that cannot be read that far has nothing to key a reply to; the host's reply timeout ends any wait.
public struct UIFallbackFailure: Codable {
    public static let schemaID = "aquatype-ui-failure/v1"

    public let schema: String
    public let clientID: String
    public let clientGeneration: UInt64
    public let rawIdentity: String
    public let eventID: String?
    public let reason: String

    enum CodingKeys: String, CodingKey {
        case schema
        case clientID = "client_id"
        case clientGeneration = "client_generation"
        case rawIdentity = "raw_identity"
        case eventID = "event_id"
        case reason
    }

    public init(clientID: String, clientGeneration: UInt64, rawIdentity: String, eventID: String?, reason: String) {
        self.schema = Self.schemaID
        self.clientID = clientID
        self.clientGeneration = clientGeneration
        self.rawIdentity = rawIdentity
        self.eventID = eventID
        self.reason = reason
    }
}
