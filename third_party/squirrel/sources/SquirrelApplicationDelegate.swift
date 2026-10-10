//
//  SquirrelApplicationDelegate.swift
//  Squirrel
//
//  Created by Leo Liu on 5/6/24.
//  Modified for AquaType by Dio Hsieh, 2026.
//

import UserNotifications
import AppKit
import Carbon
import Darwin
import InputMethodKit

final class SquirrelApplicationDelegate: NSObject, NSApplicationDelegate {
  /// Distinct from Squirrel's own, so that a machine with both installed does not have one replace the other's
  /// notifications - UNNotificationRequest replaces any existing request with the same identifier.
  static let notificationIdentifier = "AquaTypeNotification"

  let rimeAPI: RimeApi_stdbool = rime_get_api_stdbool().pointee
  /// A stand-in returned by `NSApp.squirrelAppDelegate` while the real delegate is not installed (before launch,
  /// during teardown).  Its `panel` and `config` are nil, so panel updates through it do nothing.  Never call the
  /// methods that drive Rime on it (`setupRime`, `startRime`, `deploy`, `loadSettings`): `deploy`
  /// alone would shut down the engine the real delegate is using.
  static let detached = SquirrelApplicationDelegate()

  var config: SquirrelConfig?
  var panel: SquirrelPanel?

  func applicationWillFinishLaunching(_ notification: Notification) {
    panel = SquirrelPanel(position: .zero)
    addObservers()
  }

  /// The tokens `addObservers()` got back: an observer added with a closure can only be removed with its token.
  /// Removing them at quit keeps a late reload request from restarting Rime in a process that is shutting down.
  private var distributedObservers: [NSObjectProtocol] = []

  /// Counts librime shutdowns.  A session id is the address of its session object, so after a shutdown a new session
  /// can get the id an old, destroyed one had; a controller keeps the generation its session was made in and treats a
  /// session from an earlier one as gone (SquirrelInputController.aquaTypeSessionAlive).
  static private(set) var rimeGeneration: UInt64 = 0

  func applicationWillTerminate(_ notification: Notification) {
    let notifCenter = DistributedNotificationCenter.default()
    distributedObservers.forEach(notifCenter.removeObserver)
    distributedObservers.removeAll()

    panel?.hide()
  }

  func deploy() {
    print("Start maintenance...")
    self.shutdownRime()
    self.startRime(fullCheck: true)
    self.loadSettings()
  }

  func openLogFolder() {
    NSWorkspace.shared.open(SquirrelApp.logDir)
  }

  func openRimeFolder() {
    NSWorkspace.shared.open(SquirrelApp.userDir)
  }

  static func showMessage(msgText: String?) {
    let center = UNUserNotificationCenter.current()
    // The settings are read only once the authorization request has finished, so the first message is not checked
    // against permissions that have not been granted yet.
    center.requestAuthorization(options: [.alert, .provisional]) { _, error in
      if let error = error {
        print("User notification authorization error: \(error.localizedDescription)")
      }
      center.getNotificationSettings { settings in
        if (settings.authorizationStatus == .authorized || settings.authorizationStatus == .provisional) && settings.alertSetting == .enabled {
          let content = UNMutableNotificationContent()
          content.title = NSLocalizedString("AquaType", comment: "")
          if let msgText = msgText {
            content.subtitle = msgText
          }
          content.interruptionLevel = .active
          let request = UNNotificationRequest(identifier: Self.notificationIdentifier, content: content, trigger: nil)
          center.add(request) { error in
            if let error = error {
              print("User notification request error: \(error.localizedDescription)")
            }
          }
        }
      }
    }
  }

  /// What librime tells the app about deploys and sessions goes to `notificationHandler`.  The typing self-test installs
  /// it too, so it sees what the app would show.
  func installNotificationHandler() {
    // swiftlint:disable identifier_name
    let notification_handler: @convention(c) (UnsafeMutableRawPointer?, RimeSessionId, UnsafePointer<CChar>?, UnsafePointer<CChar>?) -> Void = notificationHandler
    let context_object = Unmanaged.passUnretained(self).toOpaque()
    // swiftlint:enable identifier_name
    rimeAPI.set_notification_handler(notification_handler, context_object)
  }

  func setupRime() {
    createDirIfNotExist(path: SquirrelApp.userDir)
    createDirIfNotExist(path: SquirrelApp.logDir)
    installNotificationHandler()

    // Without SharedSupport Rime has no schemas to load, so the app stops with a message instead of crashing.  The
    // version is only a label in Rime's logs, so a missing one falls back to "unknown".
    guard let sharedSupport = Bundle.main.sharedSupportPath else {
      fputs("AquaType cannot start: the bundle has no SharedSupport directory, so Rime has no shared data to load.\n",
            stderr)
      exit(1)
    }
    let version = Bundle.main.object(forInfoDictionaryKey: kCFBundleVersionKey as String) as? String ?? "unknown"
    if let bundled = Bundle.main.resourceURL?.appending(component: "rime") {
      Self.installRimeFiles(from: bundled, to: SquirrelApp.userDir, build: version)
    }

    var squirrelTraits = RimeTraits.rimeStructInit()
    squirrelTraits.setCString(sharedSupport, to: \.shared_data_dir)
    squirrelTraits.setCString(SquirrelApp.userDir.path(), to: \.user_data_dir)
    squirrelTraits.setCString(SquirrelApp.logDir.path(), to: \.log_dir)
    squirrelTraits.setCString("AquaType", to: \.distribution_code_name)
    squirrelTraits.setCString("AquaType", to: \.distribution_name)
    squirrelTraits.setCString(version, to: \.distribution_version)
    squirrelTraits.setCString("rime.squirrel", to: \.app_name)
    // Warnings and errors only.  At INFO librime writes a line for every property set, which AquaType does on every key:
    // no text, but a timestamped record of each keystroke, in a log nothing ever deletes.
    squirrelTraits.min_log_level = 1
    rimeAPI.setup(&squirrelTraits)
  }

  /// The Rime files of this build in a person's Rime directory.  The installer puts them there, built, for the person
  /// logged in at the screen; everyone else - another account, one created later, an installation with nobody logged
  /// in - gets them here, the first time AquaType starts in their session, and librime builds them (it sees the new
  /// files).  A stamp records the build they came from, so an upgrade replaces them and an unchanged build leaves them
  /// alone.  Replaced means what the installer does too: files earlier versions shipped and this one does not are
  /// removed, and so is the build an earlier version left, which librime would otherwise keep using parts of.  Each
  /// file is copied beside its place and then moved into it, so a copy that fails leaves the old file, not none.
  static let rimeStampName = ".aquatype-build"
  static let rimeFilesNoLongerShipped = ["phrases.chtp.dict.yaml", "terra_pinyin_onion.dict.yaml",
                                         "terra_pinyin_onion_add.dict.yaml", "bopomo_onion.extended.dict.yaml",
                                         "aquatype_bopomofo.custom.yaml"]
  @discardableResult
  static func installRimeFiles(from bundled: URL, to userDir: URL, build: String) -> Bool {
    let fm = FileManager.default
    let stamp = userDir.appending(component: rimeStampName)
    if (try? String(contentsOf: stamp, encoding: .utf8)) == build { return false }
    guard let files = try? fm.contentsOfDirectory(at: bundled, includingPropertiesForKeys: nil) else { return false }
    for name in rimeFilesNoLongerShipped + ["build"] { try? fm.removeItem(at: userDir.appending(component: name)) }
    for file in files where file.pathExtension == "yaml" {
      let target = userDir.appending(component: file.lastPathComponent)
      let incoming = userDir.appending(component: ".\(file.lastPathComponent).installing")
      try? fm.removeItem(at: incoming)
      do {
        try fm.copyItem(at: file, to: incoming)
        if fm.fileExists(atPath: target.path) {
          _ = try fm.replaceItemAt(target, withItemAt: incoming)
        } else {
          try fm.moveItem(at: incoming, to: target)
        }
      } catch {
        try? fm.removeItem(at: incoming)
        fputs("AquaType: could not install \(file.lastPathComponent) into \(userDir.path): \(error)\n", stderr)
        return false
      }
    }
    try? build.write(to: stamp, atomically: true, encoding: .utf8)
    return true
  }

  func startRime(fullCheck: Bool) {
    print("Initializing la rime...")
    rimeAPI.initialize(nil)
    // `start_maintenance` returns false both when nothing has changed (the usual answer at launch) and, when
    // `fullCheck` is true, when the update failed.  Only the second is worth reporting.
    if rimeAPI.start_maintenance(fullCheck) {
      _ = rimeAPI.deploy_config_file("squirrel.yaml", "config_version")
    } else if fullCheck {
      print("AquaType: Rime maintenance did not start, so the deployment was not updated.")
    }
  }

  func loadSettings() {
    config = SquirrelConfig()
    // squirrel.yaml comes from the deployed build directory.  If it is missing (a failed deploy, a cleared build
    // directory), the panel keeps SquirrelTheme's built-in defaults and looks nothing like the designed one, so say so.
    if !config!.openBaseConfig() {
      print("AquaType: squirrel.yaml could not be opened; the candidate panel will use built-in defaults, not the "
            + "configured appearance.  Check that \(SquirrelApp.userDir.path())/build/squirrel.yaml exists.")
      return
    }

    if let panel = panel, let config = self.config {
      panel.load(config: config, forDarkMode: false)
      panel.load(config: config, forDarkMode: true)
    }
  }

  func loadSettings(for schemaID: String) {
    if schemaID.count == 0 || schemaID.first == "." {
      return
    }
    let schema = SquirrelConfig()
    if let panel = panel, let config = self.config {
      if schema.open(schemaID: schemaID, baseConfig: config) && schema.has(section: "style") {
        panel.load(config: schema, forDarkMode: false)
        panel.load(config: schema, forDarkMode: true)
      } else {
        panel.load(config: config, forDarkMode: false)
        panel.load(config: config, forDarkMode: true)
      }
    }
    schema.close()
  }

  /// Two launches close together can be normal, so a crash loop is a run of them: `launchLoopTrips` launches, each
  /// within `launchLoopWindow` of the one before.  Tripping it starts the input method without Rime, so it must not
  /// trip on ordinary launches.
  private struct LaunchRecord: Codable {
    let at: Date
    let consecutive: Int
  }
  private static let launchLoopWindow: TimeInterval = 2
  private static let launchLoopTrips = 4

  func problematicLaunchDetected() -> Bool {
    // Inside ~/.aquatype, so removing AquaType removes it too.
    let runDir = SquirrelApp.homeDir.appending(components: ".aquatype", "run")
    createDirIfNotExist(path: runDir)
    let logFile = runDir.appending(component: "launch.json")
    var consecutive = 1
    do {
      let archive = try Data(contentsOf: logFile, options: [.uncached])
      let decoder = JSONDecoder()
      decoder.dateDecodingStrategy = .millisecondsSince1970
      let previous = try decoder.decode(LaunchRecord.self, from: archive)
      if previous.at.timeIntervalSinceNow >= -Self.launchLoopWindow {
        consecutive = previous.consecutive + 1
      }
    } catch let error as NSError where error.domain == NSCocoaErrorDomain && error.code == NSFileReadNoSuchFileError {
      // the first launch, or the record was removed: not a loop
    } catch {
      // A record this build cannot read (truncated, or in an unknown format): start the count over.
      print("Launch record unreadable, starting a new count: \(error.localizedDescription)")
    }
    do {
      let encoder = JSONEncoder()
      encoder.dateEncodingStrategy = .millisecondsSince1970
      try encoder.encode(LaunchRecord(at: .now, consecutive: consecutive)).write(to: logFile)
    } catch {
      print("Error occurred during saving launch time to archive: \(error.localizedDescription)")
    }
    if consecutive >= Self.launchLoopTrips {
      print("Problematic launch: \(consecutive) launches within \(Self.launchLoopWindow)s of each other")
      return true
    }
    return false
  }

  /// Distributed notifications reach every app on the Mac, so these names are AquaType's own: with Squirrel's names, a
  /// Mac with both installed would redeploy one input method when the other was asked to.
  func addObservers() {
    // No librime shutdown on NSWorkspace.willPowerOffNotification: it is posted when a logout is asked for, before any
    // application can refuse it, and a logout that is then cancelled would leave librime finalized and every key after
    // it in Latin letters until the next login.  Nothing needs saving: librime keeps no user dictionary here and the
    // engine writes the personal memory as it goes.
    let notifCenter = DistributedNotificationCenter.default()
    distributedObservers.append(
      notifCenter.addObserver(forName: .init("AquaTypeReloadNotification"), object: nil, queue: nil, using: rimeNeedsReload))
    // Redraw the panel when the system switches between light and dark mode; without this the change shows only after
    // the input method restarts.
    distributedObservers.append(
      notifCenter.addObserver(forName: .init("AppleInterfaceThemeChangedNotification"), object: nil, queue: nil) { [weak self] _ in
        self?.panel?.contentView?.needsDisplay = true
      })
    // On macOS 26 an input source switch made by another process (an input-source switcher, a Vim plugin, Raycast)
    // never calls deactivateServer, so a composition would stay open, its panel on screen, until AquaType was chosen
    // again.  Upstream Squirrel's answer (rime/squirrel#1142): when the selected source is no longer this one, end it.
    guard !SquirrelInputController.aquaTypeSelftestIsolated else { return }
    distributedObservers.append(
      notifCenter.addObserver(forName: .init(kTISNotifySelectedKeyboardInputSourceChanged as String), object: nil,
                              queue: .main) { [weak self] _ in
        let source = TISCopyCurrentKeyboardInputSource().takeRetainedValue()
        let id = TISGetInputSourceProperty(source, kTISPropertyInputSourceID)
          .map { Unmanaged<CFString>.fromOpaque($0).takeUnretainedValue() as String } ?? ""
        self?.inputSourceChanged(to: id)
      })
  }

  /// The selected input source is now `id`: anything but AquaType's own ends the composition of the controller the
  /// panel belongs to, as deactivateServer would.
  func inputSourceChanged(to id: String) {
    // Only the controller of the field being typed in: the panel can hold on to one whose application has since gone.
    guard !id.hasPrefix(Bundle.main.bundleIdentifier ?? "com.aquatype.inputmethod.AquaType"),
          let controller = panel?.inputController,
          controller === SquirrelInputController.aquaTypeActiveController else { return }
    controller.deactivateServer(controller.client())
  }

  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    print("AquaType is quitting.")
    rimeAPI.cleanup_all_sessions()
    return .terminateNow
  }

}

private func notificationHandler(contextObject: UnsafeMutableRawPointer?, sessionId: RimeSessionId, messageTypeC: UnsafePointer<CChar>?, messageValueC: UnsafePointer<CChar>?) {
  let messageType = messageTypeC.map { String(cString: $0) }
  let messageValue = messageValueC.map { String(cString: $0) }
  if messageType == "deploy" {
    switch messageValue {
    case "start":
      SquirrelApplicationDelegate.showMessage(msgText: NSLocalizedString("deploy_start", comment: ""))
    case "success":
      SquirrelApplicationDelegate.showMessage(msgText: NSLocalizedString("deploy_success", comment: ""))
    case "failure":
      SquirrelApplicationDelegate.showMessage(msgText: NSLocalizedString("deploy_failure", comment: ""))
    default:
      break
    }
  }
  // Nothing else librime announces is shown.  Its option messages are not only the user's own switches: a new session
  // restoring the saved Chinese/English mode sends one too, and a badge for that flashed wherever the application
  // reported a caret - a corner of the screen in Finder, a hidden input box in a web page.  The input controller shows
  // the 中 / A badge itself, for a click into a field and for a switch the user made (aquaTypeShowModeBadge).
}

private extension SquirrelApplicationDelegate {
  func shutdownRime() {
    config?.close()
    rimeAPI.finalize()
    Self.rimeGeneration &+= 1
  }

  func rimeNeedsReload(_: Notification) {
    print("Reloading rime on demand.")
    self.deploy()
  }

  /// No existence check first: `createDirectory(withIntermediateDirectories: true)` already succeeds when the directory
  /// exists, and a separate check would only leave a gap between the two.
  func createDirIfNotExist(path: URL) {
    do {
      try FileManager.default.createDirectory(at: path, withIntermediateDirectories: true)
    } catch {
      print("Could not create \(path.path()): \(error.localizedDescription)")
    }
  }
}

extension NSApplication {
  /// The delegate, or the `detached` stand-in while there is none (before launch, during teardown), so the many call
  /// sites that reach the panel through this do nothing instead of crashing.
  var squirrelAppDelegate: SquirrelApplicationDelegate {
    if let delegate = self.delegate as? SquirrelApplicationDelegate { return delegate }
    return SquirrelApplicationDelegate.detached
  }
}
