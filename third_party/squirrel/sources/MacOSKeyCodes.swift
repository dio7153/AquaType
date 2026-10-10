//
//  MacOSKeyCodes.swift
//  Squirrel
//
//  Created by Leo Liu on 5/9/24.
//  Modified for AquaType by Dio Hsieh, 2026.
//

import Carbon
import AppKit

let XK_ISO_Left_Tab: Int32 = 0xfe20

enum SquirrelKeycode {

  /// The five modifiers librime is told about.  `.function` and `.numericPad` are left out on purpose: macOS sets
  /// `.function` on every arrow key, F-key, Home, End, Page Up/Down and Delete, and `.numericPad` on the arrows too, so
  /// mapping either would report a modifier nobody is holding.  (The table's `kVK_Function` entry answers a different
  /// question: which key was pressed, not which modifiers are down.)
  static func osxModifiersToRime(modifiers: NSEvent.ModifierFlags) -> UInt32 {
    var ret: UInt32 = 0
    if modifiers.contains(.capsLock) {
      ret |= kLockMask.rawValue
    }
    if modifiers.contains(.shift) {
      ret |= kShiftMask.rawValue
    }
    if modifiers.contains(.control) {
      ret |= kControlMask.rawValue
    }
    if modifiers.contains(.option) {
      ret |= kAltMask.rawValue
    }
    if modifiers.contains(.command) {
      ret |= kSuperMask.rawValue
    }
    return ret
  }

  static func osxKeycodeToRime(keycode: UInt16, keychar: Character?, shift: Bool, caps: Bool) -> UInt32 {
    if let code = keycodeMappings[Int(keycode)] {
      return UInt32(code)
    }

    if let keychar = keychar, keychar.isASCII, let codeValue = keychar.unicodeScalars.first?.value {
      // NOTE: IBus/Rime use different keycodes for uppercase/lowercase letters.
      // lowercase -> Uppercase
      if keychar.isLowercase, shift != caps, let upper = keychar.uppercased().unicodeScalars.first {
        return upper.value
      }

      // Control characters are reported as the physical, unshifted key that produces them: 0x1b is the `[` key, 0x1c
      // `\`, 0x1d `]`, and 0x1f (strictly Ctrl+underscore) the `-` key.
      switch codeValue {
      case 0x20...0x7e:
        return codeValue
      case 0x1b:
        return UInt32(XK_bracketleft)
      case 0x1c:
        return UInt32(XK_backslash)
      case 0x1d:
        return UInt32(XK_bracketright)
      case 0x1f:
        return UInt32(XK_minus)
      default:
        break
      }
    }

    if let code = additionalCodeMappings[Int(keycode)] {
      return UInt32(code)
    }

    return UInt32(XK_VoidSymbol)
  }

  /// The modifier keycodes, taken from the keys of the modifier table so the two can never disagree.
  /// `SquirrelInputController` uses this set to decide whether a flags-changed event's own keycode can be trusted, then
  /// looks the keycode up in the table; a keycode in one but not the other would be misreported.
  static let modifierKeycodes: Set<UInt16> = Set(modifierKeycodeMappings.keys.map { UInt16($0) })

  /// The keycode for one modifier flag.  A flags-changed event can carry more than one modifier, and each has to be
  /// reported to librime as itself: librime decides 中/英 from the modifier state.
  static func keycode(for flag: NSEvent.ModifierFlags) -> UInt16? {
    if flag == .shift { return UInt16(kVK_Shift) }
    if flag == .control { return UInt16(kVK_Control) }
    if flag == .option { return UInt16(kVK_Option) }
    if flag == .command { return UInt16(kVK_Command) }
    if flag == .capsLock { return UInt16(kVK_CapsLock) }
    return nil
  }

  /// One modifier to stand for an event whose own keycode cannot be trusted.  Shift first: it is the modifier this
  /// layout gives meaning to (switching 中/英).
  static func inferModifierKeycode(from changes: NSEvent.ModifierFlags) -> UInt16? {
    for flag in [NSEvent.ModifierFlags.shift, .control, .option, .command, .capsLock] where changes.contains(flag) {
      return keycode(for: flag)
    }
    return nil
  }

  /// The whole table `osxKeycodeToRime` answers from, kept in two halves so `modifierKeycodes` can be built from one of
  /// them.  A keycode that appears in both halves stops the app the first time the table is read, instead of one entry
  /// silently winning.
  private static let keycodeMappings: [Int: Int32] =
    modifierKeycodeMappings.merging(nonModifierKeycodeMappings) { _, _ in
      preconditionFailure("keycode mapped in both the modifier and non-modifier tables")
    }

  private static let modifierKeycodeMappings: [Int: Int32] = [
    kVK_CapsLock: XK_Caps_Lock,
    kVK_Command: XK_Super_L,  // XK_Meta_L?
    kVK_RightCommand: XK_Super_R,  // XK_Meta_R?
    kVK_Control: XK_Control_L,
    kVK_RightControl: XK_Control_R,
    kVK_Function: XK_Hyper_L,
    kVK_Option: XK_Alt_L,
    kVK_RightOption: XK_Alt_R,
    kVK_Shift: XK_Shift_L,
    kVK_RightShift: XK_Shift_R
  ]

  private static let nonModifierKeycodeMappings: [Int: Int32] = [
    // special
    kVK_Delete: XK_BackSpace,
    kVK_Escape: XK_Escape,
    kVK_ForwardDelete: XK_Delete,
    kVK_Help: XK_Help,
    kVK_Return: XK_Return,
    kVK_Space: XK_space,
    kVK_Tab: XK_Tab,

    // function
    kVK_F1: XK_F1,
    kVK_F2: XK_F2,
    kVK_F3: XK_F3,
    kVK_F4: XK_F4,
    kVK_F5: XK_F5,
    kVK_F6: XK_F6,
    kVK_F7: XK_F7,
    kVK_F8: XK_F8,
    kVK_F9: XK_F9,
    kVK_F10: XK_F10,
    kVK_F11: XK_F11,
    kVK_F12: XK_F12,
    kVK_F13: XK_F13,
    kVK_F14: XK_F14,
    kVK_F15: XK_F15,
    kVK_F16: XK_F16,
    kVK_F17: XK_F17,
    kVK_F18: XK_F18,
    kVK_F19: XK_F19,
    kVK_F20: XK_F20,

    // cursor
    kVK_UpArrow: XK_Up,
    kVK_DownArrow: XK_Down,
    kVK_LeftArrow: XK_Left,
    kVK_RightArrow: XK_Right,
    kVK_PageUp: XK_Page_Up,
    kVK_PageDown: XK_Page_Down,
    kVK_Home: XK_Home,
    kVK_End: XK_End,

    // keypad
    kVK_ANSI_Keypad0: XK_KP_0,
    kVK_ANSI_Keypad1: XK_KP_1,
    kVK_ANSI_Keypad2: XK_KP_2,
    kVK_ANSI_Keypad3: XK_KP_3,
    kVK_ANSI_Keypad4: XK_KP_4,
    kVK_ANSI_Keypad5: XK_KP_5,
    kVK_ANSI_Keypad6: XK_KP_6,
    kVK_ANSI_Keypad7: XK_KP_7,
    kVK_ANSI_Keypad8: XK_KP_8,
    kVK_ANSI_Keypad9: XK_KP_9,
    kVK_ANSI_KeypadClear: XK_Clear,
    kVK_ANSI_KeypadDecimal: XK_KP_Decimal,
    kVK_ANSI_KeypadEquals: XK_KP_Equal,
    kVK_ANSI_KeypadMinus: XK_KP_Subtract,
    kVK_ANSI_KeypadMultiply: XK_KP_Multiply,
    kVK_ANSI_KeypadPlus: XK_KP_Add,
    kVK_ANSI_KeypadDivide: XK_KP_Divide,
    kVK_ANSI_KeypadEnter: XK_KP_Enter,

    // other
    kVK_ISO_Section: XK_section,
    kVK_JIS_Yen: XK_yen,
    kVK_JIS_Underscore: XK_underscore,
    // The keypad's own comma, not the main-block one: XK_comma is ㄝ in this layout, and a keypad comma must not
    // type it.
    kVK_JIS_KeypadComma: XK_KP_Separator,
    kVK_JIS_Eisu: XK_Eisu_Shift,
    kVK_JIS_Kana: XK_Kana_Shift
  ]

  private static let additionalCodeMappings: [Int: Int32] = [
    // numbers
    kVK_ANSI_0: XK_0,
    kVK_ANSI_1: XK_1,
    kVK_ANSI_2: XK_2,
    kVK_ANSI_3: XK_3,
    kVK_ANSI_4: XK_4,
    kVK_ANSI_5: XK_5,
    kVK_ANSI_6: XK_6,
    kVK_ANSI_7: XK_7,
    kVK_ANSI_8: XK_8,
    kVK_ANSI_9: XK_9,

    // punctuation
    kVK_ANSI_RightBracket: XK_bracketright,
    kVK_ANSI_LeftBracket: XK_bracketleft,
    kVK_ANSI_Comma: XK_comma,
    kVK_ANSI_Grave: XK_grave,
    kVK_ANSI_Period: XK_period,
    kVK_ANSI_Semicolon: XK_semicolon,
    kVK_ANSI_Quote: XK_apostrophe,
    kVK_ANSI_Backslash: XK_backslash,
    kVK_ANSI_Minus: XK_minus,
    kVK_ANSI_Slash: XK_slash,
    kVK_ANSI_Equal: XK_equal,

    // letters
    kVK_ANSI_A: XK_a,
    kVK_ANSI_B: XK_b,
    kVK_ANSI_C: XK_c,
    kVK_ANSI_D: XK_d,
    kVK_ANSI_E: XK_e,
    kVK_ANSI_F: XK_f,
    kVK_ANSI_G: XK_g,
    kVK_ANSI_H: XK_h,
    kVK_ANSI_I: XK_i,
    kVK_ANSI_J: XK_j,
    kVK_ANSI_K: XK_k,
    kVK_ANSI_L: XK_l,
    kVK_ANSI_M: XK_m,
    kVK_ANSI_N: XK_n,
    kVK_ANSI_O: XK_o,
    kVK_ANSI_P: XK_p,
    kVK_ANSI_Q: XK_q,
    kVK_ANSI_R: XK_r,
    kVK_ANSI_S: XK_s,
    kVK_ANSI_T: XK_t,
    kVK_ANSI_U: XK_u,
    kVK_ANSI_V: XK_v,
    kVK_ANSI_W: XK_w,
    kVK_ANSI_X: XK_x,
    kVK_ANSI_Y: XK_y,
    kVK_ANSI_Z: XK_z
  ]
}
