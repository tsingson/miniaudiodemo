### name: zephyr_west_environment
description: Enforce the absolute path for the Zephyr west tool in this workspace.

### Zephyr Environment Rules

### Global Path Enforcement

* Whenever the user asks to run, generate, or debug Zephyr commands, **ALWAYS** use the absolute path for the west tool.
* The absolute path to west is precisely: /Users/qinshen/go/zephyrproject/.venv/bin/west

### Command Generation Guidelines

* **NEVER** use a bare west command in code blocks or terminal instructions.
* Ensure all custom build, flash, or emulation tasks reference this full interpreter path.

### Examples

* ❌ **Incorrect**: west build -b nucleo_f401re
* **Correct**: /Users/qinshen/go/zephyrproject/.venv/bin/west build -b nucleo_f401re

