# Building AquaType

These steps build the AquaType installer from this repository. The result matches the released package except for build stamps and signatures.

## Requirements

- A Mac with Apple silicon
- Xcode (tested with Xcode 26.3)
- Python 3.11

## 1. Set up Python

```sh
python3.11 -m venv .venv
.venv/bin/python -m pip install -r requirements-build.txt
```

The MLX version in `requirements-build.txt` is the MLX runtime the app ships.

## 2. Get librime and Rime's base data

The input method links the librime build that ships with Squirrel 1.1.2. Copy it, with Rime's base data, from the official Squirrel package:

```sh
cd third_party/squirrel
mkdir -p download lib bin data/plum
curl -fL -o download/Squirrel-1.1.2.pkg https://github.com/rime/squirrel/releases/download/1.1.2/Squirrel-1.1.2.pkg
echo "614746013212937623d5bbab9901e9c43d1ec937aa32307d6b6092a05e308287  download/Squirrel-1.1.2.pkg" | shasum -a 256 -c
pkgutil --expand-full download/Squirrel-1.1.2.pkg download/Squirrel-1.1.2
SQ=download/Squirrel-1.1.2/Payload/Squirrel.app/Contents
cp $SQ/Frameworks/librime.1.dylib lib/
cp -R $SQ/Frameworks/rime-plugins lib/
cp $SQ/MacOS/rime_deployer $SQ/MacOS/rime_dict_manager $SQ/MacOS/rime-install bin/
find $SQ/SharedSupport -maxdepth 1 -type f -exec cp {} data/plum/ \;
cp -R $SQ/SharedSupport/opencc data/opencc
touch data/plum/user.yaml
cd ../..
```

Use these steps instead of Squirrel's `action-install.sh`, which downloads a newer librime and current Rime data.

## 3. Restore the model

The model weights and the fallback dictionary aren't stored in git. Restore them from a released package:

```sh
packaging/restore_payload.sh --release v0.9.0
```

This needs the [GitHub CLI](https://cli.github.com). To restore from a package you've already downloaded, pass its path instead: `packaging/restore_payload.sh AquaType-0.9.0.pkg`. Either way, the files are checked against `packaging/payload/SHA256SUMS`.

## 4. Build

```sh
packaging/build_app.sh
```

The installer and the uninstaller are written to `dist/`. The build runs the engine's tests and typing self-tests, and stops if any of them fail. It signs with a Developer ID Application identity if your keychain has one; otherwise it signs ad hoc.

## Notes

- The dictionary files in `packaging/payload/rime/` are committed as built. The scripts that generate them are included for reference, but their source data isn't part of this repository.
- The training code isn't included in this release.
