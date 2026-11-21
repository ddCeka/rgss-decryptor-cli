RGSS Decryptor CLI
==================

Standalone, zero-dependency C11 Command-Line RGSS<sup>1</sup> Decryptor & Packer

* Native C11 implementation (no .NET or Mono runtime required)
* Unpacks & repacks `.rgssad`, `.rgss2a`, and `.rgss3a` archives
* Cross-platform path handling (Arch Linux, Termux/Android, macOS, Windows)
* Built-in path-traversal protection and truncated-file verification
* Optional RPG Maker project file generation (`Game.rxproj`, `Game.rvproj`, `Game.rvproj2`)
* Info Dump Only Mode (no extraction)

<sub><sup>1: RPG Maker XP/VX/VXAce Archives</sup></sub>

### **Usage**
----
Unpack an archive:

```bash
rgssdecrypter [options] <RgssArchive>
```

Repack a folder into an archive:

```bash
rgssdecrypter --pack <Folder> <OutputArchive>
# or
rgssdecrypter --repack <Folder> <OutputArchive>
```

### **Options**
---

| Short | Full | Description |
|-------|------|-------------|
| `-h` | `--help` | Displays Help Message |
| `-i` | `--info` | Dumps Archive Information Only |
| `-o` | `--output <dir>` | Output Directory (defaults to the archive's directory) |
| `-p` | `--project` | Creates RPG Maker Project File (`Game.rxproj`/`rvproj`/`rvproj2`) |
| `-s` | `--suppress` | Suppresses per-file extraction/packing output |
| — | `--pack <dir> <out>` | Packs an extracted directory into `.rgssad`, `.rgss2a`, or `.rgss3a` |
| — | `--repack <dir> <out>` | Alias for `--pack` |

### **Examples**
---

Extract an archive directly into its current directory and create the RPG Maker project file:

```bash
rgssdecrypter -p Game.rgss3a
```

Extract an archive into a custom subdirectory:

```bash
rgssdecrypter -o Extracted Game.rgss3a
```

Inspect archive contents (offsets, file sizes, decryption keys) without extracting:

```bash
rgssdecrypter -i Game.rgss3a
```

Repack an extracted game directory back into an encrypted RGSS3 archive:

```bash
rgssdecrypter --repack ./GameFolder Game.rgss3a
```

### **Download Prebuilt Binaries**
---

Prebuilt stripped binaries (including standalone static builds for Linux and Windows) and `SHA256SUMS.txt` are available on the **Releases** page:

* `rgssdecrypter-linux-x86_64` / `rgssdecrypter-linux-x86_64-static`
* `rgssdecrypter-windows-x86_64.exe` / `rgssdecrypter-windows-x86_64-static.exe`
* `rgssdecrypter-macos-arm64`

### **Build & Install**
---

Requires only a standard C11 compiler (`gcc` or `clang`) and `make`. Release builds are automatically stripped of debug symbols.

#### Arch Linux / Desktop Linux
```bash
make
sudo make install
```

#### Termux (Android)
```bash
pkg install clang make
make
make install
```
*(Note: Run `termux-setup-storage` once if unpacking or repacking files inside `/sdcard`.)*

#### Windows (MSYS2 / MinGW-w64)
```bash
make
```

#### Debug Build (AddressSanitizer & UBSan)
```bash
make debug
```

### **Changelog (From Original C# Release)**
---
* **Rewritten in C11:** Removed all .NET / Mono dependencies for fast, small, stripped native binaries.
* **Cross-Platform Path Normalization:** Automatically converts internal Windows `\` separators to OS-native separators during extraction and back to `\` during repacking.
* **Security & Integrity Checks:** Blocks path-traversal entries (`..`, leading slashes, drive prefixes), enforces strict path length bounds, and fails with exit code `1` on incomplete/truncated files.
* **Archive Repacking:** Added `--pack` / `--repack` support for rebuilding `.rgssad`, `.rgss2a`, and `.rgss3a` archives.
* **Multi-OS CI & Static Releases:** Added GitHub Actions builds and verification tests for Linux, macOS, and Windows (including `-static` standalone binaries and `SHA256SUMS.txt`).
* **Removed Windows-Only Shell Integration:** Dropped `--register` / `--unregister` registry context menu handlers in favor of cross-platform CLI portability.
