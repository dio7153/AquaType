//
//  SquirrelInputController.swift
//  Squirrel
//
//  Created by Leo Liu on 5/7/24.
//  Modified for AquaType by Dio Hsieh, 2026.
//

import Foundation
import CryptoKit
import Carbon
import InputMethodKit

struct AquaTypeV2ContextSnapshot: Equatable {
  let text: String
  let identity: String

  init(text: String) {
    self.text = text
    self.identity = AquaTypeV2ContextPolicy.identity(for: text)
  }

  static let empty = AquaTypeV2ContextSnapshot(text: "")
}

enum AquaTypeV2ContextPolicy {
  static let maximumUTF16Units = 256
  static let maximumUTF8Bytes = 768

  static func identity(for text: String) -> String {
    SHA256.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined()
  }

  static func boundedSuffix(_ text: String) -> String {
    var suffixReversed = [Character]()
    var utf16Units = 0
    var utf8Bytes = 0
    for character in text.reversed() {
      let value = String(character)
      let nextUTF16 = utf16Units + value.utf16.count
      let nextUTF8 = utf8Bytes + value.utf8.count
      guard nextUTF16 <= maximumUTF16Units,
            nextUTF8 <= maximumUTF8Bytes else { break }
      suffixReversed.append(character)
      utf16Units = nextUTF16
      utf8Bytes = nextUTF8
    }
    return String(suffixReversed.reversed())
  }

  static func capture(
    secureInputEnabled: Bool,
    documentLength: () throws -> Int,
    markedRange: () throws -> NSRange,
    selectedRange: () throws -> NSRange,
    attributedSubstring: (NSRange) throws -> NSAttributedString?
  ) -> AquaTypeV2ContextSnapshot {
    guard !secureInputEnabled else { return .empty }
    do {
      // IMKTextInput documents that length == NSNotFound when the client does
      // not support TSMDocumentAccess. In that state attributedSubstring may
      // return the selected text instead of the requested document range, even
      // when its UTF-16 length happens to match. Never trust that fallback.
      let lengthOfDocument = try documentLength()
      guard lengthOfDocument != NSNotFound, lengthOfDocument >= 0 else { return .empty }
      let marked = try markedRange()
      let insertionLocation: Int
      if marked.location != NSNotFound {
        guard range(marked, isWithinDocumentLength: lengthOfDocument) else { return .empty }
        insertionLocation = marked.location
      } else {
        let selected = try selectedRange()
        guard selected.location != NSNotFound,
              range(selected, isWithinDocumentLength: lengthOfDocument) else { return .empty }
        insertionLocation = selected.location
      }
      guard insertionLocation > 0, insertionLocation <= lengthOfDocument else { return .empty }
      let length = min(maximumUTF16Units, insertionLocation)
      let requested = NSRange(location: insertionLocation - length, length: length)
      guard range(requested, isWithinDocumentLength: lengthOfDocument) else { return .empty }
      guard let attributed = try attributedSubstring(requested),
            attributed.length == requested.length else {
        // IMKTextInput has no actualRange output on the macOS 13 path. A nil or
        // differently sized result is therefore treated as adjusted/partial and
        // must not be used as preceding context.
        return .empty
      }
      return AquaTypeV2ContextSnapshot(text: boundedSuffix(attributed.string))
    } catch {
      return .empty
    }
  }

  private static func range(_ range: NSRange, isWithinDocumentLength length: Int) -> Bool {
    range.location != NSNotFound
      && range.length != NSNotFound
      && range.location >= 0
      && range.length >= 0
      && range.location <= length
      && range.length <= length - range.location
  }
}

struct AquaTypeV2GenerationContextStore {
  private(set) var generation: UInt64?
  private(set) var snapshot = AquaTypeV2ContextSnapshot.empty

  // The snapshot is taken outside any mutating access to this store.  Capturing lets the run loop turn, a keystroke
  // arriving then can call clear() on this same store, and two overlapping accesses make Swift abort the process: the
  // input method dies and macOS falls back to the built-in keyboard.  So: check whether this generation is already
  // frozen, capture with nothing held, then store.
  func frozen(for nextGeneration: UInt64) -> AquaTypeV2ContextSnapshot? {
    generation == nextGeneration ? snapshot : nil
  }

  mutating func store(generation nextGeneration: UInt64, snapshot next: AquaTypeV2ContextSnapshot) {
    generation = nextGeneration
    snapshot = next
  }

  mutating func clear() {
    generation = nil
    snapshot = .empty
  }
}

struct AquaTypeV2PanelIdentity: Equatable {
  let clientID: String
  let generation: UInt64
  let rawKeys: String
  let rawIdentity: String
  let contextIdentity: String
}

enum AquaTypeV2PanelAcceptance {
  /// The answer to exactly what is typed now.  This is the only thing a candidate may be committed from.
  static func accepts(response: AquaTypeV2PanelIdentity,
                      active: AquaTypeV2PanelIdentity,
                      publishedGeneration: UInt64?) -> Bool {
    response == active && publishedGeneration != active.generation
  }

  /// An answer for keys the user has already typed past: it covers a leading part of what is in the buffer now.
  ///
  /// The engine decodes one request at a time and each takes tens of milliseconds, so while typing the answer that
  /// arrives is usually for the keys of a moment ago.  Showing it keeps the panel moving; requiring an exact match would
  /// leave it frozen for as long as the user kept typing.
  ///
  /// It is only ever interim: commit compares against the keys the panel is actually showing, so nothing from one of
  /// these can be committed until the answer for the real keys arrives, which it does as soon as the user pauses.
  static func acceptsAsInterim(response: AquaTypeV2PanelIdentity, active: AquaTypeV2PanelIdentity) -> Bool {
    response.clientID == active.clientID
      && response.contextIdentity == active.contextIdentity
      && !response.rawKeys.isEmpty
      && response.rawKeys != active.rawKeys
      && active.rawKeys.hasPrefix(response.rawKeys)
  }
}

enum AquaTypeV2ResponseContract {
  static let fallbackSchema = "aquatype-ui-fallback-response/v1"
  static let fallbackPath = "DETERMINISTIC_RIME_AQDS_PERSONAL_V1"
  static let decoderPath = "DECODER_V1"      // the engine's decoder-served response (same schema as the fallback)

  private static let fallbackSources = Set([
    "G_NATIVE_AQDS", "G_CHAR_TABLE",
  ])
  /// The engine's third source, by the provenance its candidates carry: the leading words, and the first syllable's
  /// characters.
  static let wordSource = "G_NATIVE_AQDS"
  static let characterSource = "G_CHAR_TABLE"

  private static let decoderSources = Set([
    "G_DECODER_MEMORY", "G_DECODER_MODEL", "G_NATIVE_AQDS", "G_CHAR_TABLE",
  ])

  static func accepts(schema: String, visiblePath: String) -> Bool {
    schema == fallbackSchema && (visiblePath == fallbackPath || visiblePath == decoderPath)
  }

  static func allows(provenance: [String], visiblePath: String) -> Bool {
    guard !provenance.isEmpty else { return false }
    let allowed = visiblePath == decoderPath ? decoderSources : fallbackSources
    return provenance.allSatisfy(allowed.contains)
  }
}

struct AquaTypeV2CommitMessage: Encodable, Equatable {
  let schema = "aquatype-ui-personal-commit/v1"
  let clientID: String
  let eventID: String
  let clientGeneration: UInt64
  let rawKeys: String
  let rawIdentity: String
  let selectedText: String
  let timestampUnixSeconds: Int64
  let countDelta = 1

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
}

enum AquaTypeV2CommitSequencer {
  static func perform(commit: () -> Void, enqueuePersistence: () -> Void) {
    commit()
    enqueuePersistence()
  }
}

private struct AquaTypeV2UIRequest: Encodable {
  let schema = "aquatype-ui-fallback-request/v1"
  let clientID: String
  let clientGeneration: UInt64
  let rawKeys: String
  let rawIdentity: String
  let committedContextUTF8: String
  let contextIdentity: String
  let maxCandidates: Int

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
}

private struct AquaTypeV2UICandidate: Decodable {
  let textUTF8: String
  let provenance: [String]
  /// How many of the raw keys this candidate accounts for, when it is not the whole input.  The model's candidates
  /// always cover every key and leave it nil; the third source's words and characters cover a leading span and say so,
  /// which is what lets the user commit one piece and have the rest re-decoded.
  let consumedKeys: Int?

  enum CodingKeys: String, CodingKey {
    case textUTF8 = "text_utf8"
    case provenance
    case consumedKeys = "consumed_keys"
  }
}

private struct AquaTypeV2UIResponse: Decodable {
  let schema: String
  let clientID: String
  let clientGeneration: UInt64
  let rawKeys: String
  let rawIdentity: String
  let contextIdentity: String
  let visiblePath: String
  let candidates: [AquaTypeV2UICandidate]

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

  var identity: AquaTypeV2PanelIdentity {
    AquaTypeV2PanelIdentity(
      clientID: clientID,
      generation: clientGeneration,
      rawKeys: rawKeys,
      rawIdentity: rawIdentity,
      contextIdentity: contextIdentity)
  }
}

private struct AquaTypeV2CommitAck: Decodable {
  let schema: String
  let clientID: String
  let eventID: String
  let rawIdentity: String
  let status: String
  let effectiveCount: Double

  enum CodingKeys: String, CodingKey {
    case schema
    case clientID = "client_id"
    case eventID = "event_id"
    case rawIdentity = "raw_identity"
    case status
    case effectiveCount = "effective_count"
  }
}

private struct AquaTypeV2LaunchConfiguration {
  let executableURL: URL
  let fallbackURL: URL
  let fallbackManifestURL: URL
  let engineArguments: [String]

  static func resolve() -> AquaTypeV2LaunchConfiguration? {
    let fileManager = FileManager.default
    // The same home directory `SquirrelApp` uses, so the `--state-dir` below cannot disagree with the rest of the app.
    let home = SquirrelApp.homeDir
    guard let resourceURL = Bundle.main.resourceURL else { return nil }

    func existing(_ url: URL, executable: Bool = false) -> URL? {
      let found = executable ? fileManager.isExecutableFile(atPath: url.path) : fileManager.fileExists(atPath: url.path)
      return found ? url : nil
    }

    // The engine and its dictionary are bundled in Resources.
    guard let executableURL = existing(resourceURL.appendingPathComponent("AquaTypeEngine"), executable: true),
          let fallbackURL = existing(resourceURL.appendingPathComponent("AquaTypeData/dictionary_fallback.v1.aqds")),
          let fallbackManifestURL = existing(
            resourceURL.appendingPathComponent("AquaTypeData/dictionary_fallback.v1.aqds.manifest.json"))
    else { return nil }
    // The decoder assets (the export directory, the MLX metallib and the character table) travel inside the app
    // bundle too (packaging/build_app.sh).  Missing assets mean the engine serves the traditional path only, never a
    // crash.
    var engineArguments: [String] = []
    if let decoderDir = existing(resourceURL.appendingPathComponent("AquaTypeDecoder")) {
      engineArguments += ["--decoder-dir", decoderDir.path]
    }
    if let metallib = existing(resourceURL.appendingPathComponent("lib/mlx.metallib")) {
      engineArguments += ["--metallib", metallib.path]
    }
    if let charTable = existing(resourceURL.appendingPathComponent("AquaTypeData/char_to_keys.json")) {
      engineArguments += ["--char-to-keys", charTable.path]
    }
    // AQUATYPE_STATE_DIR lets the self-tests point the engine at a temporary directory, so checking the input method
    // never writes into the user's personal memory.
    let stateDir = ProcessInfo.processInfo.environment["AQUATYPE_STATE_DIR"].map { URL(fileURLWithPath: $0) }
      ?? home.appendingPathComponent(".aquatype")
    engineArguments += ["--state-dir", stateDir.path]
    return AquaTypeV2LaunchConfiguration(
      executableURL: executableURL,
      fallbackURL: fallbackURL,
      fallbackManifestURL: fallbackManifestURL,
      engineArguments: engineArguments)
  }

}

private struct AquaTypeV2Failure: Decodable {
  let schema: String
  let clientID: String
  let clientGeneration: UInt64
  let rawIdentity: String
  let eventID: String?
  let reason: String
  enum CodingKeys: String, CodingKey {
    case schema
    case clientID = "client_id"
    case clientGeneration = "client_generation"
    case rawIdentity = "raw_identity"
    case eventID = "event_id"
    case reason
  }
}

private enum AquaTypeV2ClientError: LocalizedError {
  case unavailable
  case processExited(Int32)
  case writeFailed
  case rejected(String)
  case timedOut

  var errorDescription: String? {
    switch self {
    case .unavailable: return "AquaType V2 executable/assets are unavailable"
    case .processExited(let status): return "AquaType V2 exited with status \(status)"
    case .writeFailed: return "AquaType V2 JSONL write failed"
    case .rejected(let why): return "AquaType V2 refused the request: \(why)"
    case .timedOut: return "AquaType V2 did not answer in time"
    }
  }
}

private final class AquaTypeV2JSONLClient {
  static let shared = AquaTypeV2JSONLClient()

  fileprivate typealias PanelCompletion = (Result<AquaTypeV2UIResponse, Error>) -> Void
  fileprivate typealias CommitCompletion = (Result<AquaTypeV2CommitAck, Error>) -> Void
  private let queue = DispatchQueue(label: "com.aquatype.squirrel.v2-jsonl")
  private var process: Process?
  private var inputPipe: Pipe?
  private var outputPipe: Pipe?
  private var errorPipe: Pipe?
  private var outputBuffer = Data()
  /// An engine that stops answering is ended and the next request starts another: two requests in a row with no
  /// reply (3 s) is far past any decode.  An engine that has not answered yet is still starting - loading the model and,
  /// the first time after an install or a macOS update, compiling its GPU code - so its silence is not counted until
  /// `startupAllowance` has passed.  One that keeps exiting just after it starts (a missing GPU, a damaged model) is not
  /// started again on every key: after the second quick exit in a row it waits, 2.5 s and then twice as long each time,
  /// up to a minute.  Any reply resets both.
  private var unansweredInARow = 0
  private var launchedAt: TimeInterval = 0
  private var answeredSinceLaunch = false
  fileprivate static let startupAllowance: TimeInterval = 30
  /// Which launch the output being read belongs to: what an engine wrote just before it was replaced is dropped, not
  /// read as the start of the next engine's first reply.
  private var launchCount: UInt64 = 0
  private var quickExitsInARow = 0
  private var noLaunchBefore: TimeInterval = 0
  /// Waiters carry a token because several requests can share a key (the same generation and keys sent twice), and a
  /// timeout must cancel only its own waiter.
  private var pendingPanels: [String: [(token: UInt64, run: PanelCompletion)]] = [:]
  private var pendingCommits: [String: [(token: UInt64, run: CommitCompletion)]] = [:]
  private var nextWaiterToken: UInt64 = 0

  /// Serial queue only, which is where every access to the pending tables happens.
  private func makeWaiterToken() -> UInt64 {
    nextWaiterToken &+= 1
    return nextWaiterToken
  }

  /// Remove one waiter by token and hand it the failure.  Returns nothing when that waiter is already gone, which is
  /// the normal case: the reply arrived first.
  @discardableResult
  private func expire<C>(_ table: inout [String: [(token: UInt64, run: C)]],
                         key: String, token: UInt64, error: Error, deliver: @escaping (C, Error) -> Void) -> Bool {
    guard var waiters = table[key], let index = waiters.firstIndex(where: { $0.token == token }) else { return false }
    let waiter = waiters.remove(at: index)
    if waiters.isEmpty { table[key] = nil } else { table[key] = waiters }
    DispatchQueue.main.async { deliver(waiter.run, error) }
    return true
  }

  /// Nothing waits for ever: a panel waiting on a reply that never comes looks exactly like a broken one.  A keystroke's
  /// decode takes tens of milliseconds, so a second and a half is far outside anything normal and still fails fast; an
  /// unanswered request is asked again (aquaTypeRetryUnansweredRequest).
  fileprivate static let replyTimeout: TimeInterval = 1.5
  /// A decode that starts from nothing (after Backspace, or a piece committed) takes longer the more keys it has: a
  /// key-mash near the 190-key cap took 2.5 s on an M4.  So a request's timeout grows with its keys, by more than a
  /// decode does, and a long input that is merely slow is not taken for an engine that has stopped (two timeouts in a
  /// row end it).
  fileprivate static func replyTimeout(forKeys count: Int) -> TimeInterval { replyTimeout + 0.02 * Double(count) }

  fileprivate func send(_ request: AquaTypeV2UIRequest,
                        completion: @escaping PanelCompletion) {
    queue.async { [weak self] in
      guard let self = self else { return }
      let key = self.panelKey(clientID: request.clientID,
                              generation: request.clientGeneration,
                              rawIdentity: request.rawIdentity)
      let token = self.makeWaiterToken()
      self.pendingPanels[key, default: []].append((token, completion))
      do {
        try self.ensureProcess()
        try self.write(request)
      } catch {
        self.stopProcess()
        self.failAll(error)
        return
      }
      self.queue.asyncAfter(deadline: .now() + Self.replyTimeout(forKeys: request.rawKeys.utf8.count)) { [weak self] in
        guard let self = self else { return }
        guard self.expire(&self.pendingPanels, key: key, token: token,
                          error: AquaTypeV2ClientError.timedOut, deliver: { $0(.failure($1)) }) else { return }
        guard self.answeredSinceLaunch
                || ProcessInfo.processInfo.systemUptime - self.launchedAt > Self.startupAllowance else { return }
        self.unansweredInARow += 1
        if self.unansweredInARow >= 2 {
          self.unansweredInARow = 0
          // Killed, not asked to quit: a process that is stuck (or stopped) never gets to act on a request to quit.
          if let pid = self.process?.processIdentifier, self.process?.isRunning == true { kill(pid, SIGKILL) }
          self.stopProcess()
          self.failAll(AquaTypeV2ClientError.timedOut)
        }
      }
    }
  }

  fileprivate func sendCommit(_ message: AquaTypeV2CommitMessage,
                              completion: @escaping CommitCompletion) {
    queue.async { [weak self] in
      guard let self = self else { return }
      let key = self.commitKey(clientID: message.clientID,
                               eventID: message.eventID,
                               rawIdentity: message.rawIdentity)
      let token = self.makeWaiterToken()
      self.pendingCommits[key, default: []].append((token, completion))
      do {
        try self.ensureProcess()
        try self.write(message)
      } catch {
        self.stopProcess()
        self.failAll(error)
        return
      }
      self.queue.asyncAfter(deadline: .now() + Self.replyTimeout) { [weak self] in
        guard let self = self else { return }
        self.expire(&self.pendingCommits, key: key, token: token,
                    error: AquaTypeV2ClientError.timedOut) { $0(.failure($1)) }
      }
    }
  }

  private func write<T: Encodable>(_ value: T) throws {
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
    var data = try encoder.encode(value)
    data.append(0x0A)
    guard let handle = inputPipe?.fileHandleForWriting else {
      throw AquaTypeV2ClientError.writeFailed
    }
    try handle.write(contentsOf: data)
  }

  /// The running engine's process id, for the self-test that makes it fail.
  fileprivate var selftestProcessIdentifier: Int32? {
    queue.sync { process?.isRunning == true ? process?.processIdentifier : nil }
  }
  /// The engine unavailable, as with missing assets or during the restart backoff: the running one is ended and no
  /// other is started until this is cleared.
  fileprivate func selftestSetUnavailable(_ unavailable: Bool) {
    queue.sync {
      selftestUnavailable = unavailable
      if unavailable, let pid = process?.processIdentifier, process?.isRunning == true { kill(pid, SIGKILL) }
    }
  }
  private var selftestUnavailable = false

  /// Start the engine as soon as the input method is chosen, if it is not already up.  Otherwise the first keystroke
  /// would start it and wait for the process, the model and the GPU pipelines, many times longer than a request to a
  /// running engine takes.  The moment between choosing an input method and typing is where that wait belongs.
  fileprivate func warmUp() {
    queue.async { [weak self] in try? self?.ensureProcess() }
  }

  private func ensureProcess() throws {
    if process?.isRunning == true { return }
    guard ProcessInfo.processInfo.systemUptime >= noLaunchBefore, !selftestUnavailable else {
      throw AquaTypeV2ClientError.unavailable
    }
    guard let configuration = AquaTypeV2LaunchConfiguration.resolve() else {
      throw AquaTypeV2ClientError.unavailable
    }
    let task = Process()
    let input = Pipe()
    let output = Pipe()
    let errors = Pipe()
    task.executableURL = configuration.executableURL
    task.arguments = [
      "--command", "serve-ui-jsonl",
      "--fallback-aqds", configuration.fallbackURL.path,
      "--fallback-manifest", configuration.fallbackManifestURL.path,
    ] + configuration.engineArguments
    task.standardInput = input
    task.standardOutput = output
    task.standardError = errors
    outputBuffer.removeAll(keepingCapacity: true)
    launchCount &+= 1
    answeredSinceLaunch = false
    let launch = launchCount
    output.fileHandleForReading.readabilityHandler = { [weak self] handle in
      let data = handle.availableData
      guard !data.isEmpty else {
        // Empty means end of file, and a handler that is still installed keeps being called for it - a spin on a
        // dispatch thread for as long as it takes the termination handler to be scheduled and tear the process down.
        // Detaching here ends it; `stopProcess` clears the handler again and closes the pipe.
        handle.readabilityHandler = nil
        return
      }
      self?.queue.async { [weak self] in
        guard let self, self.launchCount == launch else { return }
        self.consume(data)
      }
    }
    // Always drain stderr so a persistent child can never block on a full pipe.
    errors.fileHandleForReading.readabilityHandler = { handle in
      if handle.availableData.isEmpty { handle.readabilityHandler = nil }
    }
    task.terminationHandler = { [weak self] terminated in
      self?.queue.async { [weak self] in
        self?.handleTermination(terminated)
      }
    }
    process = task
    inputPipe = input
    outputPipe = output
    errorPipe = errors
    do {
      try task.run()
      launchedAt = ProcessInfo.processInfo.systemUptime
    } catch {
      stopProcess()
      throw error
    }
  }

  private func consume(_ data: Data) {
    outputBuffer.append(data)
    while let newline = outputBuffer.firstIndex(of: 0x0A) {
      let line = outputBuffer.prefix(upTo: newline)
      outputBuffer.removeSubrange(...newline)
      guard !line.isEmpty else { continue }
      // Only a line the engine wrote as an answer counts as one: a stray line on its output (a library's warning)
      // must not cancel the startup allowance or the backoff after quick exits.
      func answered() {
        unansweredInARow = 0
        quickExitsInARow = 0
        answeredSinceLaunch = true
      }
      let decoder = JSONDecoder()
      if let response = try? decoder.decode(AquaTypeV2UIResponse.self, from: line) {
        answered()
        let responseKey = panelKey(clientID: response.clientID,
                                   generation: response.clientGeneration,
                                   rawIdentity: response.rawIdentity)
        let callbacks = (pendingPanels.removeValue(forKey: responseKey) ?? []).map(\.run)
        DispatchQueue.main.async {
          callbacks.forEach { $0(.success(response)) }
        }
        continue
      }
      if let failure = try? decoder.decode(AquaTypeV2Failure.self, from: line),
         failure.schema == "aquatype-ui-failure/v1" {
        answered()
        let panelWaiters = (pendingPanels.removeValue(forKey: panelKey(clientID: failure.clientID,
                                                                      generation: failure.clientGeneration,
                                                                      rawIdentity: failure.rawIdentity)) ?? []).map(\.run)
        let commitWaiters = (failure.eventID.map {
          pendingCommits.removeValue(forKey: commitKey(clientID: failure.clientID,
                                                       eventID: $0,
                                                       rawIdentity: failure.rawIdentity)) ?? []
        } ?? []).map(\.run)
        let error = AquaTypeV2ClientError.rejected(failure.reason)
        DispatchQueue.main.async {
          panelWaiters.forEach { $0(.failure(error)) }
          commitWaiters.forEach { $0(.failure(error)) }
        }
        continue
      }
      if let ack = try? decoder.decode(AquaTypeV2CommitAck.self, from: line) {
        answered()
        let responseKey = commitKey(clientID: ack.clientID,
                                    eventID: ack.eventID,
                                    rawIdentity: ack.rawIdentity)
        let callbacks = (pendingCommits.removeValue(forKey: responseKey) ?? []).map(\.run)
        DispatchQueue.main.async {
          callbacks.forEach { $0(.success(ack)) }
        }
      }
    }
  }

  private func handleTermination(_ terminated: Process) {
    guard process === terminated else { return }
    let status = terminated.terminationStatus
    let now = ProcessInfo.processInfo.systemUptime
    quickExitsInARow = now - launchedAt < 5 ? quickExitsInARow + 1 : 0
    if quickExitsInARow >= 2 {
      noLaunchBefore = now + min(60, 2.5 * pow(2, Double(quickExitsInARow - 2)))
    }
    stopProcess()
    failAll(AquaTypeV2ClientError.processExited(status))
  }

  private func failAll(_ error: Error) {
    let panelCallbacks = pendingPanels.values.flatMap { $0 }.map(\.run)
    let commitCallbacks = pendingCommits.values.flatMap { $0 }.map(\.run)
    pendingPanels.removeAll()
    pendingCommits.removeAll()
    DispatchQueue.main.async {
      panelCallbacks.forEach { $0(.failure(error)) }
      commitCallbacks.forEach { $0(.failure(error)) }
    }
  }

  private func stopProcess() {
    outputPipe?.fileHandleForReading.readabilityHandler = nil
    errorPipe?.fileHandleForReading.readabilityHandler = nil
    if process?.isRunning == true { process?.terminate() }
    try? inputPipe?.fileHandleForWriting.close()
    try? outputPipe?.fileHandleForReading.close()
    try? errorPipe?.fileHandleForReading.close()
    process = nil
    inputPipe = nil
    outputPipe = nil
    errorPipe = nil
    outputBuffer.removeAll(keepingCapacity: true)
  }

  private func panelKey(clientID: String, generation: UInt64, rawIdentity: String) -> String {
    "\(clientID)\n\(generation)\n\(rawIdentity)"
  }

  private func commitKey(clientID: String, eventID: String, rawIdentity: String) -> String {
    "\(clientID)\n\(eventID)\n\(rawIdentity)"
  }
}

final class SquirrelInputController: IMKInputController {
  private struct AquaTypeResponseRow {
    let text: String
    let source: String
    let consumedKeys: Int?
  }

  private static let keyRollOver = 50
  private static var unknownAppCnt: UInt = 0
  private static let aquaRawKeys = Set("1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/- 6347")

  private weak var client: IMKTextInput?
  private let rimeAPI: RimeApi_stdbool = rime_get_api_stdbool().pointee
  private var preedit: String = ""
  private var selRange: NSRange = .empty
  private var caretPos: Int = 0
  private var lastModifiers: NSEvent.ModifierFlags = .init()
  private var session: RimeSessionId = 0
  private var sessionGeneration: UInt64 = 0
  private var schemaId: String = ""
  private var inlinePreedit = false
  // for chord-typing
  private var chordKeyCodes: [UInt32] = .init(repeating: 0, count: SquirrelInputController.keyRollOver)
  private var chordModifiers: [UInt32] = .init(repeating: 0, count: SquirrelInputController.keyRollOver)
  private var chordKeyCount: Int = 0
  private var chordTimer: Timer?
  private var chordDuration: TimeInterval = 0
  private var currentApp: String = ""
  // Last valid Chinese candidate, kept only as an internal commit
  // fallback. The visible inline marked text is Zhuyin now; candidates stay Hanzi.
  private var lastInlineEcho = ""
  private var lastInlineEchoRaw = ""
  // Authoritative raw Dachen buffer owned by Squirrel. Rime's
  // ctx.input can be split/truncated during long compositions, so UI and
  // BackSpace must not depend on it as the only source of truth.
  private var aquaTypeSwiftRawBuffer = ""
  private var aquaTypeClientSeq: UInt64 = 0
  private var aquaTypeActiveClientSeq = ""
  private let aquaTypeV2ClientID = UUID().uuidString.lowercased()
  private var aquaTypeV2PublishedGeneration: UInt64?
  private var aquaTypeV2GenerationContext = AquaTypeV2GenerationContextStore()
  private var aquaTypeV2ResponseGeneration: UInt64?
  private var aquaTypeV2ResponseRaw = ""
  private var aquaTypeV2ResponseRows = [AquaTypeResponseRow]()
  /// Keys typed while librime could not give this controller a session, with the client each was typed into, and the
  /// timer that keeps asking for one.
  private var aquaTypeKeysAwaitingSession: [(event: NSEvent, client: Any?)] = []
  private var aquaTypeSessionRetryTimer: Timer?
  private var aquaTypeLastPanelRaw = ""
  /// The one request the engine is working on, and the newest one waiting for it to finish.
  private var aquaTypeInFlightBridgeRaw: String? = nil
  private var aquaTypeInFlightBridgeGeneration: UInt64? = nil
  private var aquaTypeQueuedBridgeRaw = ""
  /// A key that ends the composition (Return, punctuation) pressed before the engine has answered for the keys typed,
  /// with the keys typed after it, held until the answer arrives (aquaTypeHoldForAnswer); the composition they waited
  /// for, while they are handled; and the token of the wait's own time limit.
  private var aquaTypeKeysAwaitingAnswer: [(event: NSEvent, client: Any?)] = []
  private var aquaTypeAnswerWaitedGeneration: UInt64? = nil
  private var aquaTypeReplayingKeysAwaitingAnswer = false
  /// The key waiting is a Shift tap to English, already judged a tap: it switches when it is handled, whatever has
  /// happened meanwhile (a click, an arrival that reset the modifier state).
  private var aquaTypeHeldShiftTap = false
  private var aquaTypeAnswerWaitToken = 0
  /// The composition a reply timeout was last seen for, and how many times it has been asked again.
  private var aquaTypeUnansweredRaw = ""
  private var aquaTypeUnansweredAttempts = 0
  private var aquaTypeLastPanelCandidates = [String]()
  /// What the panel is currently offering, for the self-tests to read.  Read-only and derived, so it cannot change
  /// what typing does.
  var aquaTypeSelftestCandidates: [String] { aquaTypeLastPanelCandidates }
  var aquaTypeSelftestCandidateCount: Int { aquaTypeLastPanelCandidates.count }
  /// The composition's length in raw keys - the quantity `aquaTypeMaxComposingKeys` is a cap on.
  var aquaTypeSelftestComposingKeyCount: Int { aquaTypeCurrentRawBuffer().utf8.count }
  /// Keys held behind a Return or punctuation waiting for the engine's answer.
  var aquaTypeSelftestHeldKeyCount: Int { aquaTypeKeysAwaitingAnswer.count }
  /// The cap itself, which lives in a fileprivate extension.
  static var aquaTypeSelftestKeyCap: Int { aquaTypeMaxComposingKeys }
  /// The engine's process id while it runs.
  var aquaTypeSelftestEnginePID: Int32? { AquaTypeV2JSONLClient.shared.selftestProcessIdentifier }
  func aquaTypeSelftestSetEngineUnavailable(_ unavailable: Bool) {
    AquaTypeV2JSONLClient.shared.selftestSetUnavailable(unavailable)
  }
  /// librime unable to give a session for the next `refusals` attempts, as while its deployer settles on a cold start.
  func aquaTypeSelftestLoseSession(refusals: Int) {
    destroySession()
    aquaTypeSelftestSessionRefusals = refusals
  }
  private var aquaTypeSelftestSessionRefusals = 0
  /// Whether the panel is showing candidates for what is currently typed, rather than for an earlier prefix whose
  /// decode happened to finish.  Commit refuses in the second case on purpose: committing a candidate the user is
  /// looking at but which no longer matches their keys is how text they never approved gets inserted.
  var aquaTypeSelftestPanelIsCurrent: Bool {
    !aquaTypeLastPanelCandidates.isEmpty && aquaTypeLastPanelRaw == aquaTypeCurrentRawBuffer()
  }
  /// How many keys the panel's candidates were computed for, next to how many are typed.
  var aquaTypeSelftestPanelKeyCount: Int { aquaTypeLastPanelRaw.utf8.count }
  /// A Rime option, read and set as the user's own switches would.
  func aquaTypeSelftestOption(_ name: String) -> Bool { rimeAPI.get_option(session, name) }
  func aquaTypeSelftestSetOption(_ name: String, _ value: Bool) {
    rimeAPI.set_option(session, name, value)
    if name == "ascii_mode" { Self.aquaTypeChosenAsciiMode = value }    // as if chosen, so later sessions agree
  }
  /// The highlighted candidate, and where the current answer's groups start.
  var aquaTypeSelftestHighlighted: Int { aquaTypeLastPanelHighlighted }
  var aquaTypeSelftestGroupStarts: [Int]? { aquaTypeCandidateGroupStarts(raw: aquaTypeCurrentRawBuffer()) }
  /// A new session for this controller, as a new text field gets: librime restores the saved mode into it.
  func aquaTypeSelftestRecreateSession() { destroySession(); createSession() }
  /// A composition a failed case left behind, cleared so the next case starts from nothing.
  func aquaTypeSelftestReset() { _ = aquaTypeClearCompositionToEmpty() }
  /// The self-tests run while the person may be using the Mac: they install no global mouse monitors and no input
  /// source observer, and take no real click into account, so nothing the person does reaches the cases.
  static var aquaTypeSelftestIsolated = false
  /// The process of the application being typed into, set by the self-test, which types into no application.
  static var aquaTypeSelftestClientPID: pid_t?
  /// Per panel candidate, how many raw keys it accounts for, or nil when it covers the whole input.  Only the decoder
  /// response fills it; everything else leaves it all-nil.
  ///
  /// It must be written wherever `aquaTypeLastPanelCandidates` is written, and with the same number of elements:
  /// `aquaTypeCommitPanelCandidate` indexes the two together, so a stale entry against a fresh list makes a candidate
  /// that covers the whole input take the partial-commit path and leave behind a fragment the user did not type.
  private var aquaTypeLastPanelConsumed = [Int?]()
  private var aquaTypeLastPanelComments = [String]()
  private var aquaTypeLastPanelHighlighted = 0
  /// The controller that currently owns the input session, or nil.  A key arriving at any other controller makes that
  /// one the owner and clears the render state it would otherwise carry over.
  private(set) static weak var aquaTypeActiveController: SquirrelInputController?

  static var aquaTypeLastAnnouncedAt: TimeInterval = 0
  /// Bumped on every arrival in a field; an announcement chain stops as soon as it is not the current one.
  static var aquaTypeAnnounceToken: UInt64 = 0
  /// The client the badge was last raised for.  Focus has to leave and come back before it is raised again.
  static var aquaTypeAnnouncedClient: ObjectIdentifier?
  /// The watch on the user's clicks (see aquaTypeWatchClicks), and where the left button last went down, whether a
  /// composition was open then, and where the caret was.
  private static var aquaTypeClickMonitors: [Any] = []
  private static var aquaTypeMouseDown: (at: NSPoint, composing: Bool, caret: NSRect?)?
  /// Where the last badge went, for the self-test.
  private(set) var aquaTypeSelftestLastBadgeAt: NSRect?
  private let aquaTypePanelRows = 9
  // The most candidates the shell asks the engine for: seven pages of nine.  The engine passes on that many of the
  // model's candidates, and adds its third source after them.
  private let aquaTypePanelGridMax = 63

  /// The pieces of a sentence that is being committed a part at a time.  Only the shell knows when a sentence is
  /// finished (a partial commit leaves a composition running, a whole one ends it), so the shell keeps the pieces and
  /// sends the finished sentence itself.
  private var aquaTypeCommitPieces: [(text: String, keys: String)] = []
  private static let aquaTypeMaxAssembledCharacters = 120
  /// Where the panel is placed for the composition in progress, and the caret report it was taken from (see showPanel).
  private var aquaTypePanelAnchorRect: NSRect? {
    didSet {
      if aquaTypePanelAnchorRect == nil { aquaTypePanelAnchorReading = nil; aquaTypePanelOffLineReading = nil }
    }
  }
  private var aquaTypePanelAnchorReading: NSRect?
  /// The last caret report that was on a different line from the anchor's, waiting for a second one to confirm it.
  private var aquaTypePanelOffLineReading: NSRect?
  /// Bumped by every render; a scheduled re-check of the caret runs only if it is still the latest.
  private var aquaTypePanelRecheckToken: UInt64 = 0
  private var aquaTypeShiftPending = false
  private var aquaTypeShiftDownAt: TimeInterval = 0
  /// The longest a Shift press may last and still switch 中/A.
  static let aquaTypeShiftTapLimit: TimeInterval = 0.5
  private var aquaTypeShiftChordUsed = false
  /// Whether the last quote of each kind was an opening one; the next one closes it.
  private var aquaTypeSingleQuoteOpen = false
  private var aquaTypeDoubleQuoteOpen = false
  private var aquaTypeSkipNextRimeUpdate = false
  private var aquaTypeForcePaletteHideDepth = 0
  // swiftlint:disable:next cyclomatic_complexity
  override func handle(_ event: NSEvent!, client sender: Any!) -> Bool {
    guard let event = event else { return false }
    let modifiers = event.modifierFlags
    let changes = lastModifiers.symmetricDifference(modifiers)

    // Return true to indicate the key input was received and dealt with.
    // Key processing will not continue in that case.  In other words the
    // system will not deliver a key down event to the application.
    // Returning false means the original key down will be passed on to the client.
    var handled = false

    // Before a session is made, so that one made for this key is made for this application and gets its options.
    self.client ?= sender as? IMKTextInput
    if !aquaTypeSessionAlive {
      createSession()
      if session == 0 {
        // A keystroke is never lost because librime is not ready for it.  `create_session` fails while the deployer
        // is still settling - on a cold start, and on a busy machine for longer than any short wait - which is exactly
        // when the first key arrives, and returning false would hand that key to the application: the first thing
        // typed would go in as a Latin letter.  So it is held, with any keys after it, and handled in order once
        // there is a session.
        if aquaTypeHoldForSession(event, client: sender) { return true }
        return false
      }
    }
    if !aquaTypeKeysAwaitingSession.isEmpty { aquaTypeHandleKeysAwaitingSession() }
    if !aquaTypeKeysAwaitingAnswer.isEmpty {
      if aquaTypeFollowsHeldKey(event) {
        aquaTypeKeysAwaitingAnswer.append((event, sender))
        return true
      }
      // Backspace right after the held Return, punctuation or Shift tap takes it back: the composition stays as it
      // was, nothing switches, and the modifier state is what it is now.  Any other key - Backspace after keys typed
      // behind it, which then takes back the last of them, and Escape, which then cancels only what was typed behind
      // it - ends the wait with what there is, and follows: the held keys first, every one, those held again for a
      // composition typed behind the first included.
      let bare = event.modifierFlags.isDisjoint(with: [.command, .control, .option, .shift])
      if event.type == .keyDown, bare, event.keyCode == UInt16(kVK_Delete),
         !aquaTypeKeysAwaitingAnswer.dropFirst().contains(where: { $0.event.type == .keyDown }) {
        aquaTypeKeysAwaitingAnswer.removeAll()
        aquaTypeAnswerWaitToken &+= 1
        aquaTypeHeldShiftTap = false
        lastModifiers = event.modifierFlags
        aquaTypeShiftPending = false
        aquaTypeShiftChordUsed = false
        return true
      }
      while !aquaTypeKeysAwaitingAnswer.isEmpty { aquaTypeHandleKeysAwaitingAnswer() }
    }
    // Not while held keys are handled: focus may already have moved on to another controller, which is the active one.
    if SquirrelInputController.aquaTypeActiveController !== self, !aquaTypeReplayingKeysAwaitingAnswer {
      aquaTypeBecomeActiveController()
    }

    if let app = client?.bundleIdentifier(), currentApp != app {
      currentApp = app
      updateAppOptions()
    }
    // Read after the app options, so a mode that arriving in an application sets is not taken for one the key set.
    let asciiBefore = rimeAPI.get_option(session, "ascii_mode")
    defer {
      if session != 0, rimeAPI.get_option(session, "ascii_mode") != asciiBefore { aquaTypeAnnounceModeChange() }
    }

    switch event.type {
    case .flagsChanged:
      if lastModifiers == modifiers {
        handled = true
        break
      }
      var rimeModifiers: UInt32 = SquirrelKeycode.osxModifiersToRime(modifiers: modifiers)
      // For flags-changed event, keyCode is available since macOS 10.15 (#715)
      // Some remote desktop software (e.g. Parsec) sends flagsChanged events with
      // keyCode defaulting to 0 (kVK_ANSI_A) instead of the actual modifier keycode,
      // causing a ghost 'a' keypress. Validate and infer the correct keycode from
      // the changed modifier flags when necessary. (#825)
      var keyCode = event.keyCode
      if !SquirrelKeycode.modifierKeycodes.contains(keyCode) {
        guard let inferred = SquirrelKeycode.inferModifierKeycode(from: changes) else {
          lastModifiers = modifiers
          rimeUpdate()
          handled = true
          break
        }
        keyCode = inferred
      }
      let rimeKeycode: UInt32 = SquirrelKeycode.osxKeycodeToRime(keycode: keyCode, keychar: nil, shift: false, caps: false)

      // A bare Shift toggles Squirrel's 中/A mode. Shift used as a
      // chord with a letter or punctuation is handled by the following keyDown
      // and must not toggle again on release.
      if changes.contains(.shift),
         !modifiers.contains(.control),
         !modifiers.contains(.option),
         !modifiers.contains(.command) {
        if modifiers.contains(.shift) {
          aquaTypeShiftPending = true
          aquaTypeShiftChordUsed = false
          aquaTypeShiftDownAt = event.timestamp
        } else {
          // A tap, not Shift held down: one held for a while was meant for something else, and so was one during which
          // a key went to the application without coming here (Shift+Home in an editor that handles it itself).
          // An event without a time (some remote-desktop software) is not judged by it.
          let timed = event.timestamp > 0 && aquaTypeShiftDownAt > 0 && event.timestamp >= aquaTypeShiftDownAt
          let held = timed ? event.timestamp - aquaTypeShiftDownAt : 0
          // A key pressed between Shift going down and coming up, not one already typed after it.
          let sinceUp = ProcessInfo.processInfo.systemUptime - event.timestamp
          let sinceKey = CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: .keyDown)
          let keyMeanwhile = timed && !Self.aquaTypeSelftestIsolated && !aquaTypeReplayingKeysAwaitingAnswer
            && sinceKey > sinceUp && sinceKey < sinceUp + held
          if aquaTypeShiftPending && !aquaTypeShiftChordUsed && held < Self.aquaTypeShiftTapLimit && !keyMeanwhile {
            // Switching to English commits the composition, so a tap before its answer waits for it, as Return does
            // ("我要買" and Shift, then iPhone).  Nothing is changed before it is held: the release is seen again.
            if let raw = aquaTypeAnswerPending() {
              aquaTypeHeldShiftTap = aquaTypeHold(event, client: sender, raw: raw)
              return true
            }
            aquaTypeToggleAsciiModeFromBareShift()
          }
          aquaTypeShiftPending = false
          aquaTypeShiftChordUsed = false
        }
        lastModifiers = modifiers
        rimeUpdate()
        handled = true
        break
      }

      // Another modifier pressed while Shift is down makes it a chord, even when the key that completes it never
      // arrives here: the system keeps its own shortcuts (Shift+Command+4 takes a screenshot), so the release of Shift
      // that follows must not toggle 中/A.
      if aquaTypeShiftPending { aquaTypeShiftChordUsed = true }

      // Each modifier is reported to librime as itself, not as the event's one keycode.
      func keysym(for flag: NSEvent.ModifierFlags) -> UInt32 {
        guard let code = SquirrelKeycode.keycode(for: flag) else { return rimeKeycode }
        return SquirrelKeycode.osxKeycodeToRime(keycode: code, keychar: nil, shift: false, caps: false)
      }

      if changes.contains(.capsLock) {
        // NOTE: rime assumes XK_Caps_Lock to be sent before modifier changes,
        // while NSFlagsChanged event has the flag changed already.
        // so it is necessary to revert kLockMask.
        rimeModifiers ^= kLockMask.rawValue
        _ = processKey(keysym(for: .capsLock), modifiers: rimeModifiers)
      }

      // Need to process release before modifier down. Because
      // sometimes release event is delayed to next modifier keydown.
      var buffer = [(keycode: UInt32, modifier: UInt32)]()
      for flag in [NSEvent.ModifierFlags.shift, .control, .option, .command] where changes.contains(flag) {
        if modifiers.contains(flag) { // New modifier
          buffer.append((keycode: keysym(for: flag), modifier: rimeModifiers))
        } else { // Release
          buffer.insert((keycode: keysym(for: flag), modifier: rimeModifiers | kReleaseMask.rawValue), at: 0)
        }
      }
      for (keycode, modifier) in buffer {
        _ = processKey(keycode, modifiers: modifier)
      }

      lastModifiers = modifiers
      rimeUpdate()

    case .keyDown:
      if modifiers.contains(.shift) {
        aquaTypeShiftChordUsed = true
      }
      let keyCode = event.keyCode
      var keyChars = event.charactersIgnoringModifiers
      let capitalModifiers = modifiers.isSubset(of: [.shift, .capsLock])
      if let code = keyChars?.first,
         (capitalModifiers && !code.isLetter) || (!capitalModifiers && !code.isASCII) {
        keyChars = event.characters
      }

      // translate osx keyevents to rime keyevents
      if let char = keyChars?.first {
        let rimeKeycode = SquirrelKeycode.osxKeycodeToRime(keycode: keyCode, keychar: char,
                                                           shift: modifiers.contains(.shift),
                                                           caps: modifiers.contains(.capsLock))
        // osxKeycodeToRime signals "no mapping" with XK_VoidSymbol (0xffffff), not with 0.
        if rimeKeycode != UInt32(XK_VoidSymbol) {
          let rimeModifiers = SquirrelKeycode.osxModifiersToRime(modifiers: modifiers)
          if modifiers.contains(.command) {
            // Every Command chord goes to the application, including the digits.  Cmd+1..9 are the browser's tabs and
            // the window switcher's windows, and with a composition open nothing separates "give me candidate two"
            // from "take me to tab two".  Eating them would break the application being typed into, which is the
            // worse of the two failures.  A composition is committed first, as for punctuation: left marked, the
            // application can act on it (paste over it, select it, undo past it) without AquaType knowing, and the next
            // key would bring the old keys back.  Command+Backspace (to the start of the line) takes the composition
            // back instead.
            if !aquaTypeCurrentRawBuffer().isEmpty {
              if keyCode == UInt16(kVK_Delete) {
                _ = aquaTypeBeginClientSeq()
                handled = aquaTypeClearCompositionToEmpty()
                break
              }
              // Not learned: what a Command chord commits is often undone, cut or thrown away by the chord itself.
              aquaTypeCommitUnremembered = true
              aquaTypeCommitCompositionBeforePunctuation()
              aquaTypeCommitUnremembered = false
            }
            break
          }
          // Caps Lock on in Chinese mode types what the keys say, as the built-in 注音 does - digits and punctuation
          // too, which would otherwise be bopomofo in the middle of A123456789.  Shift+Option with a letter is an
          // application's shortcut (Format Document in VS Code).  Either one commits a composition first.
          let typed = event.characters ?? ""
          let capsTyping = modifiers.contains(.capsLock) && modifiers.isDisjoint(with: [.control, .option])
            && !typed.isEmpty && typed.unicodeScalars.allSatisfy { (0x20...0x7E).contains($0.value) }
          let shiftOptionLetter = modifiers.isSuperset(of: [.shift, .option]) && !modifiers.contains(.control)
            && event.charactersIgnoringModifiers?.first?.isLetter == true
          if (capsTyping || shiftOptionLetter) && !rimeAPI.get_option(session, "ascii_mode") {
            // A shortcut cannot wait: a key held is handed back to no one.
            if capsTyping, let raw = aquaTypeAnswerPending() {
              handled = aquaTypeHold(event, client: sender, raw: raw)
              break
            }
            aquaTypeCommitCompositionBeforePunctuation()
            handled = false
            break
          }
          if aquaTypeHoldForAnswer(event, client: sender, rimeKeycode: rimeKeycode, modifiers: rimeModifiers) {
            handled = true
            break
          }
          handled = processKey(rimeKeycode, modifiers: rimeModifiers)
          if aquaTypeSkipNextRimeUpdate {
            aquaTypeSkipNextRimeUpdate = false
          } else {
            rimeUpdate()
          }
        }
      } else if modifiers.contains(.command) {
        handled = false
        break
      }

    default:
      break
    }

    return handled
  }

  func selectCandidate(_ index: Int) -> Bool {
    let raw = aquaTypeCurrentRawBuffer()
    if !raw.isEmpty,
       raw == aquaTypeLastPanelRaw,
       index >= 0,
       index < aquaTypeLastPanelCandidates.count {
      aquaTypeLastPanelHighlighted = index
      // Through the same commit as the keyboard, so a candidate that covers only part of the composition leaves the
      // rest to be typed on: clicking and pressing Return mean the same thing.
      return aquaTypeCommitPanelCandidate(index: index, raw: raw)
    }
    let success = rimeAPI.select_candidate_on_current_page(session, index)
    if success {
      rimeUpdate()
    }
    return success
  }

  // swiftlint:disable:next identifier_name
  func page(up: Bool) -> Bool {
    let raw = aquaTypeCurrentRawBuffer()
    if !raw.isEmpty,
       !aquaTypeLastPanelCandidates.isEmpty,
       raw == aquaTypeLastPanelRaw {
      return up ? aquaTypeOpenPreviousPageType(raw: raw) : aquaTypeOpenNextPageType(raw: raw)
    }
    var handled = false
    handled = rimeAPI.change_page(session, up)
    if handled {
      rimeUpdate()
    }
    return handled
  }

  func moveCaret(forward: Bool) -> Bool {
    let currentCaretPos = rimeAPI.get_caret_pos(session)
    guard let input = rimeAPI.get_input(session) else { return false }
    if forward {
      if currentCaretPos <= 0 {
        return false
      }
      rimeAPI.set_caret_pos(session, currentCaretPos - 1)
    } else {
      let inputStr = String(cString: input)
      if currentCaretPos >= inputStr.utf8.count {
        return false
      }
      rimeAPI.set_caret_pos(session, currentCaretPos + 1)
    }
    rimeUpdate()
    return true
  }

  override func recognizedEvents(_ sender: Any!) -> Int {
    return Int(NSEvent.EventTypeMask.Element(arrayLiteral: .keyDown, .flagsChanged).rawValue)
  }

  override func activateServer(_ sender: Any!) {
    self.client ?= sender as? IMKTextInput
    var keyboardLayout = NSApp.squirrelAppDelegate.config?.getString("keyboard_layout") ?? ""
    if keyboardLayout == "last" || keyboardLayout == "" {
      keyboardLayout = ""
    } else if keyboardLayout == "default" {
      keyboardLayout = "com.apple.keylayout.ABC"
    } else if !keyboardLayout.hasPrefix("com.apple.keylayout.") {
      keyboardLayout = "com.apple.keylayout.\(keyboardLayout)"
    }
    if keyboardLayout != "" {
      client?.overrideKeyboard(withKeyboardNamed: keyboardLayout)
    }
    preedit = ""
    // The session as well as the engine: both of them are things the first keystroke would otherwise pay for.
    if !aquaTypeSessionAlive { createSession() }
    AquaTypeV2JSONLClient.shared.warmUp()
    aquaTypeBecomeActiveController()
    if !Self.aquaTypeSelftestIsolated { Self.aquaTypeWatchClicks() }
    // The menu bar shows one icon whichever mode this is in, so arriving in a field without being told the mode means
    // finding out by typing.  Show it briefly on arrival.
    //
    // Only on an actual arrival: the system also calls `activateServer` for its own reasons while the same field stays
    // focused, including on modifier keys.  So the badge is raised once per focused client, and not again until focus
    // has left, which `deactivateServer` records.
    //
    // And only when the arrival follows a left click on the caret's line.  Focus also comes back to a field on
    // Command-Tab, a window or tab switch, or a closed dialog, and a badge on each of those is noise.  Clicking into a
    // field puts the caret where you clicked, while clicking a tab, a title bar or the page body leaves the mouse far
    // from it.
    let arrivingClient = client.map { ObjectIdentifier($0 as AnyObject) }
    if arrivingClient == nil || arrivingClient != SquirrelInputController.aquaTypeAnnouncedClient {
      SquirrelInputController.aquaTypeAnnouncedClient = arrivingClient
      // Modifier state starts from what is held now, on an arrival only: what was held when focus last left the field
      // (a Shift held through the click that left it) says nothing about the next key here, and kept, it would swallow
      // the next Shift tap.  The calls that come while the same field stays focused - on modifier keys among them - must
      // not touch it, or they would swallow a Shift tap in progress.
      lastModifiers = NSEvent.modifierFlags
      aquaTypeShiftPending = false
      aquaTypeShiftChordUsed = false
      // The application's own options before the chosen mode, so that the mode is right before the first key here.
      if let app = client?.bundleIdentifier(), currentApp != app {
        currentApp = app
        updateAppOptions()
      }
      aquaTypeApplyChosenAsciiMode()
      SquirrelInputController.aquaTypeAnnounceToken &+= 1
      let clickAge = Self.aquaTypeSelftestIsolated ? .infinity
        : CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: .leftMouseDown)
      let buttonDown = !Self.aquaTypeSelftestIsolated && NSEvent.pressedMouseButtons & 1 != 0
      if Self.aquaTypeArrivalFollowsClick(secondsSinceLeftMouseDown: clickAge, leftButtonDown: buttonDown) {
        aquaTypeAnnounceInputModeWhenReady(token: SquirrelInputController.aquaTypeAnnounceToken, attempt: 0,
                                           click: NSEvent.mouseLocation)
      }
    }
  }

  /// A field does not always have a caret the moment it takes focus: a web view, an Electron app, or anything that lays
  /// out asynchronously answers the caret query with nothing for a while after `activateServer`.  So ask again for up
  /// to about a second and announce on the first answer that has a caret.  A window with keyboard focus and no text
  /// field in it never answers, and gets no badge.
  func aquaTypeAnnounceInputModeWhenReady(token: UInt64, attempt: Int, click: NSPoint, caretBefore: NSRect? = nil) {
    let delays: [Double] = [0.05, 0.1, 0.15, 0.2, 0.25, 0.3]
    guard attempt < delays.count else {
      return
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + delays[attempt]) { [weak self] in
      guard let self, token == SquirrelInputController.aquaTypeAnnounceToken else { return }
      // Keep asking until the caret is both there and where the click sent it - on the clicked line, or moved from
      // where it was before the click: a field that lays out asynchronously can report its old caret position first
      // and the clicked one a moment later.
      if let caret = self.aquaTypeCaretRect,
         Self.aquaTypeClick(click, isOnCaretLineOf: caret) || Self.aquaTypeCaret(caret, movedFrom: caretBefore) {
        // Two announcements in quick succession are the same arrival seen twice; the badge should not blink for that.
        let nowUp = ProcessInfo.processInfo.systemUptime
        guard nowUp - SquirrelInputController.aquaTypeLastAnnouncedAt > 0.25 else { return }
        SquirrelInputController.aquaTypeLastAnnouncedAt = nowUp
        // At the caret, which is where the user looks to type: a click past the end of a line, or in a wide field's
        // empty space, leaves the mouse well away from it.
        self.aquaTypeShowModeBadge(at: caret)
      } else {
        self.aquaTypeAnnounceInputModeWhenReady(token: token, attempt: attempt + 1, click: click, caretBefore: caretBefore)
      }
    }
  }

  /// Watch the user's clicks and scrolls, from the first activation on.  The system tells the input method when focus
  /// arrives in a field and nothing about a click inside the field that already has it, so in a note, a document or
  /// any long text the mode was never shown however often the caret was moved; and a Shift held through a click is
  /// not a bare Shift.  Mouse events can be watched without any permission (keys could not be), and only where the
  /// button went down and the click count are read.
  static func aquaTypeWatchClicks() {
    guard aquaTypeClickMonitors.isEmpty else { return }
    // A Shift held through a click or a scroll was a Shift-click (extending a selection) or a sideways scroll, not a
    // bare Shift: its release does not switch 中/A.
    if let chord = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown, .otherMouseDown, .scrollWheel],
                                                     handler: { _ in aquaTypeMouseUsed() }) {
      aquaTypeClickMonitors.append(chord)
    }
    if let down = NSEvent.addGlobalMonitorForEvents(matching: .leftMouseDown, handler: { _ in
      let controller = aquaTypeActiveController
      let composing = controller.map { !$0.aquaTypeCurrentRawBuffer().isEmpty } ?? false
      aquaTypeMouseDown = (NSEvent.mouseLocation, composing, controller?.aquaTypeCaretRect)
      if composing {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak controller] in
          controller?.aquaTypeReclaimTakenEchoAndHeldKeys()
        }
      }
    }) {
      aquaTypeClickMonitors.append(down)
    }
    if let up = NSEvent.addGlobalMonitorForEvents(matching: .leftMouseUp, handler: { event in
      defer { aquaTypeMouseDown = nil }
      guard let down = aquaTypeMouseDown, let controller = aquaTypeActiveController else { return }
      controller.aquaTypeClicked(at: NSEvent.mouseLocation, pressedAt: down.at, clickCount: event.clickCount,
                                 composing: down.composing, caretBefore: down.caret)
    }) {
      aquaTypeClickMonitors.append(up)
    }
  }

  /// A mouse button or the scroll wheel was used: a Shift held now is part of a chord.
  static func aquaTypeMouseUsed() { aquaTypeActiveController?.aquaTypeShiftChordUsed = true }

  /// A click while this controller has the focused field: the badge at the caret, once the application has put the
  /// caret where the click sends it - on the clicked line, or anywhere else the caret moved to (a click below the last
  /// line of a note puts it at the end of the text).  Only a single click: not a drag (a selection), not a double or
  /// triple click (a word or a line selected; a click state of 0, which assistive and remote-control software send, is
  /// a single click), not a click that ended a composition.  A click that leaves the caret where it was and is not on
  /// its line - a toolbar, a sidebar - stays quiet.
  func aquaTypeClicked(at point: NSPoint, pressedAt down: NSPoint, clickCount: Int, composing: Bool, caretBefore: NSRect?) {
    guard client != nil, clickCount <= 1, !composing, hypot(point.x - down.x, point.y - down.y) < 4,
          aquaTypeCurrentRawBuffer().isEmpty else { return }
    Self.aquaTypeAnnounceToken &+= 1
    aquaTypeAnnounceInputModeWhenReady(token: Self.aquaTypeAnnounceToken, attempt: 0, click: point, caretBefore: caretBefore)
  }

  /// Did the caret move from where it was when the button went down?  Only a caret that was reported before can have
  /// moved: without one, nothing says the click placed it.
  static func aquaTypeCaret(_ caret: NSRect, movedFrom before: NSRect?) -> Bool {
    guard let before else { return false }
    return abs(caret.minX - before.minX) > 1 || abs(caret.minY - before.minY) > 1
  }

  /// Is this a caret the panel can be placed against?  Applications answer the caret query with whatever they have,
  /// and not all of it is a caret: a rectangle with no height (nothing laid out, or no text field at all), one with a
  /// size that is not a number or not a line's, one at the screen's origin (which McBopomofo also treats as no
  /// answer), or one on no screen.
  static func aquaTypeIsUsableCaret(_ rect: NSRect) -> Bool {
    guard rect.origin.x.isFinite, rect.origin.y.isFinite, rect.width.isFinite, rect.height.isFinite,
          rect.height > 1, rect.height < 1000, rect.width >= 0, rect.origin != .zero else { return false }
    let point = NSPoint(x: rect.minX, y: rect.midY)
    return NSScreen.screens.contains { $0.frame.contains(point) }
  }

  /// What the client reports for the caret's line, in screen coordinates, usable or not.
  var aquaTypeReportedCaret: NSRect? {
    guard let client else { return nil }
    var caret = NSRect()
    client.attributes(forCharacterIndex: 0, lineHeightRectangle: &caret)
    return caret
  }

  /// The caret's line rectangle in screen coordinates, or nil when the client has no usable caret to report.
  var aquaTypeCaretRect: NSRect? {
    guard let caret = aquaTypeReportedCaret, Self.aquaTypeIsUsableCaret(caret) else { return nil }
    return caret
  }

  /// Did focus arrive because of a click?  The field is told about focus a little after the mouse goes down - more
  /// when the click also brings a slow app forward - so a click up to a second old counts, and a button still held
  /// down counts whatever the clock says.  The time comes from the window server's own record of the last left
  /// mouse-down, which needs no permission to read: no event tap, no Input Monitoring prompt.
  static func aquaTypeArrivalFollowsClick(secondsSinceLeftMouseDown: Double, leftButtonDown: Bool) -> Bool {
    leftButtonDown || (secondsSinceLeftMouseDown >= 0 && secondsSinceLeftMouseDown <= 1.0)
  }

  /// Was the click on the caret's line?  Clicking into a field moves the caret to the click - onto the character
  /// clicked, or to the end of the line when the click is past the text - so the click is level with the caret and
  /// not left of it (bar the field's own padding).  A browser tab, a title bar, a toolbar button or the page around a
  /// search box are all well above, below or beside it.
  ///
  /// The price is one case that stays quiet: clicking far below the last line of a text area puts the caret at the
  /// end of the text, a long way above the click.  Quiet is the right way to be wrong here.
  static func aquaTypeClick(_ click: NSPoint, isOnCaretLineOf caret: NSRect) -> Bool {
    guard caret.height > 1, click.x.isFinite, click.y.isFinite else { return false }
    let pad = max(caret.height, 16)
    let level = click.y >= caret.minY - pad && click.y <= caret.maxY + pad
    let notLeftOfField = click.x >= caret.minX - 40
    return level && notLeftOfField
  }

  // MARK: - The 中 / A badge
  //
  // Shown on two occasions only: a click that puts the caret in a text field (arriving in it, or moving the caret
  // within the field that has focus), and a change of mode made by a key the user pressed (Shift, Shift with a
  // letter, Escape in Vim).  Always at the caret, where the user looks to type.  Nothing else raises it - not a new
  // session restoring the saved mode, not an application's own option, not a window or tab switch.  A badge nobody
  // asked for, drawn wherever the application happened to report a caret, is what flashed in corners and over hidden
  // input boxes.
  //
  // It is drawn at once or not at all, and only against a caret that passed aquaTypeIsUsableCaret: with nothing
  // usable to place it against, no badge is better than one in the wrong place.

  /// A key changed the mode: show the new one at the caret.
  func aquaTypeAnnounceModeChange() {
    guard let caret = aquaTypeCaretRect else {
      return
    }
    aquaTypeShowModeBadge(at: caret)
  }

  func aquaTypeShowModeBadge(at anchor: NSRect) {
    guard session != 0, aquaTypeCurrentRawBuffer().isEmpty, preedit.isEmpty,    // never over a composition
          let panel = NSApp.squirrelAppDelegate.panel else { return }
    let ascii = rimeAPI.get_option(session, "ascii_mode")
    aquaTypeSelftestLastBadgeAt = anchor
    panel.position = anchor
    panel.avoidRect = nil
    panel.inputController = self
    panel.showStatus(long: ascii ? "英文" : "中文", short: ascii ? "A" : "中")
  }

  override init!(server: IMKServer!, delegate: Any!, client: Any!) {
    self.client = client as? IMKTextInput
    super.init(server: server, delegate: delegate, client: client)
    createSession()
  }

  override func deactivateServer(_ sender: Any!) {
    // Keys still waiting for a session belong to the field being left.
    if !aquaTypeKeysAwaitingSession.isEmpty { aquaTypeHandleKeysAwaitingSession() }
    while !aquaTypeKeysAwaitingAnswer.isEmpty { aquaTypeHandleKeysAwaitingAnswer() }
    // macOS often activates the field being entered before it deactivates the one being left: then another controller
    // is already the active one, its badge is on its way, and nothing here may reset what it set.
    let stillActive = SquirrelInputController.aquaTypeActiveController === self
      || SquirrelInputController.aquaTypeActiveController == nil
    if SquirrelInputController.aquaTypeActiveController === self {
      SquirrelInputController.aquaTypeActiveController = nil
    }
    aquaTypeResignActiveController()
    // Focus has left, so the next activateServer is a real arrival and the badge may be raised again.  Any chain
    // still retrying for the field being left is abandoned by bumping the token.
    if stillActive {
      SquirrelInputController.aquaTypeAnnouncedClient = nil
      SquirrelInputController.aquaTypeAnnounceToken &+= 1
    }
    aquaTypeHidePalettes(force: true)
    // The rest of the composition goes into the field being left, and with it a sentence assembled piece by piece is
    // remembered whole; then the pieces are cleared, since a sentence does not continue into another text field.
    commitComposition(sender)
    aquaTypeCommitPieces.removeAll()
    client = nil
  }

  override func hidePalettes() {
    let raw = aquaTypeCurrentRawBuffer()
    if aquaTypeForcePaletteHideDepth == 0,
       !raw.isEmpty,
       NSApp.squirrelAppDelegate.panel?.aquaTypeCompositionPanelVisible == true {
      return
    }
    NSApp.squirrelAppDelegate.panel?.hide(force: aquaTypeForcePaletteHideDepth > 0)
    super.hidePalettes()
  }

  func aquaTypeHidePalettes(force: Bool = false) {
    if force {
      aquaTypeForcePaletteHideDepth += 1
      defer { aquaTypeForcePaletteHideDepth -= 1 }
      hidePalettes()
    } else {
      hidePalettes()
    }
  }

  /*!
   @method
   @abstract   Called when a user action was taken that ends an input session.
   Typically triggered by the user selecting a new input method
   or keyboard layout.
   @discussion When this method is called your controller should send the
   current input buffer to the client via a call to
   insertText:replacementRange:.  Additionally, this is the time
   to clean up if that is necessary.
   */
  /// What a commit replaces in the document: nothing (.empty) but for an echo the application took as text.
  private var aquaTypeCommitReplacing: NSRange = .empty
  /// A commit that is not to be remembered (see the Command chords in `handle`).
  private var aquaTypeCommitUnremembered = false

  /// Where the composition's echo starts in the document, as the application reported it after the echo was last set,
  /// and whether that question is already waiting to be asked.
  private var aquaTypeEchoStart: Int?
  private var aquaTypeEchoStartAsking = false

  /// The composition's echo, taken by the application as typed text: its range in the document, or nil.  Chromium
  /// (Chrome, VS Code, Slack and every other Electron application) and Firefox end a composition on a mouse click by
  /// keeping what the field shows - here the bopomofo, not the characters it stands for - and only then telling the
  /// input method, by which time the caret is usually where the click put it.  The echo is looked for where it began
  /// and just before the caret, and recognised only where the text there is exactly the echo.
  func aquaTypeEchoTakenByClient() -> NSRange? {
    guard let client, !preedit.isEmpty else { return nil }
    let length = preedit.utf16.count
    var starts: [Int] = []
    if let start = aquaTypeEchoStart { starts.append(start) }
    let selected = client.selectedRange()
    if selected.location != NSNotFound, selected.length == 0, selected.location >= length {
      starts.append(selected.location - length)
    }
    for start in starts where start >= 0 {
      let range = NSRange(location: start, length: length)
      if client.attributedSubstring(from: range)?.string == preedit { return range }
    }
    return nil
  }

  /// The application has ended the composition itself (see aquaTypeEchoTakenByClient): it reports no marked text any
  /// more.  What the composition commits replaces the echo it kept, as if the click had committed normally; with
  /// nothing to commit, or the echo not found where it was in an application that answers questions about its text,
  /// the composition ends without anything inserted, so the next key starts afresh instead of bringing the old keys back.
  /// True when the application had ended it.
  @discardableResult
  func aquaTypeReclaimTakenEcho() -> Bool {
    guard !aquaTypeCurrentRawBuffer().isEmpty, let client, !preedit.isEmpty else { return false }
    let marked = client.markedRange()
    guard marked.location == NSNotFound || marked.length == 0 else { return false }
    if let taken = aquaTypeEchoTakenByClient() {
      aquaTypeCommitReplacing = taken
      defer { aquaTypeCommitReplacing = .empty }
      if aquaTypeCommitCompositionForAsciiSwitch() { return true }
    }
    // The echo not found is evidence it is gone only from an application that answers questions about its text; one
    // that reports no length (no document access) may simply not say, and its composition is committed as usual.
    guard client.length() != NSNotFound else { return false }
    _ = aquaTypeBeginClientSeq()
    _ = aquaTypeClearCompositionToEmpty(touchingClient: false)
    return true
  }

  /// aquaTypeReclaimTakenEcho, then any keys held behind the composition: the echo is replaced before they act, since
  /// a Return or punctuation handled first would commit next to the echo the application kept.
  @discardableResult
  func aquaTypeReclaimTakenEchoAndHeldKeys() -> Bool {
    let reclaimed = aquaTypeReclaimTakenEcho()
    while reclaimed, !aquaTypeKeysAwaitingAnswer.isEmpty { aquaTypeHandleKeysAwaitingAnswer() }
    return reclaimed
  }

  override func commitComposition(_ sender: Any!) {
    self.client ?= sender as? IMKTextInput
    if aquaTypeReclaimTakenEchoAndHeldKeys() { return }
    // Held keys belong before whatever ends the composition here.
    while !aquaTypeKeysAwaitingAnswer.isEmpty { aquaTypeHandleKeysAwaitingAnswer() }
    //  commit raw input
    if session != 0 {
      let raw = aquaTypeCurrentRawBuffer()
      if !raw.isEmpty {
        if aquaTypeCommitCompositionForAsciiSwitch() {
          return
        }
        aquaTypeCommitBopomofo(raw: raw)
      } else if let input = rimeAPI.get_input(session) {
        let text = String(cString: input)
        if !text.isEmpty {
          let echo = aquaTypeBopomofoEcho(for: text)
          if !echo.isEmpty {
            client?.insertText(echo, replacementRange: .empty)
          } else {
            commit(string: text)
          }
        }
        rimeAPI.clear_composition(session)
      }
    }
  }

  override func menu() -> NSMenu! {
    let deploy = NSMenuItem(title: NSLocalizedString("Deploy", comment: "Menu item"), action: #selector(deploy), keyEquivalent: "`")
    deploy.target = self
    deploy.keyEquivalentModifierMask = [.control, .option]
    let logDir = NSMenuItem(title: NSLocalizedString("Logs...", comment: "Menu item"), action: #selector(openLogFolder), keyEquivalent: "")
    logDir.target = self
    let setting = NSMenuItem(title: NSLocalizedString("Settings...", comment: "Menu item"), action: #selector(openRimeFolder), keyEquivalent: "")
    setting.target = self
    let menu = NSMenu()
    menu.addItem(deploy)
    menu.addItem(logDir)
    menu.addItem(setting)
    // No "Rime Wiki" item: that documentation describes configuration AquaType overrides (schema, key bindings and
    // panel), so following it would change settings that have no effect.

    return menu
  }

  @objc func deploy() {
    NSApp.squirrelAppDelegate.deploy()
  }

  @objc func openLogFolder() {
    NSApp.squirrelAppDelegate.openLogFolder()
  }

  @objc func openRimeFolder() {
    NSApp.squirrelAppDelegate.openRimeFolder()
  }

  deinit {
    aquaTypeResignActiveController()
    destroySession()
  }
}

private extension SquirrelInputController {

  func onChordTimer(_: Timer) {
    // chord release triggered by timer
    var processedKeys = false
    if chordKeyCount > 0 && session != 0 {
      // simulate key-ups
      for i in 0..<chordKeyCount {
        let handled = rimeAPI.process_key(session, Int32(chordKeyCodes[i]), Int32(chordModifiers[i] | kReleaseMask.rawValue))
        if handled {
          processedKeys = true
        }
      }
    }
    clearChord()
    if processedKeys {
      rimeUpdate()
    }
  }

  func updateChord(keycode: UInt32, modifiers: UInt32) {
    for i in 0..<chordKeyCount where chordKeyCodes[i] == keycode {
      return
    }
    if chordKeyCount >= Self.keyRollOver {
      // you are cheating. only one human typist (fingers <= 10) is supported.
      return
    }
    chordKeyCodes[chordKeyCount] = keycode
    chordModifiers[chordKeyCount] = modifiers
    chordKeyCount += 1
    // reset timer
    if let timer = chordTimer, timer.isValid {
      timer.invalidate()
    }
    chordDuration = 0.1
    if let duration = NSApp.squirrelAppDelegate.config?.getDouble("chord_duration"), duration > 0 {
      chordDuration = duration
    }
    chordTimer = Timer.scheduledTimer(withTimeInterval: chordDuration, repeats: false, block: onChordTimer)
  }

  func clearChord() {
    chordKeyCount = 0
    if let timer = chordTimer {
      if timer.isValid {
        timer.invalidate()
      }
      chordTimer = nil
    }
  }

  func createSession() {
    if aquaTypeSelftestSessionRefusals > 0 {
      aquaTypeSelftestSessionRefusals -= 1
      return
    }
    let app = client?.bundleIdentifier() ?? {
      SquirrelInputController.unknownAppCnt &+= 1
      return "UnknownApp\(SquirrelInputController.unknownAppCnt)"
    }()
    print("createSession: \(app)")
    currentApp = app
    // A composition in progress survives a session that had to be remade.  `handle` remakes the session whenever
    // `find_session` says the old one is gone, which on a cold start can happen on the second keystroke, and clearing
    // the buffer then would lose the key already typed (`5j4` would come out as ㄨˋ).  The keys are replayed into the
    // new session as well as kept, so librime's own composition and this buffer agree about what has been typed.
    // When no session can be had, the keys are kept as they are, for the session that comes later.
    let carried = aquaTypeSwiftRawBuffer
    session = rimeAPI.create_session()
    sessionGeneration = SquirrelApplicationDelegate.rimeGeneration
    schemaId = ""

    if session != 0 {
      Self.aquaTypeSessionNeverCame = false
      updateAppOptions()
      aquaTypeApplyChosenAsciiMode()
      for scalar in carried.unicodeScalars where scalar.isASCII {
        _ = rimeAPI.process_key(session, Int32(scalar.value), 0)
      }
    }
  }

  func updateAppOptions() {
    if currentApp == "" {
      return
    }
    if let appOptions = NSApp.squirrelAppDelegate.config?.getAppOptions(currentApp) {
      for (key, value) in appOptions {
        print("set app option: \(key) = \(value)")
        rimeAPI.set_option(session, key, value)
      }
    }
  }

  func destroySession() {
    // Only a session of the current librime: an older one is already gone, and its id may now be another controller's.
    if session != 0, sessionGeneration == SquirrelApplicationDelegate.rimeGeneration {
      _ = rimeAPI.destroy_session(session)
    }
    session = 0
    clearChord()
  }

  /// This controller's session exists in the librime running now (see SquirrelApplicationDelegate.rimeGeneration).
  var aquaTypeSessionAlive: Bool {
    session != 0 && sessionGeneration == SquirrelApplicationDelegate.rimeGeneration && rimeAPI.find_session(session)
  }

  func aquaTypeBecomeActiveController() {
    aquaTypeResignActiveController()
    lastInlineEcho = ""
    lastInlineEchoRaw = ""
    SquirrelInputController.aquaTypeActiveController = self
  }

  func aquaTypeResignActiveController() {
    if SquirrelInputController.aquaTypeActiveController === self {
      SquirrelInputController.aquaTypeActiveController = nil
    }
  }

  func aquaTypeClearBridgeState() {
    lastInlineEcho = ""
    lastInlineEchoRaw = ""
    aquaTypeSwiftRawBuffer = ""
    aquaTypeActiveClientSeq = ""
    aquaTypeV2PublishedGeneration = nil
    aquaTypeV2GenerationContext.clear()
    aquaTypeV2ResponseGeneration = nil
    aquaTypeV2ResponseRaw = ""
    aquaTypeV2ResponseRows = []
    aquaTypeLastPanelRaw = ""
    aquaTypeLastPanelCandidates = []
    aquaTypeLastPanelConsumed = []
    aquaTypeLastPanelComments = []
    aquaTypeLastPanelHighlighted = 0
    aquaTypePanelAnchorRect = nil
  }

  func aquaTypeRawInputProperty() -> String {
    var buffer = [CChar](repeating: 0, count: 4096)
    let ok = buffer.withUnsafeMutableBufferPointer { ptr in
      rimeAPI.get_property(session, "aquatype_raw_input", ptr.baseAddress, ptr.count)
    }
    guard ok else { return "" }
    return String(cString: buffer)
  }

  func aquaTypeBeginClientSeq() -> String {
    aquaTypeClientSeq &+= 1
    aquaTypeV2GenerationContext.clear()
    let seq = "\(aquaTypeClientSeq)"
    aquaTypeActiveClientSeq = seq
    rimeAPI.set_property(session, "aquatype_seq_id", seq)
    return seq
  }

  /// Hold a key that arrived while librime could not give this controller a session, if it types something (no
  /// Command, Control or Option), so that it and the ones after it are handled in order once there is a session.
  /// While keys are held, Backspace takes back the last of them and Escape all of them, as they would a composition.
  /// Any other key that types nothing - Return, Tab, an arrow, a shortcut - cannot be held, since the application
  /// never gets a key back: the held keys go in now, through librime if it can give a session after all, and the key
  /// after them.  Once librime has failed to give a session for `aquaTypeSessionWait` (it is not running at all after
  /// a launch that crashed), keys are not held again until it gives one.
  func aquaTypeHoldForSession(_ event: NSEvent, client sender: Any!) -> Bool {
    guard event.type == .keyDown, !Self.aquaTypeSessionNeverCame else { return false }
    guard event.modifierFlags.isDisjoint(with: [.command, .control, .option]), let typed = event.characters,
          !typed.isEmpty, typed.unicodeScalars.allSatisfy({ (0x20...0x7E).contains($0.value) }) else {
      guard !aquaTypeKeysAwaitingSession.isEmpty else { return false }
      let backspace = event.keyCode == UInt16(kVK_Delete), escape = event.keyCode == UInt16(kVK_Escape)
      if (backspace || escape), event.modifierFlags.isDisjoint(with: [.command, .control, .option, .shift]) {
        if backspace { aquaTypeKeysAwaitingSession.removeLast() } else { aquaTypeKeysAwaitingSession.removeAll() }
        if aquaTypeKeysAwaitingSession.isEmpty {
          aquaTypeSessionRetryTimer?.invalidate()
          aquaTypeSessionRetryTimer = nil
        }
        return true
      }
      if !aquaTypeSessionAlive { createSession() }
      aquaTypeHandleKeysAwaitingSession()
      return aquaTypeSessionAlive ? handle(event, client: sender) : false
    }
    aquaTypeKeysAwaitingSession.append((event, sender))
    guard aquaTypeSessionRetryTimer == nil else { return true }
    let since = ProcessInfo.processInfo.systemUptime
    aquaTypeSessionRetryTimer = Timer.scheduledTimer(withTimeInterval: 0.02, repeats: true) { [weak self] timer in
      guard let self else { timer.invalidate(); return }
      if !self.aquaTypeSessionAlive { self.createSession() }
      // librime building its dictionaries - an account's first start, which takes a few seconds - is worth waiting for;
      // librime not answering for any other reason is not.
      let wait = self.rimeAPI.is_maintenance_mode() ? Self.aquaTypeSessionWaitWhileBuilding : Self.aquaTypeSessionWait
      guard self.session != 0 || ProcessInfo.processInfo.systemUptime - since > wait else { return }
      if self.session == 0 { Self.aquaTypeSessionNeverCame = true }
      self.aquaTypeHandleKeysAwaitingSession()
    }
    return true
  }

  /// The held keys, in the order they were typed: through librime once there is a session.  A key it does not take
  /// (English mode, or librime still not there after `aquaTypeSessionWait`) goes into the document as the character the
  /// application would have typed, so nothing typed is lost.
  func aquaTypeHandleKeysAwaitingSession() {
    aquaTypeSessionRetryTimer?.invalidate()
    aquaTypeSessionRetryTimer = nil
    let held = aquaTypeKeysAwaitingSession
    aquaTypeKeysAwaitingSession = []
    for (event, sender) in held {
      if session != 0, handle(event, client: sender) { continue }
      guard event.modifierFlags.isDisjoint(with: [.command, .control, .option]), let typed = event.characters,
            !typed.isEmpty, typed.unicodeScalars.allSatisfy({ (0x20...0x7E).contains($0.value) }) else { continue }
      (sender as? IMKTextInput)?.insertText(typed, replacementRange: .empty)
    }
  }

  /// How long held keys wait for librime before they are typed as they are.
  internal static let aquaTypeSessionWait: TimeInterval = 3
  /// How long they wait while librime is building its dictionaries.
  internal static let aquaTypeSessionWaitWhileBuilding: TimeInterval = 30
  /// librime gave no session within `aquaTypeSessionWait`; cleared when it gives one.
  static var aquaTypeSessionNeverCame = false

  /// Hold a key that ends the composition - Return, punctuation, Shift with a letter - pressed before the engine has
  /// answered for the keys typed, until it has.  A decode takes tens of milliseconds after the last key, and a quick
  /// typist ends a sentence inside that: Return did nothing (a second one was needed, and in a chat the one meant to
  /// send went into the composition instead), and punctuation committed librime's conversion instead of the model's.
  /// Keys that type something and modifier changes follow it into the wait, so they keep their order (the Shift of a
  /// full-width punctuation mark is let go while it waits), and so does a second Return behind a Return (it must not
  /// send what librime made of the keys); Backspace takes held keys back; any other key ends the wait with what there
  /// is and follows, as do focus leaving, a failed request and the wait's own time limit.
  func aquaTypeHoldForAnswer(_ event: NSEvent, client sender: Any!, rimeKeycode: UInt32, modifiers rimeModifiers: UInt32) -> Bool {
    guard (rimeModifiers & (kReleaseMask.rawValue | kControlMask.rawValue | kSuperMask.rawValue)) == 0 else { return false }
    let code = Int32(rimeKeycode)
    // Option with any character key is punctuation or a full-width letter, bopomofo key or not.
    let option = (rimeModifiers & kAltMask.rawValue) != 0
    let endsComposition = code == XK_Return || code == XK_KP_Enter
      ? (rimeModifiers & (kShiftMask.rawValue | kAltMask.rawValue)) == 0
      : rimeKeycode > 0x20 && rimeKeycode <= 0x7E && (option || aquaTypeRawChar(for: rimeKeycode) == nil)
    guard endsComposition, let raw = aquaTypeAnswerPending() else { return false }
    return aquaTypeHold(event, client: sender, raw: raw)
  }

  /// The keys of a composition still waiting for the engine's answer, in Chinese mode: none has come for them, an
  /// engine is there to give one, and no wait has just ended for this composition.
  func aquaTypeAnswerPending() -> String? {
    let raw = aquaTypeCurrentRawBuffer()
    guard !raw.isEmpty, aquaTypeClientSeq != aquaTypeAnswerWaitedGeneration, !Self.aquaTypeEngineUnavailable,
          !rimeAPI.get_option(session, "ascii_mode"),
          !(raw == aquaTypeLastPanelRaw && !aquaTypeLastPanelCandidates.isEmpty),
          aquaTypeResponseWholeCandidate(for: raw) == nil else { return nil }
    return raw
  }

  /// Hold `event` until the answer for `raw` arrives (aquaTypeHoldForAnswer).
  func aquaTypeHold(_ event: NSEvent, client sender: Any!, raw: String) -> Bool {
    aquaTypeHeldShiftTap = false
    aquaTypeKeysAwaitingAnswer.append((event, sender))
    aquaTypeWriteBridgeRequest(for: raw)
    aquaTypeAnswerWaitToken &+= 1
    let token = aquaTypeAnswerWaitToken
    DispatchQueue.main.asyncAfter(deadline: .now() + AquaTypeV2JSONLClient.replyTimeout(forKeys: raw.count) + 0.25) { [weak self] in
      guard let self, token == self.aquaTypeAnswerWaitToken, !self.aquaTypeKeysAwaitingAnswer.isEmpty else { return }
      self.aquaTypeHandleKeysAwaitingAnswer()
    }
    return true
  }

  /// A key that types something, which waits behind a held one.
  private func aquaTypeTypesWhileAwaitingAnswer(_ event: NSEvent) -> Bool {
    guard event.type == .keyDown, event.modifierFlags.isDisjoint(with: [.command, .control, .option]),
          let typed = event.characters, !typed.isEmpty else { return false }
    return typed.unicodeScalars.allSatisfy { (0x20...0x7E).contains($0.value) }
  }

  /// What waits behind a held key: a key that types something, a modifier pressed or let go, and a second Return
  /// behind a held Return (after the composition is committed it has nothing to do and is dropped: sending what
  /// librime made of the keys would be worse).  A Return behind punctuation or a Shift tap ends the wait instead and
  /// goes on to the application ("真的嗎？" and Return sends), as does Shift+Return (a new line in a chat).
  private func aquaTypeFollowsHeldKey(_ event: NSEvent) -> Bool {
    if event.type == .flagsChanged { return true }
    if event.type == .keyDown, event.keyCode == UInt16(kVK_Return) || event.keyCode == UInt16(kVK_ANSI_KeypadEnter) {
      // Only Returns (and modifier changes) held: one behind a sentence typed after the first Return is that
      // sentence's, and goes on.
      let isReturn = { (e: NSEvent) in e.keyCode == UInt16(kVK_Return) || e.keyCode == UInt16(kVK_ANSI_KeypadEnter) }
      return event.modifierFlags.isDisjoint(with: [.command, .control, .option, .shift])
        && aquaTypeKeysAwaitingAnswer.first.map { $0.event.type == .keyDown && isReturn($0.event) } == true
        && aquaTypeKeysAwaitingAnswer.allSatisfy { $0.event.type == .flagsChanged || isReturn($0.event) }
    }
    return aquaTypeTypesWhileAwaitingAnswer(event)
  }

  /// The held keys, in order, now that the answer is in or will not come: the key that waited ends the composition with
  /// the best there is, the engine's answer or else what librime made of the keys.  A later one can hold again, for a
  /// composition typed after it.
  func aquaTypeHandleKeysAwaitingAnswer() {
    let held = aquaTypeKeysAwaitingAnswer
    aquaTypeKeysAwaitingAnswer = []
    aquaTypeAnswerWaitToken &+= 1
    aquaTypeAnswerWaitedGeneration = aquaTypeClientSeq
    let replaying = aquaTypeReplayingKeysAwaitingAnswer
    aquaTypeReplayingKeysAwaitingAnswer = true
    defer { aquaTypeAnswerWaitedGeneration = nil; aquaTypeReplayingKeysAwaitingAnswer = replaying }
    let shiftTap = aquaTypeHeldShiftTap
    aquaTypeHeldShiftTap = false
    for (index, (event, sender)) in held.enumerated() {
      if index == 0, shiftTap {
        // Judged a tap when it was held: it switches now, with Shift up.
        lastModifiers = event.modifierFlags
        aquaTypeShiftPending = false
        aquaTypeShiftChordUsed = false
        aquaTypeToggleAsciiModeFromBareShift()
        continue
      }
      if handle(event, client: sender) { continue }
      guard aquaTypeTypesWhileAwaitingAnswer(event), let typed = event.characters else { continue }
      (sender as? IMKTextInput)?.insertText(typed, replacementRange: .empty)
    }
  }

  // MARK: - Length cap
  //
  // A composition stops accepting new bopomofo keys at aquaTypeMaxComposingKeys: sixty characters, which is 190 keys
  // at 3.15 keys a character, and the width of the panel times three lines.
  //
  // Past the cap the key simply does not enter the system: the input buffer, the candidate panel and everything behind
  // them are unchanged, and the only feedback is the beep a traditional input method makes for a key it cannot use.
  // Not an auto-commit - with mistyped keys the top candidate is often not what was meant, so committing on the user's
  // behalf at the moment they are not looking at the panel would insert text they never approved.  Everything except a
  // new bopomofo key keeps working at the cap: backspace, selection, the arrows, punctuation and commit.
  //
  // The key is swallowed rather than passed on.  Returning false here would hand it back to the system and it would
  // arrive in the document as a Latin letter, which is worse than either accepting or refusing it.
  static let aquaTypeMaxComposingKeys = 190

  func aquaTypeRejectKeyPastLengthCap(_ rimeKeycode: UInt32, modifiers rimeModifiers: UInt32) -> Bool {
    guard (rimeModifiers & (kReleaseMask.rawValue | kControlMask.rawValue | kAltMask.rawValue | kSuperMask.rawValue)) == 0 else { return false }
    let code = Int32(rimeKeycode)
    guard code >= 0x20, code < 0x7F else { return false }                       // only printable keys can extend a composition
    guard aquaTypeCurrentRawBuffer().utf8.count >= Self.aquaTypeMaxComposingKeys else { return false }
    // Only a key that would make the composition longer is refused.  Punctuation is not: it commits what is there and
    // then types itself, which is the way out of the cap.  Neither is a tone (`aquaTones`: space and 6, 3, 4, 7), which
    // completes the syllable being typed rather than starting another.  The other digits (0, 1, 2, 5, 8, 9) are
    // ordinary bopomofo keys on this layout and are refused like any other.
    guard let rawChar = aquaTypeRawChar(for: rimeKeycode), let key = rawChar.first else { return false }
    if Self.aquaTones.contains(key) { return false }
    NSSound.beep()
    return true
  }

  // MARK: - Repeated commit
  //
  // Selecting a candidate that covers only the leading part of the input commits that part and leaves the rest in the
  // composition, which is then decoded again.  The keys it accounts for come from the engine as consumed_keys, so the
  // split is exact.
  @discardableResult
  func aquaTypeCommitPanelCandidate(index: Int, raw: String) -> Bool {
    guard index >= 0, index < aquaTypeLastPanelCandidates.count else { return false }
    let text = aquaTypeLastPanelCandidates[index]
    // Read only when the two lists are the pair they are supposed to be.  Indexing them separately is what made a
    // desync commit the wrong span instead of simply committing the whole candidate, and a wrong span is the failure
    // the user has to notice and undo; the whole candidate is never wrong, only sometimes less than it could be.
    let consumed = aquaTypeLastPanelConsumed.count == aquaTypeLastPanelCandidates.count
      ? aquaTypeLastPanelConsumed[index] : nil
    let keys = Array(raw)
    // consumed_keys counts keys of the raw the panel is showing, which is not always the raw being committed: the
    // panel holds its candidates while the next decode is in flight, so it can be one or two keys behind.  The count
    // still cuts `raw` in the right place as long as the panel's raw is a prefix of it - the keys it counts are then
    // the same keys.  When it is not, there is no sound split and the whole candidate is committed instead.
    let panelRawIsPrefix = aquaTypeLastPanelRaw.isEmpty || raw.hasPrefix(aquaTypeLastPanelRaw)
    guard panelRawIsPrefix, let n = consumed, n > 0, n < keys.count else {  // covers the whole input: an ordinary commit
      commit(string: text)
      return true
    }
    let consumedKeys = String(keys[0..<n])
    let remaining = String(keys[n...])
    client?.insertText(text, replacementRange: .empty)
    // Remembered like any other commit, under the keys this piece accounted for.  Committing a sentence in pieces is
    // how a name or a term is taught (segment it once by hand, have it back whole next time), and the engine assembles
    // the whole sentence from the pieces it is told about.
    aquaTypeRecordCommit(raw: consumedKeys, text: text)
    aquaTypeCommitPieces.append((text: text, keys: consumedKeys))
    rimeAPI.set_property(session, "aquatype_raw_input", "")
    rimeAPI.set_property(session, "aquatype_seq_id", "")
    rimeAPI.clear_composition(session)
    aquaTypeLastPanelRaw = ""
    aquaTypeLastPanelCandidates = []
    aquaTypeLastPanelConsumed = []
    aquaTypeSwiftRawBuffer = ""
    _ = aquaTypeBeginClientSeq()
    // Through setRawBuffer, so the remainder is decoded and the panel is redrawn for it.
    _ = aquaTypeSetRawBuffer(remaining, clearIfEmpty: false)
    return true
  }

  func aquaTypeCurrentRawBuffer() -> String {
    if !aquaTypeSwiftRawBuffer.isEmpty { return aquaTypeSwiftRawBuffer }
    let rawProperty = aquaTypeRawInputProperty()
    if !rawProperty.isEmpty { return rawProperty }
    let inputRaw = rimeAPI.get_input(session).map { String(cString: $0) } ?? ""
    if !inputRaw.isEmpty {
      return inputRaw.replacingOccurrences(of: "'", with: " ")
    }
    return ""
  }

  func aquaTypeRawChar(for rimeKeycode: UInt32) -> String? {
    guard rimeKeycode >= 32 && rimeKeycode <= 126,
          let scalar = UnicodeScalar(rimeKeycode) else { return nil }
    let ch = Character(scalar)
    guard Self.aquaRawKeys.contains(ch) else { return nil }
    return String(ch)
  }

  // MARK: - Punctuation, typed the way macOS's built-in 注音 input method types it

  // Measured key by key on the built-in 注音 (Zhuyin) input method in Chinese mode.  Keys that are bopomofo on this
  // layout (the digits, the letters, - ; , . /) are composition input when pressed alone and are not listed here.
  static let aquaTypeMacPunctuationPlain: [Character: String] = [
    "`": "·", "=": "＝", "[": "「", "]": "」", "\\": "、",
  ]
  static let aquaTypeMacPunctuationShift: [Character: String] = [
    "~": "～", "!": "！", "@": "＠", "#": "＃", "$": "＄", "%": "％", "^": "＾", "&": "＆", "*": "＊", "(": "（",
    ")": "）", "_": "＿", "+": "＋", "{": "『", "}": "』", "|": "｜", ":": "：", "<": "，", ">": "。", "?": "？",
  ]
  static let aquaTypeMacPunctuationOption: [Character: String] = [
    "`": "｀", "1": "１", "2": "２", "3": "３", "4": "４", "5": "５", "6": "６", "7": "７", "8": "８", "9": "９",
    "0": "０", "-": "—", "=": "＝", "[": "「", "]": "」", "\\": "、", ";": "；", ",": "，", ".": "．", "/": "／",
  ]

  /// Punctuation in Chinese mode, as the built-in 注音 input method types it: the plain keys that are not bopomofo,
  /// Shift with a digit or punctuation key, and Option with any character key (letters become full-width).  Quotes
  /// alternate between opening and closing: the double quote on Shift+', the single quote on ' and Option+', which
  /// share one alternation.  Shift+Option with a digit or punctuation key types nothing - the built-in one keeps those
  /// for its own shortcuts; with a letter it is the application's shortcut (handle).  A composition in progress is
  /// committed first, with the candidate that is selected.
  func aquaTypeHandleMacPunctuationKey(_ rimeKeycode: UInt32, modifiers rimeModifiers: UInt32) -> Bool {
    guard (rimeModifiers & (kReleaseMask.rawValue | kControlMask.rawValue | kSuperMask.rawValue)) == 0,
          !rimeAPI.get_option(session, "ascii_mode"),
          rimeKeycode > 32, rimeKeycode <= 126, let scalar = UnicodeScalar(rimeKeycode) else {
      return false
    }
    let ch = Character(scalar)
    let shift = (rimeModifiers & kShiftMask.rawValue) != 0, option = (rimeModifiers & kAltMask.rawValue) != 0
    var text: String?
    switch (shift, option) {
    case (true, true):
      return true
    case (false, true):
      if ch == "'" {
        text = aquaTypeNextQuote(double: false)
      } else if let mapped = Self.aquaTypeMacPunctuationOption[ch] {
        text = mapped
      } else if ch.isASCII, ch.isLetter, let full = UnicodeScalar(scalar.value + 0xFEE0) {
        text = String(Character(full))   // Option+letter: the full-width letter
      }
    case (true, false):
      text = ch == "\"" ? aquaTypeNextQuote(double: true) : Self.aquaTypeMacPunctuationShift[ch]
    case (false, false):
      text = ch == "'" ? aquaTypeNextQuote(double: false) : Self.aquaTypeMacPunctuationPlain[ch]
    }
    guard let output = text else { return false }
    aquaTypeCommitCompositionBeforePunctuation()
    client?.insertText(output, replacementRange: .empty)
    return true
  }

  private func aquaTypeNextQuote(double: Bool) -> String {
    if double {
      aquaTypeDoubleQuoteOpen.toggle()
      return aquaTypeDoubleQuoteOpen ? "\u{201C}" : "\u{201D}"
    }
    aquaTypeSingleQuoteOpen.toggle()
    return aquaTypeSingleQuoteOpen ? "\u{2018}" : "\u{2019}"
  }

  /// A punctuation key ends the composition in progress: the selected candidate (the first one, unless another was
  /// chosen) is committed, and the punctuation follows it.
  func aquaTypeCommitCompositionBeforePunctuation() {
    guard !aquaTypeCurrentRawBuffer().isEmpty || !preedit.isEmpty else { return }
    if aquaTypeCommitCompositionForAsciiSwitch() {
      rimeAPI.set_property(session, "aquatype_raw_input", "")
      rimeAPI.set_property(session, "aquatype_seq_id", "")
      rimeAPI.clear_composition(session)
      aquaTypeClearBridgeState()
      aquaTypeHidePalettes(force: true)
    } else if !aquaTypeCurrentRawBuffer().isEmpty {
      // Nothing converts these keys yet: the bopomofo goes in before the punctuation, as it does when focus leaves, so
      // no key is lost and the next one does not carry on a composition that is no longer in the document.
      aquaTypeCommitBopomofo(raw: aquaTypeCurrentRawBuffer())
    }
  }

  /// The composition goes into the document as the bopomofo it shows, and the composition ends.
  func aquaTypeCommitBopomofo(raw: String) {
    let zhuyin = aquaTypeBopomofoEcho(for: raw)
    if !zhuyin.isEmpty {
      client?.insertText(zhuyin, replacementRange: .empty)
    }
    preedit = ""
    rimeAPI.set_property(session, "aquatype_raw_input", "")
    rimeAPI.set_property(session, "aquatype_seq_id", "")
    rimeAPI.clear_composition(session)
    aquaTypeClearBridgeState()
    aquaTypeHidePalettes(force: true)
  }

  // A key that Rime's punctuator will consume as punctuation while in
  // Chinese mode: a printable ASCII char that is NOT a Dachen raw bopomofo key
  // (those are composition input, e.g. , . ; / - and tone keys) and not a letter
  // (uppercase letters reach here on Shift+letter and must not pre-commit). This
  // catches every shifted punctuation (< > ? : " | ~ ! @ # % ^ & * ( ) _ + { } …)
  // and unshifted bracket punctuation ([ ]), which is exactly the set that should
  // commit the in-progress neural composition first, then emit the punctuation.
  func aquaTypeIsPunctuationCommitKey(_ rimeKeycode: UInt32) -> Bool {
    guard !rimeAPI.get_option(session, "ascii_mode") else { return false }
    guard rimeKeycode >= 32, rimeKeycode <= 126, let scalar = UnicodeScalar(rimeKeycode) else { return false }
    if aquaTypeRawChar(for: rimeKeycode) != nil { return false }   // Dachen raw key, not punctuation
    if Character(scalar).isLetter { return false }                  // Shift+letter is not punctuation
    return true
  }

  func aquaTypeV2RawIdentity(_ raw: String) -> String {
    Data(raw.utf8).base64EncodedString()
  }

  func aquaTypeCaptureV2ContextSnapshot() -> AquaTypeV2ContextSnapshot {
    guard let client = client else { return .empty }
    return AquaTypeV2ContextPolicy.capture(
      secureInputEnabled: aquaTypeSecureInputHere(),
      documentLength: { client.length() },
      markedRange: { client.markedRange() },
      selectedRange: { client.selectedRange() },
      attributedSubstring: { client.attributedSubstring(from: $0) })
  }

  func aquaTypeWriteBridgeRequest(for raw: String) {
    guard !raw.isEmpty else { return }
    let generation = aquaTypeClientSeq
    let clientID = aquaTypeV2ClientID
    let rawIdentity = aquaTypeV2RawIdentity(raw)
    // Even when the debounce delay is zero, the request is built and sent only after the key callback has returned
    // and the immediate Zhuyin preedit is visible.
    DispatchQueue.main.async { [weak self] in
      guard let self = self,
            generation == self.aquaTypeClientSeq,
            raw == self.aquaTypeCurrentRawBuffer(),
            self.aquaTypeActiveClientSeq == "\(generation)" else { return }
      let context: AquaTypeV2ContextSnapshot
      if let already = self.aquaTypeV2GenerationContext.frozen(for: generation) {
        context = already
      } else {
        let captured = self.aquaTypeCaptureV2ContextSnapshot()          // no access held while this runs
        self.aquaTypeV2GenerationContext.store(generation: generation, snapshot: captured)
        context = captured
      }
      let request = AquaTypeV2UIRequest(
        clientID: clientID,
        clientGeneration: generation,
        rawKeys: raw,
        rawIdentity: rawIdentity,
        committedContextUTF8: context.text,
        contextIdentity: context.identity,
        maxCandidates: self.aquaTypePanelGridMax)
      // One request at a time, and always the newest one.  The engine reads one line, decodes it and answers before
      // reading the next, with no cancellation, so a request sent while another is being decoded waits behind it and
      // arrives describing keys the user has already typed past.  Sending more often only lengthens that queue.  So hold
      // the newest keys, send them the moment
      // the engine is free, and drop whatever they superseded: the panel then refreshes once per decode.
      // The same keys asked again for the same composition (Return before the answer) wait for the answer already on
      // its way: sent twice, the engine would decode them twice and the next key's answer would wait behind both.
      if let inFlight = self.aquaTypeInFlightBridgeRaw {
        if inFlight != raw || self.aquaTypeInFlightBridgeGeneration != generation { self.aquaTypeQueuedBridgeRaw = raw }
        return
      }
      self.aquaTypeInFlightBridgeRaw = raw
      self.aquaTypeInFlightBridgeGeneration = generation
      AquaTypeV2JSONLClient.shared.send(request) { [weak self] result in
        guard let self = self else { return }
        switch result {
        case .success(let response):
          Self.aquaTypeEngineUnavailable = false
          self.aquaTypeUnansweredRaw = ""
          self.aquaTypeApplyV2Response(response)
        case .failure:
          Self.aquaTypeEngineUnavailable = true
          self.aquaTypeRetryUnansweredRequest(for: raw)
        }
        // The engine is free again.  Every path through the client ends here - a reply, a rejection, or the 1.5 s
        // timeout - so this cannot be left set and the next keystroke cannot be locked out.
        self.aquaTypeInFlightBridgeRaw = nil
        self.aquaTypeInFlightBridgeGeneration = nil
        let queued = self.aquaTypeQueuedBridgeRaw
        self.aquaTypeQueuedBridgeRaw = ""
        let asksAgain = !queued.isEmpty && queued == self.aquaTypeCurrentRawBuffer()
          && (queued != raw || generation != self.aquaTypeClientSeq)
        if asksAgain {
          self.aquaTypeWriteBridgeRequest(for: queued)
        } else if !self.aquaTypeKeysAwaitingAnswer.isEmpty {
          // Nothing more is coming for the keys a Return or punctuation waited for: this was their answer, or the
          // engine failed to give one.
          self.aquaTypeHandleKeysAwaitingAnswer()
        }
      }
    }
  }

  // MARK: - Asking again when the engine did not answer in time
  //
  // A request is only ever made because a key was pressed, so when one goes unanswered and the user has stopped
  // typing, nothing would ask again: the panel would keep candidates for earlier keys, commit would correctly refuse
  // them, and the only way out would be Escape.  So an unanswered request is asked again, after a delay that lets any
  // backlog drain, and a bounded number of times so that an engine that is really gone ends up quiet instead of
  // looping.
  static let aquaTypeUnansweredRetryLimit = 4
  /// The last request got no answer: no engine could be started (missing assets, the backoff after it kept exiting),
  /// it exited, or it did not answer within the timeout; cleared by the next answer.  Return then commits what librime
  /// made of the keys instead of asking again while the composition stays.
  static var aquaTypeEngineUnavailable = false
  static let aquaTypeUnansweredRetryDelay: TimeInterval = 0.25

  func aquaTypeRetryUnansweredRequest(for raw: String) {
    guard !raw.isEmpty, raw == aquaTypeCurrentRawBuffer() else { return }
    if aquaTypeUnansweredRaw != raw {
      aquaTypeUnansweredRaw = raw
      aquaTypeUnansweredAttempts = 0
    }
    guard aquaTypeUnansweredAttempts < Self.aquaTypeUnansweredRetryLimit else {
      return
    }
    aquaTypeUnansweredAttempts += 1
    DispatchQueue.main.asyncAfter(deadline: .now() + Self.aquaTypeUnansweredRetryDelay) { [weak self] in
      guard let self = self, raw == self.aquaTypeCurrentRawBuffer() else { return }
      self.aquaTypeWriteBridgeRequest(for: raw)
    }
  }

  /// A request goes out on every key.  Only one is ever outstanding and one superseded before it is sent is dropped
  /// (aquaTypeWriteBridgeRequest), so waiting would save no work and only delay the panel.
  func aquaTypeScheduleBridgeRequest(for raw: String) {
    guard !raw.isEmpty else { return }
    aquaTypeWriteBridgeRequest(for: raw)
  }

  @discardableResult
  func aquaTypeSetRawBuffer(_ raw: String, clearIfEmpty: Bool = true, preservePanel: Bool = false) -> Bool {
    let previousRaw = aquaTypeCurrentRawBuffer()
    let wasEmpty = previousRaw.isEmpty
    let rawChanged = previousRaw != raw
    aquaTypeSwiftRawBuffer = raw
    // A keystroke invalidates the *pending* decode (a response for the previous keys must never be rendered against
    // these) but must not touch what the panel is currently *showing*: the window stays up for the whole composition
    // and its contents are swapped in place, so it does not blink on every key.
    if rawChanged {
      aquaTypeV2ResponseGeneration = nil
      aquaTypeV2ResponseRaw = ""
      aquaTypeV2ResponseRows = []
    }
    rimeAPI.set_property(session, "aquatype_raw_input", raw)
    if !aquaTypeActiveClientSeq.isEmpty {
      rimeAPI.set_property(session, "aquatype_seq_id", aquaTypeActiveClientSeq)
    }
    if raw.isEmpty {
      if clearIfEmpty {
        aquaTypeClearBridgeState()
      }
      rimeAPI.clear_composition(session)
      show(preedit: "", selRange: .empty, caretPos: 0)
      NSApp.squirrelAppDelegate.panel?.aquaTypeCompositionPanelVisible = false
      aquaTypeHidePalettes(force: true)
      return true
    }
    if preservePanel {
      NSApp.squirrelAppDelegate.panel?.navMode = true
    } else {
      NSApp.squirrelAppDelegate.panel?.navMode = false
    }
    // Claim the panel for this composition.  While this is set the panel swallows `orderOut` and ignores unforced
    // `hide()`, so nothing between two renders can make it disappear.
    NSApp.squirrelAppDelegate.panel?.aquaTypeCompositionPanelVisible = true
    // The displayed state and the caret anchor are reset when a composition *begins*, not on every keystroke, or the
    // panel would chase the caret across the line as a sentence grows.
    if wasEmpty {
      aquaTypeLastPanelRaw = ""
      aquaTypeLastPanelCandidates = []
      aquaTypeLastPanelConsumed = []
      aquaTypeLastPanelComments = []
      aquaTypeLastPanelHighlighted = 0
      aquaTypePanelAnchorRect = nil
    }
    // The callback-visible action is always the immediate Zhuyin preedit. Rime
    // capture and persistent JSONL I/O are scheduled after this method returns.
    if !rawChanged, aquaTypeShowVerifiedResponse(for: raw) {
    } else if aquaTypeRenderHeldPanel(for: raw) {
      // Backspace lands here: the shorter raw is a prefix of what the panel already shows, so its candidates are
      // clipped and re-rendered at once instead of leaving the panel stale until the decode returns.
    } else {
      let echo = aquaTypeBopomofoEcho(for: raw)
      if !echo.isEmpty {
        show(preedit: echo,
             selRange: NSRange(location: 0, length: echo.utf16.count),
             caretPos: echo.utf16.count)
      }
    }
    aquaTypeScheduleBridgeRequest(for: raw)
    return true
  }

  /// The note beside a candidate.  Only one is worth making: that this is something the user chose before.  Where the
  /// rest came from (the model, the dictionary, the character table) is nothing a reader could act on.
  func aquaTypeCommentForSource(_ source: String) -> String {
    source.contains("G_DECODER_MEMORY") ? "記憶" : ""
  }

  private func aquaTypeResponseMetadata() -> (raw: String, clientSeq: String, rows: [AquaTypeResponseRow])? {
    guard let generation = aquaTypeV2ResponseGeneration,
          generation == aquaTypeClientSeq,
          aquaTypeV2PublishedGeneration == generation,
          !aquaTypeV2ResponseRaw.isEmpty else { return nil }
    return (aquaTypeV2ResponseRaw, "\(generation)", aquaTypeV2ResponseRows)
  }

  func aquaTypeApplyV2Response(_ response: AquaTypeV2UIResponse) {
    let raw = aquaTypeCurrentRawBuffer()
    guard let context = aquaTypeV2GenerationContext.frozen(for: aquaTypeClientSeq) else {
      return
    }
    let active = AquaTypeV2PanelIdentity(
      clientID: aquaTypeV2ClientID,
      generation: aquaTypeClientSeq,
      rawKeys: raw,
      rawIdentity: aquaTypeV2RawIdentity(raw),
      contextIdentity: context.identity)
    let isCurrent = AquaTypeV2PanelAcceptance.accepts(
      response: response.identity, active: active, publishedGeneration: aquaTypeV2PublishedGeneration)
    let isInterim = !isCurrent
      && AquaTypeV2PanelAcceptance.acceptsAsInterim(response: response.identity, active: active)
    guard AquaTypeV2ResponseContract.accepts(schema: response.schema, visiblePath: response.visiblePath),
          isCurrent || isInterim else {
      return
    }
    // The keys these candidates are actually for.  Everything the panel records is keyed on this, so an interim
    // render cannot be mistaken for the answer to what is typed now.
    let panelRaw = isCurrent ? raw : response.rawKeys

    var seen = Set<String>()
    var rows = [AquaTypeResponseRow]()
    for candidate in response.candidates {
      guard !candidate.textUTF8.isEmpty,
            aquaTypeHasCJK(candidate.textUTF8),
            aquaTypeCanRender(candidate.textUTF8),
            AquaTypeV2ResponseContract.allows(
              provenance: candidate.provenance,
              visiblePath: response.visiblePath),
            // deduped on the pair, as the engine does: the same text covering a different number of keys is a
            // different offer, and collapsing them would drop the partial one
            seen.insert("\(candidate.textUTF8)#\(candidate.consumedKeys.map(String.init) ?? "all")").inserted else { continue }
      rows.append(AquaTypeResponseRow(
        text: candidate.textUTF8,
        source: candidate.provenance.joined(separator: "+"),
        consumedKeys: candidate.consumedKeys))
      // No truncation at the model's ceiling: maxCandidates bounds what the *model* is asked for, and the engine
      // appends the words and characters for the leading span after that, bounded by how many homophones the span
      // actually has.
      if rows.count >= 512 { break }                                   // a bound against a malformed response, nothing more
    }
    guard !rows.isEmpty else {
      return
    }

    let candidates = rows.map(\.text)
    let comments = rows.map { aquaTypeCommentForSource($0.source) }
    guard aquaTypeRenderCandidates(
      raw: panelRaw,
      candidates: candidates,
      highlighted: 0,
      comments: comments,
      consumed: rows.map(\.consumedKeys)) else { return }
    aquaTypeV2ResponseGeneration = response.clientGeneration
    aquaTypeV2ResponseRaw = panelRaw
    aquaTypeV2ResponseRows = rows
    // Only a current answer closes its generation off.  Marking an interim one published would stop the real answer
    // for those keys from ever being shown.
    if isCurrent {
      aquaTypeV2PublishedGeneration = response.clientGeneration
    }
  }

  private func aquaTypeResponseRows(for raw: String) -> [AquaTypeResponseRow] {
    guard let metadata = aquaTypeResponseMetadata(), metadata.raw == raw else { return [] }
    return metadata.rows
  }

  func aquaTypeResponseCandidates(for raw: String) -> [String] {
    guard let metadata = aquaTypeResponseMetadata(), metadata.raw == raw else { return [] }
    return metadata.rows.map(\.text)
  }

  /// The engine's best candidate that accounts for every key typed, for a commit that has to take the whole input.
  func aquaTypeResponseWholeCandidate(for raw: String) -> String? {
    guard let metadata = aquaTypeResponseMetadata(), metadata.raw == raw else { return nil }
    return metadata.rows.first { ($0.consumedKeys ?? raw.count) >= raw.count }?.text
  }

  /// The panel's first candidate that accounts for every key typed, when the panel is showing these keys.
  func aquaTypePanelWholeCandidate(raw: String) -> String? {
    guard raw == aquaTypeLastPanelRaw else { return nil }
    return aquaTypeLastPanelCandidates.indices.first { aquaTypePanelCandidateTakesAllKeys($0, raw: raw) }
      .map { aquaTypeLastPanelCandidates[$0] }
  }

  /// Does the panel's candidate at `index` account for every key typed?  One that takes only the leading keys (a word,
  /// the first syllable's characters) commits only those.
  func aquaTypePanelCandidateTakesAllKeys(_ index: Int, raw: String) -> Bool {
    guard raw == aquaTypeLastPanelRaw, aquaTypeLastPanelConsumed.indices.contains(index) else { return true }
    return (aquaTypeLastPanelConsumed[index] ?? raw.count) >= raw.count
  }

  /// The labels down the left of the candidates: their positions in the whole list, so the second page reads 10, 11, 12
  /// rather than 1, 2, 3 again.  The panel gives the label column the width of its widest label and right-aligns each
  /// label in it, so the candidates line up whatever the labels say.  The labels only number: a digit cannot select a
  /// candidate, because 1 to 9 are bopomofo keys on this layout and Command with a digit belongs to the application.
  func aquaTypeCandidateLabels(startingAt offset: Int, count: Int) -> [String] {
    guard count > 0 else { return [] }
    return (0..<count).map { String(offset + $0 + 1) }
  }

  // MARK: - Panel layout
  //
  // One column, nine lines.  A candidate is laid out at aquaTypePanelWidth full-width characters and takes
  // ceil(length / width) lines, so a page holds as many candidates as fit in nine lines: nine short ones, four of up to
  // forty characters, three of up to sixty.  Sixty is also where the length cap comes from - past three lines a page
  // would show fewer than three candidates.
  //
  // There is no expansion.  Expanding changes what the left and right keys mean - paging to the first candidate of a
  // page, or stepping across a grid - and a key whose meaning depends on a mode the user did not choose is worse than
  // either behaviour on its own.  The width is the panel's own (a tuned proportion of the screen), so the line count
  // asks the panel how many characters fit rather than assuming a number: a page that reserves nine lines while the
  // panel draws a different number comes out the wrong shape.
  static let aquaTypePanelWidthFallback = 20

  var aquaTypePanelWidth: Int {
    NSApp.squirrelAppDelegate.panel?.candidateColumns ?? Self.aquaTypePanelWidthFallback
  }

  /// How many lines a candidate occupies at `width` characters a line.
  func aquaTypePanelLines(_ text: String, width: Int) -> Int {
    max(1, (text.count + width - 1) / width)
  }

  /// The pages from the front up to the one containing `index`: their start indexes, and where that last page ends.
  /// Walked from the front so a page never exceeds nine lines, which makes page size vary with what is on the page - a
  /// page of whole sentences shows three, a page of single characters nine - and always makes progress even if one
  /// candidate alone overflows.
  ///
  /// This runs on every render, against lists of a hundred candidates and more, so it stops at the page it was asked
  /// about and asks the panel for its width once: the width comes from measuring the screen and the font, and asking
  /// per candidate would make the panel's arithmetic the largest cost of a keystroke.
  func aquaTypePanelPageStarts(_ candidates: [String], through index: Int) -> (starts: [Int], end: Int) {
    guard !candidates.isEmpty else { return ([0], 0) }
    let width = aquaTypePanelWidth
    var starts = [0]
    var used = 0
    for (i, c) in candidates.enumerated() {
      let lines = aquaTypePanelLines(c, width: width)
      if used > 0, used + lines > aquaTypePanelRows {
        if i > index { return (starts, i) }
        starts.append(i); used = lines
      } else {
        used += lines
      }
    }
    return (starts, candidates.count)
  }

  /// The page containing `index`, as (start, count), and its number counting from 0.
  func aquaTypePanelPage(_ candidates: [String], containing index: Int) -> (start: Int, count: Int, number: Int) {
    let safe = max(0, min(index, max(0, candidates.count - 1)))
    let walk = aquaTypePanelPageStarts(candidates, through: safe)
    let start = walk.starts.last ?? 0
    return (start, max(1, walk.end - start), walk.starts.count - 1)
  }

  // Page arithmetic has exactly one definition, aquaTypePanelPage, and everything that needs a page asks it.
  func aquaTypePanelDisplayIndexes(raw: String, candidates: [String], total: Int, highlighted: Int) -> (indexes: [Int], localHighlight: Int, page: Int, lastPage: Bool) {
    guard total > 0 else { return ([], 0, 0, true) }
    let safeHighlight = max(0, min(highlighted, total - 1))
    let page = aquaTypePanelPage(candidates, containing: safeHighlight)
    let pageStart = page.start
    let pageEnd = min(pageStart + page.count, total)
    let indexes = Array(pageStart..<pageEnd)
    let localHighlight = indexes.firstIndex(of: safeHighlight) ?? 0
    return (indexes, localHighlight, page.number, pageEnd >= total)
  }

  func aquaTypePanelRenderCurrent(raw: String, highlighted: Int? = nil) -> Bool {
    guard raw == aquaTypeLastPanelRaw, !aquaTypeLastPanelCandidates.isEmpty else { return false }
    return aquaTypeRenderCandidates(raw: raw,
                                    candidates: aquaTypeLastPanelCandidates,
                                    highlighted: highlighted ?? aquaTypeLastPanelHighlighted,
                                    comments: aquaTypeLastPanelComments,
                                    consumed: aquaTypeLastPanelConsumed)
  }

  // Right and left are paging, and nothing else.  They move a whole page and put the cursor on that page's first
  // candidate, which is what every traditional input method does, and they stop at the ends: no cycling.  Moving
  // inside the list is what up and down are for.
  func aquaTypePanelRightTarget(raw: String, current: Int, total: Int, candidates: [String]) -> Int {
    guard total > 0 else { return 0 }
    let page = aquaTypePanelPage(candidates, containing: current)
    let next = page.start + page.count
    return next < total ? next : current                                  // last page: stay
  }

  func aquaTypePanelLeftTarget(raw: String, current: Int, total: Int, candidates: [String]) -> Int {
    guard total > 0 else { return 0 }
    let page = aquaTypePanelPage(candidates, containing: current)
    guard page.start > 0 else { return current }                          // first page: stay
    return aquaTypePanelPage(candidates, containing: page.start - 1).start
  }

  /// Up and down walk the whole list and cross page boundaries: from the last candidate of a page, down lands on the
  /// first candidate of the next page and the panel turns with it.  They stop at the two ends of the list.
  func aquaTypePanelDownTarget(current: Int, total: Int) -> Int { min(max(0, total - 1), current + 1) }
  func aquaTypePanelUpTarget(current: Int) -> Int { max(0, current - 1) }

  @discardableResult
  /// Draw a candidate list and make it the panel's state.
  ///
  /// `consumed` must be passed whenever the list has per-candidate key counts, re-renders included (moving the
  /// highlight, paging, redrawing a response): leaving it out erases them, and a candidate that covers only the leading
  /// part of the input would then commit as though it covered all of it and throw the rest away.
  func aquaTypeRenderCandidates(raw: String, candidates: [String], highlighted requestedHighlight: Int, comments requestedComments: [String]? = nil, consumed: [Int?]? = nil) -> Bool {
    guard !raw.isEmpty, !candidates.isEmpty else { return false }
    // Page arithmetic reads aquaTypeLastPanelCandidates, so it has to be the list about to be drawn - not the one
    // from the render before, which is where the labels and the page number came from while the panel flickered.
    aquaTypeLastPanelCandidates = candidates
    aquaTypeLastPanelConsumed = consumed ?? Array(repeating: nil, count: candidates.count)
    let highlighted = max(0, min(requestedHighlight, candidates.count - 1))
    let expectedDisplayLength = aquaTypeSyllableCount(raw)
    let selected = candidates[highlighted]
    // The preview feeds the inline echo, and nothing else.  It must not gate the render: a key string that is not yet a
    // whole syllable (a lone tone mark, a half-typed sound) clips the preview to nothing, and the panel would then not
    // be drawn although the engine had answered.
    let preview = aquaTypeClipPreview(selected, maxCount: expectedDisplayLength)
    if !preview.isEmpty, aquaTypeHasCJK(preview) {
      lastInlineEcho = preview
      lastInlineEchoRaw = raw
    }
    NSApp.squirrelAppDelegate.panel?.navMode = true
    // The text in the document is what has been typed, whichever keys these candidates answer: an interim render is
    // for an earlier prefix, and echoing its keys would take the composition back to fewer keys than were typed.
    let typed = aquaTypeCurrentRawBuffer()
    let echo = aquaTypeBopomofoEcho(for: typed.isEmpty ? raw : typed)
    if !echo.isEmpty {
      show(preedit: echo,
           selRange: NSRange(location: 0, length: echo.utf16.count),
           caretPos: echo.utf16.count)
    }
    var comments = Array((requestedComments ?? []).prefix(candidates.count))
    if comments.count < candidates.count {
      comments.append(contentsOf: Array(repeating: "", count: candidates.count - comments.count))
    }
    let display = aquaTypePanelDisplayIndexes(raw: raw, candidates: candidates, total: candidates.count, highlighted: highlighted)
    let visibleCandidates = display.indexes.map { candidates[$0] }
    let visibleComments = display.indexes.map { comments[$0] }
    let labels = aquaTypeCandidateLabels(startingAt: display.indexes.first ?? 0, count: display.indexes.count)
    showPanel(preedit: "", selRange: NSRange(location: 0, length: 0), caretPos: 0,
              candidates: visibleCandidates,
              comments: visibleComments,
              labels: labels,
              highlighted: display.localHighlight,
              page: display.page,
              lastPage: display.lastPage,
              candidateIndexMap: display.indexes)
    aquaTypeLastPanelRaw = raw
    aquaTypeLastPanelCandidates = candidates
    aquaTypeLastPanelConsumed = consumed ?? Array(repeating: nil, count: candidates.count)
    aquaTypeLastPanelComments = comments
    aquaTypeLastPanelHighlighted = highlighted
    return true
  }

  @discardableResult
  func aquaTypeClearCompositionToEmpty(touchingClient: Bool = true) -> Bool {
    aquaTypeCommitPieces.removeAll()          // the sentence was abandoned; its pieces are not part of the next one
    aquaTypeSwiftRawBuffer = ""
    lastInlineEcho = ""
    lastInlineEchoRaw = ""
    aquaTypeLastPanelRaw = ""
    aquaTypeLastPanelCandidates = []
    aquaTypeLastPanelConsumed = []
    aquaTypeLastPanelComments = []
    aquaTypeLastPanelHighlighted = 0
    aquaTypePanelAnchorRect = nil
    rimeAPI.set_property(session, "aquatype_raw_input", "")
    rimeAPI.set_property(session, "aquatype_seq_id", "")
    rimeAPI.clear_composition(session)
    NSApp.squirrelAppDelegate.panel?.aquaTypeCompositionPanelVisible = false
    aquaTypeHidePalettes(force: true)
    NSApp.squirrelAppDelegate.panel?.navMode = false
    // An application that has already ended the composition itself is not sent an empty one: Chromium takes that as a
    // new composition and may delete the selection.
    if touchingClient { show(preedit: "", selRange: .empty, caretPos: 0) } else { preedit = "" }
    return true
  }

  @discardableResult
  func aquaTypeRenderHeldPanel(for raw: String) -> Bool {
    guard !raw.isEmpty,
          !aquaTypeLastPanelRaw.isEmpty,
          !aquaTypeLastPanelCandidates.isEmpty else { return false }
    if raw.hasPrefix(aquaTypeLastPanelRaw), raw != aquaTypeLastPanelRaw {
      return false
    }
    guard raw.hasPrefix(aquaTypeLastPanelRaw) || aquaTypeLastPanelRaw.hasPrefix(raw) else {
      return false
    }

    let maxCount = max(1, aquaTypeSyllableCount(raw))
    let (candidates, comments, originalIndexes) = aquaTypeClippedCandidateCommentPairs(aquaTypeLastPanelCandidates,
                                                                                       comments: aquaTypeLastPanelComments,
                                                                                       maxCount: maxCount)
    guard !candidates.isEmpty else { return false }

    let echo = aquaTypeBopomofoEcho(for: raw)
    if !echo.isEmpty {
      show(preedit: echo,
           selRange: NSRange(location: 0, length: echo.utf16.count),
           caretPos: echo.utf16.count)
    }
    // Paginated exactly like a fresh render, so the panel does not change shape because its candidates are held rather
    // than freshly decoded.  The highlight is kept on the same candidate: aquaTypeLastPanelHighlighted counts in the full
    // list, which Return commits from, and the clipped list can be shorter.  A highlighted candidate the clipping
    // dropped moves the highlight to the first row, in both places, so the row shown and the one Return commits agree.
    let highlighted: Int
    if let row = originalIndexes.firstIndex(of: aquaTypeLastPanelHighlighted) {
      highlighted = row
    } else {
      highlighted = 0
      aquaTypeLastPanelHighlighted = originalIndexes[0]
    }
    let display = aquaTypePanelDisplayIndexes(raw: raw, candidates: candidates, total: candidates.count, highlighted: highlighted)
    guard !display.indexes.isEmpty else { return false }
    NSApp.squirrelAppDelegate.panel?.navMode = true
    showPanel(preedit: "", selRange: NSRange(location: 0, length: 0), caretPos: 0,
              candidates: display.indexes.map { candidates[$0] },
              comments: display.indexes.map { comments[$0] },
              labels: aquaTypeCandidateLabels(startingAt: display.indexes.first ?? 0, count: display.indexes.count),
              highlighted: display.localHighlight,
              page: display.page, lastPage: display.lastPage,
              // Back to where each row came from in aquaTypeLastPanelCandidates, which is the list `selectCandidate`
              // indexes.  `display.indexes` alone points into the clipped list and is only the same thing when nothing
              // was dropped.
              candidateIndexMap: display.indexes.map { originalIndexes[$0] })
    return true
  }

  @discardableResult
  func aquaTypeOpenTopPage(raw: String, highlighted requestedHighlight: Int = 0) -> Bool {
    let rendered = aquaTypeShowVerifiedResponse(for: raw, highlighted: requestedHighlight)
    if rendered {
      NSApp.squirrelAppDelegate.panel?.navMode = true
      aquaTypeSkipNextRimeUpdate = true
    }
    return rendered
  }

  @discardableResult
  func aquaTypeOpenNextPageType(raw: String) -> Bool {
    guard raw == aquaTypeLastPanelRaw,
          !aquaTypeLastPanelCandidates.isEmpty else {
      return aquaTypeOpenTopPage(raw: raw, highlighted: 0)
    }
    let total = aquaTypeLastPanelCandidates.count
    let current = max(0, min(aquaTypeLastPanelHighlighted, total - 1))
    let target = aquaTypePanelRightTarget(raw: raw, current: current, total: total, candidates: aquaTypeLastPanelCandidates)
    let rendered = aquaTypePanelRenderCurrent(raw: raw, highlighted: target)
    if rendered {
      aquaTypeSkipNextRimeUpdate = true
    }
    return rendered
  }

  @discardableResult
  func aquaTypeOpenPreviousPageType(raw: String) -> Bool {
    guard raw == aquaTypeLastPanelRaw,
          !aquaTypeLastPanelCandidates.isEmpty else {
      return aquaTypeOpenTopPage(raw: raw, highlighted: max(0, aquaTypeResponseCandidates(for: raw).count - 1))
    }
    let total = aquaTypeLastPanelCandidates.count
    let current = max(0, min(aquaTypeLastPanelHighlighted, total - 1))
    let target = aquaTypePanelLeftTarget(raw: raw, current: current, total: total, candidates: aquaTypeLastPanelCandidates)
    let rendered = aquaTypePanelRenderCurrent(raw: raw, highlighted: target)
    if rendered {
      aquaTypeSkipNextRimeUpdate = true
    }
    return rendered
  }

  @discardableResult
  func aquaTypeHandleSwiftCandidateKey(_ rimeKeycode: UInt32, modifiers rimeModifiers: UInt32) -> Bool {
    guard (rimeModifiers & (kReleaseMask.rawValue | kControlMask.rawValue | kAltMask.rawValue | kSuperMask.rawValue)) == 0 else {
      return false
    }
    let raw = aquaTypeCurrentRawBuffer()
    // Tab and Shift+Tab never leave a composition: Tab moves to the next group of candidates, and Shift+Tab does
    // nothing.  macOS sends Shift+Tab as Tab with Shift held, so the Shift is what tells them apart.
    if !raw.isEmpty, Int32(rimeKeycode) == XK_Tab || Int32(rimeKeycode) == XK_ISO_Left_Tab {
      if Int32(rimeKeycode) == XK_Tab, (rimeModifiers & kShiftMask.rawValue) == 0 {
        aquaTypeJumpToNextCandidateGroup(raw: raw)
      }
      return true
    }
    // Keys typed but no candidates yet (the first answer is on its way, or there is no engine): a navigation key has
    // nothing to move, and it must not reach the application, which would move its caret under the composition.
    if !raw.isEmpty, aquaTypeLastPanelCandidates.isEmpty {
      switch Int32(rimeKeycode) {
      case XK_Left, XK_Right, XK_Up, XK_Down, XK_Page_Up, XK_Page_Down, XK_Home, XK_End: return true
      default: break
      }
    }
    if !raw.isEmpty,
       raw != aquaTypeLastPanelRaw,
       !aquaTypeLastPanelCandidates.isEmpty {
      switch Int32(rimeKeycode) {
      case XK_Page_Down, XK_Right:
        return aquaTypeOpenNextPageType(raw: raw)
      case XK_Page_Up, XK_Left:
        return aquaTypeOpenPreviousPageType(raw: raw)
      case XK_Down, XK_Up:
        if aquaTypeShowVerifiedResponse(for: raw, highlighted: 0) {
          aquaTypeSkipNextRimeUpdate = true
        }
        return true
      default:
        break
      }
    }
    guard !raw.isEmpty,
          raw == aquaTypeLastPanelRaw,
          !aquaTypeLastPanelCandidates.isEmpty else { return false }

    let oldHighlight = aquaTypeLastPanelHighlighted
    var newHighlight = oldHighlight
    switch Int32(rimeKeycode) {
    case XK_Page_Down, XK_Right:
      return aquaTypeOpenNextPageType(raw: raw)
    case XK_Page_Up, XK_Left:
      return aquaTypeOpenPreviousPageType(raw: raw)
    // Down and up walk the whole list and cross page boundaries, and stop at the two ends: at the bottom of a long
    // list one more press does nothing rather than jumping back to the top.
    case XK_Down:
      newHighlight = aquaTypePanelDownTarget(current: oldHighlight, total: aquaTypeLastPanelCandidates.count)
    case XK_Up:
      newHighlight = aquaTypePanelUpTarget(current: oldHighlight)
    case XK_Home:
      newHighlight = 0
    case XK_End:
      newHighlight = aquaTypeLastPanelCandidates.count - 1
    case XK_Return, XK_KP_Enter:
      // Shift+Return is a new line: the composition is committed (processKey) and the key goes on.
      guard (rimeModifiers & kShiftMask.rawValue) == 0 else { return false }
      return aquaTypeCommitPanelCandidate(index: oldHighlight, raw: raw)
    default:
      return false
    }
    NSApp.squirrelAppDelegate.panel?.navMode = true
    let rendered = aquaTypeRenderCandidates(raw: raw,
                                            candidates: aquaTypeLastPanelCandidates,
                                            highlighted: newHighlight,
                                            comments: aquaTypeLastPanelComments,
                                            consumed: aquaTypeLastPanelConsumed)
    if rendered {
      aquaTypeSkipNextRimeUpdate = true
    }
    return rendered
  }

  /// Where each group of the current answer starts: the sentences (memory and model), the words for the leading
  /// syllables, the characters of the first syllable.  An empty group has no start.  Nil when the panel is not showing
  /// that answer, whose rows are what say which group a candidate belongs to.
  func aquaTypeCandidateGroupStarts(raw: String) -> [Int]? {
    let rows = aquaTypeResponseRows(for: raw)
    guard !rows.isEmpty, raw == aquaTypeLastPanelRaw,
          Array(aquaTypeLastPanelCandidates.prefix(rows.count)) == rows.map(\.text) else { return nil }
    func group(_ source: String) -> Int {
      switch source.split(separator: "+").first.map(String.init) ?? "" {
      case AquaTypeV2ResponseContract.wordSource: return 1
      case AquaTypeV2ResponseContract.characterSource: return 2
      default: return 0
      }
    }
    var starts = [0]
    for i in rows.indices.dropFirst() where group(rows[i].source) != group(rows[i - 1].source) { starts.append(i) }
    return starts
  }

  /// Tab: the highlight moves to the first candidate of the next group, and from the last group back to the first
  /// candidate.  The pages are not changed; the panel turns to the page the highlight lands on.  When the panel is
  /// still showing an earlier answer, the one for these keys is shown first.
  func aquaTypeJumpToNextCandidateGroup(raw: String) {
    if raw != aquaTypeLastPanelRaw {
      guard aquaTypeShowVerifiedResponse(for: raw, highlighted: 0) else {
        return
      }
    }
    guard let starts = aquaTypeCandidateGroupStarts(raw: raw) else { return }
    let target = starts.first(where: { $0 > aquaTypeLastPanelHighlighted }) ?? 0
    NSApp.squirrelAppDelegate.panel?.navMode = true
    if aquaTypePanelRenderCurrent(raw: raw, highlighted: target) {
      aquaTypeSkipNextRimeUpdate = true
    }
  }

  func aquaTypeHighlightedCandidate() -> (index: Int, text: String)? {
    let raw = aquaTypeCurrentRawBuffer()
    if !raw.isEmpty,
       raw == aquaTypeLastPanelRaw,
       !aquaTypeLastPanelCandidates.isEmpty {
      let index = max(0, min(aquaTypeLastPanelHighlighted, aquaTypeLastPanelCandidates.count - 1))
      return (index, aquaTypeLastPanelCandidates[index])
    }
    // The panel has nothing for what is typed now - a commit came before the engine's answer did.  librime's own
    // conversion of the keys, composed only now, is the traditional input method's best guess, and better than
    // committing the bopomofo.  Space is passed as it is: it is the first tone on this layout.
    if !raw.isEmpty {
      _ = rimeAPI.set_input(session, raw)
      rimeAPI.set_caret_pos(session, 0)
    }
    var ctx = RimeContext_stdbool.rimeStructInit()
    guard rimeAPI.get_context(session, &ctx) else { return nil }
    defer { _ = rimeAPI.free_context(&ctx) }
    let count = Int(ctx.menu.num_candidates)
    guard count > 0 else { return nil }
    let highlighted = max(0, min(Int(ctx.menu.highlighted_candidate_index), count - 1))
    guard let ptr = ctx.menu.candidates[highlighted].text,
          let text = String(validatingUTF8: ptr),
          aquaTypeHasCJK(text) else { return nil }
    return (highlighted, text)
  }

  @discardableResult
  func aquaTypeHandleInlineAsciiKey(_ rimeKeycode: UInt32, modifiers rimeModifiers: UInt32) -> Bool {
    guard (rimeModifiers & (kReleaseMask.rawValue | kControlMask.rawValue | kAltMask.rawValue | kSuperMask.rawValue)) == 0,
          (rimeModifiers & kShiftMask.rawValue) != 0,
          !rimeAPI.get_option(session, "ascii_mode"),
          rimeKeycode >= 32,
          rimeKeycode <= 126,
          let scalar = UnicodeScalar(rimeKeycode) else {
      return false
    }
    let ch = Character(scalar)
    guard ch.isLetter else {
      return false
    }
    let hasComposition = !aquaTypeCurrentRawBuffer().isEmpty || !preedit.isEmpty
    if !hasComposition || aquaTypeCommitCompositionForAsciiSwitch() {
      // This field only: a capital typed in passing is not a choice of English for every application.
      rimeAPI.set_option(session, "ascii_mode", true)
      client?.insertText(String(ch), replacementRange: .empty)
      return true
    }
    return false
  }

  @discardableResult
  func aquaTypeCommitCompositionForAsciiSwitch() -> Bool {
    let raw = aquaTypeCurrentRawBuffer()
    guard !raw.isEmpty || !preedit.isEmpty else { return false }
    // Punctuation, Shift and focus leaving all take the whole input, so a candidate the user moved the highlight to is
    // respected only when it accounts for every key; one that takes the leading keys only (reached with Tab) would
    // leave the rest of the keys nowhere.  The whole-input candidate is committed instead.
    let text: String? = {
      if let selected = aquaTypeHighlightedCandidate(), selected.index > 0,
         raw.isEmpty || aquaTypePanelCandidateTakesAllKeys(selected.index, raw: raw) {
        return selected.text
      }
      if !raw.isEmpty, let top = aquaTypeResponseWholeCandidate(for: raw) ?? aquaTypePanelWholeCandidate(raw: raw),
         aquaTypeHasCJK(top) {
        return top
      }
      if let selected = aquaTypeHighlightedCandidate(),
         raw.isEmpty || aquaTypePanelCandidateTakesAllKeys(selected.index, raw: raw) {
        return selected.text
      }
      if !raw.isEmpty, lastInlineEchoRaw == raw, aquaTypeHasCJK(lastInlineEcho) {
        // Only a preview of the whole input: one of a highlighted word stops after the word.
        let clipped = aquaTypeClipPreview(lastInlineEcho, maxCount: aquaTypeSyllableCount(raw))
        if !clipped.isEmpty, clipped.count == aquaTypeSyllableCount(raw), clipped.allSatisfy({ aquaTypeHasCJK(String($0)) }) {
          return clipped
        }
      }
      return nil
    }()
    // Chosen before the generation moves on, since the engine's answer is looked up by it; moved on before committing,
    // so an answer still on its way describes the composition that is gone and is not shown.
    _ = aquaTypeBeginClientSeq()
    guard let text else { return false }
    commit(string: text)
    return true
  }

  func aquaTypeToggleAsciiModeFromBareShift() {
    let isAscii = rimeAPI.get_option(session, "ascii_mode")
    if isAscii {
      aquaTypeChooseAsciiMode(false)
      return
    }
    let hasComposition = !aquaTypeCurrentRawBuffer().isEmpty || !preedit.isEmpty
    if hasComposition {
      guard aquaTypeCommitCompositionForAsciiSwitch() else { return }
    }
    aquaTypeChooseAsciiMode(true)
  }

  /// The Chinese/English mode the person last chose with a key, for every field and application that follows: each
  /// gets a librime session of its own, which starts in Chinese, and librime saves an option only when its own switcher
  /// menu changes it (AquaType has none).  Kept for the login session, as the process is.
  static var aquaTypeChosenAsciiMode: Bool?

  func aquaTypeChooseAsciiMode(_ ascii: Bool) {
    rimeAPI.set_option(session, "ascii_mode", ascii)
    // An application whose options set the mode (a terminal) keeps a mode of its own: Chinese chosen there for a
    // moment is not a choice for every other application, nor English everywhere when it is switched back.
    guard NSApp.squirrelAppDelegate.config?.getAppOptions(currentApp)["ascii_mode"] == nil else { return }
    Self.aquaTypeChosenAsciiMode = ascii
  }

  /// The chosen mode, in a session that arrives in a field: not where the application's own options set it (Terminal
  /// and the editors that start in English, squirrel.yaml app_options).  Silently: no key changed it here.
  func aquaTypeApplyChosenAsciiMode() {
    guard let ascii = Self.aquaTypeChosenAsciiMode, aquaTypeSessionAlive,
          NSApp.squirrelAppDelegate.config?.getAppOptions(currentApp)["ascii_mode"] == nil,
          rimeAPI.get_option(session, "ascii_mode") != ascii else { return }
    rimeAPI.set_option(session, "ascii_mode", ascii)
  }

  @discardableResult
  func aquaTypeHandleColdEnterKey(_ rimeKeycode: UInt32, modifiers rimeModifiers: UInt32) -> Bool {
    guard (rimeModifiers & (kReleaseMask.rawValue | kControlMask.rawValue | kAltMask.rawValue | kSuperMask.rawValue | kShiftMask.rawValue)) == 0,
          (Int32(rimeKeycode) == XK_Return || Int32(rimeKeycode) == XK_KP_Enter) else {
      return false
    }
    let raw = aquaTypeCurrentRawBuffer()
    guard !raw.isEmpty else { return false }

    if raw == aquaTypeLastPanelRaw,
       !aquaTypeLastPanelCandidates.isEmpty,
       let selected = aquaTypeHighlightedCandidate(),
       !selected.text.isEmpty {
      // Through the panel commit, so a candidate that accounts for only part of the input leaves the rest in the
      // composition instead of discarding it.  Committing the text directly ignores consumed_keys.
      return aquaTypeCommitPanelCandidate(index: selected.index, raw: raw)
    }
    if let top = aquaTypeResponseWholeCandidate(for: raw), aquaTypeHasCJK(top) {
      commit(string: top)
      return true
    }

    // No engine to answer, or a Return that has waited for its answer already: what librime made of the keys, or the
    // bopomofo, the way punctuation and focus leaving commit, rather than a Return that does nothing however often it
    // is pressed.
    if Self.aquaTypeEngineUnavailable || aquaTypeClientSeq == aquaTypeAnswerWaitedGeneration {
      aquaTypeCommitCompositionBeforePunctuation()
      return true
    }
    // Nothing waits here for the answer: a wait inside a key callback freezes the application and can commit an
    // out-of-date answer.  The composition stays, and its candidates appear when the answer arrives.
    aquaTypeWriteBridgeRequest(for: raw)
    _ = aquaTypeSetRawBuffer(raw, clearIfEmpty: false, preservePanel: true)
    return true
  }

  @discardableResult
  func aquaTypeShowVerifiedResponse(for raw: String, highlighted requestedHighlight: Int? = nil) -> Bool {
    let rows = aquaTypeResponseRows(for: raw)
    let candidates = rows.map(\.text)
    guard let top = candidates.first, aquaTypeHasCJK(top) else { return false }
    if raw == aquaTypeLastPanelRaw,
       aquaTypeLastPanelCandidates.count > candidates.count,
       candidates.count <= aquaTypePanelRows,
       Array(aquaTypeLastPanelCandidates.prefix(candidates.count)) == candidates {
      let highlighted = max(0, min(requestedHighlight ?? aquaTypeLastPanelHighlighted, aquaTypeLastPanelCandidates.count - 1))
      return aquaTypeRenderCandidates(raw: raw,
                                      candidates: aquaTypeLastPanelCandidates,
                                      highlighted: highlighted,
                                      comments: aquaTypeLastPanelComments,
                                      consumed: aquaTypeLastPanelConsumed)
    }
    let highlighted = max(0, min(requestedHighlight ?? aquaTypeLastPanelHighlighted, candidates.count - 1))
    let comments = rows.map { aquaTypeCommentForSource($0.source) }
    return aquaTypeRenderCandidates(raw: raw, candidates: candidates, highlighted: highlighted, comments: comments,
                                    consumed: rows.map(\.consumedKeys))
  }

  // 大千 (Dachen) key classes, used to count syllables and to find where the last one starts (Backspace).
  private static let aquaInitials = Set("1qaz2wsxedcrfv5tgbyhn")
  private static let aquaMedials  = Set("ujm")
  private static let aquaFinals   = Set("8ik,9ol.0p;/-")
  private static let aquaTones    = Set(" 6347")
  private static let aquaBopomofoKeymap: [Character: String] = [
    "1": "ㄅ", "q": "ㄆ", "a": "ㄇ", "z": "ㄈ",
    "2": "ㄉ", "w": "ㄊ", "s": "ㄋ", "x": "ㄌ",
    "e": "ㄍ", "d": "ㄎ", "c": "ㄏ",
    "r": "ㄐ", "f": "ㄑ", "v": "ㄒ",
    "5": "ㄓ", "t": "ㄔ", "g": "ㄕ", "b": "ㄖ",
    "y": "ㄗ", "h": "ㄘ", "n": "ㄙ",
    "u": "ㄧ", "j": "ㄨ", "m": "ㄩ",
    "8": "ㄚ", "i": "ㄛ", "k": "ㄜ", ",": "ㄝ",
    "9": "ㄞ", "o": "ㄟ", "l": "ㄠ", ".": "ㄡ",
    "0": "ㄢ", "p": "ㄣ", ";": "ㄤ", "/": "ㄥ", "-": "ㄦ", " ": "ˉ",
    "6": "ˊ", "3": "ˇ", "4": "ˋ", "7": "˙"
  ]

  /// Whether the text contains a Han character.  The range has to match the contract's, extension planes included: the
  /// character table carries every reading in Unicode, and a test that stopped at 0x9FFF would drop extension-B
  /// characters on their way to the panel.
  private func aquaTypeHasCJK(_ s: String) -> Bool {
    for u in s.unicodeScalars {
      switch u.value {
      case 0x3400...0x4DBF, 0x4E00...0x9FFF, 0xF900...0xFAFF,
           0x20000...0x2A6DF, 0x2A700...0x2EBEF, 0x2F800...0x2FA1F, 0x30000...0x323AF:
        return true
      default: continue
      }
    }
    return false
  }

  private func aquaTypeClipPreview(_ s: String, maxCount: Int) -> String {
    guard maxCount >= 0 else { return s }
    if maxCount == 0 { return "" }
    if !aquaTypeHasCJK(s) { return s }
    if s.count <= maxCount { return s }
    return String(s.prefix(maxCount))
  }

  /// The held panel's rows: each candidate clipped to the shorter key string, with the ones that clip to nothing, to
  /// non-Chinese, or to something already listed dropped, and at most nine kept.
  ///
  /// The third element is where each surviving row came from in the list that was passed in.  Dropping entries means
  /// row *k* here is not candidate *k* there, and a click on a row is answered by `selectCandidate`, which indexes the
  /// original list.
  private func aquaTypeClippedCandidateCommentPairs(_ candidates: [String],
                                                    comments: [String],
                                                    maxCount: Int) -> ([String], [String], [Int]) {
    var resultCandidates = [String]()
    var resultComments = [String]()
    var resultIndexes = [Int]()
    var seen = Set<String>()
    for (index, candidate) in candidates.enumerated() {
      let clipped = aquaTypeClipPreview(candidate, maxCount: maxCount)
      if clipped.isEmpty || !aquaTypeHasCJK(clipped) || seen.contains(clipped) { continue }
      resultCandidates.append(clipped)
      resultComments.append(index < comments.count ? comments[index] : "")
      resultIndexes.append(index)
      seen.insert(clipped)
      if resultCandidates.count >= 9 { break }
    }
    return (resultCandidates, resultComments, resultIndexes)
  }

  private func aquaTypeBopomofoEcho(for raw: String) -> String {
    var result = ""
    for ch in raw {
      guard let mapped = Self.aquaBopomofoKeymap[ch] else { continue }
      result += mapped
      if Self.aquaTones.contains(ch) {
        result.append(" ")
      }
    }
    return result.trimmingCharacters(in: .whitespaces)
  }

  // MARK: - Characters this machine can draw
  //
  // The third source offers every character whose reading matches, and char_to_keys.json carries 41,232 of them -
  // well past what any font on a stock macOS install has glyphs for.  Those arrive as ⍰, a boxed question mark: on
  // `ㄍㄜ`, six of the first eight candidates had no glyph.  The user cannot read such a candidate to choose it, and one
  // chosen anyway goes into the document as the same box.  So they are filtered here, against the font the panel
  // draws with and the fallback cascade AppKit would follow, which answers "would this appear" rather than "is it in
  // this one font".
  private static var aquaTypeGlyphCache = [UnicodeScalar: Bool]()

  func aquaTypeCanRender(_ text: String) -> Bool {
    let font = NSApp.squirrelAppDelegate.panel?.candidateFont ?? NSFont.systemFont(ofSize: 15)
    for scalar in text.unicodeScalars {
      if scalar.value < 0x80 || scalar.properties.isWhitespace { continue }
      if let known = Self.aquaTypeGlyphCache[scalar] {
        if !known { return false }
        continue
      }
      let drawable = Self.aquaTypeHasGlyph(scalar, in: font)
      Self.aquaTypeGlyphCache[scalar] = drawable
      if !drawable { return false }
    }
    return true
  }

  private static func aquaTypeHasGlyph(_ scalar: UnicodeScalar, in font: NSFont) -> Bool {
    let text = String(scalar)
    let utf16 = Array(text.utf16)
    var glyphs = [CGGlyph](repeating: 0, count: utf16.count)
    if CTFontGetGlyphsForCharacters(font, utf16, &glyphs, utf16.count) { return true }
    // Not in the panel's own font, which is ordinary - 😀 is not in PingFang either and the system draws it from
    // AppleColorEmoji.  So the question is which font the system WOULD use, and whether that font is a real one.
    // Asking the substitute for glyphs answers yes for every character in Unicode, because the cascade ends at
    // LastResort, which has a glyph for everything - the boxed question mark.  The substitute's name is what tells
    // "drawn" from "drawn as a box".
    let substitute = CTFontCreateForString(font, text as CFString,
                                           CFRange(location: 0, length: utf16.count))
    return (CTFontCopyPostScriptName(substitute) as String?) != "LastResort"
  }


  private func aquaTypeSyllableCount(_ raw: String) -> Int {
    let chars = Array(raw)
    let n = chars.count
    var i = 0, count = 0
    while i < n {
      let ch = chars[i]
      if Self.aquaTones.contains(ch) {
        i += 1
      } else {
        var consumed = false
        if Self.aquaInitials.contains(ch) { i += 1; consumed = true }
        if i < n && Self.aquaMedials.contains(chars[i]) { i += 1; consumed = true }
        if i < n && Self.aquaFinals.contains(chars[i]) { i += 1; consumed = true }
        if consumed {
          count += 1
          if i < n && Self.aquaTones.contains(chars[i]) { i += 1 }
        } else {
          i += 1
        }
      }
    }
    return count
  }

  // Number of trailing keys that form the LAST bopomofo syllable/unit.
  private func aquaTypeLastSyllableKeyCount(_ raw: String) -> Int {
    let chars = Array(raw)
    let n = chars.count
    if n == 0 { return 0 }
    var i = 0
    var lastStart = 0
    while i < n {
      lastStart = i
      let ch = chars[i]
      if Self.aquaTones.contains(ch) {
        i += 1
      } else {
        var consumed = false
        if Self.aquaInitials.contains(ch) { i += 1; consumed = true }
        if i < n && Self.aquaMedials.contains(chars[i]) { i += 1; consumed = true }
        if i < n && Self.aquaFinals.contains(chars[i]) { i += 1; consumed = true }
        if consumed {
          if i < n && Self.aquaTones.contains(chars[i]) { i += 1 }
        } else {
          i += 1
        }
      }
    }
    return n - lastStart
  }

  // Backspace takes back one bopomofo key while the last syllable is unfinished, as every traditional input method
  // does, so a mistyped symbol costs one key.  Once the syllable has its tone it stands for a character, and Backspace
  // removes the whole of it, including an invisible tone-1 Space, so no blank composition is left behind.
  private func aquaTypeBackspaceSyllableKeyCount(_ raw: String) -> Int {
    guard let lastKey = raw.last else { return 0 }
    if !Self.aquaTones.contains(lastKey) { return 1 }
    var chars = Array(raw)
    var trailingInvisible = 0
    while let last = chars.last, last == " " {
      chars.removeLast()
      trailingInvisible += 1
    }
    if chars.isEmpty {
      return max(1, trailingInvisible)
    }
    let visibleTailRaw = String(chars)
    return trailingInvisible + max(1, aquaTypeLastSyllableKeyCount(visibleTailRaw))
  }

  private func aquaTypeBackspaceResult(for raw: String) -> (nextRaw: String, deleteCount: Int, shouldClear: Bool) {
    guard !raw.isEmpty else { return ("", 0, true) }
    let deleteCount = max(1, aquaTypeBackspaceSyllableKeyCount(raw))
    let keepCount = max(0, raw.count - deleteCount)
    let nextRaw = String(raw.prefix(keepCount))
    let shouldClear = nextRaw.isEmpty || aquaTypeBopomofoEcho(for: nextRaw).isEmpty
    return (nextRaw, deleteCount, shouldClear)
  }

  @discardableResult
  private func aquaTypeApplyBackspace(raw: String, path: String) -> Bool {
    let result = aquaTypeBackspaceResult(for: raw)
    let nextRaw = result.nextRaw
    let expectedDisplayLength = aquaTypeSyllableCount(nextRaw)
    if lastInlineEchoRaw == raw {
      lastInlineEcho = aquaTypeClipPreview(lastInlineEcho, maxCount: expectedDisplayLength)
      lastInlineEchoRaw = nextRaw
    } else {
      lastInlineEcho = ""
      lastInlineEchoRaw = ""
    }
    if result.shouldClear {
      _ = aquaTypeClearCompositionToEmpty()
      aquaTypeSkipNextRimeUpdate = true
      return true
    }
    if aquaTypeSetRawBuffer(nextRaw, preservePanel: true) {
      aquaTypeSkipNextRimeUpdate = true
      return true
    }
    return true
  }

  @discardableResult
  func aquaTypeHandleBackspaceCompositionKey(_ rimeKeycode: UInt32, modifiers rimeModifiers: UInt32) -> Bool {
    guard Int32(rimeKeycode) == XK_BackSpace,
          (rimeModifiers & (kReleaseMask.rawValue | kControlMask.rawValue | kAltMask.rawValue | kSuperMask.rawValue)) == 0 else {
      return false
    }
    let raw = aquaTypeCurrentRawBuffer()
    guard !raw.isEmpty else {
      rimeAPI.set_property(session, "aquatype_raw_input", "")
      aquaTypeClearBridgeState()
      NSApp.squirrelAppDelegate.panel?.navMode = false
      NSApp.squirrelAppDelegate.panel?.aquaTypeCompositionPanelVisible = false
      aquaTypeHidePalettes(force: true)
      return false
    }
    _ = aquaTypeBeginClientSeq()
    return aquaTypeApplyBackspace(raw: raw, path: "early")
  }

  func processKey(_ rimeKeycode: UInt32, modifiers rimeModifiers: UInt32) -> Bool {

    if aquaTypeRejectKeyPastLengthCap(rimeKeycode, modifiers: rimeModifiers) { return true }

    if aquaTypeHandleBackspaceCompositionKey(rimeKeycode, modifiers: rimeModifiers) {
      return true
    }
    // Editing keys with a modifier, during a composition, edit the composition rather than ending it: Control+H and
    // Control+Backspace take back one key, as Backspace does; Option+Backspace (a word back) takes back all of it; forward
    // Delete has nothing after the caret to delete.  Ending it would put its characters into the document - and into the
    // personal memory - just before the application deleted them.
    if !aquaTypeCurrentRawBuffer().isEmpty, (rimeModifiers & kReleaseMask.rawValue) == 0 {
      let key = Int32(rimeKeycode)
      let control = (rimeModifiers & kControlMask.rawValue) != 0, option = (rimeModifiers & kAltMask.rawValue) != 0
      if key == XK_BackSpace || (control && (key == XK_h || key == XK_H)) {
        if option {
          _ = aquaTypeBeginClientSeq()
          return aquaTypeClearCompositionToEmpty()
        }
        return aquaTypeHandleBackspaceCompositionKey(UInt32(XK_BackSpace), modifiers: 0)
      }
      if key == XK_Delete { return true }
    }
    if Int32(rimeKeycode) == XK_BackSpace &&
        (rimeModifiers & (kReleaseMask.rawValue | kControlMask.rawValue | kAltMask.rawValue | kSuperMask.rawValue)) == 0 {
      return false
    }

    // with linear candidate list, arrow keys may behave differently.
    if let panel = NSApp.squirrelAppDelegate.panel {
      if panel.linear != rimeAPI.get_option(session, "_linear") {
        rimeAPI.set_option(session, "_linear", panel.linear)
      }
      // with vertical text, arrow keys may behave differently.
      if panel.vertical != rimeAPI.get_option(session, "_vertical") {
        rimeAPI.set_option(session, "_vertical", panel.vertical)
      }
      // Track candidate-navigation mode: a navigation key surfaces the panel, and any other key resets it.  It only
      // changes what is drawn when the theme sets `hide_panel: true`, which AquaType's does not.
      let isReleased = (rimeModifiers & kReleaseMask.rawValue) != 0
      if !isReleased {
        switch Int32(rimeKeycode) {
        case XK_Up, XK_Down, XK_Left, XK_Right,
             XK_Tab, XK_ISO_Left_Tab,
             XK_Page_Up, XK_Page_Down,
             XK_Home, XK_End:
          panel.navMode = true
        default:
          panel.navMode = false
        }
      }
    }

    // Escape cancels the composition when there is one (passed through, TextEdit would insert a literal ESC and the old
    // keys would run into the next phrase), and otherwise belongs to the application: closing a dialog, leaving full
    // screen, dismissing a sheet.
    //
    // In an app with `vim_mode` (MacVim), Escape, Control-C and Control-[ are how Vim leaves insert mode.  They
    // cancel a composition like Escape does, and otherwise go to Vim with the input method switched to English, so
    // the normal-mode commands that follow are not typed as bopomofo.
    let isVimLeaveInsert = Int32(rimeKeycode) == XK_Escape
      || ((rimeModifiers & kControlMask.rawValue) != 0
          && (Int32(rimeKeycode) == XK_c || Int32(rimeKeycode) == XK_C || Int32(rimeKeycode) == XK_bracketleft))
    let inVimMode = rimeAPI.get_option(session, "vim_mode")
    if (rimeModifiers & kReleaseMask.rawValue) == 0,
       Int32(rimeKeycode) == XK_Escape || (isVimLeaveInsert && inVimMode) {
      if !aquaTypeCurrentRawBuffer().isEmpty {
        _ = aquaTypeBeginClientSeq()
        _ = aquaTypeClearCompositionToEmpty()
        return true
      }
      if inVimMode && !rimeAPI.get_option(session, "ascii_mode") {
        rimeAPI.set_option(session, "ascii_mode", true)
      }
      return false
    }

    if aquaTypeHandleInlineAsciiKey(rimeKeycode, modifiers: rimeModifiers) {
      return true
    }

    if aquaTypeHandleMacPunctuationKey(rimeKeycode, modifiers: rimeModifiers) {
      return true
    }

    if aquaTypeHandleSwiftCandidateKey(rimeKeycode, modifiers: rimeModifiers) {
      return true
    }

    if aquaTypeHandleColdEnterKey(rimeKeycode, modifiers: rimeModifiers) {
      return true
    }

    // Own the raw Dachen buffer in Squirrel. This prevents raw ASCII
    // pass-through and avoids losing tail keys when librime exposes only a
    // partial segment (common for long input and trailing tone-1 Space).
    if (rimeModifiers & (kReleaseMask.rawValue | kControlMask.rawValue | kAltMask.rawValue | kSuperMask.rawValue | kShiftMask.rawValue)) == 0,
       !rimeAPI.get_option(session, "ascii_mode"),
       let rawChar = aquaTypeRawChar(for: rimeKeycode) {
      let currentRaw = aquaTypeCurrentRawBuffer()
      // A Space right after a tone, or after another Space, completes nothing: the syllable before it already has its
      // tone.  People used to the built-in 注音 press it to bring up the candidates, and taken as a key it added a
      // syllable nobody typed (注意 came out as 注意阿).  It is taken and does nothing - the composition's generation
      // included, so an answer on its way for these keys is still shown when it arrives.
      if rawChar == " ", let last = currentRaw.last, Self.aquaTones.contains(last) {
        return true
      }
      _ = aquaTypeBeginClientSeq()
      if rawChar == " " && currentRaw.isEmpty {
        return false
      }
      let nextRaw = currentRaw + rawChar
      if aquaTypeSetRawBuffer(nextRaw, clearIfEmpty: false) {
        aquaTypeSkipNextRimeUpdate = true
        return true
      }
    }

    // A punctuation key the table above does not list (another keyboard layout can produce one) goes to Rime's
    // punctuator.  AquaType's composition is not librime's, so Rime would commit only the punctuation and drop the
    // characters being typed: commit them first.
    if !aquaTypeCurrentRawBuffer().isEmpty, aquaTypeIsPunctuationCommitKey(rimeKeycode) {
      aquaTypeCommitCompositionBeforePunctuation()
    }
    // Any other key pressed while a composition shows, which nothing above took - the number pad's digits and
    // operators, Shift+Space, a Control chord, a function key - would go through librime, whose own composition is
    // empty, to the application, with the bopomofo still marked in the document; what an application does with that
    // differs from one to the next.  The composition is committed first, as for punctuation, and the key follows it.
    // A modifier on its own changes nothing in the document and is left to the code below.
    let isModifierKeysym = (0xFFE1...0xFFEE).contains(Int(rimeKeycode))
    if !aquaTypeCurrentRawBuffer().isEmpty, (rimeModifiers & kReleaseMask.rawValue) == 0, !isModifierKeysym {
      aquaTypeCommitCompositionBeforePunctuation()
      return false
    }

    let handled = rimeAPI.process_key(session, Int32(rimeKeycode), Int32(rimeModifiers))
    if Int32(rimeKeycode) == XK_BackSpace && (rimeModifiers & kReleaseMask.rawValue) == 0 {
      let currentRaw = aquaTypeCurrentRawBuffer()
      if currentRaw.isEmpty {
        rimeAPI.set_property(session, "aquatype_raw_input", "")
        aquaTypeClearBridgeState()
      }
    }


    if handled {
      let isChordingKey = switch Int32(rimeKeycode) {
      case XK_space...XK_asciitilde, XK_Control_L, XK_Control_R, XK_Alt_L, XK_Alt_R, XK_Shift_L, XK_Shift_R:
        true
      default:
        false
      }
      if isChordingKey && rimeAPI.get_option(session, "_chord_typing") {
        updateChord(keycode: rimeKeycode, modifiers: rimeModifiers)
      } else if (rimeModifiers & kReleaseMask.rawValue) == 0 {
        // non-chording key pressed
        clearChord()
      }
    }

    return handled
  }

  func rimeConsumeCommittedText() {
    var commitText = RimeCommit.rimeStructInit()
    if rimeAPI.get_commit(session, &commitText) {
      if let text = commitText.text {
        commit(string: String(cString: text))
      }
      _ = rimeAPI.free_commit(&commitText)
    }
  }

  // swiftlint:disable:next cyclomatic_complexity
  func rimeUpdate() {
    rimeConsumeCommittedText()

    var status = RimeStatus_stdbool.rimeStructInit()
    if rimeAPI.get_status(session, &status) {
      // enable schema specific ui style
      // swiftlint:disable:next identifier_name
      if let schema_id = status.schema_id, schemaId == "" || schemaId != String(cString: schema_id) {
        schemaId = String(cString: schema_id)
        NSApp.squirrelAppDelegate.loadSettings(for: schemaId)
        // inline preedit
        if let panel = NSApp.squirrelAppDelegate.panel {
          inlinePreedit = (panel.inlinePreedit && !rimeAPI.get_option(session, "no_inline")) || rimeAPI.get_option(session, "inline")
          // if not inline, embed soft cursor in preedit string
          rimeAPI.set_option(session, "soft_cursor", !inlinePreedit)
        }
      }
      _ = rimeAPI.free_status(&status)
    }

    var ctx = RimeContext_stdbool.rimeStructInit()
    if rimeAPI.get_context(session, &ctx) {
      let currentRaw = aquaTypeCurrentRawBuffer()
      // While AquaType owns a raw composition, librime's own is empty (the keys are not fed to it), so neither its
      // preedit nor its candidates are drawn: the preedit would clear the bopomofo from the document on any key that
      // reaches here - a modifier, Return or Tab before the engine has answered - until the answer drew it back.  Its
      // candidates would repaint over the AquaType panel, make it blink, turn the labels back to 1-9 and lose the
      // navigation state.  The AquaType panel is redrawn instead; librime draws only when there is no AquaType
      // composition (punctuation menus and the like).
      if !currentRaw.isEmpty {
        if !aquaTypeShowVerifiedResponse(for: currentRaw, highlighted: aquaTypeLastPanelHighlighted) {
          _ = aquaTypeRenderHeldPanel(for: currentRaw)
        }
        _ = rimeAPI.free_context(&ctx)
        return
      }
      // update preedit text
      let preedit = ctx.composition.preedit.map({ String(cString: $0) }) ?? ""

      // `limitedBy`, because the unbounded `index(_:offsetBy:)` traps rather than returning nil when the offset runs
      // past the end.  The offsets come from librime's composition and should be within its own preedit; a context that
      // disagrees with itself must not take the input method down.
      func preeditIndex(_ utf8Offset: Int32) -> String.Index {
        preedit.utf8.index(preedit.utf8.startIndex, offsetBy: Int(utf8Offset), limitedBy: preedit.utf8.endIndex)
          .flatMap { String.Index($0, within: preedit) } ?? preedit.startIndex
      }
      let start = preeditIndex(ctx.composition.sel_start)
      let end = preeditIndex(ctx.composition.sel_end)
      let caretPos = preeditIndex(ctx.composition.cursor_pos)

      if inlinePreedit {
        show(preedit: preedit, selRange: NSRange(location: start.utf16Offset(in: preedit), length: preedit.utf16.distance(from: start, to: end)), caretPos: caretPos.utf16Offset(in: preedit))
      } else {
        // TRICKY: display a non-empty string to prevent iTerm2 from echoing
        // each character in preedit. note this is a full-shape space U+3000;
        // using half shape characters like "..." will result in an unstable
        // baseline when composing Chinese characters.
        show(preedit: preedit.isEmpty ? "" : "　", selRange: NSRange(location: 0, length: 0), caretPos: 0)
      }

      // update candidates
      let numCandidates = Int(ctx.menu.num_candidates)
      var candidates = [String]()
      var comments = [String]()
      for i in 0..<numCandidates {
        let candidate = ctx.menu.candidates[i]
        candidates.append(candidate.text.map { String(cString: $0) } ?? "")
        comments.append(candidate.comment.map { String(cString: $0) } ?? "")
      }
      var labels = [String]()
      // swiftlint:disable identifier_name
      if let select_keys = ctx.menu.select_keys {
        labels = String(cString: select_keys).map { String($0) }
      } else if let select_labels = ctx.select_labels {
        let pageSize = Int(ctx.menu.page_size)
        for i in 0..<pageSize {
          labels.append(select_labels[i].map { String(cString: $0) } ?? "")
        }
      }
      // swiftlint:enable identifier_name
      let page = Int(ctx.menu.page_no)
      let lastPage = ctx.menu.is_last_page
      let highlightedIndex = candidates.isEmpty ? 0 : max(0, min(Int(ctx.menu.highlighted_candidate_index), candidates.count - 1))
      showPanel(preedit: "", selRange: NSRange(location: 0, length: 0), caretPos: 0,
                candidates: candidates, comments: comments, labels: labels, highlighted: highlightedIndex,
                page: page, lastPage: lastPage)
      if !candidates.isEmpty {
        aquaTypeLastPanelRaw = currentRaw
        aquaTypeLastPanelCandidates = candidates
        // All-nil: these are librime's own candidates, and each of them covers the whole input.
        aquaTypeLastPanelConsumed = Array(repeating: nil, count: candidates.count)
        aquaTypeLastPanelComments = comments
        aquaTypeLastPanelHighlighted = highlightedIndex
      }
      _ = rimeAPI.free_context(&ctx)
    } else {
      lastInlineEcho = ""
      hidePalettes()
    }
  }

  /// Secure input is on for the application being typed into.  `IsSecureEventInputEnabled` reports the whole system,
  /// and a password manager, a VPN client or the login window can leave it on for hours; taken alone it would stop
  /// everything AquaType learns and every context it reads, in every application, with nothing to show for it.  So it
  /// counts only when the process holding it is this client's application, or when that cannot be told.
  func aquaTypeSecureInputHere() -> Bool {
    guard IsSecureEventInputEnabled() else { return false }
    guard let session = CGSessionCopyCurrentDictionary() as? [String: Any],
          let holder = (session["kCGSSessionSecureInputPID"] as? NSNumber)?.int32Value else { return true }
    let pids = Self.aquaTypeSelftestClientPID.map { [$0] }
      ?? (client?.bundleIdentifier()).map { NSRunningApplication.runningApplications(withBundleIdentifier: $0).map(\.processIdentifier) }
      ?? []
    return pids.isEmpty || pids.contains(holder)
  }

  func aquaTypeRecordCommit(raw: String, text: String) {
    // Not while secure input is on here (Terminal's Secure Keyboard Entry): what is typed there is not remembered, as the
    // text around the caret is not read there either.
    guard !raw.isEmpty, !text.isEmpty, aquaTypeHasCJK(text), !aquaTypeCommitUnremembered, !aquaTypeSecureInputHere() else {
      return
    }
    let generation = max(1, aquaTypeV2ResponseGeneration ?? aquaTypeClientSeq)
    let message = AquaTypeV2CommitMessage(
      clientID: aquaTypeV2ClientID,
      eventID: UUID().uuidString.lowercased(),
      clientGeneration: generation,
      rawKeys: raw,
      rawIdentity: Data(raw.utf8).base64EncodedString(),
      selectedText: text,
      timestampUnixSeconds: Int64(Date().timeIntervalSince1970))
    // Only queued for the engine, which keeps it in the personal memory.  Nothing waits for the reply: a commit is
    // never reopened or reordered by what the engine says about it.
    AquaTypeV2JSONLClient.shared.sendCommit(message) { _ in }
  }

  func commit(string: String) {
    guard let client = client else { return }
    let raw = aquaTypeCurrentRawBuffer()
    AquaTypeV2CommitSequencer.perform(
      commit: { client.insertText(string, replacementRange: self.aquaTypeCommitReplacing) },
      enqueuePersistence: { self.aquaTypeRecordCommit(raw: raw, text: string) })
    aquaTypeFinishAssembledSentence(finalText: string, finalKeys: raw)
    preedit = ""
    rimeAPI.set_property(session, "aquatype_raw_input", "")
    rimeAPI.set_property(session, "aquatype_seq_id", "")
    rimeAPI.clear_composition(session)
    aquaTypeClearBridgeState()
    aquaTypeHidePalettes(force: true)
  }

  /// A sentence that took several commits is remembered as one, under the keys it took altogether.  One commit is not
  /// an assembly - it was already recorded when it happened - so nothing is written for it.
  private func aquaTypeFinishAssembledSentence(finalText: String, finalKeys: String) {
    defer { aquaTypeCommitPieces.removeAll() }
    guard !aquaTypeCommitPieces.isEmpty else { return }
    // The record is a text and the keys that produced it, and the two have to describe the same thing.  A final commit
    // with no keys behind it - `commit(string:)` can be reached that way when the composition is torn down from the
    // outside - contributes its characters but no keys, and a row whose text is longer than its key string accounts
    // for is a row that will be recalled for the wrong input.  Leave it out rather than record a mismatch.
    let pieces = finalKeys.isEmpty
      ? aquaTypeCommitPieces
      : aquaTypeCommitPieces + [(text: finalText, keys: finalKeys)]
    // Fewer than two pieces is not an assembly: that one piece was already recorded when it was committed.
    guard pieces.count >= 2 else { return }
    let text = pieces.map(\.text).joined()
    let keys = pieces.map(\.keys).joined()
    // The same bounds the contract enforces, checked here so a long sentence is dropped rather than rejected.
    guard text.count > 1, text.count <= Self.aquaTypeMaxAssembledCharacters,
          text.utf8.count <= 256, !keys.isEmpty, keys.utf8.count <= 256 else { return }
    aquaTypeRecordCommit(raw: keys, text: text)
  }

  func show(preedit: String, selRange: NSRange, caretPos: Int) {
    guard let client = client else { return }
    if self.preedit == preedit && self.caretPos == caretPos && self.selRange == selRange {
      return
    }

    self.preedit = preedit
    self.caretPos = caretPos
    self.selRange = selRange

    // Clearing passes a caret at 0 of the empty string, as upstream Squirrel and McBopomofo do.  NSNotFound reaches a
    // Chromium page unchecked when the clear comes outside a key press, where it becomes -1 and puts the caret one
    // character before where the composition began.
    if preedit.isEmpty {
      client.setMarkedText(NSAttributedString(string: ""),
                           selectionRange: NSRange(location: 0, length: 0),
                           replacementRange: .empty)
      return
    }

    // The selection and the caret come from librime, which describes the composition this string was built from a
    // moment earlier rather than this string itself, and NSRange.empty is NSNotFound.  Either can land out of range,
    // and `setAttributes` would raise and take the input method down, so both are clamped into the string.  A caret at
    // `length` is legitimate: it sits after the last character.
    let length = preedit.utf16.count
    let start = selRange.location == NSNotFound ? 0 : min(max(0, selRange.location), length)
    let caret = min(max(0, caretPos), length)
    let attrString = NSMutableAttributedString(string: preedit)
    if start > 0 {
      let attrs = mark(forStyle: kTSMHiliteConvertedText, at: NSRange(location: 0, length: start))! as! [NSAttributedString.Key: Any]
      attrString.setAttributes(attrs, range: NSRange(location: 0, length: start))
    }
    let remainingRange = NSRange(location: start, length: length - start)
    let attrs = mark(forStyle: kTSMHiliteSelectedRawText, at: remainingRange)! as! [NSAttributedString.Key: Any]
    attrString.setAttributes(attrs, range: remainingRange)
    client.setMarkedText(attrString, selectionRange: NSRange(location: caret, length: 0), replacementRange: .empty)
    // Where the echo starts is asked after the key has been handled, once for however many keys came meanwhile.
    // Setting the marked text does not wait for the application; a question right after it does, until the
    // application has laid the new text out, so every keystroke waited for the application to draw it.
    if !aquaTypeEchoStartAsking {
      aquaTypeEchoStartAsking = true
      DispatchQueue.main.async { [weak self] in
        guard let self else { return }
        self.aquaTypeEchoStartAsking = false
        guard let client = self.client, !self.preedit.isEmpty else { return }
        let marked = client.markedRange()
        self.aquaTypeEchoStart = marked.location == NSNotFound ? nil : marked.location
      }
    }
  }

  /// The composition's lines on screen: from the top of the line its first character is on to the bottom of the line
  /// its last character is on, at `reference`'s x.  Nil when the application does not answer sensibly: not every
  /// application implements `firstRect` with care, so an empty rect, a last line above the first, or lines far from
  /// `reference` are refused and the panel keeps its place.
  func aquaTypeCompositionLines(_ client: IMKTextInput, near reference: NSRect) -> NSRect? {
    let marked = client.markedRange()
    guard marked.location != NSNotFound, marked.length > 0 else { return nil }
    let first = client.firstRect(forCharacterRange: NSRange(location: marked.location, length: 1), actualRange: nil)
    let last = client.firstRect(forCharacterRange: NSRange(location: marked.location + marked.length - 1, length: 1),
                                actualRange: nil)
    let line = max(reference.height, 16)
    guard first.height > 1, last.height > 1, last.minY <= first.minY + 1,
          first.maxY - last.minY <= line * 20,
          abs(first.maxY - reference.maxY) <= line * 20 else { return nil }
    return NSRect(x: reference.minX, y: last.minY, width: reference.width, height: first.maxY - last.minY)
  }

  /// Ask the application again a moment after a render.  Applications that lay out asynchronously - Chromium and
  /// everything built on it, VS Code included - answer a query made right after the composition changed from their
  /// previous layout: the newest characters are not placed yet, so the composition's last line comes back empty and a
  /// field that has just grown a line is not seen.  Typing on only repeats that, and the panel stayed over a chat box
  /// that had grown until some other key happened to ask once the layout had caught up.  So the caret is read again
  /// shortly after each render, and twice in case the first is still early; only an answer that would move the panel
  /// causes a redraw, through the same rules a render applies.
  func aquaTypeScheduleCaretRecheck(raw: String) {
    aquaTypePanelRecheckToken &+= 1
    let token = aquaTypePanelRecheckToken
    for delay in [0.08, 0.25] {
      DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
        guard let self, token == self.aquaTypePanelRecheckToken, let client = self.client,
              raw == self.aquaTypeCurrentRawBuffer(), raw == self.aquaTypeLastPanelRaw,
              let anchor = self.aquaTypePanelAnchorRect else { return }
        var reported = NSRect()
        client.attributes(forCharacterIndex: 0, lineHeightRectangle: &reported)
        let halfLine = max(Self.aquaTypeIsUsableCaret(reported) ? reported.height : 0, 16) / 2
        let lines = self.aquaTypeCompositionLines(client, near: anchor)
        let linesMoved = lines.map { abs($0.maxY - anchor.maxY) >= halfLine || abs($0.minY - anchor.minY) >= halfLine } ?? false
        let caretMoved = Self.aquaTypeIsUsableCaret(reported)
          && self.aquaTypePanelAnchorReading.map { abs(reported.midY - $0.midY) >= halfLine } == true
        guard linesMoved || caretMoved else { return }
        _ = self.aquaTypePanelRenderCurrent(raw: raw)
      }
    }
  }

  // swiftlint:disable:next function_parameter_count
  func showPanel(preedit: String, selRange: NSRange, caretPos: Int, candidates: [String], comments: [String], labels: [String], highlighted: Int, page: Int, lastPage: Bool, candidateIndexMap: [Int] = []) {
    guard let client = client else { return }
    var reported = NSRect()
    client.attributes(forCharacterIndex: 0, lineHeightRectangle: &reported)
    let usable = Self.aquaTypeIsUsableCaret(reported)
    // The anchor is taken from the composition's first usable caret report, and its x is held for the rest of it: in
    // some applications the reported caret moves right as the user types, and a panel that followed it would move on
    // every keystroke.  A report that is not usable (aquaTypeIsUsableCaret) is never taken; with no anchor yet the
    // panel stays where it was rather than going wherever such a report points.
    //
    // The caret is still asked for on every render, because the first answer can be a stale one: a web page moves the
    // hidden input box it takes text through to the caret a moment after the composition starts, and until then it
    // reports the box's old place.  So a report on a different line from the one the anchor came from, made twice in
    // a row, replaces the anchor.  Twice, so that one odd answer in the middle of a sentence does not move the panel.
    //
    // The anchor's height follows the composition's lines, because some fields grow as the text wraps: a chat box
    // grows upward and pushes the first line into the space above it, where the panel sits.  The lines are re-read on
    // every render, but the anchor moves only when they change by half a line or more, so the panel moves only when a
    // line is added or removed, together with the text.
    let raw = aquaTypeCurrentRawBuffer()
    let halfLine = max(usable ? reported.height : 0, 16) / 2
    var position: NSRect? = usable ? reported : nil
    var linesNow: NSRect? = nil                                         // the composition's lines, as asked below
    if raw.isEmpty {
      aquaTypePanelAnchorRect = nil
    } else {
      if usable, let from = aquaTypePanelAnchorReading, abs(reported.midY - from.midY) >= halfLine {
        if let seen = aquaTypePanelOffLineReading, abs(reported.midY - seen.midY) < halfLine {
          aquaTypePanelAnchorRect = nil                             // taken again from this report, below
          NSApp.squirrelAppDelegate.panel?.forgetAboveOrBelow()
        } else {
          aquaTypePanelOffLineReading = reported
        }
      } else if usable {
        aquaTypePanelOffLineReading = nil
      }
      if var anchor = aquaTypePanelAnchorRect {
        linesNow = aquaTypeCompositionLines(client, near: anchor)
        if let lines = linesNow,
           abs(lines.maxY - anchor.maxY) >= halfLine || abs(lines.minY - anchor.minY) >= halfLine {
          anchor.origin.y = lines.minY
          anchor.size.height = lines.height
          aquaTypePanelAnchorRect = anchor
        }
        position = anchor
      } else if usable {
        var anchor = reported
        linesNow = aquaTypeCompositionLines(client, near: reported)
        if let lines = linesNow {
          anchor = NSRect(x: reported.minX, y: lines.minY, width: reported.width, height: lines.height)
        }
        aquaTypePanelAnchorRect = anchor
        aquaTypePanelAnchorReading = reported
        position = anchor
      }
    }
    if !raw.isEmpty { aquaTypeScheduleCaretRecheck(raw: raw) }
    // Everything the composition is reported to occupy - the position, the caret, its lines - for the panel to keep
    // clear of.  Nothing more is asked for: these are the answers this render already has.  A report more than three
    // lines from the position is a stale one, left out rather than allowed to stretch the area across the screen.
    var avoid = position
    if let anchor = position, !raw.isEmpty {
      let near = 3 * max(anchor.height, reported.height, 16)
      if usable, abs(reported.midY - anchor.midY) <= near { avoid = anchor.union(reported) }
      if let lines = linesNow, abs(lines.midY - anchor.midY) <= near + lines.height { avoid = (avoid ?? anchor).union(lines) }
    }
    if let panel = NSApp.squirrelAppDelegate.panel {
      if let position { panel.position = position }
      panel.avoidRect = raw.isEmpty ? nil : avoid
      panel.inputController = self
      // The claim is held for as long as there is something being composed, not just for as long as this particular
      // render had rows.  A render that momentarily comes back empty must not hand the panel back to `orderOut`
      // mid-sentence; the composition-end paths all release it explicitly, and `hide(force:)` still overrides it.
      panel.aquaTypeCompositionPanelVisible = !candidates.isEmpty || !aquaTypeCurrentRawBuffer().isEmpty
      panel.update(preedit: preedit, selRange: selRange, caretPos: caretPos, candidates: candidates, comments: comments, labels: labels,
                   highlighted: highlighted, page: page, lastPage: lastPage, update: true, candidateIndexMap: candidateIndexMap)
    }
  }
}
