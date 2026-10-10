//
//  SquirrelPanel.swift
//  Squirrel
//
//  Created by Leo Liu on 5/10/24.
//  Modified for AquaType by Dio Hsieh, 2026.
//

import AppKit

final class SquirrelPanel: NSPanel {
  private let view: SquirrelView
  private let back: NSVisualEffectView
  var inputController: SquirrelInputController?

  var position: NSRect
  /// Everything the composition is reported to occupy on screen - its position, the caret, its lines.  The panel never
  /// overlaps it.  Nil means `position` alone.
  var avoidRect: NSRect?
  private var screenRect: NSRect = .zero
  private var maxHeight: CGFloat = 0

  private var statusTimer: Timer?

  private var preedit: String = ""
  private var selRange: NSRange = .empty
  private var caretPos: Int = 0
  private var candidates: [String] = .init()
  private var comments: [String] = .init()
  private var labels: [String] = .init()
  /// AquaType paginates in the controller and hands this panel one page at a time, so row N here is not candidate N
  /// in the controller's list.  A click has to be translated back or it commits the wrong candidate.
  private var candidateIndexMap: [Int] = .init()
  private var index: Int = 0
  private var cursorIndex: Int = 0
  private var scrollDirection: CGVector = .zero
  private var scrollTime: Date = .distantPast
  private var page: Int = 0
  private var lastPage: Bool = true
  private var pagingUp: Bool?
  // Above or below the caret is decided once per composition and reset on hide(): otherwise a change in the number of
  // candidates, and so in the panel's height, could flip the panel across the caret mid-sentence.
  private var lockedAbove: Bool?
  /// Set once the usual place would cover this composition: the panel keeps clear of it for the rest of the composition,
  /// on the side `avoidAbove`, with the edge facing the text at `avoidingEdge`.
  private var avoiding = false
  private var avoidAbove: Bool?
  private var avoidingEdge: CGFloat?

  /// The composition's anchor moved to another line: decide above or below again, for the new place.
  func forgetAboveOrBelow() { lockedAbove = nil; avoiding = false; avoidAbove = nil; avoidingEdge = nil }

  /// Onto the screen, sideways and then up and down.
  static func clamp(_ rect: inout NSRect, to screen: NSRect) {
    if rect.maxX > screen.maxX { rect.origin.x = screen.maxX - rect.width }
    if rect.minX < screen.minX { rect.origin.x = screen.minX }
    if rect.maxY > screen.maxY { rect.origin.y = screen.maxY - rect.height }
    if rect.minY < screen.minY { rect.origin.y = screen.minY }
  }

  init(position: NSRect) {
    self.position = position
    self.view = SquirrelView(frame: position)
    self.back = NSVisualEffectView()
    super.init(contentRect: position, styleMask: .nonactivatingPanel, backing: .buffered, defer: true)
    // No implicit resize or fade animation: candidate updates appear at once, as McBopomofo's do.
    self.animationBehavior = .none
    self.level = .init(Int(CGShieldingWindowLevel()))
    self.hasShadow = true
    self.isOpaque = false
    self.backgroundColor = .clear
    back.blendingMode = .behindWindow
    // The popover material: light and frosted like Spotlight's panel, and dark in dark mode on its own.
    back.material = .popover
    back.state = .followsWindowActiveState
    back.wantsLayer = true
    back.layer?.mask = view.shape
    let contentView = NSView()
    contentView.addSubview(back)
    contentView.addSubview(view)
    contentView.addSubview(view.textView)
    self.contentView = contentView
  }

  var linear: Bool {
    view.currentTheme.linear
  }
  var vertical: Bool {
    view.currentTheme.vertical
  }
  var inlinePreedit: Bool {
    view.currentTheme.inlinePreedit
  }

  // Whether candidates are being navigated: set by the controller on the arrows, Tab and Page Up/Down, cleared by any
  // other key.  It only matters when the theme sets `hide_panel` (AquaType's does not): the panel is then shown only
  // while this is true.
  var navMode = false
  // Set while a composition is open: the panel stays up from its first candidates to the commit.  Typing and Backspace
  // do not go through navMode, which would take the panel down for a moment between two renders - a flicker.
  var aquaTypeCompositionPanelVisible = false

  /// The font the candidates are actually drawn in, so the controller can ask whether a character has a glyph in it
  /// before offering it.
  var candidateFont: NSFont { view.currentTheme.font }
  var aquaTypeSelftestCandidateOrigins: [CGFloat] { view.aquaTypeSelftestCandidateTextOrigins }
  /// The text the panel is showing, for a self-test to tell when there is something new to look at.
  var aquaTypeSelftestShownText: String { view.textContentStorage.attributedString?.string ?? "" }

  /// How many full-width characters fit on one line.  The controller pages by lines, so it has to ask rather than
  /// assume: when the two disagree the panel reserves nine lines and draws a different number.
  var candidateColumns: Int {
    currentScreen()
    return max(8, Int(maxTextWidth() / view.currentTheme.font.pointSize))
  }
  private var aquaTypeForceOrderOutDepth = 0

  // swiftlint:disable:next cyclomatic_complexity
  override func sendEvent(_ event: NSEvent) {
    switch event.type {
    case .leftMouseDown:
      let (index, _, pagingUp) =  view.click(at: mousePosition())
      if let pagingUp {
        self.pagingUp = pagingUp
      } else {
        self.pagingUp = nil
      }
      if let index, index >= 0 && index < candidates.count {
        self.index = index
      }
    case .leftMouseUp:
      let (index, preeditIndex, pagingUp) = view.click(at: mousePosition())

      if let pagingUp, pagingUp == self.pagingUp {
        _ = inputController?.page(up: pagingUp)
      } else {
        self.pagingUp = nil
      }
      if let preeditIndex, preeditIndex >= 0 && preeditIndex < preedit.utf16.count {
        if preeditIndex < caretPos {
          _ = inputController?.moveCaret(forward: true)
        } else if preeditIndex > caretPos {
          _ = inputController?.moveCaret(forward: false)
        }
      }
      if let index, index == self.index && index >= 0 && index < candidates.count {
        let mappedIndex = index < candidateIndexMap.count ? candidateIndexMap[index] : index
        _ = inputController?.selectCandidate(mappedIndex)
      }
    case .mouseEntered:
      acceptsMouseMovedEvents = true
    case .mouseExited:
      acceptsMouseMovedEvents = false
      if cursorIndex != index {
        update(preedit: preedit, selRange: selRange, caretPos: caretPos, candidates: candidates, comments: comments, labels: labels, highlighted: index, page: page, lastPage: lastPage, update: false)
      }
      pagingUp = nil
    case .mouseMoved:
      let (index, _, _) = view.click(at: mousePosition())
      if let index = index, cursorIndex != index && index >= 0 && index < candidates.count {
        update(preedit: preedit, selRange: selRange, caretPos: caretPos, candidates: candidates, comments: comments, labels: labels, highlighted: index, page: page, lastPage: lastPage, update: false)
      }
    case .scrollWheel:
      if event.phase == .began {
        scrollDirection = .zero
        // Scrollboard span
      } else if event.phase == .ended || (event.phase == .init(rawValue: 0) && event.momentumPhase != .init(rawValue: 0)) {
        if abs(scrollDirection.dx) > abs(scrollDirection.dy) && abs(scrollDirection.dx) > 10 {
          _ = inputController?.page(up: (scrollDirection.dx < 0) == vertical)
        } else if abs(scrollDirection.dx) < abs(scrollDirection.dy) && abs(scrollDirection.dy) > 10 {
          _ = inputController?.page(up: scrollDirection.dy > 0)
        }
        scrollDirection = .zero
        // Mouse scroll wheel
      } else if event.phase == .init(rawValue: 0) && event.momentumPhase == .init(rawValue: 0) {
        if scrollTime.timeIntervalSinceNow < -1 {
          scrollDirection = .zero
        }
        scrollTime = .now
        if (scrollDirection.dy >= 0 && event.scrollingDeltaY > 0) || (scrollDirection.dy <= 0 && event.scrollingDeltaY < 0) {
          scrollDirection.dy += event.scrollingDeltaY
        } else {
          scrollDirection = .zero
        }
        if abs(scrollDirection.dy) > 10 {
          _ = inputController?.page(up: scrollDirection.dy > 0)
          scrollDirection = .zero
        }
      } else {
        scrollDirection.dx += event.scrollingDeltaX
        scrollDirection.dy += event.scrollingDeltaY
      }
    default:
      break
    }
    super.sendEvent(event)
  }

  override func orderOut(_ sender: Any?) {
    if aquaTypeCompositionPanelVisible && aquaTypeForceOrderOutDepth == 0 {
      return
    }
    super.orderOut(sender)
  }

  func hide(force: Bool = false) {
    if aquaTypeCompositionPanelVisible && !force {
      return
    }
    statusTimer?.invalidate()
    statusTimer = nil
    aquaTypeForceOrderOutDepth += 1
    orderOut(nil)
    aquaTypeForceOrderOutDepth = max(0, aquaTypeForceOrderOutDepth - 1)
    aquaTypeCompositionPanelVisible = false
    maxHeight = 0
    forgetAboveOrBelow()   // where the panel goes is decided afresh for the next composition
  }

  // Main function to add attributes to text output from librime
  // swiftlint:disable:next cyclomatic_complexity function_parameter_count
  func update(preedit: String, selRange: NSRange, caretPos: Int, candidates: [String], comments: [String], labels: [String], highlighted index: Int, page: Int, lastPage: Bool, update: Bool, candidateIndexMap newCandidateIndexMap: [Int] = []) {
    // The draw loop reads one comment per candidate.  Every caller pairs them, but one comment short would not draw a
    // row without a note - it would trap on the subscript and take the input method down mid-sentence.
    let safeComments = comments.count >= candidates.count
      ? comments
      : comments + Array(repeating: "", count: candidates.count - comments.count)
    if update {
      self.preedit = preedit
      self.selRange = selRange
      self.caretPos = caretPos
      self.candidates = candidates
      self.comments = safeComments
      self.labels = labels
      self.candidateIndexMap = newCandidateIndexMap.isEmpty ? Array(candidates.indices) : newCandidateIndexMap
      self.index = index
      self.page = page
      self.lastPage = lastPage
    }
    cursorIndex = index

    if !candidates.isEmpty || !preedit.isEmpty {
      statusTimer?.invalidate()
      statusTimer = nil
    } else {
      if statusTimer == nil {   // a badge on screen is left to its own timer
        hide()
      }
      return
    }

    let theme = view.currentTheme
    currentScreen()

    let text = NSMutableAttributedString()
    let preeditRange: NSRange
    let highlightedPreeditRange: NSRange

    // preedit
    if !preedit.isEmpty {
      preeditRange = NSRange(location: 0, length: preedit.utf16.count)
      highlightedPreeditRange = selRange

      let line = NSMutableAttributedString(string: preedit)
      line.addAttributes(theme.preeditAttrs, range: preeditRange)
      line.addAttributes(theme.preeditHighlightedAttrs, range: selRange)
      text.append(line)

      text.addAttribute(.paragraphStyle, value: theme.preeditParagraphStyle, range: NSRange(location: 0, length: text.length))
      if !candidates.isEmpty {
        text.append(NSAttributedString(string: "\n", attributes: theme.preeditAttrs))
      }
    } else {
      preeditRange = .empty
      highlightedPreeditRange = .empty
    }

    // What goes in front of each candidate - the label and whatever the format puts around it - measured before
    // anything is laid out, so the column can be made as wide as its widest entry.
    //
    // Every candidate then starts at the same x whatever its label says.  Labels are not the same width - in the label
    // font `1` is 4.7 points against 7.1 for the other digits, and two- and three-digit labels differ more - and
    // padding a label with spaces cannot even them out, because a space is not a digit's width.
    func labelText(_ i: Int) -> String {
      guard theme.candidateFormat.contains(/\[label\]/) else { return "" }
      if i < labels.count { return labels[i] }
      if labels.count == 1, candidates.count > 1, let first = labels.first, i < first.count {
        return String(first[first.index(first.startIndex, offsetBy: i)])   // custom: A. B. C...
      }
      return "\(i + 1)"
    }
    var labelPrefixWidths = [CGFloat](repeating: 0, count: candidates.count)
    // How many UTF-16 units of the row come before the candidate itself.  The same number that gives the label its
    // width also gives the candidate's offset within the finished row, which is what the alignment check measures.
    var labelPrefixLengths = [Int](repeating: 0, count: candidates.count)
    if !linear {
      for i in 0..<candidates.count {
        let prefix = NSMutableAttributedString(string: theme.candidateFormat, attributes: theme.labelAttrs)
        prefix.mutableString.replaceOccurrences(of: "[label]", with: labelText(i),
                                                range: NSRange(location: 0, length: prefix.length))
        guard let end = prefix.string.firstMatch(of: /\[(candidate|comment)\]/)?.range.lowerBound else { continue }
        let headLength = end.utf16Offset(in: prefix.string)
        let head = prefix.attributedSubstring(from: NSRange(location: 0, length: headLength))
        labelPrefixWidths[i] = head.boundingRect(with: .zero, options: [.usesLineFragmentOrigin]).width
        labelPrefixLengths[i] = headLength
      }
    }
    let labelColumnWidth = labelPrefixWidths.max() ?? 0

    // candidates
    var candidateRanges = [NSRange]()
    var candidateTextRanges = [NSRange]()
    for i in 0..<candidates.count {
      let attrs = i == index ? theme.highlightedAttrs : theme.attrs
      let labelAttrs = i == index ? theme.labelHighlightedAttrs : theme.labelAttrs
      let commentAttrs = i == index ? theme.commentHighlightedAttrs : theme.commentAttrs

      let label = labelText(i)

      let candidate = candidates[i].precomposedStringWithCanonicalMapping
      let comment = safeComments[i].precomposedStringWithCanonicalMapping

      let line = NSMutableAttributedString(string: theme.candidateFormat, attributes: labelAttrs)
      for range in line.string.ranges(of: /\[candidate\]/) {
        let convertedRange = convert(range: range, in: line.string)
        line.addAttributes(attrs, range: convertedRange)
        if candidate.count <= 5 {
          line.addAttribute(.noBreak, value: true, range: NSRange(location: convertedRange.location+1, length: convertedRange.length-1))
        }
      }
      for range in line.string.ranges(of: /\[comment\]/) {
        line.addAttributes(commentAttrs, range: convert(range: range, in: line.string))
      }
      line.mutableString.replaceOccurrences(of: "[label]", with: label, range: NSRange(location: 0, length: line.length))
      line.mutableString.replaceOccurrences(of: "[candidate]", with: candidate, range: NSRange(location: 0, length: line.length))
      line.mutableString.replaceOccurrences(of: "[comment]", with: comment, range: NSRange(location: 0, length: line.length))

      // The lower bound matters: `candidate_format: ""` in the config gives a zero-length line, and
      // `NSRange(location: 1, length: -1)` would raise inside a draw and take the input method down.
      if line.length >= 1, line.length <= 10 {
        line.addAttribute(.noBreak, value: true, range: NSRange(location: 1, length: line.length-1))
      }

      let lineSeparator = NSAttributedString(string: linear ? "  " : "\n", attributes: attrs)
      if i > 0 {
        text.append(lineSeparator)
      }
      let str = lineSeparator.mutableCopy() as! NSMutableAttributedString
      if vertical {
        str.addAttribute(.verticalGlyphForm, value: 1, range: NSRange(location: 0, length: str.length))
      }
      view.separatorWidth = str.boundingRect(with: .zero).width

      let paragraphStyleCandidate = (i == 0 ? theme.firstParagraphStyle : theme.paragraphStyle).mutableCopy() as! NSMutableParagraphStyle
      if linear {
        paragraphStyleCandidate.paragraphSpacingBefore -= theme.linespace
        paragraphStyleCandidate.lineSpacing = theme.linespace
      }
      if !linear, labelColumnWidth > 0 {
        // The row is pushed right by whatever its label is narrower than the widest one, which right-aligns the
        // labels and lines every candidate up; wrapped lines hang under the candidate, as before.
        paragraphStyleCandidate.firstLineHeadIndent = labelColumnWidth - labelPrefixWidths[i]
        paragraphStyleCandidate.headIndent = labelColumnWidth
      }
      line.addAttribute(.paragraphStyle, value: paragraphStyleCandidate, range: NSRange(location: 0, length: line.length))

      candidateRanges.append(NSRange(location: text.length, length: line.length))
      if !linear, labelPrefixLengths[i] + candidate.utf16.count <= line.length {
        candidateTextRanges.append(NSRange(location: text.length + labelPrefixLengths[i],
                                           length: candidate.utf16.count))
      }
      text.append(line)
    }

    // text done!
    view.textView.textContentStorage?.attributedString = text
    view.textView.setLayoutOrientation(vertical ? .vertical : .horizontal)

    // Lay the text out now, so the window size and the highlight computed next come from the wrapped text.
    let textWidth = maxTextWidth()
    let maxTextHeight = vertical ? screenRect.width - theme.edgeInset.width * 2 : screenRect.height - theme.edgeInset.height * 2
    view.textContainer.size = NSSize(width: textWidth, height: maxTextHeight)
    view.textLayoutManager.ensureLayout(for: view.textLayoutManager.documentRange)

    // Back to the top: when wrapping makes the text taller, the text view scrolls to its end and pushes the first line
    // out of view.
    view.textView.scrollToBeginningOfDocument(nil)

    view.candidateTextRanges = candidateTextRanges
    view.drawView(candidateRanges: candidateRanges, hilightedIndex: index, preeditRange: preeditRange, highlightedPreeditRange: highlightedPreeditRange, canPageUp: page > 0, canPageDown: !lastPage)
    show()
  }

  /// Draw a status badge at `position` now.  Never queued for a later redraw: by then `position` may belong to another
  /// field or another application.
  func showStatus(long longMessage: String, short shortMessage: String) {
    let theme = view.currentTheme
    let message: String
    switch theme.statusMessageType {
    case .mix:
      message = shortMessage.isEmpty ? longMessage : shortMessage
    case .long:
      message = longMessage
    case .short:
      message = !shortMessage.isEmpty ? shortMessage : longMessage.first.map(String.init) ?? ""
    }
    if !message.isEmpty {
      show(status: message)
    }
  }

  func load(config: SquirrelConfig, forDarkMode isDark: Bool) {
    if isDark {
      view.darkTheme = SquirrelTheme()
      view.darkTheme.load(config: config, dark: true)
    } else {
      view.lightTheme = SquirrelTheme()
      view.lightTheme.load(config: config, dark: isDark)
    }
  }
}

private extension SquirrelPanel {
  func mousePosition() -> NSPoint {
    var point = NSEvent.mouseLocation
    point = self.convertPoint(fromScreen: point)
    return view.convert(point, from: nil)
  }

  func currentScreen() {
    // visibleFrame, not frame: frame includes the menu bar and the Dock, so clamping against it lets the panel come
    // to rest underneath them and be clipped.
    if let screen = NSScreen.main {
      screenRect = screen.visibleFrame
    }
    for screen in NSScreen.screens where screen.frame.contains(position.origin) {
      screenRect = screen.visibleFrame
      break
    }
  }

  /// The width a candidate wraps at: a fraction of the screen that grows with the font (about 44% at 15 points).  This
  /// is the panel's tuned proportion; a fixed twenty-character column instead halved the panel.
  func maxTextWidth() -> CGFloat {
    let theme = view.currentTheme
    let font: NSFont = theme.font
    let fontScale = font.pointSize / 12
    let textWidthRatio = min(1, 1 / (vertical ? 4 : 3) + fontScale / 12)
    let maxWidth = if vertical {
      screenRect.height * textWidthRatio - theme.edgeInset.height * 2
    } else {
      screenRect.width * textWidthRatio - theme.edgeInset.width * 2
    }
    return maxWidth
  }

  // swiftlint:disable:next cyclomatic_complexity
  func show() {
    currentScreen()
    let theme = view.currentTheme
    if theme.native || view.darkTheme.available {
      // `view.isDark` reads the system-wide setting: this app's own NSApp does not follow the system appearance.
      self.appearance = NSAppearance(named: view.isDark ? .darkAqua : .aqua)
    } else {
      // user configured only a light theme, set window appearance to light.
      self.appearance = NSAppearance(named: .aqua)
    }

    view.textView.textContainerInset = theme.edgeInset

    // Unbounded height, so a long text is measured at its natural height; and the container must not track the
    // view's size, which would shrink it to the current width and lose text in a layout loop.
    view.textContainer.size = NSSize(width: maxTextWidth(), height: .greatestFiniteMagnitude)
    view.textContainer.widthTracksTextView = false
    view.textContainer.heightTracksTextView = false
    view.textLayoutManager.ensureLayout(for: view.textLayoutManager.documentRange)
    view.textView.bounds.origin = .zero

    var contentRect = view.contentRect

    // Memorize the size (upstream's `memorize_size`, which only applies to vertical text).  The parentheses make
    // `memorize_size` govern both halves: without them `&&` binds tighter than `||` and the second half would apply
    // whatever the setting says.
    if theme.memorizeSize &&
        ((vertical && position.midY / screenRect.height < 0.5) ||
         (vertical && position.minX + max(contentRect.width, maxHeight) + theme.edgeInset.width * 2 > screenRect.maxX)) {
      if contentRect.width >= maxHeight {
        maxHeight = contentRect.width
      } else {
        contentRect.size.width = maxHeight
      }
    }

    // Sized to the content, and never more than 95% of the screen either way, as upstream does: past that the content
    // is clipped.  With AquaType's page of nine lines and line width this is never reached.  There is no minimum
    // width: one would make a list of one-character candidates, and the 中 / A badge, as wide as a sentence.
    var panelRect = NSRect.zero
    if vertical {
      panelRect.size = NSSize(width: min(0.95 * screenRect.width, contentRect.height + theme.edgeInset.height * 2),
                              height: min(0.95 * screenRect.height,
                                          contentRect.width + theme.edgeInset.width * 2 + theme.pagingOffset))
      // To avoid jumping up and down while typing
      if position.midY / screenRect.height >= 0.5 {
        panelRect.origin.y = position.minY - SquirrelTheme.offsetHeight - panelRect.height + theme.pagingOffset
      } else {
        panelRect.origin.y = position.maxY + SquirrelTheme.offsetHeight
      }
      panelRect.origin.x = position.minX - panelRect.width - SquirrelTheme.offsetHeight
      if view.preeditRange.length > 0, let preeditTextRange = view.convert(range: view.preeditRange) {
        let preeditRect = view.contentRect(range: preeditTextRange)
        panelRect.origin.x += preeditRect.height + theme.edgeInset.width
      }
    } else {
      panelRect.size = NSSize(width: min(0.95 * screenRect.width,
                                         contentRect.width + theme.edgeInset.width * 2 + theme.pagingOffset),
                              height: min(0.95 * screenRect.height, contentRect.height + theme.edgeInset.height * 2))
      // Where the panel has always gone: above the position, as traditional Zhuyin input methods place it, below only
      // when above would run off the top of the screen, decided once per composition (lockedAbove), clamped onto the
      // screen.  It depends on the position alone, which is held for the composition, so it does not move while a
      // sentence is typed.  It is used whenever it leaves the composition uncovered, which is nearly always.
      let off = SquirrelTheme.offsetHeight
      if lockedAbove == nil { lockedAbove = position.maxY + off + panelRect.height <= screenRect.maxY }
      panelRect.origin = NSPoint(x: position.minX - theme.pagingOffset,
                                 y: lockedAbove == true ? position.maxY + off : position.minY - off - panelRect.height)
      Self.clamp(&panelRect, to: screenRect)
      // Otherwise - a panel clamped back onto the screen over the text, or a field that reports the composition off its
      // position - the panel goes on a side it fits on, clear of everything the composition is reported to occupy, for
      // the rest of the composition.  The caret and the lines are reported afresh on every key and can wander by a
      // point or two, so the edge facing the text stays where it was for as long as that still clears the text.
      let occupied = (avoidRect ?? position).union(position)
      if avoiding || panelRect.intersects(occupied) {
        avoiding = true
        let aboveY = occupied.maxY + off
        let belowY = occupied.minY - off - panelRect.height
        let aboveFits = aboveY + panelRect.height <= screenRect.maxY
        let belowFits = belowY >= screenRect.minY
        if avoidAbove == nil { avoidAbove = aboveFits || !belowFits }
        if avoidAbove == true && !aboveFits && belowFits { avoidAbove = false; avoidingEdge = nil }
        if avoidAbove == false && !belowFits && aboveFits { avoidAbove = true; avoidingEdge = nil }
        if (avoidAbove == true && aboveFits) || (avoidAbove == false && belowFits) {
          // The edge facing the text: the bottom when above, the top when below.
          var edge = avoidAbove == true ? aboveY : belowY + panelRect.height
          if let kept = avoidingEdge,
             avoidAbove == true ? (kept >= occupied.maxY + off && kept + panelRect.height <= screenRect.maxY)
                                : (kept <= occupied.minY - off && kept - panelRect.height >= screenRect.minY) {
            edge = kept
          }
          avoidingEdge = edge
          panelRect.origin = NSPoint(x: position.minX - theme.pagingOffset,
                                     y: avoidAbove == true ? edge : edge - panelRect.height)
        } else {
          // No room above or below (a composition filling most of the screen): beside it, right if there is room.
          let rightX = occupied.maxX + off
          panelRect.origin.x = rightX + panelRect.width <= screenRect.maxX ? rightX : occupied.minX - off - panelRect.width
          panelRect.origin.y = min(max(occupied.maxY - panelRect.height, screenRect.minY), screenRect.maxY - panelRect.height)
        }
        // Sideways only: the vertical position was chosen to fit, and moving it now would move it onto the text.
        if panelRect.maxX > screenRect.maxX { panelRect.origin.x = screenRect.maxX - panelRect.width }
        if panelRect.minX < screenRect.minX { panelRect.origin.x = screenRect.minX }
      }
    }

    if vertical { Self.clamp(&panelRect, to: screenRect) }

    // `display: false`, so the window server does not animate the resize; the content is redrawn through
    // needsDisplay, or by the orderFront below.
    self.setFrame(panelRect, display: false)
    contentView!.frame = NSRect(origin: .zero, size: panelRect.size)

    // rotate the view, the core in vertical mode!
    if vertical {
      contentView!.boundsRotation = -90
      contentView!.setBoundsOrigin(NSPoint(x: 0, y: panelRect.width))
    } else {
      contentView!.boundsRotation = 0
      contentView!.setBoundsOrigin(.zero)
    }

    view.textView.boundsRotation = 0
    view.textView.setBoundsOrigin(.zero)

    // The subviews take the content view's bounds after rotation: in vertical mode Cocoa offsets the origin and
    // swaps the two dimensions.
    let subviewFrame = contentView!.bounds
    view.frame = subviewFrame

    var textFrame = subviewFrame
    textFrame.size.width -= theme.pagingOffset
    textFrame.origin.x += theme.pagingOffset
    view.textView.frame = textFrame

    if theme.translucency {
      var backFrame = subviewFrame
      backFrame.size.width += theme.pagingOffset
      back.frame = backFrame
      back.appearance = NSAppearance(named: view.isDark ? .darkAqua : .aqua)   // the system setting, as above
      back.isHidden = false
    } else {
      back.isHidden = true
    }

    alphaValue = theme.alpha
    invalidateShadow()
    // With `hide_panel`, the panel stays hidden while typing and shows only while candidates are being navigated.
    if theme.hidePanel && !navMode && !aquaTypeCompositionPanelVisible {
      orderOut(nil)
    } else if !isVisible {
      orderFront(nil)   // first show of this composition: raise once
    } else {
      // Let AppKit redraw on the next frame. Forcing a synchronous display here
      // makes every candidate refresh pay the full text-layout/draw cost on the
      // keyboard path, which is exactly the visible typing jank AquaType is trying
      // to avoid.
      contentView?.needsDisplay = true
    }
    // voila!
  }

  func show(status message: String) {
    let theme = view.currentTheme
    let text = NSMutableAttributedString(string: message, attributes: theme.attrs)
    text.addAttribute(.paragraphStyle, value: theme.paragraphStyle, range: NSRange(location: 0, length: text.length))
    view.textContentStorage.attributedString = text
    view.textView.setLayoutOrientation(vertical ? .vertical : .horizontal)
    view.drawView(candidateRanges: [NSRange(location: 0, length: text.length)], hilightedIndex: -1,
                  preeditRange: .empty, highlightedPreeditRange: .empty, canPageUp: false, canPageDown: false)
    show()

    statusTimer?.invalidate()
    // `.common`, not the default mode `scheduledTimer` installs, so the 中 / 英 badge is dismissed even while a menu is
    // open or a scroll is tracking; `[weak self]` because the timer holds its block.
    let timer = Timer(timeInterval: SquirrelTheme.showStatusDuration, repeats: false) { [weak self] _ in
      self?.hide()
    }
    RunLoop.main.add(timer, forMode: .common)
    statusTimer = timer
  }

  func convert(range: Range<String.Index>, in string: String) -> NSRange {
    let startPos = range.lowerBound.utf16Offset(in: string)
    let endPos = range.upperBound.utf16Offset(in: string)
    return NSRange(location: startPos, length: endPos - startPos)
  }
}
