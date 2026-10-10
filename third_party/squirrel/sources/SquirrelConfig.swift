//
//  SquirrelConfig.swift
//  Squirrel
//
//  Created by Leo Liu on 5/9/24.
//  Modified for AquaType by Dio Hsieh, 2026.
//

import AppKit

final class SquirrelConfig {
  private let rimeAPI: RimeApi_stdbool = rime_get_api_stdbool().pointee
  private(set) var isOpen = false

  /// A colour is only meaningful together with the space it was read in, so both belong in the key.
  private struct ColorKey: Hashable {
    let option: String
    let space: SquirrelTheme.RimeColorSpace
  }

  // One cache per type, and the colour cache keyed by colour space as well: in a single shared cache, a string and a
  // colour read under the same option path would overwrite each other.
  private var boolCache: [String: Bool] = [:]
  private var doubleCache: [String: Double] = [:]
  private var stringCache: [String: String] = [:]
  private var colorCache: [ColorKey: NSColor] = [:]

  private var config: RimeConfig = .init()
  private var baseConfig: SquirrelConfig?

  func openBaseConfig() -> Bool {
    close()
    isOpen = rimeAPI.config_open("squirrel", &config)
    return isOpen
  }

  func open(schemaID: String, baseConfig: SquirrelConfig?) -> Bool {
    close()
    // Kept whether or not the schema opens: `close()` has just dropped the previous base, and a failed open still owes
    // the base's answers, which every getter falls through to while `isOpen` is false.
    self.baseConfig = baseConfig
    isOpen = rimeAPI.schema_open(schemaID, &config)
    return isOpen
  }

  /// Both open methods must start with this call.  librime leaves `config.ptr` untouched when an open fails, and
  /// `config_close` is the only thing that nulls it, so closing first is what makes "not open" and "no pointer" the
  /// same state, and keeps a later open from leaking the previous config.
  func close() {
    if isOpen {
      _ = rimeAPI.config_close(&config)
      isOpen = false
    }
    // Cleared every time, not only when something was open: both open methods start here, and a cache that outlived
    // `close()` would answer the next config's questions with the previous config's values.
    baseConfig = nil
    boolCache.removeAll()
    doubleCache.removeAll()
    stringCache.removeAll()
    colorCache.removeAll()
  }

  deinit {
    close()
  }

  /// Whether *this* config defines the section itself.  Deliberately not asked of the base: the caller uses it to
  /// decide whether a schema brings its own styling, and squirrel.yaml always has a `style` section, so consulting the
  /// base would make the answer always yes.
  func has(section: String) -> Bool {
    if isOpen {
      var iterator: RimeConfigIterator = .init()
      if rimeAPI.config_begin_map(&iterator, &config, section) {
        rimeAPI.config_end(&iterator)
        return true
      }
      // Nothing to end on the failing path: from here `config_begin_map` can only fail after it has cleared the
      // iterator, and without having allocated anything.
    }
    return false
  }

  func getBool(_ option: String) -> Bool? {
    if let cachedValue = boolCache[option] {
      return cachedValue
    }
    var value = false
    // `isOpen` carries weight here; it is not belt and braces.  librime's `config_get_bool` and `config_get_double`
    // reinterpret_cast `config->ptr` and dereference it with no null check of their own - unlike `config_get_cstring`
    // and `config_begin_map`, which do check - and a closed config's ptr is null.  See
    // third_party/squirrel/librime/src/rime_api_impl.h.
    if isOpen && rimeAPI.config_get_bool(&config, option, &value) {
      boolCache[option] = value
      return value
    }
    return baseConfig?.getBool(option)
  }

  func getDouble(_ option: String) -> CGFloat? {
    if let cachedValue = doubleCache[option] {
      return cachedValue
    }
    // `value` is a `Double` because that is what librime writes through the pointer (`config_get_double` takes a
    // `double*`); the `CGFloat` in the signature is the caller's unit and converts implicitly.
    var value: Double = 0
    if isOpen && rimeAPI.config_get_double(&config, option, &value) {
      doubleCache[option] = value
      return value
    }
    return baseConfig?.getDouble(option)
  }

  func getString(_ option: String) -> String? {
    if let cachedValue = stringCache[option] {
      return cachedValue
    }
    // The pointer belongs to the open config tree, so it is good here and only here, and is copied before anything can
    // close the config.
    if isOpen, let value = rimeAPI.config_get_cstring(&config, option) {
      let string = String(cString: value)
      stringCache[option] = string
      return string
    }
    return baseConfig?.getString(option)
  }

  func getColor(_ option: String, inSpace colorSpace: SquirrelTheme.RimeColorSpace) -> NSColor? {
    let key = ColorKey(option: option, space: colorSpace)
    if let cachedValue = colorCache[key] {
      return cachedValue
    }
    if let colorStr = getString(option), let color = color(from: colorStr, inSpace: colorSpace) {
      colorCache[key] = color
      return color
    }
    return baseConfig?.getColor(option, inSpace: colorSpace)
  }

  /// The per-application option overrides this config declares for `appName`, or an empty dictionary.
  ///
  /// Unlike the getters it does not fall through to the base: it is asked of squirrel.yaml, which is the base, and
  /// `app_options` belongs to squirrel.yaml, not to a schema.  The values still come from `getBool`, which does
  /// consult the base.
  func getAppOptions(_ appName: String) -> [String: Bool] {
    var appOptions = [String: Bool]()
    var iterator = RimeConfigIterator()
    // Guarded on `isOpen` like every other accessor, and on what `config_begin_map` answers: there is nothing to walk
    // when it fails.
    guard isOpen, rimeAPI.config_begin_map(&iterator, &config, "app_options/\(appName)") else {
      return appOptions
    }
    while rimeAPI.config_next(&iterator) {
      if let key = iterator.key, let path = iterator.path, let value = getBool(String(cString: path)) {
        appOptions[String(cString: key)] = value
      }
    }
    rimeAPI.config_end(&iterator)
    return appOptions
  }
}

private extension SquirrelConfig {
  // Built once, not per colour: a regex literal written inline compiles a fresh `Regex` on every evaluation, and
  // `color(from:)` runs for roughly fifteen colours × light and dark × base and schema whenever the schema changes
  // (measured: 96 µs a call inline against 3 µs hoisted).
  static let argbPattern = /0x([A-Fa-f0-9]{2})([A-Fa-f0-9]{2})([A-Fa-f0-9]{2})([A-Fa-f0-9]{2})/
  static let rgbPattern = /0x([A-Fa-f0-9]{2})([A-Fa-f0-9]{2})([A-Fa-f0-9]{2})/

  /// Rime writes colours as `0xAABBGGRR` or `0xBBGGRR` - alpha first, then blue, green, red - which is the order these
  /// are destructured in.  The two force-unwraps cannot fail: the pattern has already established that each capture is
  /// exactly two hexadecimal digits.
  func color(from colorStr: String, inSpace colorSpace: SquirrelTheme.RimeColorSpace) -> NSColor? {
    if let matched = try? Self.argbPattern.wholeMatch(in: colorStr) {
      let (_, alpha, blue, green, red) = matched.output
      return color(alpha: Int(alpha, radix: 16)!, red: Int(red, radix: 16)!, green: Int(green, radix: 16)!, blue: Int(blue, radix: 16)!, colorSpace: colorSpace)
    } else if let matched = try? Self.rgbPattern.wholeMatch(in: colorStr) {
      let (_, blue, green, red) = matched.output
      return color(alpha: 255, red: Int(red, radix: 16)!, green: Int(green, radix: 16)!, blue: Int(blue, radix: 16)!, colorSpace: colorSpace)
    } else {
      return nil
    }
  }

  func color(alpha: Int, red: Int, green: Int, blue: Int, colorSpace: SquirrelTheme.RimeColorSpace) -> NSColor {
    switch colorSpace {
    case .displayP3:
      return NSColor(displayP3Red: CGFloat(red) / 255,
                     green: CGFloat(green) / 255,
                     blue: CGFloat(blue) / 255,
                     alpha: CGFloat(alpha) / 255)
    case .sRGB:
      return NSColor(srgbRed: CGFloat(red) / 255,
                     green: CGFloat(green) / 255,
                     blue: CGFloat(blue) / 255,
                     alpha: CGFloat(alpha) / 255)
    }
  }
}
