//
//  BridgingFunctions.swift
//  Squirrel
//
//  Created by Leo Liu on 5/11/24.
//  Modified for AquaType by Dio Hsieh, 2026.
//

import Foundation

protocol DataSizeable {
  /// Every C struct Swift imports carries a no-argument initializer that zeroes all of its fields; requiring it here
  /// is what lets `rimeStructInit()` produce a cleared struct without going through raw memory.
  init()
  // swiftlint:disable:next identifier_name
  var data_size: Int32 { get set }
}

extension RimeContext_stdbool: DataSizeable {}
extension RimeTraits: DataSizeable {}
extension RimeCommit: DataSizeable {}
extension RimeStatus_stdbool: DataSizeable {}

extension DataSizeable {
  static func rimeStructInit() -> Self {
    var value = Self()

    // librime's own macro sets `data_size = sizeof(Type) - sizeof(var.data_size)`, and librime uses that length to
    // decide which trailing members of the caller's struct it may fill.  For a struct imported from C,
    // `MemoryLayout.size` is the C `sizeof`, trailing padding included.
    value.data_size = Int32(MemoryLayout<Self>.size - MemoryLayout<Int32>.size)
    return value
  }

  /// Copy a Swift string into a C string field that librime will keep.
  ///
  /// The previous value is freed: every field this is used on starts zeroed by `rimeStructInit()` and is only set
  /// here, so what is freed is always an earlier `strdup`, never memory librime owns.  A failed `strdup` leaves the
  /// field as it was.
  mutating func setCString(_ swiftString: String, to keypath: WritableKeyPath<Self, UnsafePointer<CChar>?>) {
    swiftString.withCString { cStr in
      guard let mutableCStr = strdup(cStr) else { return }
      if let existing = self[keyPath: keypath] {
        free(UnsafeMutableRawPointer(mutating: existing))
      }
      self[keyPath: keypath] = UnsafePointer(mutableCStr)
    }
  }
}

/// Assign only if the right-hand side has a value; otherwise leave the left-hand side alone.
///
/// This is how a schema's styling is layered over squirrel.yaml's: each setting is read from the more specific config
/// and applied only when that config actually defines it, so an absent key leaves whatever the base already put there.
infix operator ?= : AssignmentPrecedence

/// The non-optional destination: a setting that always has a value, refined when one is offered.
// swiftlint:disable:next operator_whitespace
func ?=<T>(left: inout T, right: T?) {
  if let right = right {
    left = right
  }
}

/// The optional destination, for settings that are legitimately absent - the font sizes, the font names.  Both
/// overloads are live; when both are viable Swift takes this one, because it needs no promotion of `right`.
///
/// Note that it cannot *clear* the left-hand side: a nil on the right means "this config says nothing", never "this
/// config says none".  Nothing in the styling layers needs to express the latter, and `= nil` says it if it ever does.
// swiftlint:disable:next operator_whitespace
func ?=<T>(left: inout T?, right: T?) {
  if let right = right {
    left = right
  }
}

extension NSRange {
  /// The *absent* range, not merely a zero-length one: its location is `NSNotFound`, so a real empty range sitting at
  /// a real offset - `NSRange(location: 5, length: 0)` - is not equal to it, which is what the `== .empty` tests that
  /// gate the preedit drawing rely on.  Check `length == 0` instead when the question is "is this range empty".
  static let empty = NSRange(location: NSNotFound, length: 0)
}

extension NSPoint {
  static func += (lhs: inout Self, rhs: Self) {
    lhs.x += rhs.x
    lhs.y += rhs.y
  }
  static func - (lhs: Self, rhs: Self) -> Self {
    Self.init(x: lhs.x - rhs.x, y: lhs.y - rhs.y)
  }
  static func -= (lhs: inout Self, rhs: Self) {
    lhs.x -= rhs.x
    lhs.y -= rhs.y
  }
  static func * (lhs: Self, rhs: CGFloat) -> Self {
    Self.init(x: lhs.x * rhs, y: lhs.y * rhs)
  }
  static func / (lhs: Self, rhs: CGFloat) -> Self {
    Self.init(x: lhs.x / rhs, y: lhs.y / rhs)
  }
  var length: CGFloat {
    sqrt(pow(self.x, 2) + pow(self.y, 2))
  }
}
