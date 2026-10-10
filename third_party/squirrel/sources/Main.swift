//
//  Main.swift
//  Squirrel
//
//  Created by Leo Liu on 5/10/24.
//  Modified for AquaType by Dio Hsieh, 2026.
//

import Foundation
import InputMethodKit
import Darwin

/// A text client that records what the input method sends it.
///
/// `IMKInputController` reaches the document it is typing into only through `IMKTextInput`, so anything that conforms
/// to that protocol is a complete stand-in for a text field.  `--type-selftest` hands the real controller one of these
/// and reads back what a real text field would have shown.
final class SelftestTextClient: NSObject, IMKTextInput {
  private(set) var committed = ""
  private(set) var marked = ""
  /// Every update the input method pushed into the document, in order, with the time it arrived: what the user sees
  /// change while typing.
  private(set) var updates: [(at: TimeInterval, marked: String, committed: String)] = []
  var recordUpdates = false

  func reset() {
    committed = ""; marked = ""; caretFromEnd = 0; reportsLength = true; updates.removeAll(); markedHistory = [(0, 0)]; positionAtCompositionStart = nil
  }

  private func record() {
    guard recordUpdates else { return }
    updates.append((ProcessInfo.processInfo.systemUptime, marked, committed))
  }

  private func plain(_ string: Any!) -> String {
    if let attributed = string as? NSAttributedString { return attributed.string }
    if let text = string as? String { return text }
    return ""
  }

  func insertText(_ string: Any!, replacementRange: NSRange) {
    if replacementRange.location != NSNotFound,
       let range = Range(replacementRange, in: committed) {
      committed.replaceSubrange(range, with: plain(string))
    } else {
      committed += plain(string)
    }
    marked = ""
    record()
  }

  /// What Chromium and Firefox do on a mouse click during a composition: keep what the field shows as typed text and
  /// end the composition, before the input method hears of it.
  func takeMarkedAsTyped() {
    committed += marked
    marked = ""
    record()
  }

  /// Text typed by the person outside any composition.
  func type(_ text: String) {
    committed += text
    record()
  }

  func setMarkedText(_ string: Any!, selectionRange: NSRange, replacementRange: NSRange) {
    let began = marked.isEmpty
    marked = plain(string)
    if marked.isEmpty { positionAtCompositionStart = nil }
    else if began { positionAtCompositionStart = rect(forUnit: committed.utf16.count) }
    markedHistory.append((ProcessInfo.processInfo.systemUptime, marked.utf16.count))
    record()
  }

  /// How far before the end of the text the caret is: 0 but where a test has moved it, as a click does.
  var caretFromEnd = 0
  func selectedRange() -> NSRange { NSRange(location: max(0, committed.utf16.count - caretFromEnd), length: 0) }

  /// Text changed by the application on its own, as an editor might after it kept a composition.
  func replaceAll(with text: String) {
    committed = text
    record()
  }

  /// How often the input method has asked where the marked text is: each question is a round trip to a real application.
  private(set) var markedRangeCalls = 0
  func markedRange() -> NSRange {
    markedRangeCalls += 1
    return marked.isEmpty ? NSRange(location: NSNotFound, length: 0)
                   : NSRange(location: committed.utf16.count, length: marked.utf16.count)
  }

  /// False for an application without document access, which IMK reports as a length of NSNotFound.
  var reportsLength = true
  func length() -> Int { reportsLength ? committed.utf16.count : NSNotFound }

  func attributedSubstring(from range: NSRange) -> NSAttributedString! {
    let all = committed + marked
    let utf16 = Array(all.utf16)
    guard range.location != NSNotFound, range.location >= 0,
          range.location + range.length <= utf16.count else { return NSAttributedString(string: "") }
    return NSAttributedString(string: String(decoding: utf16[range.location..<(range.location + range.length)], as: UTF16.self))
  }

  func characterIndex(for point: NSPoint, tracking: IMKLocationToOffsetMappingMode, inMarkedRange: UnsafeMutablePointer<ObjCBool>!) -> Int {
    inMarkedRange?.pointee = false
    return NSNotFound
  }

  /// How the text is laid out.  Nil keeps it on one line.  A number wraps it every that many UTF-16 units, and
  /// `growsUpward` then keeps the last line where it is and pushes the earlier ones up, the way a chat box grows as a
  /// message wraps.
  var lineUnits: Int? = nil
  var growsUpward = false

  /// A field that lays out after the fact, as Chromium does: layout answers describe the composition as it was this
  /// long ago, and a character typed since has no place yet (an empty rect).
  var layoutLag: TimeInterval? = nil
  /// Report the caret's line rather than the composition's start.
  var reportsCaretLine = false
  /// VS Code's editor fields: one position, taken when the composition begins and not updated while it grows, and an
  /// invalid rectangle for every character.  Beginning the composition again (setting it empty first) refreshes it.
  var reportsLikeVSCode = false
  private var positionAtCompositionStart: NSRect? = nil
  private var markedHistory: [(at: TimeInterval, count: Int)] = [(0, 0)]
  private var laidOutMarkedCount: Int? {
    guard let layoutLag else { return nil }
    let now = ProcessInfo.processInfo.systemUptime
    return markedHistory.last(where: { now - $0.at >= layoutLag })?.count ?? 0
  }

  /// Where the character at document index `index` is drawn, in screen coordinates (y up), eighteen points a line.
  /// Where the field is and how tall its lines are.  The defaults are the layout the other cases were written against.
  var originX: CGFloat = 300
  var baseY: CGFloat? = nil
  var lineHeight: CGFloat = 18

  func rect(forUnit index: Int) -> NSRect {
    guard let width = lineUnits else {
      return NSRect(x: originX + CGFloat(index) * 8, y: baseY ?? 500, width: 8, height: lineHeight)
    }
    let markedLaidOut = laidOutMarkedCount ?? marked.utf16.count
    if index >= committed.utf16.count + markedLaidOut, laidOutMarkedCount != nil { return .zero }
    let lines = max(1, (committed.utf16.count + markedLaidOut + width - 1) / width)
    let line = index / width
    let y0 = baseY ?? 300
    let y = growsUpward ? y0 + CGFloat(lines - 1 - line) * lineHeight : y0 - CGFloat(line) * lineHeight
    return NSRect(x: originX + CGFloat(index % width) * 8, y: y, width: 8, height: lineHeight)
  }

  /// Caret reports that are not the caret, as some applications give them: `caretReportsFirst` answers the next queries
  /// one each before the real caret takes over (a web page whose hidden input box is still at its old place), and
  /// `caretReportAlways` answers every query (a window with no text field).
  var caretReportsFirst: [NSRect] = []
  var caretReportAlways: NSRect? = nil
  /// Reports that wander by a point or two from one query to the next, up and down and in height, as real applications'
  /// do while a composition grows.
  var wanders = false
  private var wanderStep = 0
  private func wandered(_ rect: NSRect) -> NSRect {
    guard wanders, rect.height > 0 else { return rect }
    let offsets: [CGFloat] = [0, 1, -1, 2, -2, 1, 0, -1]
    wanderStep += 1
    var r = rect
    r.origin.y += offsets[wanderStep % offsets.count]
    r.size.height += offsets[(wanderStep + 3) % offsets.count] < 0 ? 0 : offsets[(wanderStep + 3) % offsets.count]
    return r
  }

  /// Where the caret is, on screen.  A real text field moves it along as characters arrive and the panel is placed
  /// against it, so the caret advances here too: eight points a character, about right for the preedit at this size.
  /// A wrapping layout answers with the start of the composition instead.
  func attributes(forCharacterIndex index: Int, lineHeightRectangle lineRect: UnsafeMutablePointer<NSRect>!) -> [AnyHashable: Any]! {
    if reportsLikeVSCode, let start = positionAtCompositionStart {
      lineRect?.pointee = NSRect(x: start.minX, y: start.minY, width: 1, height: start.height)
      return [:]
    }
    if let report = caretReportAlways ?? (caretReportsFirst.isEmpty ? nil : caretReportsFirst.removeFirst()) {
      lineRect?.pointee = report
      return [:]
    }
    if lineUnits != nil {
      // VS Code reports the line the caret is on, which in a box that grows upward stays where it is.
      let start = reportsCaretLine
        ? rect(forUnit: committed.utf16.count + max(0, (laidOutMarkedCount ?? marked.utf16.count) - 1))
        : rect(forUnit: committed.utf16.count)
      lineRect?.pointee = NSRect(x: start.minX, y: start.minY, width: 1, height: start.height)
    } else {
      let advance = CGFloat((committed.count + marked.count)) * 8
      lineRect?.pointee = wandered(NSRect(x: originX + advance, y: baseY ?? 500, width: 1, height: lineHeight))
    }
    return [:]
  }

  func string(from range: NSRange, actualRange: NSRangePointer?) -> String? {
    let text = attributedSubstring(from: range)?.string ?? ""
    actualRange?.pointee = NSRange(location: range.location, length: text.utf16.count)
    return text
  }

  func firstRect(forCharacterRange range: NSRange, actualRange: NSRangePointer?) -> NSRect {
    if reportsLikeVSCode { return NSRect(x: 1.6e-314, y: 956320.9, width: 1.6e-314, height: -1) }   // what VS Code answered
    actualRange?.pointee = range
    return wandered(rect(forUnit: range.location))
  }

  func validAttributesForMarkedText() -> [Any]! { [] }
  func overrideKeyboard(withKeyboardNamed keyboardUniqueName: String!) {}
  func selectMode(_ modeIdentifier: String!) {}
  func supportsUnicode() -> Bool { true }
  /// The application being typed into, which decides the per-app options (English by default, Vim mode).
  var appBundleID = "com.apple.TextEdit"
  func bundleIdentifier() -> String! { appBundleID }
  func windowLevel() -> Int32 { 0 }
  func supportsProperty(_ property: TSMDocumentPropertyTag) -> Bool { false }
  func uniqueClientIdentifierString() -> String! { "aquatype-type-selftest" }
}

/// Looks at the candidate panel the whole time a self-test types, the way someone watching the screen would: every
/// time the panel shows something new, it is drawn and the drawing is read.  What is checked is what can be seen, not
/// the layout that produced it: the text has to sit in the panel with as much room after it as before it - a panel
/// wider or taller than its text, with blank space on one side, is wrong however it came about - and none of the text
/// may be cut off at an edge.
final class SelftestPanelWatcher {
  struct Look {
    /// Blank space between the text and each edge of the panel, in points.
    var left: CGFloat, right: CGFloat, top: CGFloat, bottom: CGFloat
  }

  private(set) var looked = 0
  private(set) var problems: [String] = []
  /// The most blank space seen after the text compared with before it, across and down.
  private(set) var worstExtraRight: CGFloat = 0
  private(set) var worstExtraBottom: CGFloat = 0
  private var timer: Timer?
  private var lastSeen = ""
  /// Room after the text beyond the room before it; more than this is a panel bigger than what it shows.
  private let allowance: CGFloat

  init(allowance: CGFloat) { self.allowance = allowance }

  func start(_ panel: @escaping () -> SquirrelPanel?) {
    timer = Timer.scheduledTimer(withTimeInterval: 0.005, repeats: true) { [weak self] _ in
      guard let self, let panel = panel(), panel.isVisible else { return }
      let seen = "\(NSStringFromRect(panel.frame))|\(panel.aquaTypeSelftestShownText)"
      guard seen != self.lastSeen else { return }
      self.lastSeen = seen
      let shown = panel.aquaTypeSelftestShownText.replacingOccurrences(of: "\n", with: " ").prefix(40)
      // The panel is drawn into a bitmap here, synchronously, so a panel with text to show that comes out with no ink
      // at all was drawn blank.
      guard let look = Self.look(at: panel) else {
        if !shown.trimmingCharacters(in: .whitespaces).isEmpty {
          self.looked += 1
          self.problems.append("a \(Int(panel.frame.width))x\(Int(panel.frame.height)) panel showing \"\(shown)\" was drawn blank")
        }
        return
      }
      self.looked += 1
      self.worstExtraRight = max(self.worstExtraRight, look.right - look.left)
      self.worstExtraBottom = max(self.worstExtraBottom, look.bottom - look.top)
      if min(look.left, look.right, look.top, look.bottom) < 1 {
        self.problems.append("text cut off at an edge of a \(Int(panel.frame.width))x\(Int(panel.frame.height)) panel "
                             + "showing \"\(shown)\" (room left \(Int(look.left)), right \(Int(look.right)), "
                             + "top \(Int(look.top)), bottom \(Int(look.bottom)))")
      } else if look.right - look.left > self.allowance || look.bottom - look.top > self.allowance {
        self.problems.append("blank space in a \(Int(panel.frame.width))x\(Int(panel.frame.height)) panel showing "
                             + "\"\(shown)\": room left \(Int(look.left)) and right \(Int(look.right)), "
                             + "top \(Int(look.top)) and bottom \(Int(look.bottom))")
      }
    }
    RunLoop.current.add(timer!, forMode: .common)
  }

  func stop() { timer?.invalidate(); timer = nil }

  /// The panel drawn as it is on the screen, and where its text is.  Each row is filled with the panel's background or,
  /// on the highlighted candidate, with the highlight; text is whatever differs from the fill it is drawn on.  The
  /// rounded ends of the highlight are not text, so a band of a few points at each end of it is not read.
  static func look(at panel: SquirrelPanel) -> Look? {
    guard let content = panel.contentView else { return nil }
    let bounds = content.bounds
    guard bounds.width >= 4, bounds.height >= 4,
          let rep = content.bitmapImageRepForCachingDisplay(in: bounds) else { return nil }
    content.cacheDisplay(in: bounds, to: rep)
    guard let data = rep.bitmapData, rep.bitsPerSample == 8, rep.samplesPerPixel == 4 else { return nil }
    let w = rep.pixelsWide, h = rep.pixelsHigh, stride = rep.bytesPerRow
    let alphaFirst = rep.bitmapFormat.contains(.alphaFirst)
    let scale = CGFloat(w) / bounds.width
    @inline(__always) func pixel(_ x: Int, _ y: Int) -> (r: Int, g: Int, b: Int, a: Int) {
      let p = data + y * stride + x * 4
      return alphaFirst ? (Int(p[1]), Int(p[2]), Int(p[3]), Int(p[0])) : (Int(p[0]), Int(p[1]), Int(p[2]), Int(p[3]))
    }
    @inline(__always) func near(_ c: (r: Int, g: Int, b: Int, a: Int), _ k: (Int, Int, Int)) -> Bool {
      abs(c.r - k.0) <= 40 && abs(c.g - k.1) <= 40 && abs(c.b - k.2) <= 40
    }
    // The two fills: the background is the commonest colour, the highlight the commonest of those far from it.
    var counts: [Int: Int] = [:]
    for y in Swift.stride(from: 0, to: h, by: 2) {
      for x in Swift.stride(from: 0, to: w, by: 2) {
        let c = pixel(x, y)
        if c.a >= 200 { counts[(c.r >> 4) << 8 | (c.g >> 4) << 4 | (c.b >> 4), default: 0] += 1 }
      }
    }
    func colour(_ key: Int) -> (Int, Int, Int) { (((key >> 8) & 15) * 16 + 8, ((key >> 4) & 15) * 16 + 8, (key & 15) * 16 + 8) }
    guard let bgKey = counts.max(by: { $0.value < $1.value })?.key else { return nil }
    let bg = colour(bgKey)
    let total = counts.values.reduce(0, +)
    let hlKey = counts.filter { entry in
      let c = colour(entry.key)
      return max(abs(c.0 - bg.0), abs(c.1 - bg.1), abs(c.2 - bg.2)) > 80 && entry.value * 50 > total
    }.max(by: { $0.value < $1.value })?.key
    let hl = hlKey.map(colour)

    let edge = Int((2 * scale).rounded(.up))          // the panel's own outline and rounded corners
    let bandEnd = Int((4 * scale).rounded(.up))       // the highlight's rounded ends
    var x0 = Int.max, x1 = -1, y0 = Int.max, y1 = -1
    for y in edge..<(h - edge) {
      var bandFrom = -1, bandTo = -1
      if let hl {
        var n = 0
        for x in 0..<w where near(pixel(x, y), hl) { if bandFrom < 0 { bandFrom = x }; bandTo = x; n += 1 }
        if n * 10 < w { bandFrom = -1 }
      }
      for x in edge..<(w - edge) {
        let c = pixel(x, y)
        guard c.a >= 200 else { continue }
        let ink: Bool
        if bandFrom >= 0, let hl, x >= bandFrom - bandEnd, x <= bandTo + bandEnd {
          ink = x > bandFrom + bandEnd && x < bandTo - bandEnd && !near(c, hl)
        } else {
          ink = !near(c, bg)
        }
        if ink { x0 = min(x0, x); x1 = max(x1, x); y0 = min(y0, y); y1 = max(y1, y) }
      }
    }
    guard x1 >= 0 else { return nil }
    return Look(left: CGFloat(x0) / scale, right: CGFloat(w - 1 - x1) / scale,
                top: CGFloat(y0) / scale, bottom: CGFloat(h - 1 - y1) / scale)
  }
}

@main
struct SquirrelApp {
  /// The user's home directory (e.g. /Users/name); every AquaType path is built from it.
  ///
  /// Read from the system's user database rather than `NSHomeDirectory()`: in a sandboxed app the latter returns a
  /// private container folder instead, and the files the installer put in ~/.aquatype would not be found there.
  static let homeDir: URL = {
    if let pwuid = getpwuid(getuid()) {
      return URL(fileURLWithFileSystemRepresentation: pwuid.pointee.pw_dir, isDirectory: true, relativeTo: nil)
    }
    return URL(fileURLWithPath: NSHomeDirectory())
  }()
  static let userDir = homeDir.appending(components: ".aquatype", "rime")
  static let logDir = homeDir.appending(components: ".aquatype", "logs", "rime")

  // swiftlint:disable:next cyclomatic_complexity
  static func main() {
    // The engine is a child process fed through a pipe.  If it dies, the next write to that pipe would raise SIGPIPE,
    // whose default action ends this process - the input method, with whatever was being typed, or a self-test that
    // kills the engine on purpose.  Ignored, the write fails with EPIPE instead, which the client handles by starting
    // the engine again.
    signal(SIGPIPE, SIG_IGN)
    let rimeAPI: RimeApi_stdbool = rime_get_api_stdbool().pointee

    let handled = autoreleasepool {
      let args = CommandLine.arguments
      if args.count > 1 {
        switch args[1] {
        case "--quit":
          if let bundleId = Bundle.main.bundleIdentifier {
            NSRunningApplication.runningApplications(withBundleIdentifier: bundleId).forEach { $0.terminate() }
          }
          return true
        case "--reload":
          DistributedNotificationCenter.default().postNotificationName(.init("AquaTypeReloadNotification"), object: nil)
          return true
        case "--print-user-dir":
          print(SquirrelApp.userDir.path())
          return true
        case "--print-shared-support-dir":
          print(Bundle.main.sharedSupportPath ?? "")
          return true
        case "--build":
          let defaultDataDir = FileManager.default.currentDirectoryPath
          let userDataDir = args.count > 2 ? args[2] : defaultDataDir
          let sharedDataDir = args.count > 3 ? args[3] : defaultDataDir
          let stagingDir = args.count > 4 ? args[4] : URL(fileURLWithPath: userDataDir).appendingPathComponent("build").path

          var builderTraits = RimeTraits.rimeStructInit()
          builderTraits.setCString(sharedDataDir, to: \.shared_data_dir)
          builderTraits.setCString(userDataDir, to: \.user_data_dir)
          builderTraits.setCString(stagingDir, to: \.staging_dir)
          builderTraits.setCString("AquaType", to: \.distribution_code_name)
          builderTraits.setCString("AquaType", to: \.distribution_name)
          builderTraits.setCString("rime.squirrel-builder", to: \.app_name)
          // Errors only, and to stderr (an empty log_dir), where the installer's log picks them up; left alone, glog
          // writes INFO and WARNING files into the temporary folder on every build.
          builderTraits.setCString("", to: \.log_dir)
          builderTraits.min_log_level = 2
          rimeAPI.setup(&builderTraits)
          rimeAPI.deployer_initialize(nil)
          let deployOK = rimeAPI.deploy()
          let squirrelConfigOK = rimeAPI.deploy_config_file("squirrel.yaml", "config_version")
          if !deployOK || !squirrelConfigOK {
            fputs("AquaType build failed: deploy=\(deployOK) squirrel_config=\(squirrelConfigOK)\n", stderr)
            Darwin.exit(1)
          }
          return true
        case "--typing-smoothness":
          // What the panel does while a sentence is typed at human speed, sampled fast enough to see it.
          //
          // Every keystroke redraws the panel in two parts: the bopomofo echo lands immediately and the candidates
          // arrive tens of milliseconds later, each with its own width and height.  A window that resizes twice per
          // keystroke looks like stuttering even when every answer is fast, so this counts the changes.
          Darwin.exit(typingSmoothness(userDir: args.count > 2 ? args[2] : SquirrelApp.userDir.path()) ? 0 : 1)
        case "--type-selftest":
          // Typing, end to end, with nobody at the keyboard: a stub text client, the real SquirrelInputController, the
          // real Rime session it creates, and the real AquaTypeEngine child process it spawns out of this bundle.
          // packaging/build_app.sh runs it, so a package in which pressing a key produces nothing cannot be built.
          // An optional third argument runs a part of it, so the parts can run side by side: "main" is everything but
          // the field matrix, "matrix:<i>/<n>" the i-th of n parts of the field matrix.
          Darwin.exit(typeSelftest(userDir: args.count > 2 ? args[2] : SquirrelApp.userDir.path(),
                                   part: args.count > 3 ? args[3] : "all") ? 0 : 1)
        case "--render-selftest":
          // The candidate panel, driven the way SquirrelInputController drives it, with no input method client and
          // nobody watching: open the deployed configuration, build the panel, hand it pages of candidates, let
          // AppKit lay them out, draw the result and look at what came out.
          //
          // The optional argument is the Rime user directory to read the deployed squirrel.yaml from; without it the
          // one this build uses.  It prints `render-selftest ok` as its last line only when every case ran and passed;
          // packaging/build_app.sh checks for that line.
          Darwin.exit(renderSelftest(userDir: args.count > 2 ? args[2] : SquirrelApp.userDir.path()) ? 0 : 1)
        case "--help":
          print(helpDoc)
          return true
        default:
          // An unknown argument is a mistake, not a request to start the input method server; starting it anyway
          // would make a typo look like it had worked.
          fputs("Unknown argument: \(args[1])\n\(helpDoc)\n", stderr)
          Darwin.exit(2)
        }
      }
      return false
    }
    if handled {
      return
    }

    autoreleasepool {
      // find the bundle identifier and then initialize the input method server
      let main = Bundle.main
      guard let connectionName = main.object(forInfoDictionaryKey: "InputMethodConnectionName") as? String,
            let bundleIdentifier = main.bundleIdentifier else {
        fputs("Info.plist is missing InputMethodConnectionName or CFBundleIdentifier; cannot start the input method "
              + "server.\n", stderr)
        Darwin.exit(1)
      }
      // The input method server.  Every app the user types into talks to AquaType through it, so it must live until
      // the app quits; `withExtendedLifetime` below guarantees that.
      let server = IMKServer(name: connectionName, bundleIdentifier: bundleIdentifier)
      // load the bundle explicitly because in this case the input method is a
      // background only application
      let app = NSApplication.shared
      let delegate = SquirrelApplicationDelegate()
      app.delegate = delegate
      app.setActivationPolicy(.accessory)

      // opencc resolves its dictionary paths relative to the working directory, so the working directory has to be
      // SharedSupport.  If it is missing the app still runs, but says why conversion between character sets will
      // not work.
      if let sharedSupport = main.sharedSupportPath {
        FileManager.default.changeCurrentDirectoryPath(sharedSupport)
      } else {
        fputs("The bundle has no SharedSupport directory; opencc cannot resolve its dictionaries and conversion "
              + "between character sets will not work.\n", stderr)
      }

      if NSApp.squirrelAppDelegate.problematicLaunchDetected() {
        print("Problematic launch detected!")
        SquirrelApplicationDelegate.showMessage(msgText: NSLocalizedString("launch_failure", comment: ""))
      } else {
        NSApp.squirrelAppDelegate.setupRime()
        NSApp.squirrelAppDelegate.startRime(fullCheck: false)
        NSApp.squirrelAppDelegate.loadSettings()
        print("AquaType reporting!")
      }

      // finally run everything
      withExtendedLifetime(server) {
        app.run()
      }
      print("AquaType is quitting...")
      rimeAPI.finalize()
    }
    return
  }

  // MARK: - typing

  private static func keyDown(_ characters: String,
                              keyCode: UInt16 = 0xFFFF,
                              flags: NSEvent.ModifierFlags = []) -> NSEvent {
    // Bopomofo keys are recognised by their character, so they get a keycode no real key has (0xFFFF); named keys
    // such as Return and Delete below pass their real keycode.
    NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags,
                     timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: 0, context: nil,
                     characters: characters, charactersIgnoringModifiers: characters,
                     isARepeat: false, keyCode: keyCode)!
  }

  private static let keyReturn = keyDown("\r", keyCode: 36)
  private static let keyDelete = keyDown("\u{8}", keyCode: 51)
  private static let keyEscape = keyDown("\u{1b}", keyCode: 53)
  private static let keyDownArrow = keyDown("\u{f701}", keyCode: 125)

  /// Runs the run loop for a while, because the decoder's answer does not arrive on the keystroke that asked for it.
  private static func settle(_ seconds: TimeInterval) {
    let deadline = Date().addingTimeInterval(seconds)
    while Date() < deadline { RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.02)) }
  }

  private static func waitUntil(_ timeout: TimeInterval, _ condition: () -> Bool) -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
      if condition() { return true }
      RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.02))
    }
    return condition()
  }

  private static func isHan(_ text: String) -> Bool {
    !text.isEmpty && text.unicodeScalars.allSatisfy { (0x3400...0x9FFF).contains(Int($0.value)) || (0x20000...0x2FA1F).contains(Int($0.value)) }
  }

  /// Types a sentence at human speed and reports how much the candidate panel moves while it happens.
  static func typingSmoothness(userDir: String) -> Bool {
    // Milliseconds between keys; AQUATYPE_SMOOTHNESS_PACE_MS tries a faster or slower typist.
    let pace = ProcessInfo.processInfo.environment["AQUATYPE_SMOOTHNESS_PACE_MS"].flatMap(Double.init).map { $0 / 1000 } ?? 0.120
    guard let harness = makeTypingHarness(userDir: userDir, appName: "rime.squirrel-smoothness") else { return false }
    let controller = harness.controller, client = harness.client
    defer {
      controller.deactivateServer(client)
      try? FileManager.default.removeItem(atPath: harness.stateDir)
    }
    guard let panel = NSApp.squirrelAppDelegate.panel else {
      fputs("typing-smoothness: there is no panel\n", stderr)
      return false
    }

    struct Sample { let at: TimeInterval; let key: Int; let size: NSSize; let origin: NSPoint; let visible: Bool; let top: String }
    // A class, so the timer below can keep adding samples to it.
    final class Cursor { var key = 0; var samples: [Sample] = [] }
    let cursor = Cursor()
    let began = ProcessInfo.processInfo.systemUptime
    // 8 ms is about half a frame at 120 Hz, so even a shape the panel takes for a single frame is sampled.
    let sampler = Timer(timeInterval: 0.008, repeats: true) { _ in
      cursor.samples.append(Sample(at: ProcessInfo.processInfo.systemUptime - began, key: cursor.key,
                                   size: panel.frame.size, origin: panel.frame.origin, visible: panel.isVisible,
                                   top: controller.aquaTypeSelftestCandidates.prefix(3).joined(separator: "|")))
    }
    RunLoop.main.add(sampler, forMode: .common)
    defer { sampler.invalidate() }

    client.recordUpdates = true
    // A real sentence at an ordinary brisk pace (120 ms a key).  The decoder usually answers well within that, so
    // stepping seen here comes from the panel, not from waiting for candidates.
    let keys = Array("5j4u4 z96u84 g4 284j/6 ru04")
    var keyTimes: [TimeInterval] = []
    var handleMs: [Double] = []
    for (i, ch) in keys.enumerated() {
      cursor.key = i + 1
      let t0 = ProcessInfo.processInfo.systemUptime
      keyTimes.append(t0)
      _ = controller.handle(keyDown(String(ch)), client: client)
      handleMs.append((ProcessInfo.processInfo.systemUptime - t0) * 1000)
      settle(pace)
    }
    // How long after the last keystroke the panel is answering the keys that are actually there.  While typing the
    // panel is deliberately allowed to run behind; this is the window in which it catches up, and it is also the
    // window in which Return does nothing because there is nothing current to commit.
    let lastKeyAt = ProcessInfo.processInfo.systemUptime
    var caughtUpAfter: Double? = nil
    for _ in 0..<200 {
      if controller.aquaTypeSelftestPanelIsCurrent {
        caughtUpAfter = (ProcessInfo.processInfo.systemUptime - lastKeyAt) * 1000
        break
      }
      settle(0.01)
    }
    settle(0.3)
    print("  caught up after stop : " + (caughtUpAfter.map { String(format: "%.0f ms", $0) } ?? "never within 2 s"))

    // What the text field was shown, per keystroke: the count is how many times the preedit was rewritten for one
    // key, and the strings say whether each rewrite changed anything visible.
    var perKeyUpdates: [Int] = []
    var redundant = 0
    var lastText = ""
    let boundary = keyTimes
    for (i, t) in boundary.enumerated() {
      let next = i + 1 < boundary.count ? boundary[i + 1] : .greatestFiniteMagnitude
      let inWindow = client.updates.filter { $0.at >= t && $0.at < next }
      perKeyUpdates.append(inWindow.count)
      for u in inWindow {
        let text = u.committed + u.marked
        if text == lastText { redundant += 1 }
        lastText = text
      }
    }
    // How long the main thread is inside the input method for one keystroke.  Nothing else can be drawn while it is,
    // so this is the budget the whole interface has per key: past about ten milliseconds the screen stops keeping up
    // with the keyboard and typing reads as stepping rather than as slow.
    let sortedHandle = handleMs.sorted()
    func q(_ f: Double) -> String { String(format: "%.1f", sortedHandle[min(sortedHandle.count - 1, Int(f * Double(sortedHandle.count)))]) }
    // What the panel is SHOWING, which is what the user is reading.  A keystroke that changes nothing on the panel is
    // a keystroke whose feedback has not arrived yet; several of those in a row, then a jump, is the stepping.
    var contentChangesPerKey = [Int](repeating: 0, count: keys.count)
    for (a, b) in zip(cursor.samples, cursor.samples.dropFirst()) where a.top != b.top {
      let k = min(max(b.key - 1, 0), keys.count - 1)
      contentChangesPerKey[k] += 1
    }
    let keysWithNoUpdate = contentChangesPerKey.filter { $0 == 0 }.count
    print("  panel content updates: \(contentChangesPerKey.reduce(0, +)) over \(keys.count) keys;"
          + " \(keysWithNoUpdate) keystrokes changed nothing on the panel")
    print("  per keystroke        : " + contentChangesPerKey.map(String.init).joined(separator: " "))
    print("  main thread per key  : p50 \(q(0.5)) ms  p95 \(q(0.95)) ms  max \(String(format: "%.1f", sortedHandle.last ?? 0)) ms")
    print("  slowest keys (ms)    : " + handleMs.enumerated().sorted { $0.element > $1.element }.prefix(6)
            .map { "key \($0.offset + 1): \(String(format: "%.0f", $0.element))" }.joined(separator: ", "))
    print("  document updates     : \(client.updates.count) for \(keys.count) keys"
          + "  (\(String(format: "%.2f", Double(client.updates.count) / Double(keys.count))) per keystroke,"
          + " \(redundant) of them changed nothing)")
    print("  updates per keystroke: " + perKeyUpdates.map(String.init).joined(separator: " "))

    let samples = cursor.samples
    let shown = samples.filter { $0.visible && $0.size.width > 1 }
    guard !shown.isEmpty else {
      fputs("typing-smoothness: the panel was never visible while typing\n", stderr)
      return false
    }
    var changes: [(TimeInterval, Int, NSSize, NSSize)] = []
    for (a, b) in zip(shown, shown.dropFirst()) where a.size != b.size {
      changes.append((b.at, b.key, a.size, b.size))
    }
    var originChanges = 0
    var originSteps: [CGFloat] = []
    for (a, b) in zip(shown, shown.dropFirst()) where a.origin != b.origin {
      originChanges += 1
      originSteps.append(abs(b.origin.x - a.origin.x))
    }
    let stepSummary = originSteps.isEmpty ? "none"
      : "\(originSteps.count) moves, x step \(Int(originSteps.min()!))..\(Int(originSteps.max()!)) pt"
    print("  panel followed caret : \(stepSummary)")
    var visibilityFlips = 0
    for (a, b) in zip(samples, samples.dropFirst()) where a.visible != b.visible { visibilityFlips += 1 }
    print("  panel appeared/vanished: \(visibilityFlips) times while typing \(keys.count) keys")
    let distinct = Set(shown.map { "\(Int($0.size.width))x\(Int($0.size.height))" })
    print("typing-smoothness: \(keys.count) keys, \(samples.count) samples, panel visible for \(shown.count)")
    print("  distinct panel sizes : \(distinct.count)")
    print("  size changes         : \(changes.count)  (\(String(format: "%.1f", Double(changes.count) / Double(keys.count))) per keystroke)")
    var perKey: [Int: Int] = [:]
    for c in changes { perKey[c.1, default: 0] += 1 }
    let worst = perKey.sorted { $0.value > $1.value }.prefix(5)
    print("  busiest keystrokes   : " + worst.map { "key \($0.key): \($0.value) changes" }.joined(separator: ", "))
    let widths = shown.map { Int($0.size.width) }
    let heights = shown.map { Int($0.size.height) }
    print("  width  \(widths.min()!)..\(widths.max()!)   height \(heights.min()!)..\(heights.max()!)")
    for c in changes.prefix(14) {
      print(String(format: "    %6.3fs key %2d  %.0fx%.0f -> %.0fx%.0f", c.0, c.1, c.2.width, c.2.height, c.3.width, c.3.height))
    }
    return true
  }

  /// Everything a typing test needs standing up: Rime deployed, an application delegate with a real candidate panel,
  /// a redirected state directory so nothing is written into the user's own memory, and a controller attached to a
  /// stub client.  `--type-selftest` and `--typing-smoothness` both run against exactly this.
  private static func makeTypingHarness(userDir: String, appName: String)
    -> (controller: SquirrelInputController, client: SelftestTextClient, stateDir: String)? {
    let rimeAPI: RimeApi_stdbool = rime_get_api_stdbool().pointee
    guard let sharedSupport = Bundle.main.sharedSupportPath else {
      fputs("\(appName): the bundle has no SharedSupport directory\n", stderr)
      return nil
    }
    SquirrelInputController.aquaTypeSelftestIsolated = true
    // Typing here commits real phrases, and commits are remembered, so the state directory points at a temporary
    // folder that is deleted afterwards, and librime's logs go there too.  It has to be set before the controller is
    // built: the engine reads it at launch.
    let stateDir = NSTemporaryDirectory() + "\(appName)-\(ProcessInfo.processInfo.processIdentifier)"
    try? FileManager.default.createDirectory(atPath: stateDir + "/logs", withIntermediateDirectories: true)
    setenv("AQUATYPE_STATE_DIR", stateDir, 1)
    var traits = RimeTraits.rimeStructInit()
    traits.setCString(sharedSupport, to: \.shared_data_dir)
    traits.setCString(userDir, to: \.user_data_dir)
    traits.setCString(stateDir + "/logs", to: \.log_dir)
    traits.setCString("AquaType", to: \.distribution_code_name)
    traits.setCString("AquaType", to: \.distribution_name)
    traits.setCString(appName, to: \.app_name)
    rimeAPI.setup(&traits)
    rimeAPI.initialize(nil)
    _ = rimeAPI.start_maintenance(false)

    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    guard NSScreen.main != nil else {
      fputs("\(appName): no screen - this needs a window server session (run it as the logged-in user).\n", stderr)
      return nil
    }


    let delegate = SquirrelApplicationDelegate()
    app.delegate = delegate
    delegate.installNotificationHandler()
    delegate.applicationWillFinishLaunching(
      Notification(name: NSApplication.willFinishLaunchingNotification, object: app))
    delegate.loadSettings()

    // The stub client is passed to `activateServer` and to every `handle`, not to the initialiser: the initialiser
    // accepts only the system's own client objects, while the controller itself works with any `IMKTextInput`.
    let client = SelftestTextClient()
    guard let controller = SquirrelInputController(server: nil, delegate: nil, client: nil) else {
      fputs("\(appName): the input controller could not be created\n", stderr)
      return nil
    }
    controller.activateServer(client)
    return (controller, client, stateDir)
  }

  /// Types through the real controller and reports what the client received.
  /// `part`: "all", "main" (all but the field matrix) or "matrix:<i>/<n>" (the i-th of n parts of the field matrix).
  static func typeSelftest(userDir: String, part: String = "all") -> Bool {
    guard let harness = makeTypingHarness(userDir: userDir, appName: "rime.squirrel-type-selftest") else { return false }
    let controller = harness.controller, client = harness.client
    defer {
      controller.deactivateServer(client)
      try? FileManager.default.removeItem(atPath: harness.stateDir)
    }
    // Everything below is typed with the panel watched, so every case is also a case of how the panel looks.
    let watcher = SelftestPanelWatcher(allowance: 8)
    watcher.start { NSApp.squirrelAppDelegate.panel }

    var failures = 0
    func fail(_ what: String, _ detail: String) {
      fputs("type-selftest FAIL \(what): \(detail)\n", stderr)
      failures += 1
    }
    func send(_ event: NSEvent, settleFor: TimeInterval = 0.05) {
      _ = controller.handle(event, client: client)
      settle(settleFor)
    }
    func type(_ keys: String) { for ch in keys { send(keyDown(String(ch))) } }
    func clear(_ what: String = "") {
      send(keyEscape, settleFor: 0.15)
      _ = waitUntil(3) { controller.aquaTypeSelftestComposingKeyCount == 0 }
      let left = controller.aquaTypeSelftestComposingKeyCount
      if left > 0 {
        fail("escape\(what.isEmpty ? "" : " (\(what))")",
             "escape left \(left) keys in the composition, so the next thing typed continues the old one")
        controller.aquaTypeSelftestReset()                                // so one failure does not become every later one
      }
      client.reset()
    }

    // The rule, over every kind of field rather than the few met so far.  A composition on one line is never covered:
    // wherever the field is on the screen, however tall its lines, whatever it reports as the caret (VS Code's way
    // included) and however late it lays out, the panel does not overlap a character of it and stays on the screen.  A
    // composition that wraps is not covered either, in every field that says where its lines are.
    func fieldMatrix(part: Int, of parts: Int) {
      clear("before the field matrix")
      let screen = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
      defer {
        client.lineUnits = nil; client.growsUpward = false; client.layoutLag = nil
        client.reportsCaretLine = false; client.reportsLikeVSCode = false; client.originX = 300; client.baseY = nil; client.lineHeight = 18
      }
      // An application that lays out only when the composition is set again is not in the list: it says nothing about
      // a line it has not laid out, and there is nothing to follow until it does.
      enum Timing: String, CaseIterable { case immediate, late }
      var cases = 0, index = -1
      var covered: [String] = []
      for (place, baseY, upward) in [("top", screen.maxY - 50, false), ("middle", screen.midY, true),
                                     ("middle", screen.midY, false), ("bottom", screen.minY + 20, true)] {
        for (wraps, height) in [(false, CGFloat(18)), (false, 36), (true, 18), (true, 36)] {
          for (side, x) in [("left", screen.minX + 10), ("right", screen.maxX - 70)] {
            for report in wraps ? ["start", "caret"] : ["start", "caret", "vscode"] {
              for timing in Timing.allCases {
                index += 1
                if index % parts != part { continue }
                send(keyEscape, settleFor: 0.01)
                _ = waitUntil(3) { controller.aquaTypeSelftestComposingKeyCount == 0 }
                client.reset()
                client.lineUnits = wraps ? 6 : 200; client.growsUpward = upward; client.baseY = baseY; client.lineHeight = height
                client.originX = x; client.reportsCaretLine = report == "caret"; client.reportsLikeVSCode = report == "vscode"
                client.layoutLag = timing == .late ? 0.15 : nil
                // Typed quickly: what is judged is where the panel ends up for the whole composition.  How it follows a
                // composition as it grows is the wrapping field, late layout and wandering reports cases.
                for ch in "5j4u4 5j4u4 5j4" { send(keyDown(String(ch)), settleFor: 0.005) }
                _ = waitUntil(30) { controller.aquaTypeSelftestPanelIsCurrent }
                // Long enough for the second look the panel takes at the field after a render (0.25 s), which is the one
                // that finds a field laid out late.
                settle(timing == .late ? 0.32 : 0.1)
                cases += 1
                client.layoutLag = nil        // judge against the true layout
                let marked = client.markedRange()
                guard let panel = NSApp.squirrelAppDelegate.panel, panel.isVisible,
                      marked.location != NSNotFound, marked.length > 0 else {
                  covered.append("\(place)/\(wraps ? "wrapped" : "one line")/\(Int(height))pt/\(side)/\(report)/\(timing): no panel"); continue
                }
                let chars = (marked.location..<(marked.location + marked.length)).map { client.rect(forUnit: $0) }
                let hit = chars.contains { panel.frame.insetBy(dx: 0.5, dy: 0.5).intersects($0) }
                let off = !NSScreen.screens.contains { $0.visibleFrame.insetBy(dx: -1, dy: -1).contains(panel.frame) }
                if hit || off {
                  covered.append("\(place)/\(wraps ? "wrapped" : "one line")/\(Int(height))pt/\(side)/\(report)/\(timing): "
                                 + (hit ? "covers the text" : "off the screen") + " \(NSStringFromRect(panel.frame))")
                }
              }
            }
          }
        }
      }
      if covered.isEmpty {
        print("type-selftest ok field matrix\(parts > 1 ? " part \(part + 1) of \(parts)" : ""): the panel clear of the text "
              + "and on the screen in all \(cases) fields")
      } else {
        for c in covered.prefix(6) { fail("field matrix", c) }
        if covered.count > 6 { fail("field matrix", "\(covered.count - 6) more") }
      }
      clear("after the field matrix")
    }

    func finish() -> Bool {
      watcher.stop()
      for problem in watcher.problems.prefix(6) { fail("panel as drawn", problem) }
      if watcher.problems.count > 6 { fail("panel as drawn", "\(watcher.problems.count - 6) more") }
      if watcher.problems.isEmpty {
        print("type-selftest ok panel as drawn: \(watcher.looked) panels looked at, none with text cut off or blank space "
              + String(format: "beside it (at most %.1f pt more room after the text than before it across, %.1f pt down)",
                       watcher.worstExtraRight, watcher.worstExtraBottom))
      }

      if failures > 0 {
        fputs("type-selftest: \(failures) problems\n", stderr)
        return false
      }
      print("type-selftest ok - " + (part == "all" ? "typing, candidates, commit, backspace, escape, navigation and a 200-key composition"
                                     : part == "main" ? "everything but the field matrix"
                                     : "part \(part.dropFirst("matrix:".count)) of the field matrix"))
      return true
    }

    // A part of the field matrix on its own, so that the parts can run side by side with the rest.
    guard part == "all" || part == "main" || part.hasPrefix("matrix:") else {
      fail("arguments", "\(part) is not all, main or matrix:<part>/<parts>")
      return finish()
    }
    if part.hasPrefix("matrix:") {
      let numbers = part.dropFirst("matrix:".count).split(separator: "/").compactMap { Int($0) }
      guard numbers.count == 2, numbers[1] > 0, (0..<numbers[1]).contains(numbers[0]) else {
        fail("arguments", "\(part) is not matrix:<part>/<parts>")
        return finish()
      }
      fieldMatrix(part: numbers[0], of: numbers[1])
      return finish()
    }

    // An account the installer did not set up gets the bundle's Rime files at AquaType's first start, once per build:
    // copied with a stamp, left alone while the stamp matches, replaced (and the files earlier versions shipped
    // removed) when it does not.
    do {
      let fm = FileManager.default
      let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appending(component: "aquatype-rimefiles-\(getpid())")
      defer { try? fm.removeItem(at: dir) }
      try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
      let bundled = Bundle.main.resourceURL!.appending(component: "rime")
      let shipped = ((try? fm.contentsOfDirectory(atPath: bundled.path)) ?? []).filter { $0.hasSuffix(".yaml") }.sorted()
      let present = { ((try? fm.contentsOfDirectory(atPath: dir.path)) ?? []).filter { $0.hasSuffix(".yaml") }.sorted() }
      let schema = dir.appending(component: "aquatype_bopomofo.schema.yaml")
      if shipped.isEmpty {
        fail("rime files", "the bundle has no Rime files to install")
      } else if !SquirrelApplicationDelegate.installRimeFiles(from: bundled, to: dir, build: "A") || present() != shipped {
        fail("rime files", "an empty Rime folder got \(present()) instead of \(shipped)")
      } else {
        try? "edited".write(to: schema, atomically: true, encoding: .utf8)
        // A name an earlier version shipped and this bundle does not (a bundle still carrying them all checks none).
        let legacyName = SquirrelApplicationDelegate.rimeFilesNoLongerShipped.first { !shipped.contains($0) }
        let legacy = dir.appending(component: legacyName ?? ".none")
        if legacyName != nil { try? "old".write(to: legacy, atomically: true, encoding: .utf8) }
        let oldBuild = dir.appending(component: "build")
        try? fm.createDirectory(at: oldBuild, withIntermediateDirectories: true)
        if SquirrelApplicationDelegate.installRimeFiles(from: bundled, to: dir, build: "A")
            || (try? String(contentsOf: schema, encoding: .utf8)) != "edited" {
          fail("rime files", "the same build copied its files again")
        } else if !SquirrelApplicationDelegate.installRimeFiles(from: bundled, to: dir, build: "B")
                    || (try? String(contentsOf: schema, encoding: .utf8)) == "edited" || fm.fileExists(atPath: legacy.path)
                    || fm.fileExists(atPath: oldBuild.path) {
          fail("rime files", "a new build did not replace the files, or left a file or the build earlier versions left")
        } else {
          print("type-selftest ok rime files: \(shipped.count) installed once per build, earlier versions' files and build removed")
        }
      }
    }

    // 0. Keys typed while the engine is still loading: the composition in the document only grows.  The answer for an
    //    earlier prefix arrives in the middle and fills the panel, and it must not take the text back to those keys.
    do {
      client.recordUpdates = true
      send(keyDown("5"), settleFor: 0.0)
      for ch in "j4u4" { send(keyDown(String(ch)), settleFor: 0.03) }
      _ = waitUntil(30) { controller.aquaTypeSelftestPanelIsCurrent }
      settle(0.3)
      client.recordUpdates = false
      var shown: [String] = []
      for u in client.updates where !u.marked.isEmpty && shown.last != u.marked { shown.append(u.marked) }
      let full = "ㄓㄨˋ ㄧˋ"
      if let i = shown.firstIndex(of: full), shown[i...].contains(where: { $0 != full }) {
        fail("composition", "after all five keys the text went back: \(shown.joined(separator: " -> "))")
      } else if !shown.contains(full) {
        fail("composition", "the five keys were never all shown: \(shown.joined(separator: " -> "))")
      } else {
        print("type-selftest ok composition while loading: \(shown.joined(separator: " -> "))")
      }
      send(keyEscape, settleFor: 0.2)
      client.reset()
    }

    // Keys typed while librime cannot give a session (its deployer settling on a cold start, longer on a busy machine)
    // are not lost and none reaches the document as a Latin letter: once there is a session they compose, in order.
    do {
      controller.aquaTypeSelftestLoseSession(refusals: 15)            // 15 attempts at 20 ms: about 0.3 s
      var passedOn = 0
      for ch in "5j4u4" {
        if !controller.handle(keyDown(String(ch)), client: client) { passedOn += 1 }
        settle(0.03)
      }
      _ = waitUntil(5) { client.marked == "ㄓㄨˋ ㄧˋ" }
      if passedOn > 0 || client.marked != "ㄓㄨˋ ㄧˋ" || !client.committed.isEmpty {
        fail("no session", "\(passedOn) of 5 keys passed on to the document; it shows \(client.marked.debugDescription), "
                          + "with \(client.committed.debugDescription) committed")
      } else {
        print("type-selftest ok no session: 5 keys typed while librime could not give a session came out as ㄓㄨˋ ㄧˋ")
      }
      clear("after the no session case")
    }

    // Backspace while keys wait for a session takes back the last of them, as it would in a composition: what comes out
    // once there is a session is the keys without it, and nothing reaches the document as Latin letters.
    do {
      client.reset()
      controller.aquaTypeSelftestLoseSession(refusals: 15)
      for ch in "5j4u4x" { _ = controller.handle(keyDown(String(ch)), client: client); settle(0.02) }
      let backspaceKept = controller.handle(keyDelete, client: client)
      _ = waitUntil(5) { client.marked == "ㄓㄨˋ ㄧˋ" }
      if !backspaceKept || client.marked != "ㄓㄨˋ ㄧˋ" || !client.committed.isEmpty {
        fail("held backspace", "Backspace kept \(backspaceKept); the composition is \(client.marked.debugDescription), "
                               + "\(client.committed.debugDescription) went into the document")
      } else {
        print("type-selftest ok held backspace: it took back the last held key")
      }
      clear("after the held backspace case")
    }

    // A logout that is asked for and then cancelled (an application with unsaved work refuses it): the input method
    // goes on typing.  The notification comes when the logout is asked for, before anyone can refuse it.
    do {
      client.reset()
      NSWorkspace.shared.notificationCenter.post(name: NSWorkspace.willPowerOffNotification, object: NSWorkspace.shared)
      settle(0.2)
      type("5j4u4")
      _ = waitUntil(5) { client.marked == "ㄓㄨˋ ㄧˋ" }
      if client.marked != "ㄓㄨˋ ㄧˋ" {
        fail("cancelled logout", "after a logout was asked for, 5j4u4 showed \(client.marked.debugDescription) and "
                                 + "\(client.committed.debugDescription) went into the document")
      } else {
        print("type-selftest ok cancelled logout: typing goes on after a logout is asked for")
      }
      clear("after the cancelled logout case")
    }

    // A key that types nothing (Return) while keys are held for a session: the held keys go into the document first and
    // the key goes to the application after them - a held Return could never be given back to it.  And once librime
    // has given no session for the whole wait, keys go to the application at once instead of each waiting again.
    do {
      client.reset()
      controller.aquaTypeSelftestLoseSession(refusals: 1_000_000)
      let okHeld = controller.handle(keyDown("o"), client: client) && controller.handle(keyDown("k"), client: client)
      let returnPassed = !controller.handle(keyReturn, client: client)
      let typedBeforeReturn = client.committed
      client.reset()
      _ = controller.handle(keyDown("a"), client: client)
      settle(SquirrelInputController.aquaTypeSessionWait + 0.3)
      let afterWait = client.committed
      let nextPassed = !controller.handle(keyDown("b"), client: client)
      controller.aquaTypeSelftestLoseSession(refusals: 0)
      _ = waitUntil(1) { controller.handle(keyDown("1"), client: client) }   // a session again: keys are its once more
      if !okHeld || !returnPassed || typedBeforeReturn != "ok" {
        fail("held keys", "o and k held \(okHeld), Return passed on \(returnPassed), \(typedBeforeReturn.debugDescription) "
                          + "in the document before it (want \"ok\")")
      } else if afterWait != "a" || !nextPassed {
        fail("held keys", "after the wait \(afterWait.debugDescription) was typed (want \"a\") and the next key "
                          + (nextPassed ? "went to the application" : "was held again"))
      } else {
        print("type-selftest ok held keys: Return after held keys goes to the application after them; "
              + "with no session for \(Int(SquirrelInputController.aquaTypeSessionWait)) s keys go straight through")
      }
      clear("after the held keys case")
    }

    // 1. A composition appears while typing.  "5j4u4" is ㄓㄨˋ ㄧˋ on this layout - two real syllables, not a
    //    keystroke that happens to be legal.  The first one pays for the engine's model load, so it waits longer.
    let failuresBeforeComposition = failures
    type("5j4")
    let composed = waitUntil(30) { !client.marked.isEmpty }
    if !composed {
      fail("composition", "nothing was marked after typing 5j4 - the input method shows nothing while typing")
    }
    let markedAfterThree = client.marked
    // What those three keys spell, not merely that they spelled something: on a freshly built bundle the first key
    // (ㄓ) can be lost while the engine is still loading, which turns 注意 into 惡意 and still shows a composition.
    if composed, !markedAfterThree.contains("ㄓㄨ") {
      fail("composition", "typing 5j4 (ㄓㄨˋ) produced \(markedAfterThree.debugDescription) - a keystroke was lost")
    }
    type("u4")
    _ = waitUntil(10) { client.marked != markedAfterThree }
    if composed && client.marked.isEmpty {
      fail("composition", "the marked text disappeared as more keys arrived")
    }
    let markedAfterFive = client.marked
    if composed, !markedAfterFive.contains("ㄓㄨ") || !markedAfterFive.contains("ㄧ") {
      fail("composition", "typing 5j4u4 (ㄓㄨˋ ㄧˋ) produced \(markedAfterFive.debugDescription)")
    }
    if failures == failuresBeforeComposition {
      print("type-selftest ok composition: 5j4 -> \(markedAfterThree.debugDescription), 5j4u4 -> \(markedAfterFive.debugDescription)")
    }

    // 2. Candidates, and a commit that puts Han characters into the client.
    // Waits for the panel to be showing candidates for THESE keys, not merely for candidates to exist.  While typing,
    // the panel is deliberately filled in with the answers for earlier prefixes to keep it moving, and a candidate
    // from one of those must not be committable.
    if !waitUntil(30, { controller.aquaTypeSelftestPanelIsCurrent }) {
      fail("candidates", "no candidate for 5j4u4 itself was ever offered"
                       + " (panel is on \(controller.aquaTypeSelftestPanelKeyCount) keys,"
                       + " composition is \(controller.aquaTypeSelftestComposingKeyCount))")
    }
    let offered = controller.aquaTypeSelftestCandidates.prefix(5).joined(separator: " ")
    let highlightedFirst = controller.aquaTypeSelftestCandidates.first ?? ""
    send(keyReturn, settleFor: 0.4)
    if client.committed.isEmpty {
      fail("commit", "Return committed nothing; typing produces no text at all")
    } else if !isHan(client.committed) {
      fail("commit", "Return committed \(client.committed.debugDescription), which is not Han text")
    } else if client.committed != highlightedFirst {
      fail("commit", "Return committed \(client.committed.debugDescription), not the highlighted \(highlightedFirst.debugDescription)")
    } else {
      print("type-selftest ok commit: \(client.committed) (offered: \(offered))")
    }
    if !client.marked.isEmpty {
      fail("commit", "the composition was still showing \(client.marked.debugDescription) after the commit")
    }
    clear()

    // A commit while secure input is on in the application typed into (Secure Keyboard Entry) is not remembered.  The
    // same commit is remembered with it off, and with it on but held by another process - a password manager or the
    // login window can leave it on for hours, and AquaType would otherwise learn nothing anywhere meanwhile.  Each
    // commit that is remembered also shows the wait is long enough for a record to land.
    do {
      let memory = URL(fileURLWithPath: harness.stateDir).appending(component: "personal_memory.v2.tsv")
      let contents = { (try? Data(contentsOf: memory)) ?? Data() }
      // macOS names the process responsible for secure input, which for this one is the application it was started
      // from (Terminal, an editor); that is the "application typed into" for the case where it holds it.
      func holder() -> pid_t? {
        ((CGSessionCopyCurrentDictionary() as? [String: Any])?["kCGSSessionSecureInputPID"] as? NSNumber)?.int32Value
      }
      func remembered(secure: Bool, here: Bool, _ keys: String) -> Bool {
        let before = contents()
        if secure { EnableSecureEventInput() }
        defer { if secure { DisableSecureEventInput() } }
        SquirrelInputController.aquaTypeSelftestClientPID = here ? (holder() ?? getpid()) : 1
        defer { SquirrelInputController.aquaTypeSelftestClientPID = nil }
        type(keys)
        _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
        send(keyReturn, settleFor: 0.2)
        clear()
        return waitUntil(2) { contents() != before }
      }
      let here = remembered(secure: true, here: true, "fu4")
      let plain = remembered(secure: false, here: true, "fu4")
      let elsewhere = remembered(secure: true, here: false, "cl3")
      if here || !plain || !elsewhere {
        fail("secure input", "remembered with it on here: \(here), off: \(plain), on in another process: \(elsewhere)")
      } else {
        print("type-selftest ok secure input: not remembered where it is on, remembered where it is off or held elsewhere")
      }
    }

    // 2b. A Space after a finished syllable (a habit from the built-in 注音, where it brings up the candidates) adds
    //     nothing: the composition and its first candidate stay as they were.
    clear("before the space after a tone case")
    type("5j4u4")
    _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
    let firstBeforeSpace = controller.aquaTypeSelftestCandidates.first ?? ""
    send(keyDown(" ", keyCode: 49), settleFor: 0.1)
    send(keyDown(" ", keyCode: 49), settleFor: 0.1)
    _ = waitUntil(5) { controller.aquaTypeSelftestPanelIsCurrent }
    if controller.aquaTypeSelftestComposingKeyCount != 5 || controller.aquaTypeSelftestCandidates.first != firstBeforeSpace {
      fail("space after a tone", "two Spaces after 5j4u4 left \(controller.aquaTypeSelftestComposingKeyCount) keys and "
                                 + "\(controller.aquaTypeSelftestCandidates.first.debugDescription) first, "
                                 + "not 5 and \(firstBeforeSpace.debugDescription)")
    } else {
      print("type-selftest ok space after a tone: ignored, \(firstBeforeSpace) still first")
    }
    clear("after the space after a tone case")

    // 2c. Keys pressed before the engine has answered, as a quick typist presses them: nothing runs between them, so
    //     no answer can arrive in between.  A Space after the tone leaves the answer on its way to be shown; Return
    //     commits the engine's answer once it arrives, and keys typed after it start a composition of their own;
    //     punctuation commits the engine's answer, not what librime made of the keys, with its Shift let go while it
    //     waits; Return twice commits the engine's answer, and the second has nothing left to do; Backspace takes the
    //     held Return back; a click in Chromium while it waits replaces the bopomofo; leaving the field while it waits
    //     leaves the next field's badge alone.
    func shift(_ down: Bool) -> NSEvent {
      NSEvent.keyEvent(with: .flagsChanged, location: .zero, modifierFlags: down ? .shift : [],
                       timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: 0, context: nil,
                       characters: "", charactersIgnoringModifiers: "", isARepeat: false, keyCode: 56)!
    }
    func quickly(_ keys: String, then last: NSEvent...) -> [Bool] {
      for ch in keys { _ = controller.handle(keyDown(String(ch)), client: client) }
      return last.map { controller.handle($0, client: client) }
    }
    clear("before the keys before the answer cases")
    _ = quickly("5j4u4", then: keyDown(" ", keyCode: 49))
    let spaceKept = waitUntil(5) { controller.aquaTypeSelftestPanelIsCurrent }
    let spaceFirst = controller.aquaTypeSelftestCandidates.first ?? ""
    clear()
    _ = quickly("5j4u4", then: keyReturn)
    let returnCommitted = waitUntil(5) { !client.committed.isEmpty && controller.aquaTypeSelftestComposingKeyCount == 0 }
    let returnText = client.committed
    clear()
    _ = quickly("5j4u4", then: keyReturn, keyDown("j"), keyDown("4"))
    _ = waitUntil(5) { !client.committed.isEmpty }
    settle(0.2)
    let afterReturn = (client.committed, controller.aquaTypeSelftestComposingKeyCount)
    clear()
    // Keys librime alone gets wrong (他已經道宮撕了).
    let sentenceKeys = "w8 u3ru/ 2l4ej/ n xk7"
    type(sentenceKeys)
    _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
    let sentenceFirst = controller.aquaTypeSelftestCandidates.first ?? ""
    clear()
    _ = quickly(sentenceKeys, then: shift(true), keyDown("<", flags: .shift), shift(false))
    _ = waitUntil(5) { client.committed.hasSuffix("，") }
    settle(0.1)
    let punctuationText = client.committed, punctuationEnglish = controller.aquaTypeSelftestOption("ascii_mode")
    controller.aquaTypeSelftestSetOption("ascii_mode", false)
    clear()
    // A Shift tap to English before the answer ("我要買" then iPhone): the answer, then English.
    _ = quickly(sentenceKeys, then: shift(true), shift(false))
    _ = waitUntil(5) { !client.committed.isEmpty }
    settle(0.1)
    let shiftTapText = client.committed, shiftTapEnglish = controller.aquaTypeSelftestOption("ascii_mode")
    controller.aquaTypeSelftestSetOption("ascii_mode", false)
    clear()
    // Backspace takes a held Shift tap back: nothing committed, still Chinese.
    _ = quickly(sentenceKeys, then: shift(true), shift(false), keyDelete)
    settle(0.5)
    let tapTakenBack = (client.committed, controller.aquaTypeSelftestComposingKeyCount, controller.aquaTypeSelftestOption("ascii_mode"))
    controller.aquaTypeSelftestSetOption("ascii_mode", false)
    clear()
    // Option with a bopomofo key is punctuation (Option+, is ，) and waits for the answer too.
    _ = quickly(sentenceKeys, then: keyDown("≤", keyCode: 43, flags: .option))
    _ = waitUntil(5) { client.committed.hasSuffix("，") }
    let optionText = client.committed
    clear()
    // Return behind a held question mark ends the wait and goes on to the application ("真的嗎？" and Return sends).
    let questionReturn = quickly("5j4u4", then: shift(true), keyDown("?", keyCode: 44, flags: .shift), shift(false), keyReturn)
    settle(0.3)
    let questionText = client.committed
    clear()
    // Two clauses before the first answer: "好，真的嗎？" and Return sends - the Return is not passed by the ？ held again
    // for the second clause.
    let comma = [shift(true), keyDown("<", keyCode: 43, flags: .shift), shift(false)]
    let question = [shift(true), keyDown("?", keyCode: 44, flags: .shift), shift(false)]
    _ = quickly("cl3", then: comma[0], comma[1], comma[2])
    _ = quickly("5p 2k7a87", then: question[0], question[1], question[2])
    let twoClauses = quickly("", then: keyReturn)
    _ = waitUntil(5) { client.committed.hasSuffix("？") }
    let twoClausesText = client.committed
    clear()
    // A Return behind a sentence typed after a held Return is that sentence's, and goes on.
    _ = quickly("5j4u4", then: keyReturn)
    _ = quickly("ji3", then: question[0], question[1], question[2])
    let laterReturn = quickly("", then: keyReturn)
    _ = waitUntil(5) { client.committed.hasSuffix("？") }
    clear()
    // Backspace after a Shift+1 typed behind a held Return deletes the ！ and leaves the mode alone.
    _ = quickly("5j4u4", then: keyReturn, shift(true), keyDown("!", keyCode: 18, flags: .shift), shift(false), keyDelete)
    settle(0.3)
    let bangText = client.committed, bangEnglish = controller.aquaTypeSelftestOption("ascii_mode")
    controller.aquaTypeSelftestSetOption("ascii_mode", false)
    clear()
    // Shift+Return with the answer shown is a new line: the composition is committed and the key goes on.
    type("5j4u4")
    _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
    let shiftReturnShown = controller.handle(keyDown("\r", keyCode: 36, flags: .shift), client: client)
    let shiftReturnShownText = client.committed
    clear()
    let twice = quickly("5j4u4", then: keyReturn, keyReturn)
    _ = waitUntil(5) { !client.committed.isEmpty }
    settle(0.3)
    let twiceText = client.committed, twiceLeft = controller.aquaTypeSelftestComposingKeyCount
    clear()
    _ = quickly("5j4u4", then: keyReturn, keyDelete)
    settle(0.5)
    let takenBack = (client.committed, controller.aquaTypeSelftestComposingKeyCount)
    clear()
    // Escape after the held Return cancels only what follows it; Shift+Return (a new line) goes on to the application.
    _ = quickly("5j4u4", then: keyReturn, keyEscape)
    settle(0.3)
    let escaped = (client.committed, controller.aquaTypeSelftestComposingKeyCount)
    clear()
    let shiftReturn = quickly("5j4u4", then: keyReturn, keyDown("\r", keyCode: 36, flags: .shift))
    settle(0.3)
    let shiftReturnText = client.committed
    clear()
    // Focus leaving with a second Return held behind a second composition: nothing is left held.
    _ = quickly("5j4u4", then: keyReturn, keyDown("j"), keyDown("4"), keyReturn)
    controller.commitComposition(client)
    let heldAfterCommit = controller.aquaTypeSelftestHeldKeyCount
    settle(0.3)
    clear()
    _ = quickly("5j4u4", then: keyReturn)
    client.takeMarkedAsTyped()
    controller.commitComposition(client)
    settle(0.3)
    let clickedText = client.committed, clickedLeft = controller.aquaTypeSelftestComposingKeyCount
    clear()
    var badgeKept = true, leftText = ""
    if let other = SquirrelInputController(server: nil, delegate: nil, client: nil) {
      _ = quickly("5j4u4", then: keyReturn)
      let otherClient = SelftestTextClient()
      other.activateServer(otherClient)
      let token = SquirrelInputController.aquaTypeAnnounceToken
      controller.deactivateServer(client)
      badgeKept = SquirrelInputController.aquaTypeAnnounceToken == token && SquirrelInputController.aquaTypeActiveController === other
      leftText = client.committed
      other.deactivateServer(otherClient)
      controller.activateServer(client)
      settle(0.3)
    }
    clear("after the keys before the answer cases")
    var beforeAnswer: [String] = []
    if !spaceKept || spaceFirst != firstBeforeSpace {
      beforeAnswer.append("a Space before the answer left \(spaceFirst.debugDescription) first (current: \(spaceKept))")
    }
    if !returnCommitted || returnText != firstBeforeSpace {
      beforeAnswer.append("Return before the answer committed \(returnText.debugDescription), not \(firstBeforeSpace)")
    }
    if afterReturn != (firstBeforeSpace, 2) {
      beforeAnswer.append("Return then ㄨˋ committed \(afterReturn.0.debugDescription) and left \(afterReturn.1) keys")
    }
    if !isHan(sentenceFirst) || punctuationText != sentenceFirst + "，" || punctuationEnglish {
      beforeAnswer.append("punctuation before the answer committed \(punctuationText.debugDescription), not "
                          + "\(sentenceFirst)，, English after it: \(punctuationEnglish)")
    }
    if shiftTapText != sentenceFirst || !shiftTapEnglish {
      beforeAnswer.append("a Shift tap before the answer committed \(shiftTapText.debugDescription), English: \(shiftTapEnglish)")
    }
    if tapTakenBack.0 != "" || tapTakenBack.1 != sentenceKeys.count || tapTakenBack.2 {
      beforeAnswer.append("Backspace after a held Shift tap: committed \(tapTakenBack.0.debugDescription), "
                          + "\(tapTakenBack.1) keys, English \(tapTakenBack.2)")
    }
    if optionText != sentenceFirst + "，" {
      beforeAnswer.append("Option+, before the answer committed \(optionText.debugDescription)")
    }
    if questionReturn.last != false || questionText.last != "？" || !isHan(String(questionText.dropLast())) {
      beforeAnswer.append("Return behind ？: handled \(questionReturn), committed \(questionText.debugDescription)")
    }
    if twoClauses != [false] || !twoClausesText.contains("，") || !twoClausesText.hasSuffix("？") {
      beforeAnswer.append("好，真的嗎？ and Return: Return handled \(twoClauses), committed \(twoClausesText.debugDescription)")
    }
    if laterReturn != [false] {
      beforeAnswer.append("a Return behind a sentence typed after a held Return was kept: \(laterReturn)")
    }
    if bangText != firstBeforeSpace + "！" || bangEnglish {
      beforeAnswer.append("Shift+1 and Backspace behind a held Return: committed \(bangText.debugDescription), English \(bangEnglish)")
    }
    if shiftReturnShown || shiftReturnShownText != firstBeforeSpace {
      beforeAnswer.append("Shift+Return with the answer shown: handled \(shiftReturnShown), committed \(shiftReturnShownText.debugDescription)")
    }
    if twice != [true, true] || twiceText != firstBeforeSpace || twiceLeft != 0 {
      beforeAnswer.append("Return twice: handled \(twice), committed \(twiceText.debugDescription), \(twiceLeft) keys left")
    }
    if takenBack != ("", 5) {
      beforeAnswer.append("Backspace after Return committed \(takenBack.0.debugDescription), \(takenBack.1) keys left")
    }
    if !isHan(escaped.0) || escaped.1 != 0 {
      beforeAnswer.append("Escape after Return left \(escaped.0.debugDescription) committed, \(escaped.1) keys")
    }
    if shiftReturn != [true, false] || !isHan(shiftReturnText) {
      beforeAnswer.append("Shift+Return after Return: handled \(shiftReturn), committed \(shiftReturnText.debugDescription)")
    }
    if heldAfterCommit != 0 {
      beforeAnswer.append("focus leaving left \(heldAfterCommit) keys held")
    }
    if !isHan(clickedText) || clickedLeft != 0 {
      beforeAnswer.append("a click while Return waited left \(clickedText.debugDescription), \(clickedLeft) keys")
    }
    if !badgeKept || !isHan(leftText) {
      beforeAnswer.append("leaving the field while Return waited: badge kept \(badgeKept), committed \(leftText.debugDescription)")
    }
    if beforeAnswer.isEmpty {
      print("type-selftest ok keys before the answer: Space kept it, Return committed \(firstBeforeSpace), punctuation "
            + "\(sentenceFirst)，, a Shift tap and Option+, the same, Return twice the same, Return behind ？ went on, "
            + "two clauses and Return sent, Backspace took Return and a Shift tap back, Escape and Shift+Return followed it, "
            + "a click or leaving the field kept no bopomofo and nothing held")
    } else {
      fail("keys before the answer", beforeAnswer.joined(separator: "; "))
    }

    // 3. Backspace shortens the composition instead of ending it.  A finished syllable (its tone typed) is taken back
    //    whole, as the built-in 注音 takes back the character it has become; 3b covers an unfinished one.
    type("5j4u4")
    _ = waitUntil(10) { !client.marked.isEmpty }
    let beforeDelete = client.marked
    send(keyDelete, settleFor: 0.2)
    if beforeDelete.isEmpty {
      fail("backspace", "there was no composition to shorten")
    } else if client.marked != "ㄓㄨˋ" || controller.aquaTypeSelftestComposingKeyCount != 3 {
      fail("backspace", "backspace took \(beforeDelete.debugDescription) to \(client.marked.debugDescription) with "
                        + "\(controller.aquaTypeSelftestComposingKeyCount) of 5 keys left, not ㄓㄨˋ (its last syllable)")
    } else {
      print("type-selftest ok backspace: \(beforeDelete.debugDescription) -> \(client.marked.debugDescription)")
    }

    // 3b. Backspace on an unfinished syllable takes back one bopomofo key, not the whole syllable.
    send(keyEscape, settleFor: 0.2)
    client.reset()
    type("u45j")
    _ = waitUntil(10) { client.marked.contains("ㄓㄨ") }
    let beforeKeyDelete = client.marked
    send(keyDelete, settleFor: 0.2)
    if beforeKeyDelete.hasSuffix("ㄓㄨ"), client.marked.hasSuffix("ㄓ"), !client.marked.hasSuffix("ㄓㄨ") {
      print("type-selftest ok backspace unfinished: \(beforeKeyDelete.debugDescription) -> \(client.marked.debugDescription)")
    } else {
      fail("backspace unfinished", "\(beforeKeyDelete.debugDescription) became \(client.marked.debugDescription), expected one symbol less")
    }

    // 4. Escape abandons it, and abandons it without committing anything.
    let committedBeforeEscape = client.committed
    send(keyEscape, settleFor: 0.2)
    if !client.marked.isEmpty {
      fail("escape", "escape left \(client.marked.debugDescription) marked")
    } else if client.committed != committedBeforeEscape {
      fail("escape", "escape committed \(client.committed.dropFirst(committedBeforeEscape.count).debugDescription)")
    } else {
      print("type-selftest ok escape: composition abandoned, nothing committed")
    }
    client.reset()

    // 5. Arrow navigation then commit: the path that partial commits go through.
    type("5j4u4")
    _ = waitUntil(15) { controller.aquaTypeSelftestPanelIsCurrent && controller.aquaTypeSelftestCandidateCount > 2 }
    let third = controller.aquaTypeSelftestCandidates.dropFirst(2).first ?? ""
    send(keyDownArrow, settleFor: 0.2)
    send(keyDownArrow, settleFor: 0.2)
    send(keyReturn, settleFor: 0.4)
    if client.committed.isEmpty {
      fail("navigate and commit", "Down Down Return committed nothing")
    } else if client.committed != third {
      fail("navigate and commit", "Down Down Return committed \(client.committed.debugDescription), not the third "
                                  + "candidate \(third.debugDescription)")
    } else if !isHan(client.committed) {
      fail("navigate and commit", "committed \(client.committed.debugDescription), which is not Han text")
    } else {
      print("type-selftest ok navigate and commit: \(client.committed)")
    }
    clear()

    // 6a. The length cap (190 keys) holds against every key that extends a composition, including the digits: on this
    //     layout only 3, 4, 6 and 7 are tones, and 0, 1, 2, 5, 8 and 9 are ordinary bopomofo keys.  Past 256 keys the
    //     engine rejects the request outright and the input method stops answering.
    clear("before the length cap case")
    for _ in 0..<300 { send(keyDown("5"), settleFor: 0.004) }
    settle(0.6)
    let capKeys = controller.aquaTypeSelftestComposingKeyCount
    if capKeys != SquirrelInputController.aquaTypeSelftestKeyCap {
      fail("length cap", "300 presses of `5` (ㄓ, an extending bopomofo key) left \(capKeys) keys in the composition, "
                       + "not the \(SquirrelInputController.aquaTypeSelftestKeyCap)-key cap")
    } else {
      print("type-selftest ok length cap: 300 presses of `5` stopped at \(capKeys) keys")
    }
    clear("after the length cap case")

    // 6b. Going well past the cap neither crashes nor wedges: the composition is still alive and still commits.
    for _ in 0..<40 { type("5j4u4") }
    settle(1.0)
    if client.marked.isEmpty {
      fail("long input", "the composition was lost somewhere in 200 keystrokes")
    } else {
      print("type-selftest ok long input: 200 keystrokes, composition "
            + "\(controller.aquaTypeSelftestComposingKeyCount) keys / \(client.marked.count) characters")
    }
    // Waits for the panel to be showing candidates for THIS composition, not merely for some candidates to exist.
    // 200 keys in ten seconds is far faster than the decoder keeps up with, so until it catches up the panel shows an
    // earlier prefix's candidates and a commit is correctly refused.  A commit that fails after the wait is a failure.
    let longHasCandidates = waitUntil(120) { controller.aquaTypeSelftestPanelIsCurrent }
    send(keyReturn, settleFor: 0.5)
    _ = waitUntil(10) { !client.committed.isEmpty }
    if client.committed.isEmpty {
      fail("long input", "nothing could be committed after a long composition"
                       + " (panel caught up with the composition: \(longHasCandidates ? "yes" : "no"),"
                       + " composition \(controller.aquaTypeSelftestComposingKeyCount) keys,"
                       + " panel computed for \(controller.aquaTypeSelftestPanelKeyCount) keys,"
                       + " still marked: \(client.marked.count) characters)")
    } else {
      print("type-selftest ok long input commit: \(client.committed.count) characters")
    }
    // A panel that never catches up with what was typed is a failure even when the commit went through: someone who
    // types a long sentence quickly and then stops would be left with candidates for keys they typed long ago.  One
    // more key is tried, to say whether it was wedged or only waiting for another keystroke.
    if !longHasCandidates {
      send(keyDelete, settleFor: 0.3)
      let recovered = waitUntil(30) { controller.aquaTypeSelftestPanelIsCurrent }
      fail("long input", "the panel did not catch up with the composition in 120 s; after one Backspace it "
                         + (recovered ? "did (it was waiting for another keystroke)"
                                      : "was still computed for \(controller.aquaTypeSelftestPanelKeyCount) of "
                                        + "\(controller.aquaTypeSelftestComposingKeyCount) keys"))
    }
    clear()

    // The 中 / A badge on arrival: raised for a click into the field, not for focus coming back by any other route.
    // A caret in a search box at (500, 600), 18pt tall, in screen coordinates (y up).
    do {
      let caret = NSRect(x: 500, y: 600, width: 1, height: 18)
      let arrivals: [(String, Double, Bool, Bool)] = [
        ("Command-Tab back to a field, last click 30 s ago", 30, false, false),
        ("click 0.2 s before focus arrived", 0.2, false, true),
        ("button still down as focus arrives", 5, true, true),
        ("click 1.5 s before focus arrived", 1.5, false, false),
      ]
      let clicks: [(String, NSPoint, Bool)] = [
        ("on the caret's line, right of it (past the end of the text)", NSPoint(x: 720, y: 606), true),
        ("on the caret itself", NSPoint(x: 501, y: 609), true),
        ("in the field's left padding", NSPoint(x: 480, y: 606), true),
        ("on a browser tab 300pt above", NSPoint(x: 520, y: 900), false),
        ("in the page 200pt below", NSPoint(x: 520, y: 400), false),
        ("far left of the field, same height", NSPoint(x: 200, y: 606), false),
      ]
      var wrong: [String] = []
      for (what, age, down, expected) in arrivals
      where SquirrelInputController.aquaTypeArrivalFollowsClick(secondsSinceLeftMouseDown: age, leftButtonDown: down) != expected {
        wrong.append("\(what): expected \(expected ? "a click" : "no click")")
      }
      for (what, point, expected) in clicks
      where SquirrelInputController.aquaTypeClick(point, isOnCaretLineOf: caret) != expected {
        wrong.append("click \(what): expected \(expected ? "badge" : "no badge")")
      }
      if wrong.isEmpty {
        let quiet = arrivals.filter { !$0.3 }.count + clicks.filter { !$0.2 }.count
        print("type-selftest ok mode badge: shown for a click into the field, quiet in \(quiet) other cases "
              + "(Command-Tab, a stale click, a tab or title bar, the page around the field)")
      } else {
        for w in wrong { fail("mode badge", w) }
      }
    }

    // A click inside the field that already has focus, which the system does not tell the input method about: the
    // badge at the caret, never at the mouse - on the clicked line, or wherever else the click moved the caret (below
    // the last line of a note, to the end of the text); nothing for a double click, a drag, a click that ends a
    // composition, or a click away from the caret's line that left the caret where it was.  The stub's caret is at
    // (300, 500), 18pt tall; a caret "before" elsewhere stands for one the click moved.
    do {
      clear("before the click badge case")
      let here = NSRect(x: 300, y: 500, width: 1, height: 18), earlier = NSRect(x: 520, y: 590, width: 1, height: 18)
      let cases: [(String, NSPoint, NSPoint, Int, Bool, NSRect?, Bool)] = [
        ("a single click past the end of the line", NSPoint(x: 700, y: 509), NSPoint(x: 700, y: 509), 1, false, here, true),
        ("a click 200pt below the text that moved the caret to its end", NSPoint(x: 340, y: 300), NSPoint(x: 340, y: 300), 1, false, earlier, true),
        ("a click with click state 0 (assistive software)", NSPoint(x: 700, y: 509), NSPoint(x: 700, y: 509), 0, false, here, true),
        ("a double click", NSPoint(x: 340, y: 509), NSPoint(x: 340, y: 509), 2, false, here, false),
        ("a drag along the line", NSPoint(x: 420, y: 509), NSPoint(x: 330, y: 509), 1, false, here, false),
        ("a click that ends a composition", NSPoint(x: 340, y: 509), NSPoint(x: 340, y: 509), 1, true, here, false),
        ("a click on a toolbar 300pt above, the caret left where it was", NSPoint(x: 340, y: 809), NSPoint(x: 340, y: 809), 1, false, here, false),
        ("a click 200pt below with no caret reported before it", NSPoint(x: 340, y: 300), NSPoint(x: 340, y: 300), 1, false, nil, false),
      ]
      var wrong: [String] = []
      for (what, up, down, count, composing, caretBefore, expected) in cases {
        settle(0.3)                                                      // past the badge's own repeat guard
        let before = SquirrelInputController.aquaTypeLastAnnouncedAt
        controller.aquaTypeClicked(at: up, pressedAt: down, clickCount: count, composing: composing, caretBefore: caretBefore)
        let shown = waitUntil(1.3) { SquirrelInputController.aquaTypeLastAnnouncedAt != before }
        if shown != expected {
          wrong.append("\(what): \(shown ? "a badge" : "no badge"), expected \(expected ? "a badge" : "none")")
        } else if shown, let at = controller.aquaTypeSelftestLastBadgeAt, abs(at.minX - 300) > 0.5 || abs(at.minY - 500) > 0.5 {
          wrong.append("\(what): the badge at \(NSStringFromRect(at)), not at the caret (300, 500)")
        }
      }
      if wrong.isEmpty {
        print("type-selftest ok click badge: a click inside the focused field shows the mode at the caret, not the mouse, "
              + "on the clicked line or wherever the click moved the caret; quiet for a double click, a drag, a click "
              + "ending a composition and a click that left the caret where it was")
      } else {
        for w in wrong { fail("click badge", w) }
      }
    }

    // The panel follows the composition's lines in a field that grows as the text wraps.  A chat box grows upward:
    // each wrap pushes the earlier lines up, into the space above the text where the panel sits, so a panel held where
    // the composition started ends up covering it.  It must move with the lines, and only when they change.
    do {
      clear("before the wrapping field case")
      client.lineUnits = 6
      client.growsUpward = true
      defer { client.lineUnits = nil; client.growsUpward = false }
      var covered: [String] = []
      var panelBottoms = Set<Int>(), lineCounts = Set<Int>()
      for syllables in stride(from: 2, through: 10, by: 2) {
        type("5j4u4")
        settle(0.2)
        guard let panel = NSApp.squirrelAppDelegate.panel, panel.isVisible else { continue }
        let marked = client.markedRange()
        guard marked.location != NSNotFound, marked.length > 0 else { continue }
        let rows = Set((marked.location..<(marked.location + marked.length)).map { client.rect(forUnit: $0).minY })
        lineCounts.insert(rows.count)
        panelBottoms.insert(Int(panel.frame.minY))
        for y in rows.sorted() where panel.frame.intersects(NSRect(x: 300, y: y, width: 6 * 8, height: 18)) {
          covered.append("after \(syllables) syllables (\(rows.count) lines) the panel covers the line at y \(Int(y))")
        }
      }
      if !covered.isEmpty {
        for c in covered.prefix(3) { fail("wrapping field", c) }
      } else if lineCounts.count < 2 {
        fail("wrapping field", "the composition never wrapped (\(lineCounts.sorted()) lines), so nothing was tested")
      } else if panelBottoms.count > lineCounts.count {
        fail("wrapping field", "the panel took \(panelBottoms.count) positions for \(lineCounts.count) line counts: "
                             + "it moves more often than the lines change")
      } else {
        print("type-selftest ok wrapping field: the composition grew upward to \(lineCounts.max() ?? 0) lines and "
              + "the panel moved with it (\(panelBottoms.count) positions) without covering it")
      }
      clear("after the wrapping field case")
    }

    // Where the panel goes when the application's caret report is not the caret.  A web page reports the old place of
    // the hidden input box it takes text through for a moment after a composition starts; a window with no text field
    // reports an empty rectangle.  The panel has to end up at the real caret (y 500 here), and one odd report in the
    // middle of a sentence must not move it.
    do {
      clear("before the caret report case")
      defer { client.caretReportsFirst = []; client.caretReportAlways = nil }
      let screen = NSScreen.main?.frame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
      let elsewhere = NSRect(x: screen.minX + 60, y: screen.maxY - 160, width: 1, height: 18)
      // Placed against the caret's line [500, 518] - above it, or below it when above does not fit - and starting where
      // the composition does (x 300 and right of it here), not at the stale report's x.
      func panelAtCaret() -> Bool {
        guard let panel = NSApp.squirrelAppDelegate.panel, panel.isVisible else { return false }
        return (abs(panel.frame.minY - 523) <= 12 || abs(panel.frame.maxY - 495) <= 12) && panel.frame.minX > 250
      }
      var wrong: [String] = []
      client.caretReportsFirst = [elsewhere, elsewhere]
      type("5j4u4")
      _ = waitUntil(30) { controller.aquaTypeSelftestPanelIsCurrent }
      settle(0.2)
      if !panelAtCaret() { wrong.append("two stale reports at the start: the panel is at \(NSApp.squirrelAppDelegate.panel.map { NSStringFromRect($0.frame) } ?? "nowhere"), not at the caret") }
      let settledX = NSApp.squirrelAppDelegate.panel?.frame.minX ?? 0
      client.caretReportsFirst = [elsewhere]
      type("u4")
      _ = waitUntil(30) { controller.aquaTypeSelftestPanelIsCurrent }
      settle(0.2)
      if !panelAtCaret() || abs((NSApp.squirrelAppDelegate.panel?.frame.minX ?? 0) - settledX) > 1 {
        wrong.append("one odd report mid-sentence moved the panel to \(NSApp.squirrelAppDelegate.panel.map { NSStringFromRect($0.frame) } ?? "nowhere")")
      }
      clear("after the stale caret case")
      client.caretReportsFirst = [.zero, .zero]
      type("5")
      settle(0.1)
      if let panel = NSApp.squirrelAppDelegate.panel, panel.isVisible, panel.frame.minX < 60, panel.frame.minY < 60 {
        wrong.append("an empty report put the panel in the screen's corner")
      }
      type("j4")
      _ = waitUntil(30) { controller.aquaTypeSelftestPanelIsCurrent }
      settle(0.2)
      if !panelAtCaret() { wrong.append("after two empty reports the panel never reached the caret") }
      clear("after the empty caret case")
      if wrong.isEmpty {
        print("type-selftest ok caret reports: the panel reaches the caret after stale and empty reports, "
              + "and one odd report mid-sentence does not move it")
      } else {
        for w in wrong { fail("caret reports", w) }
      }
    }

    // A sentence typed on one line of a field whose reports wander by a point or two from key to key, as real
    // applications' do: the panel does not move at all.  It is placed from the position held for the composition, not
    // from each new report - placed from the reports, it went up and down by those points as the sentence was typed.
    do {
      clear("before the wandering reports case")
      client.wanders = true
      defer { client.wanders = false }
      var bottoms = Set<Int>(), covered = false, samples = 0
      for ch in "5j4u4 z96u84 g4 284j/6 ru04" {
        send(keyDown(String(ch)), settleFor: 0.12)                       // a brisk human pace
        guard let panel = NSApp.squirrelAppDelegate.panel, panel.isVisible else { continue }
        samples += 1
        bottoms.insert(Int((panel.frame.minY * 2).rounded()))         // half a point
        if panel.frame.intersects(NSRect(x: 300, y: 500, width: 400, height: 18)) { covered = true }
      }
      _ = waitUntil(30) { controller.aquaTypeSelftestPanelIsCurrent }
      if samples < 10 {
        fail("wandering reports", "the panel was visible for only \(samples) keys, so nothing was tested")
      } else if covered {
        fail("wandering reports", "the panel covered the line being typed")
      } else if bottoms.count > 1 {
        fail("wandering reports", "the panel took \(bottoms.count) heights over one sentence on one line "
                                  + "(\(bottoms.sorted().map { String(format: "%.1f", Double($0) / 2) }.joined(separator: ", ")))")
      } else {
        print("type-selftest ok wandering reports: one sentence on one line, \(samples) keys, the panel never moved")
      }
      clear("after the wandering reports case")
    }

    // The 中 / A badge for a mode change: shown, at the caret, for a switch made by a key; never for a mode a new
    // session restores or anything else sets; never against a caret report that is not a caret.
    do {
      clear("before the mode switch badge case")
      defer { client.caretReportAlways = nil; controller.aquaTypeSelftestSetOption("ascii_mode", false) }
      let panel = NSApp.squirrelAppDelegate.panel
      let shiftA = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: .shift,
                                    timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: 0, context: nil,
                                    characters: "A", charactersIgnoringModifiers: "A", isARepeat: false, keyCode: 0)!
      // A bare Shift: pressed and released with nothing in between.
      func flags(_ modifiers: NSEvent.ModifierFlags) -> NSEvent {
        NSEvent.keyEvent(with: .flagsChanged, location: .zero, modifierFlags: modifiers,
                         timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: 0, context: nil,
                         characters: "", charactersIgnoringModifiers: "", isARepeat: false, keyCode: 56)!
      }
      func hidePanel() { panel?.hide(force: true); settle(0.6) }
      var wrong: [String] = []
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      hidePanel()
      send(shiftA, settleFor: 0.05)
      if !controller.aquaTypeSelftestOption("ascii_mode") { wrong.append("Shift+A did not switch to English") }
      if panel?.isVisible != true || abs((panel?.frame.minY ?? 0) - 523) > 12 && abs((panel?.frame.maxY ?? 0) - 495) > 12 {
        wrong.append("no badge at the caret after Shift+A switched the mode")
      }
      hidePanel()
      send(flags(.shift), settleFor: 0.02)
      send(flags([]), settleFor: 0.05)
      if controller.aquaTypeSelftestOption("ascii_mode") { wrong.append("a bare Shift did not switch back to Chinese") }
      if panel?.isVisible != true { wrong.append("no badge after a bare Shift switched the mode") }
      hidePanel()
      // Shift held through a click (extending a selection) or a scroll is not a bare Shift.
      send(flags(.shift), settleFor: 0.02)
      SquirrelInputController.aquaTypeMouseUsed()
      send(flags([]), settleFor: 0.05)
      if controller.aquaTypeSelftestOption("ascii_mode") { wrong.append("a Shift held through a click switched the mode") }
      hidePanel()
      // Shift held through a system shortcut (Shift+Command+4): the system takes the key, only the modifiers arrive.
      send(flags(.shift), settleFor: 0.02)
      send(flags([.shift, .command]), settleFor: 0.02)
      send(flags(.shift), settleFor: 0.02)
      send(flags([]), settleFor: 0.05)
      if controller.aquaTypeSelftestOption("ascii_mode") { wrong.append("a Shift held through Shift+Command switched the mode") }
      hidePanel()
      // The system calls activateServer again while the same field stays focused, on modifier keys among other times:
      // a Shift tap in progress through one of those still switches.
      send(flags(.shift), settleFor: 0.02)
      controller.activateServer(client)
      send(flags([]), settleFor: 0.05)
      if !controller.aquaTypeSelftestOption("ascii_mode") {
        wrong.append("a Shift tap with activateServer called during it did not switch the mode")
      }
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      hidePanel()
      // Shift held through a click that leaves the field, then a Shift tap after coming back: the tap switches.
      send(flags(.shift), settleFor: 0.02)
      SquirrelInputController.aquaTypeMouseUsed()
      controller.deactivateServer(client)
      controller.activateServer(client)
      send(flags(.shift), settleFor: 0.02)
      send(flags([]), settleFor: 0.05)
      if !controller.aquaTypeSelftestOption("ascii_mode") {
        wrong.append("a Shift tap after leaving the field with Shift held did not switch the mode")
      }
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      hidePanel()
      // The mode chosen with a key carries to the next field and application, except where the application's own
      // options set it (Terminal starts in English).
      func modeIn(_ bundle: String) -> Bool? {
        guard let other = SquirrelInputController(server: nil, delegate: nil, client: nil) else { return nil }
        let otherClient = SelftestTextClient()
        otherClient.appBundleID = bundle
        other.activateServer(otherClient)
        defer { other.deactivateServer(otherClient); controller.activateServer(client) }
        return other.aquaTypeSelftestOption("ascii_mode")
      }
      // Shift held for a while and let go switches nothing: it was meant for something else.
      send(flags(.shift), settleFor: 0.6)
      send(flags([]), settleFor: 0.05)
      if controller.aquaTypeSelftestOption("ascii_mode") {
        wrong.append("Shift held for 0.6 s switched the mode")
      }
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      send(shiftA, settleFor: 0.05)                                        // a capital typed in passing: this field only
      let afterCapital = modeIn("com.apple.Notes")
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      send(flags(.shift), settleFor: 0.02)
      send(flags([]), settleFor: 0.05)                                     // to English
      let carriedEnglish = modeIn("com.apple.Notes")
      send(flags(.shift), settleFor: 0.02)
      send(flags([]), settleFor: 0.05)                                     // back to Chinese
      let carriedChinese = modeIn("com.apple.Notes"), terminal = modeIn("com.apple.Terminal")
      // Switched to Chinese and back in Terminal, which keeps a mode of its own: no choice for the other applications.
      let afterTerminal: Bool? = {
        guard let term = SquirrelInputController(server: nil, delegate: nil, client: nil) else { return nil }
        let termClient = SelftestTextClient()
        termClient.appBundleID = "com.apple.Terminal"
        term.activateServer(termClient)
        for _ in 0..<2 {
          _ = term.handle(flags(.shift), client: termClient); settle(0.02)
          _ = term.handle(flags([]), client: termClient); settle(0.05)
        }
        term.deactivateServer(termClient)
        controller.activateServer(client)
        return modeIn("com.apple.Notes")
      }()
      if carriedEnglish != true || carriedChinese != false || terminal != true || afterCapital != false
          || afterTerminal != false {
        wrong.append("the chosen mode reached another field as English \(String(describing: carriedEnglish)), "
                      + "as Chinese \(String(describing: carriedChinese)); Terminal \(String(describing: terminal)); "
                      + "after Shift+A elsewhere \(String(describing: afterCapital)); after switching in Terminal "
                      + "\(String(describing: afterTerminal))")
      }
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      hidePanel()
      // macOS activating the next field before deactivating this one: the late deactivate leaves the next field's badge
      // alone.
      if let other = SquirrelInputController(server: nil, delegate: nil, client: nil) {
        let otherClient = SelftestTextClient()
        other.activateServer(otherClient)
        let token = SquirrelInputController.aquaTypeAnnounceToken
        controller.deactivateServer(client)
        if SquirrelInputController.aquaTypeAnnounceToken != token {
          wrong.append("a late deactivate of the field left cancelled the badge of the field entered")
        }
        other.deactivateServer(otherClient)
        controller.activateServer(client)
      }
      // Not a key: the saved mode restored into a new session, and an option set from outside.
      controller.aquaTypeSelftestSetOption("ascii_mode", true)
      controller.aquaTypeSelftestRecreateSession()
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      _ = controller.handle(keyEscape, client: client)                         // a redraw with nothing composed
      settle(0.1)
      if panel?.isVisible == true { wrong.append("a badge appeared without a key switching the mode") }
      hidePanel()
      client.caretReportAlways = .zero
      send(shiftA, settleFor: 0.05)
      if panel?.isVisible == true { wrong.append("a badge was drawn against an empty caret report") }
      client.caretReportAlways = nil
      if wrong.isEmpty {
        print("type-selftest ok mode switch badge: shown at the caret for Shift+A and a bare Shift, not for a "
              + "restored or set mode, not without a usable caret; a Shift held through a click or a shortcut switches nothing")
      } else {
        for w in wrong { fail("mode switch badge", w) }
      }
      client.reset()
    }

    // The same chat box in an application that lays out after the fact (Chromium, VS Code): an answer given within
    // 0.15 s of a change describes the composition before it, so the render that follows the last key reads a field
    // that has not grown yet.  Once the field has caught up, the panel has to get there by itself, with no key pressed.
    do {
      clear("before the late layout case")
      client.lineUnits = 6
      client.growsUpward = true
      client.layoutLag = 0.15
      defer { client.lineUnits = nil; client.growsUpward = false; client.layoutLag = nil }
      for _ in 0..<4 { type("5j4u4"); settle(0.05) }
      _ = waitUntil(30) { controller.aquaTypeSelftestPanelIsCurrent }
      settle(0.6)
      let marked = client.markedRange()
      if let panel = NSApp.squirrelAppDelegate.panel, panel.isVisible, marked.location != NSNotFound, marked.length > 0 {
        let rows = Set((marked.location..<(marked.location + marked.length)).map { client.rect(forUnit: $0).minY })
        let covered = rows.sorted().filter { panel.frame.intersects(NSRect(x: 300, y: $0, width: 6 * 8, height: 18)) }
        if rows.count < 2 {
          fail("late layout", "the composition never wrapped, so nothing was tested")
        } else if !covered.isEmpty {
          fail("late layout", "the panel stayed over the composition's lines at y \(covered.map { Int($0) }) after the "
                            + "field caught up (\(rows.count) lines)")
        } else {
          print("type-selftest ok late layout: the panel moved off the \(rows.count)-line composition once the field "
                + "had laid it out, with no key pressed")
        }
      } else {
        fail("late layout", "no panel or no composition to check")
      }
      clear("after the late layout case")
    }

    if part == "all" { fieldMatrix(part: 0, of: 1) }

    // In MacVim (vim_mode), Escape and Control-C cancel a composition first, and with nothing composed they reach Vim
    // with the input method switched to English.  Everywhere else Escape leaves the mode alone.
    do {
      clear("before the Vim mode case")
      let keyCtrlC = keyDown("c", keyCode: 8, flags: .control)
      client.appBundleID = "org.vim.MacVim"
      send(keyEscape)                                                   // arriving in MacVim applies its app options
      var steps: [String] = []
      let failuresBefore = failures
      for (name, key) in [("Escape", keyEscape), ("Control-C", keyCtrlC)] {
        controller.aquaTypeSelftestSetOption("ascii_mode", false)       // the user has switched to Chinese
        type("5j4")
        _ = waitUntil(5) { !client.marked.isEmpty }
        send(key, settleFor: 0.2)
        let cancelled = client.marked.isEmpty && client.committed.isEmpty
        let stillChinese = !controller.aquaTypeSelftestOption("ascii_mode")
        let passed = !controller.handle(key, client: client)
        settle(0.1)
        let nowEnglish = controller.aquaTypeSelftestOption("ascii_mode")
        if !(cancelled && stillChinese && passed && nowEnglish) {
          fail("vim mode", "\(name): cancelled \(cancelled), still Chinese after cancelling \(stillChinese), "
                         + "passed to Vim \(passed), English after \(nowEnglish)")
        }
        steps.append(name)
        client.reset()
      }
      // Another app has a session of its own, with no Vim mode in it; this one controller stands in for it.
      client.appBundleID = "com.apple.TextEdit"
      controller.aquaTypeSelftestSetOption("vim_mode", false)
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      _ = controller.handle(keyEscape, client: client)
      if controller.aquaTypeSelftestOption("ascii_mode") {
        fail("vim mode", "Escape outside Vim switched the input method to English")
      } else if failures == failuresBefore {
        print("type-selftest ok vim mode: \(steps.joined(separator: " and ")) cancel a composition, then leave Vim "
              + "insert mode in English; Escape elsewhere leaves the mode alone")
      }
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      clear("after the Vim mode case")
    }

    // Committing a sentence piece by piece: a word for the leading syllables, then the rest decoded again and committed
    // the same way, until nothing is left - with the word reached by the arrow keys, by a click, and by Tab.
    do {
      clear("before the piece-by-piece case")
      let keyTab = keyDown("\t", keyCode: 48)
      type("rup wu0 wu0 fu4cp3cl3")                                     // 今天天氣很好
      var pieces: [String] = []
      for (step, how) in ["arrows", "click", "tab", "tab", "tab"].enumerated() {
        guard waitUntil(30, { controller.aquaTypeSelftestPanelIsCurrent }),
              let starts = controller.aquaTypeSelftestGroupStarts else { break }
        let before = client.committed
        let keysBefore = controller.aquaTypeSelftestComposingKeyCount
        let target = starts.count > 1 ? starts[1] : 0
        switch how {
        case "arrows": for _ in 0..<target { send(keyDownArrow, settleFor: 0.02) }; send(keyReturn, settleFor: 0.4)
        case "click": _ = controller.selectCandidate(target); settle(0.4)
        default: if target > 0 { send(keyTab, settleFor: 0.15) }; send(keyReturn, settleFor: 0.4)
        }
        guard client.committed.count > before.count else {
          fail("piece by piece", "step \(step + 1) (\(how)) committed nothing with \(keysBefore) keys composed")
          break
        }
        pieces.append("\(client.committed.dropFirst(before.count)) (\(how))")
        let keysAfter = controller.aquaTypeSelftestComposingKeyCount
        if keysAfter == 0 { break }
        if step < 2, target > 0, keysAfter >= keysBefore {
          fail("piece by piece", "step \(step + 1) (\(how)) left \(keysAfter) of \(keysBefore) keys")
          break
        }
      }
      if pieces.count >= 3, controller.aquaTypeSelftestComposingKeyCount == 0, isHan(client.committed) {
        print("type-selftest ok piece by piece: \(pieces.joined(separator: " + ")) = \(client.committed)")
      } else {
        fail("piece by piece", "committed \(pieces), \(controller.aquaTypeSelftestComposingKeyCount) keys left")
      }
      clear("after the piece-by-piece case")
    }

    // A commit that comes before the engine's answer - focus leaving mid-sentence, straight after the last key - takes
    // librime's own conversion of the keys, composed at that moment, rather than the bopomofo.
    do {
      clear("before the early commit case")
      for ch in "5j4u4" { _ = controller.handle(keyDown(String(ch)), client: client) }
      controller.commitComposition(client)
      settle(0.3)
      if isHan(client.committed), client.marked.isEmpty {
        print("type-selftest ok early commit: \(client.committed) before the engine answered")
      } else {
        fail("early commit", "committed \(client.committed.debugDescription), still marked \(client.marked.debugDescription)")
      }
      // One syllable, where librime's own order is the whole answer: ㄧㄡˇ is 有, not the rare character an unweighted
      // dictionary puts first.
      clear("before the early single-syllable commit")
      for ch in "u.3" { _ = controller.handle(keyDown(String(ch)), client: client) }
      controller.commitComposition(client)
      settle(0.3)
      if client.committed == "有" {
        print("type-selftest ok early commit of one syllable: ㄧㄡˇ -> 有")
      } else {
        fail("early commit", "ㄧㄡˇ committed \(client.committed.debugDescription) before the engine answered, not 有")
      }
      clear("after the early commit case")
    }

    // Tab moves the highlight to the first candidate of the next group - the sentences, the words for the leading
    // syllables, the first syllable's characters - and from the last group back to the first candidate.  Shift+Tab
    // does nothing while composing, and neither key reaches the document.
    do {
      clear("before the Tab case")
      let keyTab = keyDown("\t", keyCode: 48)
      let keyShiftTab = keyDown("\t", keyCode: 48, flags: .shift)
      type("5j4u4 z96u84 g4 284j/6 ru04")
      if !waitUntil(30, { controller.aquaTypeSelftestPanelIsCurrent }) {
        fail("tab", "no answer for the typed keys arrived")
      } else if let starts = controller.aquaTypeSelftestGroupStarts, starts.count == 3 {
        var visited: [Int] = []
        for _ in 0..<3 {
          send(keyTab, settleFor: 0.15)
          visited.append(controller.aquaTypeSelftestHighlighted)
        }
        let markedBefore = client.marked
        send(keyShiftTab, settleFor: 0.15)
        let afterShiftTab = controller.aquaTypeSelftestHighlighted
        if visited != [starts[1], starts[2], 0] {
          fail("tab", "three presses highlighted \(visited), expected \([starts[1], starts[2], 0]) (groups start at \(starts))")
        } else if afterShiftTab != 0 || client.marked != markedBefore {
          fail("tab", "Shift+Tab changed something: highlighted \(afterShiftTab), marked \(client.marked.debugDescription)")
        } else if client.committed.contains("\t") || !client.committed.isEmpty {
          fail("tab", "a Tab reached the document: \(client.committed.debugDescription)")
        } else {
          print("type-selftest ok tab: groups start at \(starts), Tab visited \(visited), Shift+Tab did nothing")
        }
      } else {
        fail("tab", "expected three groups (sentences, words, characters), found \(controller.aquaTypeSelftestGroupStarts.map { "\($0)" } ?? "none")")
      }
      clear("after the Tab case")
    }

    // The engine crashing, and the engine hanging: the input method carries on (a write to the dead engine's pipe must
    // not take it down), and candidates come back from a newly started engine each time, with no other key pressed.
    do {
      clear("before the engine failure case")
      for (what, signal) in [("crashed", SIGKILL), ("hung", SIGSTOP)] {
        guard let pid = controller.aquaTypeSelftestEnginePID else { fail("engine \(what)", "no engine running"); break }
        kill(pid, signal)
        type("5j4u4")
        let back = waitUntil(20) { controller.aquaTypeSelftestPanelIsCurrent }
        let now = controller.aquaTypeSelftestEnginePID
        if signal == SIGSTOP { kill(pid, SIGKILL) }
        if back, let now, now != pid {
          print("type-selftest ok engine \(what): candidates came back from a new engine")
        } else {
          fail("engine \(what)", "candidates \(back ? "came back" : "never came back"), engine \(now.map(String.init) ?? "none") "
                                  + "(was \(pid))")
        }
        clear("after the engine \(what) case")
      }
    }

    // Another process selecting a different input source (macOS 26 then never calls deactivateServer): the composition
    // ends as it would on leaving the field.  AquaType's own source being selected changes nothing.
    do {
      clear("before the input source case")
      client.reset()
      type("5j4u4")
      _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
      NSApp.squirrelAppDelegate.inputSourceChanged(to: (Bundle.main.bundleIdentifier ?? "") + ".Hant")
      let keptForOwn = controller.aquaTypeSelftestComposingKeyCount > 0 && client.committed.isEmpty
      NSApp.squirrelAppDelegate.inputSourceChanged(to: "com.apple.keylayout.ABC")
      let ended = controller.aquaTypeSelftestComposingKeyCount == 0 && isHan(client.committed)
      controller.activateServer(client)
      if !keptForOwn || !ended {
        fail("input source", "AquaType's own source left the composition alone: \(keptForOwn); another one committed it: "
                             + "\(ended) (\(client.committed.debugDescription))")
      } else {
        print("type-selftest ok input source: switching away commits the composition, AquaType's own source does not")
      }
      clear("after the input source case")
    }

    // Editing chords during a composition edit it: Option+Backspace takes all of it back and Control+H one key, with
    // nothing committed.  A Command chord commits it first and goes to the application after it.
    do {
      clear("before the editing chords case")
      client.reset()
      var wrong: [String] = []
      type("5j4u4")
      _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
      send(keyDown("\u{8}", keyCode: 51, flags: .option), settleFor: 0.1)
      if controller.aquaTypeSelftestComposingKeyCount != 0 || !client.committed.isEmpty {
        wrong.append("Option+Backspace left \(controller.aquaTypeSelftestComposingKeyCount) keys and committed "
                     + client.committed.debugDescription)
      }
      clear()
      type("5j4u4")
      send(keyDelete, settleFor: 0.1)
      let afterBackspace = client.marked
      clear()
      type("5j4u4")
      send(keyDown("\u{8}", keyCode: 4, flags: .control), settleFor: 0.1)
      if client.marked != afterBackspace || afterBackspace.isEmpty || !client.committed.isEmpty {
        wrong.append("Control+H left \(client.marked.debugDescription) marked (Backspace leaves "
                     + "\(afterBackspace.debugDescription)) and committed \(client.committed.debugDescription)")
      }
      clear()
      client.reset()
      type("5j4u4")
      _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
      let memory = URL(fileURLWithPath: harness.stateDir).appending(component: "personal_memory.v2.tsv")
      let memoryBefore = (try? Data(contentsOf: memory)) ?? Data()
      let pastePassed = !controller.handle(keyDown("v", keyCode: 9, flags: .command), client: client)
      if !pastePassed || !isHan(client.committed) || !client.marked.isEmpty {
        wrong.append("Command+V went on: \(pastePassed); \(client.committed.debugDescription) committed, "
                     + "\(client.marked.debugDescription) still marked")
      } else if waitUntil(1, { ((try? Data(contentsOf: memory)) ?? Data()) != memoryBefore }) {
        wrong.append("what Command+V committed was remembered")
      }
      clear("after the editing chords case")
      if wrong.isEmpty {
        print("type-selftest ok editing chords: Option+Backspace takes all back, Control+H one key, Command+V commits first")
      } else {
        wrong.forEach { fail("editing chords", $0) }
      }
    }

    // A click during a composition in Chromium (Chrome, VS Code, Slack...) or Firefox: the application keeps the bopomofo
    // the field shows as typed text and ends the composition itself, with the caret moved to where the click went.  What
    // the composition commits replaces the bopomofo, whether the input method is then told (commitComposition) or only
    // sees the click, wherever the caret now is; and where the bopomofo is no longer there as it was, nothing is
    // replaced and the composition simply ends, so the next key does not bring it back.
    do {
      clear("before the click in Chromium case")
      var wrong: [String] = []
      // Chromium may not report its length: the bopomofo found is replaced all the same.
      for (told, moved, length) in [(true, false, true), (false, true, true), (true, false, false)] {
        client.reset()
        client.reportsLength = length
        client.type("前文")
        type("5j4u4")
        _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
        client.takeMarkedAsTyped()
        if moved { client.caretFromEnd = client.committed.utf16.count }            // clicked at the start of the text
        if told { controller.commitComposition(client) } else { controller.aquaTypeReclaimTakenEcho() }
        if client.committed != "前文注意" || controller.aquaTypeSelftestComposingKeyCount > 0 {
          wrong.append("\(told ? "told" : "not told")\(moved ? ", caret moved" : "")\(length ? "" : ", no length"): the document has "
                       + "\(client.committed.debugDescription), \(controller.aquaTypeSelftestComposingKeyCount) keys still composing")
        }
        clear()
      }
      client.reset()
      type("5j4u4")
      _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
      client.takeMarkedAsTyped()
      client.replaceAll(with: "ㄓㄨˋ ㄧ")                                   // not the echo, anywhere
      let ended = controller.aquaTypeReclaimTakenEcho()
      if !ended || client.committed != "ㄓㄨˋ ㄧ" || controller.aquaTypeSelftestComposingKeyCount > 0 {
        wrong.append("with the echo gone the document became \(client.committed.debugDescription) and "
                     + "\(controller.aquaTypeSelftestComposingKeyCount) keys are still composing")
      }
      clear("after the click in Chromium case")
      if wrong.isEmpty {
        print("type-selftest ok click in Chromium: the bopomofo it kept is replaced, told or not, caret moved or not, length or not; "
              + "where it is gone the composition just ends")
      } else {
        wrong.forEach { fail("click in Chromium", $0) }
      }
    }

    // A keystroke does not wait for the application: while it is handled, the input method sets the marked text and
    // asks nothing back (a question right after setting it waits until the application has laid it out).  It asks
    // where the echo starts afterwards, once, and a click still finds the echo.
    do {
      clear("before the keystroke-does-not-wait case")
      client.reset()
      type("5j4")
      _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
      settle(0.1)
      let before = client.markedRangeCalls
      _ = controller.handle(keyDown("u"), client: client)
      _ = controller.handle(keyDown("4"), client: client)
      let during = client.markedRangeCalls - before
      settle(0.2)
      let after = client.markedRangeCalls - before
      clear("after the keystroke-does-not-wait case")
      if during != 0 || after == 0 {
        fail("keystroke waits", "asked where the marked text is \(during) times while two keys were handled, \(after) by "
                                + "the time they had been")
      } else {
        print("type-selftest ok keystroke does not wait: nothing asked of the application while a key is handled, "
              + "\(after) question(s) after")
      }
    }

    // A key nothing in AquaType takes, pressed while a composition shows (the number pad's 1): the composition goes into
    // the document first and the key to the application after it, instead of the key arriving with bopomofo marked.
    do {
      clear("before the number pad case")
      client.reset()
      type("5j4u4")
      _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
      let passedOn = !controller.handle(keyDown("1", keyCode: 83, flags: .numericPad), client: client)
      settle(0.1)
      if !passedOn || !isHan(client.committed) || !client.marked.isEmpty {
        fail("number pad", "the key went on to the application: \(passedOn); \(client.committed.debugDescription) "
                           + "committed, \(client.marked.debugDescription) still marked")
      } else {
        print("type-selftest ok number pad: \(client.committed) committed first, the key passed on after it")
      }
      clear("after the number pad case")
    }

    // No engine can be started (missing assets, or the backoff after it kept exiting): Return commits what librime made
    // of the keys, instead of asking again on every press while the composition stays.
    do {
      clear("before the unavailable engine case")
      client.reset()
      controller.aquaTypeSelftestSetEngineUnavailable(true)
      _ = waitUntil(3) { controller.aquaTypeSelftestEnginePID == nil }
      type("5j4u4")
      settle(0.3)
      // A modifier pressed and released before any answer leaves the bopomofo in the document.
      for flags in [NSEvent.ModifierFlags.option, []] {
        send(NSEvent.keyEvent(with: .flagsChanged, location: .zero, modifierFlags: flags,
                              timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: 0, context: nil,
                              characters: "", charactersIgnoringModifiers: "", isARepeat: false, keyCode: 58)!,
             settleFor: 0.05)
      }
      let markedAfterModifier = client.marked
      // No candidates: an arrow has nothing to move and stays out of the application.
      let arrowKept = controller.handle(keyDown("\u{F702}", keyCode: 123), client: client)
      send(keyReturn, settleFor: 0.2)
      let committed = client.committed, left = controller.aquaTypeSelftestComposingKeyCount
      controller.aquaTypeSelftestSetEngineUnavailable(false)
      if markedAfterModifier != "ㄓㄨˋ ㄧˋ" {
        fail("unavailable engine", "Option pressed and released left \(markedAfterModifier.debugDescription) marked")
      } else if !arrowKept {
        fail("unavailable engine", "Left reached the application while the composition had no candidates")
      } else if committed.isEmpty || left > 0 || !isHan(committed) {          // librime's conversion, not the bopomofo
        fail("unavailable engine", "Return committed \(committed.debugDescription) and left \(left) keys composing")
      } else {
        print("type-selftest ok unavailable engine: Left kept, Return committed \(committed) from librime")
      }
      type("5j4u4")
      if !waitUntil(20, { controller.aquaTypeSelftestPanelIsCurrent }) {
        fail("unavailable engine", "no candidates once the engine could be started again")
      }
      clear("after the unavailable engine case")
    }

    // Punctuation takes the whole input.  With the highlight moved by Tab to a word that takes only the leading keys,
    // the whole-input candidate is what goes in before the punctuation, and no key is left behind or lost.
    do {
      clear("before the Tab and punctuation case")
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      type("rup wu0 wu0 fu4cp3cl3")                                     // 今天天氣很好: six syllables
      _ = waitUntil(30) { controller.aquaTypeSelftestPanelIsCurrent }
      send(keyDown("\t", keyCode: 48), settleFor: 0.15)                 // to the words for the leading syllables
      send(NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: .shift,
                            timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: 0, context: nil,
                            characters: "<", charactersIgnoringModifiers: "<", isARepeat: false, keyCode: 43)!,
           settleFor: 0.3)
      let text = client.committed
      if text.hasSuffix("，"), text.count == 7, isHan(String(text.dropLast())), client.marked.isEmpty,
         controller.aquaTypeSelftestComposingKeyCount == 0 {
        print("type-selftest ok tab then punctuation: \(text) - the whole input, not the highlighted word alone")
      } else {
        fail("tab then punctuation", "committed \(text.debugDescription) with \(client.marked.debugDescription) still "
                                    + "marked and \(controller.aquaTypeSelftestComposingKeyCount) keys composing")
      }
      clear("after the Tab and punctuation case")
    }

    // 11. Punctuation, against what macOS's built-in 注音 input method typed for the same keys, in the same order
    //     (the quotes alternate, so the order is part of the expectation), recorded by hand from that input method.
    do {
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      client.reset()
      // (key code, the key's character, its character with Shift) on a US keyboard
      let keys: [(UInt16, String, String)] = [
        (50, "`", "~"), (18, "1", "!"), (19, "2", "@"), (20, "3", "#"), (21, "4", "$"), (23, "5", "%"), (22, "6", "^"),
        (26, "7", "&"), (28, "8", "*"), (25, "9", "("), (29, "0", ")"), (27, "-", "_"), (24, "=", "+"), (33, "[", "{"),
        (30, "]", "}"), (42, "\\", "|"), (41, ";", ":"), (39, "'", "\""), (43, ",", "<"), (47, ".", ">"), (44, "/", "?"),
      ]
      func press(_ key: (UInt16, String, String), _ flags: NSEvent.ModifierFlags) {
        let shifted = flags.contains(.shift) ? key.2 : key.1
        send(NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags,
                              timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: 0, context: nil,
                              characters: shifted, charactersIgnoringModifiers: shifted, isARepeat: false,
                              keyCode: key.0)!, settleFor: 0.02)
      }
      let letterA: (UInt16, String, String) = (0, "a", "A")
      let apostrophe = keys[17]
      let lines: [(String, () -> Void, String)] = [
        ("Shift", { keys.forEach { press($0, .shift) } }, "～！＠＃＄％＾＆＊（）＿＋『』｜：“，。？"),
        ("Option", { keys.forEach { press($0, .option) }; press(letterA, .option) }, "｀１２３４５６７８９０—＝「」、；‘，．／ａ"),
        ("Shift+Option", { keys.forEach { press($0, [.shift, .option]) } }, ""),
        ("plain", { [0, 12, 13, 14, 15, 17].forEach { press(keys[$0], []) } }, "·＝「」、’"),
        ("Shift+' four times", { for _ in 0..<4 { press(apostrophe, .shift) } }, "”“”“"),
        ("' four times", { for _ in 0..<4 { press(apostrophe, []) } }, "‘’‘’"),
        ("Option+' four times", { for _ in 0..<4 { press(apostrophe, .option) } }, "‘’‘’"),
      ]
      var matched = 0
      for (name, typeLine, expected) in lines {
        client.reset()
        typeLine()
        if client.committed != expected || !client.marked.isEmpty {
          fail("punctuation", "\(name): typed \(client.committed.debugDescription), macOS's 注音 types \(expected.debugDescription)")
        } else {
          matched += 1
        }
      }
      // Mid-composition: the candidate is committed, then the punctuation.
      for (name, key, flags, mark) in [("Shift+,", keys[18], NSEvent.ModifierFlags.shift, "，"),
                                        ("Option+;", keys[16], NSEvent.ModifierFlags.option, "；")] {
        client.reset()
        type("5j4")
        _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
        press(key, flags)
        let chars = Array(client.committed)
        if chars.count != 2 || String(chars[1]) != mark || !chars[0].unicodeScalars.allSatisfy({ (0x3400...0x9FFF).contains($0.value) || (0x20000...0x3134F).contains($0.value) }) || !client.marked.isEmpty {
          fail("punctuation", "ㄓㄨˋ then \(name) committed \(client.committed.debugDescription), expected a character then \(mark)")
        } else {
          matched += 1
        }
      }
      if matched == lines.count + 2 {
        print("type-selftest ok punctuation: \(lines.count) key sequences match macOS's 注音, and punctuation commits the composition first")
      }
      clear("after the punctuation case")
    }

    // 12. Caps Lock on in Chinese mode types what the keys say (A123), committing a composition first; Shift+Option with
    //     a letter is an application's shortcut and goes to it.
    do {
      clear("before the Caps Lock case")
      controller.aquaTypeSelftestSetOption("ascii_mode", false)
      func key(_ chars: String, _ code: UInt16, _ flags: NSEvent.ModifierFlags, ignoring: String? = nil) -> NSEvent {
        NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags,
                         timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: 0, context: nil,
                         characters: chars, charactersIgnoringModifiers: ignoring ?? chars, isARepeat: false, keyCode: code)!
      }
      let alone = [key("A", 0, .capsLock), key("1", 18, .capsLock), key("2", 19, .capsLock)]
        .map { controller.handle($0, client: client) }
      let aloneLeft = controller.aquaTypeSelftestComposingKeyCount
      type("5j4u4")
      _ = waitUntil(10) { controller.aquaTypeSelftestPanelIsCurrent }
      let midway = controller.handle(key("3", 20, .capsLock), client: client)
      settle(0.1)
      let midwayText = client.committed, midwayLeft = controller.aquaTypeSelftestComposingKeyCount
      clear()
      let shortcut = controller.handle(key("Ï", 3, [.shift, .option], ignoring: "F"), client: client)
      var wrong: [String] = []
      if alone != [false, false, false] || aloneLeft != 0 {
        wrong.append("A12 with Caps Lock: handled \(alone), \(aloneLeft) keys composing")
      }
      if midway || !isHan(midwayText) || midwayLeft != 0 {
        wrong.append("Caps Lock 3 mid-composition: handled \(midway), committed \(midwayText.debugDescription), \(midwayLeft) keys")
      }
      if shortcut { wrong.append("Shift+Option+F was kept from the application") }
      if wrong.isEmpty {
        print("type-selftest ok Caps Lock: A12 went to the application, a composition was committed first; Shift+Option+F went on")
      } else {
        wrong.forEach { fail("Caps Lock", $0) }
      }
      clear("after the Caps Lock case")
    }

    return finish()
  }

  /// One case for the panel: the preedit and candidates the controller would hand it.
  private struct RenderCase {
    let name: String
    let preedit: String
    let candidates: [String]
    let dark: Bool
    var labels: [String]? = nil
  }

  /// Lays the candidate panel out over a handful of real cases and reports what came back.
  ///
  /// Every case is one the product produces.  The long sentence is the 190-key cap; the rare characters are what the
  /// engine's character table can return for a partial commit (U+29xxx and up, which no bundled font covers); the
  /// bare preedit is the first keystroke of every sentence; and each is drawn light and dark because the two themes
  /// are loaded separately.
  ///
  /// Geometry is checked rather than appearance: NaN-sized, zero-sized or larger than the screen are the shapes the
  /// layout arithmetic produces when it goes wrong.  Drawing is checked only for "more than one colour came out",
  /// which is the difference between a panel and an empty rectangle.
  static func renderSelftest(userDir: String) -> Bool {
    let rimeAPI: RimeApi_stdbool = rime_get_api_stdbool().pointee
    guard let sharedSupport = Bundle.main.sharedSupportPath else {
      fputs("render-selftest: the bundle has no SharedSupport directory, so Rime has no shared data to load.\n", stderr)
      return false
    }
    var traits = RimeTraits.rimeStructInit()
    traits.setCString(sharedSupport, to: \.shared_data_dir)
    traits.setCString(userDir, to: \.user_data_dir)
    let renderLogs = NSTemporaryDirectory() + "rime.squirrel-render-selftest-\(ProcessInfo.processInfo.processIdentifier)"
    try? FileManager.default.createDirectory(atPath: renderLogs, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(atPath: renderLogs) }
    traits.setCString(renderLogs, to: \.log_dir)
    traits.setCString("AquaType", to: \.distribution_code_name)
    traits.setCString("AquaType", to: \.distribution_name)
    traits.setCString("rime.squirrel-render-selftest", to: \.app_name)
    rimeAPI.setup(&traits)
    rimeAPI.initialize(nil)
    _ = rimeAPI.start_maintenance(false)

    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    // Without a screen nothing can be laid out, so this counts as a failure, not a pass.
    guard let screen = NSScreen.main else {
      fputs("render-selftest: no screen - this needs a window server session (run it as the logged-in user).\n", stderr)
      return false
    }

    let config = SquirrelConfig()
    guard config.openBaseConfig() else {
      fputs("render-selftest: squirrel.yaml could not be opened from \(userDir)/build - the panel would silently "
            + "fall back to built-in defaults instead of the configured appearance.\n", stderr)
      return false
    }

    let rare = ["𩍲", "𩍵", "𩙾", "𪂅", "𪉆", "𪒊", "㐀", "㴝"]
    let short = ["注意", "助益", "注射", "註意", "著意", "註譯", "助役", "駐驛", "注譯"]
    // 63 characters, about what the 190-key cap allows.
    let long = String(repeating: "這是一個很長的句子", count: 7)
    var cases: [RenderCase] = []
    for dark in [false, true] {
      cases.append(RenderCase(name: "nine candidates", preedit: "ㄓㄨˋㄧˋ", candidates: short, dark: dark))
      cases.append(RenderCase(name: "one long sentence", preedit: String(repeating: "ㄓㄨˋ", count: 20), candidates: [long], dark: dark))
      cases.append(RenderCase(name: "rare characters", preedit: "ㄍㄜ", candidates: rare, dark: dark))
      // One page can mix one-, two- and three-digit labels; the candidates must still line up.
      cases.append(RenderCase(name: "wide labels", preedit: "ㄓㄨˋ", candidates: short, dark: dark,
                              labels: ["8", "9", "10", "11", "99", "100", "101", "102", "103"]))
      cases.append(RenderCase(name: "preedit only", preedit: "ㄅ", candidates: [], dark: dark))
      cases.append(RenderCase(name: "empty", preedit: "", candidates: [], dark: dark))
    }

    // The panel chooses light or dark from the system setting, not from what it is told, so the test overrides that
    // setting for this process only, in the volatile argument domain: never in the application's own preferences,
    // which are the installed input method's too (a "Dark" left there would keep its panel dark in light mode).  What is
    // checked is that the panel lays out and draws in both, not that the two themes look different.
    let appearanceKey = "AppleInterfaceStyle"
    let argumentsBefore = UserDefaults.standard.volatileDomain(forName: UserDefaults.argumentDomain)
    func appearance(_ style: String) {
      var arguments = argumentsBefore
      arguments[appearanceKey] = style
      UserDefaults.standard.setVolatileDomain(arguments, forName: UserDefaults.argumentDomain)
    }
    defer { UserDefaults.standard.setVolatileDomain(argumentsBefore, forName: UserDefaults.argumentDomain) }

    var failures = 0
    var sizes: [String: NSSize] = [:]
    var alignment: [String: CGFloat] = [:]
    for testCase in cases {
      let label = "\(testCase.name)\(testCase.dark ? " (dark)" : "")"
      appearance(testCase.dark ? "Dark" : "Light")
      let panel = SquirrelPanel(position: NSRect(x: screen.frame.midX, y: screen.frame.midY, width: 1, height: 1))
      // The panel is shown on whatever screen is attached while it is measured.  `cacheDisplay` below draws the view
      // hierarchy directly, so what is measured does not depend on that screen.
      panel.load(config: config, forDarkMode: false)
      panel.load(config: config, forDarkMode: true)
      panel.aquaTypeCompositionPanelVisible = !testCase.candidates.isEmpty
      let selectionRange = NSRange(location: 0, length: testCase.preedit.utf16.count)
      panel.update(preedit: testCase.preedit,
                   selRange: selectionRange,
                   caretPos: testCase.preedit.utf16.count,
                   candidates: testCase.candidates,
                   comments: Array(repeating: "", count: testCase.candidates.count),
                   labels: testCase.labels ?? (1...9).map(String.init),
                   highlighted: 0,
                   page: 0,
                   lastPage: true,
                   update: true)

      // Nothing to draw is the correct answer for the last case, and the panel says so by staying hidden.
      let shouldShow = !(testCase.preedit.isEmpty && testCase.candidates.isEmpty)
      if !shouldShow {
        if panel.isVisible {
          fputs("render-selftest FAIL \(label): the panel is visible with nothing to show\n", stderr)
          failures += 1
        } else {
          print("render-selftest ok \(label): hidden, as it should be")
        }
        panel.hide(force: true)
        continue
      }

      let frame = panel.frame
      var problems: [String] = []
      if !frame.width.isFinite || !frame.height.isFinite || !frame.origin.x.isFinite || !frame.origin.y.isFinite {
        problems.append("frame is not finite: \(frame)")
      } else {
        if frame.width < 1 || frame.height < 1 { problems.append("frame is degenerate: \(frame)") }
        if frame.width > screen.frame.width || frame.height > screen.frame.height {
          problems.append("frame is larger than the screen: \(frame) vs \(screen.frame)")
        }
      }
      if problems.isEmpty, let content = panel.contentView {
        content.displayIfNeeded()
        let bounds = content.bounds
        if let rep = content.bitmapImageRepForCachingDisplay(in: bounds) {
          content.cacheDisplay(in: bounds, to: rep)
          // AQUATYPE_RENDER_DUMP=<dir> saves each drawn panel as a PNG to look at.  Off unless set; the build never
          // uses it.
          if let dumpDir = ProcessInfo.processInfo.environment["AQUATYPE_RENDER_DUMP"] {
            let name = label.replacingOccurrences(of: " ", with: "_")
              .replacingOccurrences(of: "(", with: "").replacingOccurrences(of: ")", with: "")
            try? FileManager.default.createDirectory(atPath: dumpDir, withIntermediateDirectories: true)
            if let png = rep.representation(using: .png, properties: [:]) {
              try? png.write(to: URL(fileURLWithPath: dumpDir).appendingPathComponent("\(name).png"))
            }
          }
          if distinctPixelCount(rep) < 2 { problems.append("drew a single flat colour over \(bounds)") }
        } else {
          problems.append("no bitmap could be made for \(bounds)")
        }
      }

      // Are the candidates in a column?  The numbers down the left are 1, 2, ... into the hundreds, and they differ
      // in width by a factor of four, so a panel that simply concatenates "label + candidate" steps each row right by
      // a different amount.  The origins come from the laid-out text, not from the indents that were requested.
      if problems.isEmpty, testCase.candidates.count > 1 {
        let origins = panel.aquaTypeSelftestCandidateOrigins
        if origins.count != testCase.candidates.count {
          problems.append("measured \(origins.count) candidate origins for \(testCase.candidates.count) candidates")
        } else if let lo = origins.min(), let hi = origins.max() {
          let spread = hi - lo
          // A quarter of a point: below what a display can show, and well under the ~4pt step one extra digit costs.
          if spread > 0.25 {
            let worst = origins.enumerated().max(by: { abs($0.element - lo) < abs($1.element - lo) })
            problems.append("candidates are not aligned: they start across \(String(format: "%.2f", spread))pt "
                            + "(candidate \((worst?.offset ?? 0) + 1) at \(String(format: "%.2f", worst?.element ?? 0)), "
                            + "leftmost at \(String(format: "%.2f", lo)))")
          } else {
            alignment[label] = spread
          }
        }
      }

      if problems.isEmpty {
        sizes[label] = frame.size
        print("render-selftest ok \(label): \(Int(frame.width))x\(Int(frame.height))")
      } else {
        for problem in problems { fputs("render-selftest FAIL \(label): \(problem)\n", stderr) }
        failures += 1
      }
      panel.hide(force: true)
    }

    // Every check above is about one panel on its own, and a collapsed layout passes all of them (with
    // `SquirrelView.contentRect` returning `.zero`, all four shapes come out 46x42, finite and drawn).  What separates
    // a working layout is that the sizes DIFFER: nine rows are taller than none, a 63-character sentence is wider
    // than one bopomofo syllable.  Those hold for any theme and any font, which an absolute threshold would not.
    for suffix in ["", " (dark)"] {
      guard let nine = sizes["nine candidates" + suffix],
            let long = sizes["one long sentence" + suffix],
            let rare = sizes["rare characters" + suffix],
            let bare = sizes["preedit only" + suffix] else { continue }
      var collapsed: [String] = []
      if nine.height <= bare.height {
        collapsed.append("nine candidates (\(Int(nine.height))) is no taller than a bare preedit (\(Int(bare.height)))")
      }
      if long.width <= bare.width {
        collapsed.append("a 63-character sentence (\(Int(long.width))) is no wider than one syllable (\(Int(bare.width)))")
      }
      if rare.height <= bare.height {
        collapsed.append("eight characters (\(Int(rare.height))) is no taller than a bare preedit (\(Int(bare.height)))")
      }
      if Set([nine, long, rare, bare].map { "\(Int($0.width))x\(Int($0.height))" }).count < 3 {
        collapsed.append("four different candidate sets produced fewer than three different panel sizes")
      }
      for problem in collapsed { fputs("render-selftest FAIL layout\(suffix): \(problem)\n", stderr) }
      failures += collapsed.count
    }

    if failures > 0 {
      fputs("render-selftest: \(failures) problems across \(cases.count) cases\n", stderr)
      return false
    }
    if let worst = alignment.max(by: { $0.value < $1.value }) {
      print("render-selftest ok alignment: candidates share a left edge in all \(alignment.count) multi-candidate "
            + "cases (worst spread \(String(format: "%.3f", worst.value))pt, in \(worst.key))")
    }
    print("render-selftest ok - \(cases.count) cases")
    return true
  }

  /// How many different pixels a drawn bitmap has, giving up once it has seen enough to answer the only question
  /// asked of it: more than one, i.e. something other than a flat fill was drawn.
  private static func distinctPixelCount(_ rep: NSBitmapImageRep, limit: Int = 8) -> Int {
    guard let data = rep.bitmapData, rep.bitsPerSample == 8 else { return limit }
    let samples = rep.samplesPerPixel
    guard samples >= 1 else { return limit }
    var seen = Set<UInt64>()
    for y in 0..<rep.pixelsHigh {
      let row = data + y * rep.bytesPerRow
      for x in 0..<rep.pixelsWide {
        let pixel = row + x * (rep.bitsPerPixel / 8)
        var key: UInt64 = 0
        for sample in 0..<min(samples, 8) { key = key << 8 | UInt64(pixel[sample]) }
        seen.insert(key)
        if seen.count >= limit { return seen.count }
      }
    }
    return seen.count
  }

  static let helpDoc = """
Usage: Squirrel [option]
  --quit                     quit every running AquaType process
  --reload                   rebuild and reload the configuration, e.g. after editing ~/.aquatype/rime
                             (an upgrade replaces that folder with the new version's files)
  --build [user] [shared] [out]
                             build the configuration (user and shared default to the current directory,
                             out to user/build)
  --render-selftest [dir]    lay the candidate panel out over the product's real cases and report what came back
  --type-selftest [dir]      type through the real input controller and engine, and report what got committed
  --typing-smoothness [dir]  type at human speed and report how often the candidate panel changes shape
  --print-user-dir           print the Rime user directory this build uses
  --print-shared-support-dir print the bundled shared support directory
  --help                     print this list

AquaType is installed with AquaType-<version>.pkg and removed with AquaType-<version>-Uninstall.pkg.
"""
}
