# AquaType Squirrel Fork Base

- Upstream repository: https://github.com/rime/squirrel
- Selected fork base: `1.1.2` (`876adeb`)
- Baseline import commit in AquaType: `9d0b142 Baseline stable AquaType lrpages runtime`
- Baseline commit date: 2026-06-03
- Confidence: medium

## Method

No explicit upstream SHA was found in local project notes or early AquaType commit messages. I cloned `https://github.com/rime/squirrel` with `--depth 50` and compared the AquaType baseline snapshot at `9d0b142:third_party/squirrel/sources/` against the available upstream tags using `git diff --no-index --shortstat`.

Comparison results:

| Upstream tag | SHA | Date | `sources/` diff against AquaType baseline |
| --- | --- | --- | --- |
| `latest` | `679c271` | 2026-06-12 | 7 files changed, 1570 insertions(+), 283 deletions(-) |
| `1.1.2` | `876adeb` | 2026-01-13 | 8 files changed, 1740 insertions(+), 104 deletions(-) |
| `1.1.1` | `bece016` | 2026-01-11 | 8 files changed, 1740 insertions(+), 104 deletions(-) |
| `1.1.0` | `4184891` | 2026-01-11 | 8 files changed, 1740 insertions(+), 104 deletions(-) |
| `1.0.3` | `8cf1e84` | 2025-01-23 | 8 files changed, 1781 insertions(+), 117 deletions(-) |

The moving `latest` tag was the smallest `sources/` diff, but it postdates the AquaType baseline import date, so it is not a plausible historical fork base. `1.1.2` matches `CURRENT_PROJECT_VERSION = 1.1.2` in the imported Squirrel Xcode project and is the closest plausible tagged release found in the shallow upstream comparison. Confidence is therefore medium rather than high.

## Main AquaType Modification Surface

- `third_party/squirrel/resources/Info.plist`: AquaType bundle identity and input-source metadata.
- `third_party/squirrel/sources/InputSource.swift`: AquaType-specific registration, enablement, and cleanup logic.
- `third_party/squirrel/sources/SquirrelInputController.swift`: AquaType candidate-panel, composition, fallback, memory, and backend integration behavior.
- `third_party/squirrel/sources/SquirrelPanel.swift`, `SquirrelView.swift`, `SquirrelTheme.swift`: AquaType candidate-panel rendering and UI behavior.
- `third_party/squirrel/data/`: AquaType Rime schema/config assets.
- `third_party/squirrel/resources/aquatype.pdf`: AquaType menu-bar badge asset.
