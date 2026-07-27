---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: TUI

Terminal UI library (`src/tui/`) for text-based interfaces. All paths below are relative to `src/tui/`.

## Overview

- **Total source files**: 108 (.hpp/.cpp)
- **Subdirectories**: include/, src/, apps/, tests/

## Application Entry Point (`apps/`)

| File           | Purpose                |
|----------------|------------------------|
| `tui_demo.cpp` | Sample TUI application |

## Public Headers (`include/tui/`)

### Core API

| File                       | Purpose                            |
|----------------------------|------------------------------------|
| `include/tui/app.hpp`      | Main application class             |
| `include/tui/version.hpp`  | Library version information        |

### Terminal (`include/tui/term/`)

| File                               | Purpose                        |
|------------------------------------|--------------------------------|
| `include/tui/term/clipboard.hpp`   | Clipboard access               |
| `include/tui/term/CsiParser.hpp`   | CSI escape sequence parser     |
| `include/tui/term/input.hpp`       | Input handling                 |
| `include/tui/term/key_event.hpp`   | Key event types                |
| `include/tui/term/session.hpp`     | Terminal session management    |
| `include/tui/term/term_io.hpp`     | Terminal I/O abstractions      |
| `include/tui/term/Utf8Decoder.hpp` | UTF-8 decoding                 |

### Rendering (`include/tui/render/`)

| File                               | Purpose                      |
|------------------------------------|------------------------------|
| `include/tui/render/box.hpp`       | Box drawing utilities        |
| `include/tui/render/renderer.hpp`  | Rendering interface          |
| `include/tui/render/screen.hpp`    | Screen buffer                |
| `include/tui/render/text.hpp`      | Text rendering utilities     |

### Text Handling (`include/tui/text/`)

| File                                | Purpose                      |
|-------------------------------------|------------------------------|
| `include/tui/text/EditHistory.hpp`  | Undo/redo history            |
| `include/tui/text/LineIndex.hpp`    | Line indexing                |
| `include/tui/text/PieceTable.hpp`   | Piece table implementation   |
| `include/tui/text/search.hpp`       | Text search functionality    |
| `include/tui/text/text_buffer.hpp`  | Text buffer interface        |

### UI Framework (`include/tui/ui/`)

| File                            | Purpose                      |
|---------------------------------|------------------------------|
| `include/tui/ui/container.hpp`  | Container widget             |
| `include/tui/ui/event.hpp`      | UI event types               |
| `include/tui/ui/focus.hpp`      | Focus management             |
| `include/tui/ui/modal.hpp`      | Modal dialog support         |
| `include/tui/ui/widget.hpp`     | Base widget interface        |

### Views (`include/tui/views/`)

| File                              | Purpose                      |
|-----------------------------------|------------------------------|
| `include/tui/views/text_view.hpp` | Text editor view             |

### Widgets (`include/tui/widgets/`)

| File                                       | Purpose                |
|--------------------------------------------|------------------------|
| `include/tui/widgets/button.hpp`           | Button widget          |
| `include/tui/widgets/command_palette.hpp`  | Command palette widget |
| `include/tui/widgets/label.hpp`            | Label widget           |
| `include/tui/widgets/list_view.hpp`        | List view widget       |
| `include/tui/widgets/search_bar.hpp`       | Search bar widget      |
| `include/tui/widgets/splitter.hpp`         | Splitter layout        |
| `include/tui/widgets/status_bar.hpp`       | Status bar widget      |
| `include/tui/widgets/tree_view.hpp`        | Tree view widget       |

### Style (`include/tui/style/`)

| File                           | Purpose                      |
|--------------------------------|------------------------------|
| `include/tui/style/theme.hpp`  | Theme definitions            |

### Syntax (`include/tui/syntax/`)

| File                            | Purpose                      |
|---------------------------------|------------------------------|
| `include/tui/syntax/rules.hpp`  | Syntax highlighting rules    |

### Configuration (`include/tui/config/`)

| File                              | Purpose                    |
|-----------------------------------|----------------------------|
| `include/tui/config/config.hpp`   | Configuration types        |

### Input (`include/tui/input/`)

| File                             | Purpose                    |
|----------------------------------|----------------------------|
| `include/tui/input/keymap.hpp`   | Keymap definitions         |

### Utilities (`include/tui/util/`, `include/tui/support/`)

| File                                    | Purpose                    |
|-----------------------------------------|----------------------------|
| `include/tui/util/color.hpp`            | Color manipulation         |
| `include/tui/support/function_ref.hpp`  | Function reference utility |
| `include/tui/util/numeric.hpp`          | Numeric helpers            |
| `include/tui/util/string.hpp`           | String helpers             |
| `include/tui/util/unicode.hpp`          | Unicode helpers            |

## Source Implementation (`src/`)

### Core

| File                | Purpose                            |
|---------------------|------------------------------------|
| `src/app.cpp`       | Application implementation         |
| `src/version.cpp`   | Version API implementation         |

### Terminal (`src/term/`)

| File                       | Purpose                        |
|----------------------------|--------------------------------|
| `src/term/clipboard.cpp`   | Clipboard implementation       |
| `src/term/CsiParser.cpp`   | CSI parser implementation      |
| `src/term/input.cpp`       | Input handling implementation  |
| `src/term/session.cpp`     | Session implementation         |
| `src/term/term_io.cpp`     | Terminal I/O implementation    |
| `src/term/Utf8Decoder.cpp` | UTF-8 decoder implementation   |

### Rendering (`src/render/`)

| File                      | Purpose                        |
|---------------------------|--------------------------------|
| `src/render/box.cpp`      | Box drawing implementation     |
| `src/render/renderer.cpp` | Renderer implementation        |
| `src/render/screen.cpp`   | Screen buffer implementation   |

### Text (`src/text/`)

| File                       | Purpose                        |
|----------------------------|--------------------------------|
| `src/text/EditHistory.cpp` | Edit history implementation    |
| `src/text/LineIndex.cpp`   | Line index implementation      |
| `src/text/PieceTable.cpp`  | Piece table implementation     |
| `src/text/search.cpp`      | Search implementation          |
| `src/text/text_buffer.cpp` | Text buffer implementation     |

### UI (`src/ui/`)

| File                   | Purpose                        |
|------------------------|--------------------------------|
| `src/ui/container.cpp` | Container implementation       |
| `src/ui/focus.cpp`     | Focus management impl          |
| `src/ui/modal.cpp`     | Modal dialog implementation    |
| `src/ui/widget.cpp`    | Widget implementation          |

### Views (`src/views/`)

| File                          | Purpose                        |
|-------------------------------|--------------------------------|
| `src/views/text_view_cursor.cpp` | Text view cursor handling   |
| `src/views/text_view_input.cpp`  | Text view input handling    |
| `src/views/text_view_render.cpp` | Text view rendering         |

### Widgets (`src/widgets/`)

| File                              | Purpose                        |
|-----------------------------------|--------------------------------|
| `src/widgets/button.cpp`          | Button implementation          |
| `src/widgets/command_palette.cpp` | Command palette implementation |
| `src/widgets/label.cpp`           | Label implementation           |
| `src/widgets/list_view.cpp`       | List view implementation       |
| `src/widgets/search_bar.cpp`      | Search bar implementation      |
| `src/widgets/splitter.cpp`        | Splitter implementation        |
| `src/widgets/status_bar.cpp`      | Status bar implementation      |
| `src/widgets/tree_view.cpp`       | Tree view implementation       |

### Style (`src/style/`)

| File                 | Purpose                        |
|----------------------|--------------------------------|
| `src/style/theme.cpp`| Theme implementation           |

### Syntax (`src/syntax/`)

| File                  | Purpose                        |
|-----------------------|--------------------------------|
| `src/syntax/rules.cpp`| Syntax rules implementation    |

### Config (`src/config/`)

| File                   | Purpose                        |
|------------------------|--------------------------------|
| `src/config/config.cpp`| Configuration implementation   |

### Input (`src/input/`)

| File                  | Purpose                        |
|-----------------------|--------------------------------|
| `src/input/keymap.cpp`| Keymap implementation          |

### Utilities (`src/util/`)

| File                   | Purpose                        |
|------------------------|--------------------------------|
| `src/util/unicode.cpp` | Unicode helpers implementation |
| `src/util/color.cpp`   | Color manipulation impl        |

## Tests (`tests/`)

| File                                          | Purpose                          |
|-----------------------------------------------|----------------------------------|
| `tests/test_app_layout.cpp`                   | Application layout tests         |
| `tests/test_app_resize.cpp`                   | Application resize tests         |
| `tests/test_clipboard.cpp`                    | Clipboard tests                  |
| `tests/test_config.cpp`                       | Configuration tests              |
| `tests/test_demo_headless.cpp`                | Headless demo tests              |
| `tests/test_focus.cpp`                        | Focus management tests           |
| `tests/test_focus_hooks.cpp`                  | Focus hook tests                 |
| `tests/test_input_csi.cpp`                    | CSI input tests                  |
| `tests/test_input_mouse_paste.cpp`            | Mouse/paste input tests          |
| `tests/test_input_utf8.cpp`                   | UTF-8 input tests                |
| `tests/test_keymap_palette.cpp`               | Keymap/palette tests             |
| `tests/test_list_tree.cpp`                    | List/tree view tests             |
| `tests/test_modal.cpp`                        | Modal dialog tests               |
| `tests/test_renderer_minimal.cpp`             | Minimal renderer tests           |
| `tests/test_screen_diff.cpp`                  | Screen diff tests                |
| `tests/test_search.cpp`                       | Search functionality tests       |
| `tests/test_session.cpp`                      | Session tests                    |
| `tests/test_smoke.cpp`                        | Basic smoke tests                |
| `tests/test_split_status.cpp`                 | Splitter/status bar tests        |
| `tests/test_splitter_keyboard.cpp`            | Splitter keyboard tests          |
| `tests/test_syntax.cpp`                       | Syntax highlighting tests        |
| `tests/test_term_io.cpp`                      | Terminal I/O tests               |
| `tests/test_text_buffer.cpp`                  | Text buffer tests                |
| `tests/test_text_view.cpp`                    | Text view tests                  |
| `tests/test_unicode_grapheme.cpp`             | Unicode grapheme tests           |
| `tests/test_unicode_width.cpp`                | Unicode width tests              |
| `tests/test_widgets_basic.cpp`                | Basic widget tests               |
| `tests/test_win_vt.cpp`                       | Windows VT tests                 |
| `tests/views/test_text_view_large_buffer.cpp` | Large buffer text view tests     |
