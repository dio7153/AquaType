// The personal memory: what this person has committed before, and the two ways it comes back.
//
// A - exact recall.  A commit is stored under two key strings: the keys the person actually pressed, and the keys the
//     committed text should have.  Typing either again, exactly, brings the text back.  There is no tolerance and so
//     no threshold: it is what they did before, returned verbatim.  The second anchor is what makes the memory help a
//     person who types cleanly: the model often has their sentence on the first page but not in first place, and
//     exact recall of a previous choice closes that gap.
//
// B - fusion.  A phrase of two or more syllables is also stored by its reading.  When the model produces a candidate
//     whose readings contain that sequence anywhere, the characters over that span are replaced and the result is
//     offered as an extra candidate.  This is the mechanism for names and jargon: 謝子軒 and 謝子萱 sound identical
//     and the model cannot know which was meant, but the person has already said which one is theirs.
//
//     Fusion compares readings, not keys, and that is the point.  By the time the model has produced a candidate it
//     has already turned the noisy keys into syllables, so the comparison is exact and needs no tolerance of its own.
//     It also never changes a sentence's sounds, only its characters, so the worst it can do is offer a homophone.
//
// Deliberately absent: fuzzy whole-string matching.  It would be the only part with a tunable threshold and the only
// part that can fire wrongly, and B covers the same ground from the syllable side.  One-syllable phrases are absent
// from B for the same reason: which character a lone sound should be depends on the sentence, which is the model's
// job, not the memory's.
import AquaTypeUIContract
import Darwin
import Foundation

// Darwin imports both `struct flock` and the BSD `flock(2)` function under the same Swift name, and the struct wins.
// Binding the function explicitly is what makes the lock below a kernel advisory lock rather than a name collision.
@_silgen_name("flock")
private func aquaTypeMemoryFlock(_ descriptor: Int32, _ operation: Int32) -> Int32

struct MemoryRecord {
    var text: String
    var typedKeys: String          // what was pressed that time, noise and all
    var cleanKeys: String          // what the committed text should have been typed as
    var readings: [Int32]          // one syllable id per character; empty when the span did not segment exactly
    var count: Double
    var lastSeen: Int64
}

final class PersonalMemoryStore {
    /// A memory committed at least this many times keeps a floor in `support` however long it goes unused; below it,
    /// support decays to nothing.
    static let confirmedCount: Double = 2
    /// Fusion needs at least this many syllables.  A single sound is decided by context, and the model has context.
    static let minFusionReadings = 2
    /// How many memories for one key string the panel is offered.  A limit on what is *shown*, not on what is kept:
    /// a fourth memory for the same keys stays in the file, gathers count and recency each time it is chosen, and
    /// overtakes the third by `support` on its own.
    static let displayPerKey = 3
    /// The only bound on what is stored.  It is global rather than per key, so a key string may hold as many memories
    /// as the person has actually made for it, and the file still cannot grow without end: past this, the lowest
    /// `support` rows go first, wherever they are.
    static let maxRows = 4000

    private let path: URL
    private(set) var rows: [MemoryRecord] = []
    private var byTyped: [String: [Int]] = [:]
    private var byClean: [String: [Int]] = [:]
    private var byFirstReading: [Int32: [Int]] = [:]

    init(stateDir: String) {
        path = URL(fileURLWithPath: stateDir).appendingPathComponent("personal_memory.v2.tsv")
        load()
    }

    // MARK: - storage

    /// Set when the file is there but could not be read.  Nothing is written while it is true, because writing would
    /// replace what could not be read with what little this session has learned.
    private var unreadable = false
    /// Set when the file could not even be looked at this time (a network home reconnecting): this change is not
    /// written, and the next one looks again.
    private var notLookedAt = false

    private func load() {
        removeStaleTemporaries()
        readRowsFromDisk()
        reindex()
    }

    /// A save writes `.<file>.<pid>.tmp` beside the file and renames it; an engine ended in between (the input method
    /// kills one that stops answering) leaves its copy behind, holding the whole memory.  Those of processes that are
    /// gone are removed; another engine's, still being written, is left alone.
    private func removeStaleTemporaries() {
        let folder = path.deletingLastPathComponent()
        let prefix = ".\(path.lastPathComponent)."
        for name in (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        where name.hasPrefix(prefix) && name.hasSuffix(".tmp") {
            guard let pid = Int32(name.dropFirst(prefix.count).dropLast(".tmp".count)) else { continue }
            if pid != getpid(), kill(pid, 0) != 0, errno == ESRCH {
                try? FileManager.default.removeItem(at: folder.appendingPathComponent(name))
            }
        }
    }

    /// Replace `rows` with what is in the file right now.  Every mutation calls this again, under the lock, before it
    /// writes: this process's copy may be minutes old.
    ///
    /// No file is an ordinary first run.  A file that exists but cannot be read (a permission change, an I/O error,
    /// bytes that are not UTF-8) is moved aside rather than overwritten, and the session carries on able to learn
    /// again.  If even the move fails, nothing is written at all: a session that remembers nothing is recoverable, a
    /// file that has been overwritten is not.
    private func readRowsFromDisk() {
        rows.removeAll()
        notLookedAt = false
        // Taken before the read: a write that lands during it changes the identity, so the next refresh reads again.
        lastSeenOnDisk = identityOnDisk()
        // Only a file that is not there is an empty memory.  Any other failure to look (a network home reconnecting)
        // must not be taken for one, or the next save would replace every row with the one just committed.
        var info = stat()
        if stat(path.path, &info) != 0 {
            let error = errno
            if error == ENOENT { return }
            lastSeenOnDisk = nil
            notLookedAt = true
            log("[engine] personal memory could not be looked at (errno \(error)); this change is not written")
            return
        }
        let text: String
        do {
            text = try String(contentsOf: path, encoding: .utf8)
        } catch {
            lastSeenOnDisk = nil
            let aside = path.appendingPathExtension("unreadable-\(Int64(Date().timeIntervalSince1970))")
            do {
                try FileManager.default.moveItem(at: path, to: aside)
                log("[engine] personal memory could not be read (\(error.localizedDescription)); "
                    + "kept at \(aside.path) and starting a new one")
            } catch {
                unreadable = true
                log("[engine] personal memory could not be read or moved aside (\(error.localizedDescription)); "
                    + "it will not be written to this session")
            }
            return
        }
        var inconsistent = 0
        for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
            let f = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
            guard f.count >= 6 else { continue }
            let readings = f[3].isEmpty ? [] : f[3].split(separator: ",").compactMap { Int32($0) }
            // Left behind by commits whose keys and characters did not line up (see `record`): kept out, and gone
            // from the file at the next write.
            guard Self.consistent(text: f[0], readings: readings) else { inconsistent += 1; continue }
            // Numbers within what a commit can produce: the file is the person's to edit, and a count that is not a
            // number or a time far outside the clock's range would break the ordering or overflow the arithmetic on
            // every recall.
            let count = Double(f[4]).flatMap { $0.isFinite ? min(max($0, 0), 1_000_000) : nil } ?? 1
            let lastSeen = min(max(Int64(f[5]) ?? 0, 0), Int64(Date().timeIntervalSince1970) + 86_400)
            rows.append(MemoryRecord(text: f[0], typedKeys: f[1], cleanKeys: f[2], readings: readings,
                                     count: count, lastSeen: lastSeen))
        }
        if inconsistent > 0 { log("[engine] personal memory: \(inconsistent) rows whose readings do not match their text left out") }
    }

    /// Six tab-separated fields, with no escaping, which is safe only because of what the envelope already refuses:
    /// `UIFallbackCommitEvent.validate` rejects a `selected_text` containing any control character - tab and newline
    /// among them - and restricts `raw_keys` to printable ASCII, and every path that reaches `record` has been through
    /// it.  There is no second line of defence here, so a future caller that skips validation corrupts the file by
    /// shifting its columns, silently.
    private func save() {
        guard !unreadable, !notLookedAt else {
            log("[engine] personal memory not written: the existing file could not be read and must not be replaced")
            return
        }
        var out = ""
        for r in rows {
            let readings = r.readings.map(String.init).joined(separator: ",")
            out += "\(r.text)\t\(r.typedKeys)\t\(r.cleanKeys)\t\(readings)\t\(r.count)\t\(r.lastSeen)\n"
        }
        // Readable by its owner only, like the folder it is in: it holds everything they have typed in Chinese.  Written
        // whole to a file beside it, handed to the file system (fsync) and then renamed over the old one, so a reader, or
        // the engine or the Mac stopping in the middle, finds the old file or the new one rather than half of one.  Not
        // F_FULLFSYNC: that waits for the drive's own cache, tens of milliseconds the next keystroke would wait for too.
        let temporary = path.deletingLastPathComponent().appendingPathComponent(".\(path.lastPathComponent).\(getpid()).tmp")
        guard FileManager.default.createFile(atPath: temporary.path, contents: nil, attributes: [.posixPermissions: 0o600]),
              let handle = FileHandle(forWritingAtPath: temporary.path) else {
            log("[engine] personal memory could not be written beside \(path.path)")
            unlink(temporary.path)
            return
        }
        do {
            try handle.write(contentsOf: Data(out.utf8))
            try handle.synchronize()
            try handle.close()
        } catch {
            log("[engine] personal memory could not be written beside \(path.path): \(error.localizedDescription)")
            try? handle.close()
            unlink(temporary.path)
            return
        }
        if rename(temporary.path, path.path) != 0 {
            log("[engine] personal memory could not be replaced at \(path.path): \(String(cString: strerror(errno)))")
            unlink(temporary.path)
            lastSeenOnDisk = nil
        } else {
            lastSeenOnDisk = identityOnDisk()
        }
    }

    // MARK: - one writer at a time

    /// Whether the advisory lock is usable here.  Logged once: a filesystem that does not support `flock` - a network
    /// home, most often - would otherwise say so on every commit.
    private var lockUnavailableReported = false

    /// The lock is on a file of its own, beside the memory rather than on it.
    ///
    /// `save()` replaces the memory atomically: it writes a temporary file and renames it into place.  A lock taken on
    /// the memory file would therefore be a lock on an inode that is about to stop being the memory, and the next
    /// process to come along would lock the new one and see nothing held.  A lock file is never replaced, so everyone
    /// locks the same inode.
    private var lockPath: URL { path.appendingPathExtension("lock") }

    /// Run `body` with no other process inside it.
    ///
    /// When the lock cannot be taken at all, `body` still runs: a memory that refuses to learn on a filesystem without
    /// `flock` is worse than one that learns with the small risk of two concurrent writers.
    private func withExclusiveLock(_ body: () -> Void) {
        let descriptor = lockPath.path.withCString {
            open($0, O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR)
        }
        guard descriptor >= 0 else {
            if !lockUnavailableReported {
                lockUnavailableReported = true
                log("[engine] personal memory lock could not be opened at \(lockPath.path); writing without it")
            }
            body(); return
        }
        defer { close(descriptor) }
        guard aquaTypeMemoryFlock(descriptor, LOCK_EX) == 0 else {
            if !lockUnavailableReported {
                lockUnavailableReported = true
                log("[engine] personal memory lock is not supported on this filesystem; writing without it")
            }
            body(); return
        }
        defer { _ = aquaTypeMemoryFlock(descriptor, LOCK_UN) }
        body()
    }

    /// Apply a change to the memory and write it back, with the file re-read first and nobody else inside.
    ///
    /// The re-read is the half that matters.  Without it two engines each holding a hundred rows would each write a
    /// hundred and one, and the second write would take the first one's new row with it; a lock alone only stops the
    /// two writes from interleaving.  Two engines on one state directory happen in practice: `--state-dir` defaults
    /// to `~/.aquatype`, so any engine started by hand writes the person's real memory.
    private func mutate(_ change: () -> Void) {
        if inTransaction { change(); reindex(); return }
        withExclusiveLock {
            refreshFromDisk()
            change()
            save()
        }
        reindex()
    }

    /// Several changes made as one: the file is read and written once, under one lock.  A commit is up to three
    /// changes (a correction taken back, a passed-over memory demoted, the commit itself), and rewriting the file for
    /// each delayed the next keystroke.  Each change still sees the ones before it.
    func transaction(_ changes: () -> Void) {
        withExclusiveLock {
            refreshFromDisk()
            reindex()
            inTransaction = true
            changes()
            inTransaction = false
            save()
        }
        reindex()
    }
    private var inTransaction = false

    /// The file as this process last read or wrote it.  A save replaces the file by renaming a new one over it, so any
    /// write - this engine's or another's - gives it a new inode: the same identity, size and time mean the rows here
    /// are what is on disk, and parsing all of them again (most of a commit's cost at 4000 rows) can be skipped.
    private var lastSeenOnDisk: (device: dev_t, inode: ino_t, size: off_t, seconds: Int, nanoseconds: Int)?

    private func identityOnDisk() -> (device: dev_t, inode: ino_t, size: off_t, seconds: Int, nanoseconds: Int)? {
        var info = stat()
        guard stat(path.path, &info) == 0 else { return nil }
        return (info.st_dev, info.st_ino, info.st_size, info.st_mtimespec.tv_sec, info.st_mtimespec.tv_nsec)
    }

    private func refreshFromDisk() {
        if let seen = lastSeenOnDisk, let now = identityOnDisk(), seen == now { return }
        readRowsFromDisk()
    }

    private func reindex() {
        byTyped.removeAll(); byClean.removeAll(); byFirstReading.removeAll()
        for (i, r) in rows.enumerated() {
            if !r.typedKeys.isEmpty { byTyped[r.typedKeys, default: []].append(i) }
            if !r.cleanKeys.isEmpty { byClean[r.cleanKeys, default: []].append(i) }
            if let first = r.readings.first, r.readings.count >= Self.minFusionReadings {
                byFirstReading[first, default: []].append(i)
            }
        }
    }

    /// Support is what ranks one memory against another for the same span: how often, how recently.  The half-life is
    /// 180 days and a phrase committed only once keeps decaying, so a mistaken commit loses to whatever is chosen for
    /// those sounds after it; one that has been confirmed keeps a floor, because a real phrase should still be there
    /// after a quiet year.
    func support(_ r: MemoryRecord, now: Int64) -> Double {
        let days = max(0.0, Double(now - r.lastSeen) / 86400.0)
        let decay = pow(2.0, -days / 180.0)
        let recency = r.count >= Self.confirmedCount ? 0.25 + 0.75 * decay : decay
        return r.count * recency
    }

    // MARK: - recording

    /// One reading per character, or no readings at all (keys too mistyped to parse).  Anything else is a text that
    /// does not belong to its keys - four characters remembered for the two syllables of ㄐㄧㄣ ㄊㄧㄢ - and recalling or
    /// fusing it would put characters nobody typed into a sentence.
    static func consistent(text: String, readings: [Int32]) -> Bool {
        readings.isEmpty || readings.count == text.count
    }

    func record(text: String, typedKeys: String, cleanKeys: String, readings: [Int32], now: Int64) {
        guard !text.isEmpty else { return }
        guard Self.consistent(text: text, readings: readings) else {
            log("[engine] personal memory: \(text.count) characters for \(readings.count) readings not remembered")
            return
        }
        mutate {
            // A memory the commit contains - a name it was rewritten with, or one typed again inside a longer sentence -
            // has just been used, and stays as recent as its use: otherwise one typed once and then only ever reached
            // inside sentences would age out under the one-off sentences committed meanwhile.
            let chars = Array(text)
            if chars.count == readings.count {
                for start in readings.indices {
                    for i in byFirstReading[readings[start]] ?? [] where i < rows.count {
                        let n = rows[i].readings.count
                        guard start + n <= readings.count, rows[i].text != text,
                              readings[start..<(start + n)].elementsEqual(rows[i].readings),
                              String(chars[start..<(start + n)]) == rows[i].text else { continue }
                        rows[i].lastSeen = max(rows[i].lastSeen, now)
                    }
                }
            }
            // A memory is identified by what the person typed and what they chose.  Readings and clean keys are
            // things learned about a memory, not part of its name, so they are merged into the row already there:
            // clean keys are filled in only when the readings agree, and the same phrase typed the same way must
            // still count as one memory.
            if let i = rows.firstIndex(where: { $0.text == text && $0.typedKeys == typedKeys }) {
                rows[i].count += 1; rows[i].lastSeen = now
                if rows[i].readings.isEmpty { rows[i].readings = readings }
                if rows[i].cleanKeys.isEmpty { rows[i].cleanKeys = cleanKeys }
            } else {
                // A mistyped commit usually carries no readings, because mistyped keys are exactly the ones the
                // syllable table cannot parse.  The readings belong to the text, not to that attempt, so a row that
                // has none borrows them from another row for the same text.
                var readings = readings
                if readings.isEmpty, let known = rows.first(where: { $0.text == text && !$0.readings.isEmpty }) {
                    readings = known.readings
                }
                rows.append(MemoryRecord(text: text, typedKeys: typedKeys, cleanKeys: cleanKeys, readings: readings,
                                         count: 1, lastSeen: now))
            }
            // ... and the same in the other direction: this commit may be the one that learned them, for a text whose
            // earlier attempts were all mistyped.
            if !readings.isEmpty {
                for i in rows.indices where rows[i].text == text && rows[i].readings.isEmpty {
                    rows[i].readings = readings
                }
            }
            prune(now: now)
        }
    }

    /// The count now held for this commit, reported in the commit ack.
    func count(typedKeys: String, text: String) -> Double {
        (byTyped[typedKeys] ?? []).lazy.filter { self.rows[$0].text == text }.map { self.rows[$0].count }.max() ?? 0
    }

    /// A commit that is immediately taken back is a correction, not a memory: the person deleted what they had just
    /// inserted and chose something else for the same keys.  Without this the only way a wrong commit leaves is decay,
    /// and there is no key binding for "forget that" that would not cost more than it is worth.
    func retract(typedKeys: String, text: String) {
        mutate {
            guard let i = rows.firstIndex(where: { $0.typedKeys == typedKeys && $0.text == text }) else { return }
            // Down to nothing at most, and kept: a memory with no count is neither offered nor fused, and the same commit
            // made again brings it back.  Taken out, a word committed between two uses of its homophone (她們, then
            // 他們) would be gone.
            rows[i].count = max(0, rows[i].count - 1)
        }
    }

    /// A memory that keeps being passed over for the same keys is losing an argument with the person: demote it so it
    /// stops taking the first position, without deleting something they may still want.
    func demote(keys: String, keeping chosen: String) {
        mutate {
            for i in rows.indices where (rows[i].cleanKeys == keys || rows[i].typedKeys == keys)
                                     && rows[i].text != chosen && rows[i].count > 1 {
                rows[i].count -= 0.5
            }
        }
    }

    /// Everything committed is kept.  The only thing that removes a memory is the global ceiling, and then it removes
    /// the least supported rows in the file rather than the least supported rows for one key string.  How many
    /// memories a key shows is `exactRecall`'s business.
    private func prune(now: Int64) {
        guard rows.count > Self.maxRows else { return }
        let ordered = rows.indices.sorted { support(rows[$0], now: now) > support(rows[$1], now: now) }
        let drop = Set(ordered.dropFirst(Self.maxRows))
        rows = rows.enumerated().filter { !drop.contains($0.offset) }.map(\.element)
    }

    // MARK: - learning from what happens after a commit

    /// A wrong commit has to be able to leave, and a memory that keeps being refused has to stop taking first place.
    /// Both signals are in the commit stream itself, so neither needs a key binding for "forget that" and neither
    /// needs the shell to watch the document:
    ///
    ///   a correction  - the same keys committed again, differently, within a minute.  The person deleted what was
    ///                   inserted and chose something else, which is as clear a retraction as a dedicated key would
    ///                   be.  The first choice loses the count it just gained.
    ///   a disagreement - the memory's own answer for these keys was on offer and something else was chosen.  That
    ///                   does not make the memory wrong, so it is demoted rather than removed; enough refusals and it
    ///                   stops being confirmed and loses its floor.
    private var lastCommit: [String: (keys: String, text: String, at: Int64)] = [:]
    private static let correctionWindow: Int64 = 60

    func noteOutcome(client: String, typedKeys: String, text: String, now: Int64) {
        // Words only, not single characters: 他 and 她, 的 and 得, are typed with the same keys a few seconds apart in
        // ordinary writing, and the second is not a correction of the first.
        if let prev = lastCommit[client], prev.keys == typedKeys, prev.text != text,
           prev.text.count >= 2, text.count >= 2, now - prev.at <= Self.correctionWindow {
            retract(typedKeys: typedKeys, text: prev.text)
        }
        if let top = exactRecall(keys: typedKeys, now: now).first, top != text {
            demote(keys: typedKeys, keeping: text)
        }
        // Only a commit within the window can be corrected, so nothing older is kept: one entry per text field ever
        // typed in would otherwise pile up for as long as the engine runs.
        lastCommit = lastCommit.filter { now - $0.value.at <= Self.correctionWindow }
        lastCommit[client] = (typedKeys, text, now)
    }

    // MARK: - A: exact recall

    /// The best `displayPerKey` texts this person committed for exactly these keys, best first.  Both anchors are
    /// consulted: the keys they pressed last time (they may mistype it the same way again) and the keys the text
    /// should have (they may get it right this time).  Everything else committed for these keys is still in the file
    /// and still gaining or losing `support`.
    func exactRecall(keys: String, now: Int64) -> [String] {
        var seen = Set<String>(); var hits: [(String, Double)] = []
        for i in (byTyped[keys] ?? []) + (byClean[keys] ?? []) where rows[i].count > 0 && seen.insert(rows[i].text).inserted {
            hits.append((rows[i].text, support(rows[i], now: now)))
        }
        return hits.sorted { $0.1 != $1.1 ? $0.1 > $1.1 : $0.0 < $1.0 }.prefix(Self.displayPerKey).map(\.0)
    }

    // MARK: - B: fusion

    /// The candidate rewritten with every non-overlapping remembered phrase its readings contain, or nil when it
    /// contains none.  Longest and best-supported spans win, and each span is replaced once, so a sentence with three
    /// names comes back with all three fixed and still as a single extra candidate - the explosion to avoid is
    /// enumerating subsets, not applying them all.
    func fuse(text: String, readings: [Int32], now: Int64) -> String? {
        let chars = Array(text)
        guard chars.count == readings.count, !readings.isEmpty else { return nil }
        struct Match { let start: Int; let len: Int; let text: String; let support: Double }
        var matches: [Match] = []
        // Memories of exactly what the model wrote for a span: a later "yes, that" to the model's own characters.
        var agreements: [Match] = []
        for start in readings.indices {
            guard let bucket = byFirstReading[readings[start]] else { continue }
            for i in bucket {
                let r = rows[i]
                let n = r.readings.count
                guard r.count > 0, n >= Self.minFusionReadings, start + n <= readings.count else { continue }
                // No confirmation gate: one commit is already this person saying which characters they meant for
                // exactly these sounds, and a name is usually typed once.
                guard readings[start..<(start + n)].elementsEqual(r.readings) else { continue }   // no array per row
                let s = support(r, now: now)
                if String(chars[start..<(start + n)]) == r.text {
                    agreements.append(Match(start: start, len: n, text: r.text, support: s))
                    continue
                }
                matches.append(Match(start: start, len: n, text: r.text, support: s))
            }
        }
        // A rewrite is not applied where a memory of the model's own characters for any of the same sounds has more
        // support (`support`: how often, how recently) than the rewrite: choosing 今天注意 is an answer to an old 記甲.
        // One with less support - typically older, like a sentence that had a name wrong before the name was corrected
        // - does not hold the rewrite back.
        matches.removeAll { m in
            agreements.contains { a in a.start < m.start + m.len && m.start < a.start + a.len && a.support > m.support }
        }
        guard !matches.isEmpty else { return nil }
        // Where memories overlap, the better supported wins, and between equals the longer: a name corrected yesterday
        // goes before a sentence from last year that had it wrong, however much longer that sentence is.  Spans that do
        // not overlap are all applied, so a sentence with three names comes back with all three.
        matches.sort { $0.support != $1.support ? $0.support > $1.support : $0.len > $1.len }
        var taken = [Bool](repeating: false, count: readings.count)
        var applied: [Match] = []
        for m in matches {
            if (m.start..<(m.start + m.len)).contains(where: { taken[$0] }) { continue }
            for k in m.start..<(m.start + m.len) { taken[k] = true }
            applied.append(m)
        }
        guard !applied.isEmpty else { return nil }
        applied.sort { $0.start < $1.start }
        var out = ""; var i = 0
        for m in applied {
            if i < m.start { out += String(chars[i..<m.start]) }
            out += m.text; i = m.start + m.len
        }
        if i < chars.count { out += String(chars[i...]) }
        guard out != text else { return nil }
        return out
    }
}
