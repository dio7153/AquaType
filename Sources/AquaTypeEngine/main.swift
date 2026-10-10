// AquaTypeEngine: the engine of the AquaType input method.  The app (the Squirrel-based shell) starts it as a child process
// and talks to it in JSON lines over stdin/stdout, through the contract in AquaTypeUIContract: a request is answered by a
// response or a failure, a commit by an ack.
// Candidates come from the decoder (personal memory first, then the model's sentences) and, for the leading syllables,
// from the dictionary store and the character table.  Without a usable decoder (no GPU, missing model) the engine still
// answers from the dictionary store and the character table, so a decoder that cannot be loaded never takes typing down.
//   AquaTypeEngine --command serve-ui-jsonl --fallback-aqds F --fallback-manifest M
//                  [--decoder-dir D] [--metallib L] [--char-to-keys C] [--state-dir S]
//   AquaTypeEngine --check-char-coverage C          (no model; exits 0 when every char returns for its own keys)
//   AquaTypeEngine --selftest --decoder-dir D ...   (one request through the whole path; prints the response)
import AquaTypeDecoderBridge
import AquaTypeUIContract
import Foundation
import Darwin

struct UISchemaProbe: Decodable { let schema: String }

/// Whatever of a rejected line can still be read, so the failure can be addressed to the request that caused it.
/// A field that is missing or of the wrong type reads as empty: the line failed validation, so nothing in it may be assumed.
struct UIFailureProbe: Decodable {
    let clientID: String
    let clientGeneration: UInt64
    let rawIdentity: String
    let eventID: String?
    enum CodingKeys: String, CodingKey {
        case clientID = "client_id", clientGeneration = "client_generation", rawIdentity = "raw_identity", eventID = "event_id"
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        clientID = (try? c.decode(String.self, forKey: .clientID)) ?? ""
        clientGeneration = (try? c.decode(UInt64.self, forKey: .clientGeneration)) ?? 0
        rawIdentity = (try? c.decode(String.self, forKey: .rawIdentity)) ?? ""
        eventID = try? c.decode(String.self, forKey: .eventID)
    }
}

struct EngineArguments {
    var values: [String: String] = [:]
    var flags = Set<String>()
    init(_ argv: [String]) {
        var i = 0
        while i < argv.count {
            let key = argv[i]
            if key.hasPrefix("--"), i + 1 < argv.count, !argv[i + 1].hasPrefix("--") { values[key] = argv[i + 1]; i += 2 } else { flags.insert(key); i += 1 }
        }
    }
    func required(_ key: String) throws -> String { guard let v = values[key], !v.isEmpty else { throw UIContractError.invalidArgument("missing \(key)") }; return v }
}

final class CharKeyIndex {
    let count: Int
    private let byRaw: [String: [String]]
    init(path: String) throws {
        let data = try Data(contentsOf: URL(fileURLWithPath: path))
        guard let dict = try JSONSerialization.jsonObject(with: data) as? [String: String] else { throw UIContractError.invalidArtifact("char_to_keys.json must be {char: keys}") }
        var by: [String: [String]] = [:]
        for (ch, keys) in dict.sorted(by: { $0.key < $1.key }) { by[keys, default: []].append(ch) }
        count = dict.count; byRaw = by
    }
    func chars(for raw: String) -> [String] { byRaw[raw] ?? [] }

    /// The keys a bopomofo key string may be made of - the schema's alphabet, space included, which is the first tone.
    static let keyboard = Set("1234567890-qwertyuiop asdfghjkl;zxcvbnm,./")

    /// The characters no key sequence reaches: an empty key string, a key that is not on the bopomofo keyboard, or an
    /// entry that is not one character.  `--check-char-coverage` runs this as a release gate in packaging/build_app.sh.
    /// A truncated table is structurally valid and passes, which is why the count is printed beside the verdict.
    func coverageGaps() -> [String] {
        var missing: [String] = []
        for (raw, chars) in byRaw {
            let typable = !raw.isEmpty && raw.allSatisfy { CharKeyIndex.keyboard.contains($0) }
            for ch in chars where !typable || ch.count != 1 { missing.append(ch) }
        }
        return missing.sorted()
    }
}

/// Frequency order for the third source's characters, in three tiers.
///
/// `tables.json["chars"]` is the model's character vocabulary and `norm_rank.bin` lists indices into it - without the
/// offset the embedding table adds - by corpus frequency.  So a
/// character the list ranks is common and carries its rank; one in the vocabulary that the list never ranks did not
/// occur in the Traditional corpus the list was built from; one the model does not know at all is further out still -
/// a variant, or a simplified form that only reaches us through the character table.  Ordering by this instead of by
/// code point is what stops a rare variant sitting above the character the user meant.
final class CharFrequency {
    private var rank: [String: Int] = [:]
    private var known: Set<String> = []
    init?(decoderDir: String) {
        let t = URL(fileURLWithPath: decoderDir).appendingPathComponent("tables.json")
        let r = URL(fileURLWithPath: decoderDir).appendingPathComponent("norm_rank.bin")
        guard let td = try? Data(contentsOf: t),
              let obj = try? JSONSerialization.jsonObject(with: td) as? [String: Any],
              let chars = obj["chars"] as? [String], !chars.isEmpty else { return nil }
        known = Set(chars)
        guard let rd = try? Data(contentsOf: r), rd.count >= 4 else { return nil }
        // `loadUnaligned`, here and below: `Data` promises no alignment.
        let n = rd.withUnsafeBytes { Int($0.loadUnaligned(fromByteOffset: 0, as: Int32.self)) }
        guard n > 0, rd.count >= 4 + n * 4 else { return nil }
        rd.withUnsafeBytes { buf in
            for i in 0..<n {
                let id = Int(buf.loadUnaligned(fromByteOffset: 4 + i * 4, as: Int32.self))
                if id >= 0, id < chars.count, rank[chars[id]] == nil { rank[chars[id]] = i }
            }
        }
    }
    /// Lower sorts first.  Tier 0 keeps the corpus order; tiers 1 and 2 have no order of their own, so they keep the
    /// character table's, which is by code point.
    func order(_ ch: String) -> (tier: Int, rank: Int) {
        if let r = rank[ch] { return (0, r) }
        return (known.contains(ch) ? 1 : 2, Int.max)
    }
}

/// The bopomofo keyboard, for the third source.
enum Bopomofo {
    /// The tone marks that take a key: ˇ ˋ ˊ ˙.  The first tone has none, which is why a syllable can end on a vowel
    /// key (`一` is `u`, `天` is `wu0`) and why "drop the tone" has to mean "drop a trailing key only if it is one of
    /// these".
    static let toneKeys: Set<Character> = ["3", "4", "6", "7"]
    /// What ends a syllable as typed: a tone key, or Space for the first tone.  The tables write the first tone with no
    /// key at all, so the Space is part of what was typed but not part of what is looked up.
    static let syllableEnds: Set<Character> = [" ", "3", "4", "6", "7"]
    private static let rows = ["1234567890-", "qwertyuiop", "asdfghjkl;", "zxcvbnm,./"]
    /// Physically adjacent keys: left, right, and the key at roughly the same position one row up or down.  A slip of
    /// the finger lands on one of these, which is the only kind of key error worth guessing at - and it is guessed at
    /// only when the exact and tone-blind lookups have both come back empty.
    static let neighbours: [Character: [Character]] = {
        var m: [Character: Set<Character>] = [:]
        let grid = rows.map { Array($0) }
        for (ri, row) in grid.enumerated() {
            for (ci, k) in row.enumerated() {
                var n: Set<Character> = []
                if ci > 0 { n.insert(row[ci - 1]) }
                if ci + 1 < row.count { n.insert(row[ci + 1]) }
                for dr in [-1, 1] where ri + dr >= 0 && ri + dr < grid.count {
                    let other = grid[ri + dr]
                    for dc in [0, -1] where ci + dc >= 0 && ci + dc < other.count { n.insert(other[ci + dc]) }
                }
                m[k, default: []].formUnion(n)
            }
        }
        return m.mapValues { Array($0).sorted() }
    }()
}

func log(_ s: String) { FileHandle.standardError.write(Data((s + "\n").utf8)) }
/// Through the contract's own predicate rather than another copy of the ranges.  The shell keeps a copy of its own,
/// because the app does not link this package.
func isCJK(_ text: String) -> Bool { !text.isEmpty && text.unicodeScalars.allSatisfy(UIFallbackRequest.isHan) }

/// The decoder session (libaquadecoder); nil when unavailable.
final class DecoderSession {
    let handle: OpaquePointer
    /// The keys of each syllable the model reads, indexed by reading id, as `tables.json` spells them: the first tone as
    /// a trailing Space, which the character table and the dictionary leave out.
    let syllableKeys: [String]
    init?(dir: String, metallib: String?) {
        guard let d = aq_open(dir, metallib) else { log("[engine] decoder unavailable: \(String(cString: aq_last_error()))"); return nil }
        handle = d
        let t = URL(fileURLWithPath: dir).appendingPathComponent("tables.json")
        if let td = try? Data(contentsOf: t), let obj = try? JSONSerialization.jsonObject(with: td) as? [String: Any],
           let s = obj["syllables"] as? [String] { syllableKeys = s }
        else { syllableKeys = []; log("[engine] tables.json has no syllables; the third source cannot follow the model's reading") }
    }
    /// The keys a reading spells, as the character table and the dictionary write them; nil for an id out of range.
    func keys(ofReading id: Int) -> String? {
        id >= 0 && id < syllableKeys.count ? syllableKeys[id].filter { $0 != " " } : nil
    }
    func compose(raw: String, contextJSON: String) -> [[String: Any]]? {
        guard let out = aq_decode(handle, raw, contextJSON.isEmpty ? nil : contextJSON) else { log("[engine] aq_decode failed: \(String(cString: aq_last_error()))"); return nil }
        defer { aq_free(out) }
        guard let payload = (try? JSONSerialization.jsonObject(with: Data(String(cString: out).utf8))) as? [String: Any], let rows = payload["candidates"] as? [[String: Any]] else {
            // Its length only: the output is candidate text, and nothing that is typed is written anywhere.
            log("[engine] compose JSON unexpected: \(strlen(out)) bytes"); return nil }
        return rows
    }
    /// How many of the model's candidates are passed on: 63, everything the panel can show.  A shorter list, cut by how
    /// clean the keys look, would drop sentences the user can still reach by paging, and save no time.
    var candidateBudget: Int { 63 }
    /// The syllables a clean key string spells, empty when it does not segment exactly.
    func readings(for keys: String) -> [Int32] {
        guard !keys.isEmpty, let out = aq_readings(handle, keys) else { return [] }
        defer { aq_free(out) }
        guard let arr = (try? JSONSerialization.jsonObject(with: Data(String(cString: out).utf8))) as? [Int] else { return [] }
        return arr.map(Int32.init)
    }
    /// A commit ends the sentence: the decoder drops the state it kept between that sentence's keystrokes.
    func endSentence() {
        aq_reset(handle)
    }
    func charID(_ ch: String) -> Int32 { aq_char_id(handle, ch) }
    /// The decoder's context is the previous committed sentence, one token per character, as training builds it.  The
    /// committed text is split on sentence enders; the last complete piece, edge punctuation stripped, is the previous
    /// sentence, and a trailing fragment is the sentence in progress, which is not context.
    static let sentenceEnders: Set<Character> = ["。", "！", "？", "!", "?", "；", ";", "…", "\n", "\r"]
    static let edgePunctuation: Set<Character> = [" ", "\t", "\r", "　", "「", "」", "『", "』", "（", "）", "(", ")", "《", "》", "〈", "〉", "【", "】", ",", "，", "、", ":", "：", "“", "”", "\"", "'"]
    static func previousSentence(_ committed: String) -> String {
        var pieces: [String] = []; var current = ""
        for ch in committed {
            if sentenceEnders.contains(ch) { if !current.isEmpty { pieces.append(current); current = "" } }
            else { current.append(ch) }
        }
        // A trailing fragment after the last ender stays in `current` and never becomes a piece.
        guard let last = pieces.last else { return "" }
        var s = Substring(last); while let f = s.first, edgePunctuation.contains(f) { s = s.dropFirst() }; while let l = s.last, edgePunctuation.contains(l) { s = s.dropLast() }
        return String(s)
    }
    /// previous-sentence tokens (kind 0, one per character), as JSON objects
    func prevSentenceTokens(committed: String, client: String = "") -> [String] {
        var prev = DecoderSession.previousSentence(committed)
        if prev.isEmpty, let last = lastCommitText[client], !last.isEmpty,
           Date().timeIntervalSince1970 - (lastCommitAt[client] ?? 0) <= DecoderSession.commitContextLifetime {
            prev = last                                                                                        // no sentence ender in the committed text: the last commit is the previous sentence, while it is still recent
        }
        let sentence = Array(prev).suffix(64); let ids = sentence.map { charID(String($0)) }
        return ids.map { "{\"kind\":0,\"chars\":[\($0)],\"weights\":[1.0]}" }
    }

    /// Per client (a text field): the rows of its last answer - the most the panel shows, which is all a commit can
    /// have chosen from - for the commit that follows; and the text of its last commit, since a commit ends a sentence
    /// even without punctuation (chat-style typing), and when it was made.  Kept for the most recent clients only:
    /// every text field ever typed in would otherwise add an entry for as long as the engine runs.
    private(set) var lastRowsByClient: [String: [[String: Any]]] = [:]
    var lastCommitText: [String: String] = [:]
    var lastCommitAt: [String: Double] = [:]
    private var clientsByRecency: [String] = []
    static let clientsKept = 16
    static let rowsKept = 64

    func keepRows(_ rows: [[String: Any]], client: String) {
        lastRowsByClient[client] = Array(rows.prefix(Self.rowsKept))
        touch(client)
    }
    private func touch(_ client: String) {
        clientsByRecency.removeAll { $0 == client }
        clientsByRecency.append(client)
        while clientsByRecency.count > Self.clientsKept {
            let gone = clientsByRecency.removeFirst()
            lastRowsByClient[gone] = nil; lastCommitText[gone] = nil; lastCommitAt[gone] = nil
        }
    }
    /// How long a commit goes on counting as the previous sentence: longer than the gap between the sentences of one
    /// message, shorter than a pause that means the user has moved on.  Carried for ever, an old commit would go on
    /// steering the answers long after the user had moved on, with nothing on screen to say why.
    static let commitContextLifetime: Double = 120

    /// Decodes the keys typed for one client, with the previous sentence as context.
    func decode(client: String, raw: String, committed: String) -> [[String: Any]]? {
        let tokens = prevSentenceTokens(committed: committed, client: client)
        return compose(raw: raw, contextJSON: tokens.isEmpty ? "" : "[" + tokens.joined(separator: ",") + "]")
    }
    /// A commit ends the sentence in progress, and its text becomes the previous sentence for a while.
    func noteCommit(client: String, text: String) {
        lastCommitText[client] = text
        lastCommitAt[client] = Date().timeIntervalSince1970
        touch(client)
    }
    /// warm-up: the first decode of a process compiles the GPU kernels it needs (a one-off +30..90 MB, ~100 ms); do it before the user types
    func warmUp() { _ = compose(raw: "u04vu/6", contextJSON: ""); _ = compose(raw: "5j/ 2u6ru03g4jp6ej4", contextJSON: "") }
}

struct Engine {
    let fallback: AQDSFallbackStore
    let charIndex: CharKeyIndex?
    let decoder: DecoderSession?
    let charFrequency: CharFrequency?
    let memory: PersonalMemoryStore?

    func respond(_ request: UIFallbackRequest) -> UIFallbackResponse {
        // max_candidates caps the memory and the model.  The third source is appended after them without a cap, so
        // every character stays reachable by paging.
        var limit = request.maxCandidates
        var output: [UIFallbackCandidate] = []; var indexByText: [String: Int] = [:]
        // Deduplication is global and keeps the earlier entry: a text that a later source repeats does not appear twice,
        // it gains that source in its provenance and stays where the first source put it.
        func add(_ text: String, _ source: UIFallbackCandidateSource, _ consumed: Int? = nil) {
            guard !text.isEmpty, isCJK(text) else { return }
            let key = consumed == nil ? text : "\(text)#\(consumed!)"
            if let i = indexByText[key] { var p = output[i].provenance; if !p.contains(source) { p.append(source) }; output[i] = UIFallbackCandidate(textUTF8: text, provenance: p, consumedKeys: output[i].consumedKeys) }
            else if output.count < limit { indexByText[key] = output.count; output.append(UIFallbackCandidate(textUTF8: text, provenance: [source], consumedKeys: consumed)) }
        }
        var visiblePath = UIFallbackResponse.visiblePathID
        var modelTop: [String: Any]? = nil
        if let decoder, let rows = decoder.decode(client: request.clientID, raw: request.rawKeys, committed: request.committedContextUTF8) {
            visiblePath = UIFallbackResponse.decoderVisiblePathID
            decoder.keepRows(rows, client: request.clientID)
            modelTop = rows.first
            let now = Int64(Date().timeIntervalSince1970)

            // A - exact recall leads the list.  This is the only thing that goes above the model: it is not a guess,
            // it is what the user chose for exactly these keys before.
            if let memory {
                for t in memory.exactRecall(keys: request.rawKeys, now: now) { add(t, .decoderMemory) }
            }

            // The model's candidates are trimmed to its budget, never past what the request asked for.
            // Exact recall is not part of that budget: it is a different source and it has already been added.
            limit = min(request.maxCandidates, decoder.candidateBudget) + output.count

            // B - fusion sits *inside* the model's block, next to the candidate it came from, because a fused
            // sentence is the model's sentence with one span rewritten: the model decided it was the right sentence,
            // the memory only decided how a name in it is spelled.  Putting it in the memory block would send a
            // rewrite of the fortieth candidate to the top of the panel.
            //
            // It goes *before* its source only when the model never wrote that phrase anywhere in its own pool.  A
            // phrase the model knows - 異議 against 意義 - it withheld for a reason it can see and the memory cannot,
            // namely the rest of the sentence; there the memory may offer, not overrule.
            var modelTexts = Set<String>()
            for row in rows { if let t = row["text"] as? String { modelTexts.insert(t) } }
            for (rank, row) in rows.enumerated() {
                guard let text = row["text"] as? String else { continue }
                let fused = memory?.fuse(text: text, readings: (row["reading"] as? [Int]).map { $0.map(Int32.init) } ?? [], now: now)
                // The rewrite of the model's own best sentence leads, whether or not the model also wrote it elsewhere:
                // the model picks the sentence and the memory picks how the user spells the names in it (王小明
                // against 王曉明 is the same sentence, spelled the user's way).  Below the top, the model's ordering
                // carries information about the rest of the sentence, so the rule above applies.
                //
                // A fusion is labelled as the model's, not as a memory: 記憶 marks the user's own choice for exactly
                // these keys, shown above the model, and a fusion is the model's sentence with a span respelled.
                if let f = fused, rank == 0 || !modelTexts.contains(f) { add(f, .decoderModel) }
                add(text, .decoderModel)
                if let f = fused, rank != 0, modelTexts.contains(f) { add(f, .decoderModel) }
            }
        }
        // Without a decoder (it failed to load) the third source below is the whole answer.
        // The traditional path - the dictionary's words and the character table - is asked about the *leading part* of
        // the input, not the whole of it: once a sentence is being typed, the whole input is almost never one word or one
        // character's reading.  Each of its candidates says how many keys it accounts for, the shell commits that much
        // and asks again for the rest, which is how a person commits a piece at a time when the model's sentence is
        // wrong.  The model answers about the whole input, always.
        limit = Int.max
        let rawKeys = Array(request.rawKeys)

        // The input as syllables.  Each ends at its tone key, Space being the first tone; `rawCount` is how many typed
        // keys it takes, and `keys` is how the tables spell it - without the Space, because they write the first tone
        // with no key.  `pending` is a trailing syllable whose tone has not been typed yet.  A Space straight after a
        // tone key spells nothing, and is counted with the syllable before it.
        var syllables: [(keys: String, rawCount: Int)] = []
        var pending = ""
        for k in rawKeys {
            pending.append(k)
            if Bopomofo.syllableEnds.contains(k) {
                let keys = pending.filter { $0 != " " }
                if keys.isEmpty, !syllables.isEmpty { syllables[syllables.count - 1].rawCount += pending.count }
                else { syllables.append((keys, pending.count)) }
                pending = ""
            }
        }
        /// The keys the first `count` syllables take, or nil when that is the whole input.
        func consumed(_ count: Int) -> Int? {
            let used = syllables[0..<count].reduce(0) { $0 + $1.rawCount }
            return used == rawKeys.count ? nil : used
        }

        /// Every tone of a toneless reading: the first tone, which writes no key, then the rest.
        func everyTone(_ base: String) -> [String] {
            guard let charIndex, !base.isEmpty else { return [] }
            var seen = Set<String>(); var out: [String] = []
            for q in [base] + Bopomofo.toneKeys.sorted().map({ base + String($0) }) {
                for c in charIndex.chars(for: q) where seen.insert(c).inserted { out.append(c) }
            }
            return out
        }

        /// Characters for one syllable, with the fault-tolerance chain.  Each step runs only when the one before it came
        /// back empty, so a legal reading never competes with a guess: exact, then blind to the tone (the tone is the key
        /// people get wrong, and dropping it turns one reading into its whole tone family), then one key replaced by a
        /// physical neighbour, then two neighbouring keys in the wrong order, then one key too many - the same single
        /// mistakes the decoder corrects, so a syllable the model read through a typo still has characters here, and
        /// Tab still has a group to go to.  The query is one syllable, so the result is bounded by its homophones.
        func charsWithTolerance(_ syllable: String) -> [String] {
            guard let charIndex, !syllable.isEmpty else { return [] }
            let exact = charIndex.chars(for: syllable)
            if !exact.isEmpty { return exact }

            var keys = Array(syllable)
            if let last = keys.last, Bopomofo.toneKeys.contains(last) { keys.removeLast() }
            let toneBlind = everyTone(String(keys))
            if !toneBlind.isEmpty { return toneBlind }

            let original = Array(syllable)
            func union(_ variants: [[Character]]) -> [String] {
                var seen = Set<String>(); var out: [String] = []
                for v in variants { for c in charIndex.chars(for: String(v)) where seen.insert(c).inserted { out.append(c) } }
                return out
            }
            let neighbour = union(original.indices.flatMap { i in
                (Bopomofo.neighbours[original[i]] ?? []).map { n -> [Character] in var v = original; v[i] = n; return v }
            })
            if !neighbour.isEmpty { return neighbour }
            // Swapped keys before an extra one: dropping a key from a swapped pair can leave a different legal reading
            // (ㄐㄣㄧ without its ㄣ is ㄐㄧ), while swapping an extra key back rarely spells anything.
            let swapped = union(original.indices.dropLast().map { i -> [Character] in var v = original; v.swapAt(i, i + 1); return v })
            if !swapped.isEmpty { return swapped }
            return union(original.indices.map { i -> [Character] in var v = original; v.remove(at: i); return v })
        }

        /// The model's best sentence as syllables: the keys each of its characters is read as, and how many typed keys
        /// the input has used up by the end of that character, or nil when that is the whole input.  The model segments
        /// the keys itself, so this holds where the tone keys do not: a syllable typed without its tone, or with a
        /// mistake the model read through.
        var modelSyllables: [(keys: String, consumed: Int?)] = []
        if let decoder, let top = modelTop, let reading = top["reading"] as? [Int], let ends = top["char_ends"] as? [Int],
           reading.count == ends.count {
            for (id, end) in zip(reading, ends) {
                guard let keys = decoder.keys(ofReading: id), end >= 0, end < rawKeys.count else { modelSyllables = []; break }
                modelSyllables.append((keys, end + 1 == rawKeys.count ? nil : end + 1))
            }
        }

        /// The third source: the leading part of the input, as words and then characters.
        ///
        /// A word is the longest run of whole syllables from the start that the dictionary knows, two syllables or more,
        /// matched exactly; words come before characters because a word is a more specific answer than one of its
        /// characters, and within a length they keep the dictionary's own weight.  The characters are those of the first
        /// syllable, ordered by corpus frequency in three tiers so a rare variant or a simplified form cannot sit above
        /// the character that was meant.  While the first syllable is still being typed it has no tone yet, and its
        /// characters are offered in every tone.  Neither list is capped: its size is set by how many words and
        /// homophones the leading syllables have.
        ///
        /// The syllables are cut at the tone keys.  Where that cut does not hold - the first syllable as cut is not a
        /// reading (its tone was left out, so it runs into the next one, or it has a mistake in it), or no run of
        /// syllables is a word (a later tone was left out) - the model's own reading of the keys is used instead, the
        /// same way: the words its first two to six characters are read as, and the homophones of its first character.
        var words: [(text: String, consumed: Int?, weight: Double)] = []
        var chars: [(text: String, consumed: Int?)] = []

        func addWords(_ pieces: [(keys: String, consumed: Int?)]) {
            guard pieces.count >= 2 else { return }
            for count in stride(from: min(pieces.count, 6), through: 2, by: -1) {
                let piece = pieces[0..<count].map(\.keys).joined()
                guard let rows = try? fallback.exactRows(raw: Data(piece.utf8), limit: 4096), !rows.isEmpty else { continue }
                for r in rows { if let t = String(data: r.textBytes, encoding: .utf8) { words.append((t, pieces[count - 1].consumed, r.weight)) } }
                break
            }
        }
        addWords(syllables.indices.map { (syllables[$0].keys, consumed($0 + 1)) })
        if words.isEmpty { addWords(modelSyllables) }

        if let first = syllables.first, let charIndex, !charIndex.chars(for: first.keys).isEmpty {
            for c in charIndex.chars(for: first.keys) { chars.append((c, consumed(1))) }
        } else if syllables.isEmpty, case let typing = everyTone(pending), !typing.isEmpty {
            for c in typing { chars.append((c, nil)) }
        } else if let first = modelSyllables.first, let charIndex, !charIndex.chars(for: first.keys).isEmpty {
            for c in charIndex.chars(for: first.keys) { chars.append((c, first.consumed)) }
        } else if let first = syllables.first {
            for c in charsWithTolerance(first.keys) { chars.append((c, consumed(1))) }
        }
        words.sort { a, b in
            let ca = a.consumed ?? rawKeys.count, cb = b.consumed ?? rawKeys.count
            if ca != cb { return ca > cb }                                    // the longer match first
            return a.weight > b.weight
        }
        for w in words { add(w.text, .nativeAQDS, w.consumed) }
        if let freq = charFrequency {
            chars.sort { a, b in
                let ca = a.consumed ?? rawKeys.count, cb = b.consumed ?? rawKeys.count
                if ca != cb { return ca > cb }
                let (ta, ra) = freq.order(a.text), (tb, rb) = freq.order(b.text)
                return ta != tb ? ta < tb : (ra != rb ? ra < rb : a.text < b.text)
            }
        }
        for c in chars { add(c.text, .charTable, c.consumed) }
        return UIFallbackResponse(request: request, candidates: output, visiblePath: visiblePath)
    }

    func commit(_ event: UIFallbackCommitEvent) -> UIFallbackCommitAck {
        guard let decoder else { return UIFallbackCommitAck(event: event, status: .recorded, effectiveCount: 0) }
        rememberCommit(event, decoder: decoder)
        decoder.noteCommit(client: event.clientID, text: event.selectedText)
        decoder.endSentence()
        // The ack reports how many times the personal memory now holds this commit.
        let remembered = memory?.count(typedKeys: event.rawKeys, text: event.selectedText) ?? 0
        return UIFallbackCommitAck(event: event, status: .recorded, effectiveCount: remembered)
    }

    /// What a commit teaches the memory.
    ///
    /// The committed text, under both the keys that were pressed and the keys it should have taken, so exact recall
    /// finds it whichever way it is typed next time.  One commit, one record: a sentence that took several commits is
    /// assembled by the shell and arrives here as its own commit, because only the shell knows when a sentence has
    /// finished.
    ///
    /// The readings come from the decoder's own row for this text when there is one, and are derived from the keys
    /// otherwise: a commit from the traditional path has no decoder row, and those are the commits most worth
    /// remembering, because going there at all means the model had failed.
    private func rememberCommit(_ event: UIFallbackCommitEvent, decoder: DecoderSession) {
        guard let memory else { return }
        let now = Int64(event.timestampUnixSeconds)
        let text = event.selectedText, typed = event.rawKeys
        var readings: [Int32] = []
        if let rows = decoder.lastRowsByClient[event.clientID] {
            for r in rows where (r["text"] as? String) == text {
                readings = (r["reading"] as? [Int]).map { $0.map(Int32.init) } ?? []; break
            }
        }
        // Without a decoder row, the readings the typed keys spell.  The keys this text should have are the typed keys
        // when they spell its readings; otherwise they are left empty rather than guessed.
        let typedReadings = decoder.readings(for: typed)
        if readings.isEmpty { readings = typedReadings }
        let clean = !readings.isEmpty && typedReadings == readings ? typed : ""

        memory.transaction {
            memory.noteOutcome(client: event.clientID, typedKeys: typed, text: text, now: now)
            memory.record(text: text, typedKeys: typed, cleanKeys: clean, readings: readings, now: now)
        }
    }
}

/// Set while a decoder session exists, so the serve loop can do the decoder's between-keystroke housekeeping after the
/// response is on its way out: compaction inside the request is time the user spends waiting for candidates.
var maintainAfterEmit: (() -> Void)? = nil

/// One encoder for the process, like the one decoder the serve loop keeps.
let responseEncoder = JSONEncoder()

func emit<T: Encodable>(_ value: T) throws {
    let data = try responseEncoder.encode(value)
    FileHandle.standardOutput.write(data); FileHandle.standardOutput.write(Data("\n".utf8))
    maintainAfterEmit?()
}

func makeEngine(_ a: EngineArguments) throws -> Engine {
    let fallback = try AQDSFallbackStore(binaryURL: URL(fileURLWithPath: try a.required("--fallback-aqds")), manifestURL: URL(fileURLWithPath: try a.required("--fallback-manifest")))
    let home = NSHomeDirectory(); let stateDir = a.values["--state-dir"] ?? "\(home)/.aquatype"
    // The installer creates this too, but the personal memory is written here and must not find it missing: its writer
    // would only log, and nothing would be remembered.  Only its owner may look inside.
    try? FileManager.default.createDirectory(atPath: stateDir, withIntermediateDirectories: true,
                                             attributes: [.posixPermissions: 0o700])
    chmod(stateDir, 0o700)
    let charIndex: CharKeyIndex? = {
        if let path = a.values["--char-to-keys"], let index = try? CharKeyIndex(path: path) { return index }
        log("[engine] char_to_keys.json not found; the third source has no character table"); return nil
    }()
    var decoder: DecoderSession? = nil
    if let dir = a.values["--decoder-dir"] { decoder = DecoderSession(dir: dir, metallib: a.values["--metallib"]); decoder?.warmUp() }
    if let d = decoder { maintainAfterEmit = { aq_maintain(d.handle) } }
    let charFrequency = a.values["--decoder-dir"].flatMap { CharFrequency(decoderDir: $0) }
    if charFrequency == nil { log("[engine] no character frequency table; the third source falls back to code-point order") }
    log("[engine] ready decoder=\(decoder != nil) chars=\(charIndex?.count ?? 0) freq=\(charFrequency != nil) state=\(stateDir)")
    let memory = PersonalMemoryStore(stateDir: stateDir)
    log("[engine] personal memory rows=\(memory.rows.count)")
    return Engine(fallback: fallback, charIndex: charIndex, decoder: decoder, charFrequency: charFrequency, memory: memory)
}

func main() throws {
    let a = EngineArguments(Array(CommandLine.arguments.dropFirst()))
    if a.flags.contains("--version") { print("AquaTypeEngine \(String(cString: aq_version()))"); return }
    if let path = a.values["--check-char-coverage"] {
        let index = try CharKeyIndex(path: path); let gaps = index.coverageGaps()
        print("{\"chars\":\(index.count),\"missing\":\(gaps.count),\"ok\":\(gaps.isEmpty)}"); exit(gaps.isEmpty ? 0 : 1)
    }
    let engine = try makeEngine(a)
    if a.flags.contains("--selftest") {
        let raw = a.values["--raw"] ?? "ji3"
        let request = UIFallbackRequest(clientID: "selftest", clientGeneration: 1, rawKeys: raw, maxCandidates: 9)
        try emit(engine.respond(request)); return
    }
    guard (a.values["--command"] ?? "serve-ui-jsonl") == "serve-ui-jsonl" else { throw UIContractError.invalidArgument("unknown --command") }
    // A process that spends its life blocked on stdin is taken for background work and may be run on the efficiency
    // cores, where each decode takes about twice as long.  The engine answers a user who is waiting, so it asks for the
    // class that says so.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)
    let decoder = JSONDecoder()
    while let line = readLine(strippingNewline: true) {
        // Not `try autoreleasepool`: nothing inside can throw out of it - the whole body is a `do`/`catch` and the
        // catch's own emit is a `try?`.
        autoreleasepool {
            guard !line.isEmpty, let data = line.data(using: .utf8) else { return }
            do {
                switch try decoder.decode(UISchemaProbe.self, from: data).schema {
                case UIFallbackRequest.schemaID:
                    let request = try decoder.decode(UIFallbackRequest.self, from: data); try request.validate(); try emit(engine.respond(request))
                case UIFallbackCommitEvent.schemaID:
                    let event = try decoder.decode(UIFallbackCommitEvent.self, from: data); try event.validate(); try emit(engine.commit(event))
                default: log("[engine] unknown message schema")
                }
            } catch {
                // Say so, on the channel the host is listening to: a request answered with nothing leaves the host
                // waiting for a reply that never comes, and the symptom is a panel that never appears.
                log("[engine] request failed: \(error.localizedDescription)")
                if let probe = try? decoder.decode(UIFailureProbe.self, from: data) {
                    try? emit(UIFallbackFailure(clientID: probe.clientID,
                                                clientGeneration: probe.clientGeneration,
                                                rawIdentity: probe.rawIdentity,
                                                eventID: probe.eventID,
                                                reason: error.localizedDescription))
                }
            }
        }
    }
}

// Not a background process, as far as the scheduler is concerned.
//
// This is a child of an accessory app with no window and no run loop: it blocks on a read for as long as nobody is
// typing, which is exactly the shape App Nap throttles - timers coalesced, priority dropped - and the cost lands on
// the first keystroke after a pause.  `beginActivity` is the supported way to say that latency here is user-visible.
// Not `.userInitiated`: that one also holds off idle system sleep, and this process lives as long as the login session,
// so the Mac would never sleep on its own while AquaType is in use.
let aquaTypeActivity = ProcessInfo.processInfo.beginActivity(
    options: [.userInitiatedAllowingIdleSystemSleep, .latencyCritical],
    reason: "AquaType decodes keystrokes as they are typed")

do { try main() } catch { log("AquaTypeEngine failed: \(error.localizedDescription)"); exit(1) }
ProcessInfo.processInfo.endActivity(aquaTypeActivity)
