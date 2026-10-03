# CLI parser

The CLI accepts shell-like whitespace, single and double quotes, backslash
escaping, empty quoted arguments, and a built-in `help [command]` command.
Arguments are positional. Required arguments must be declared before optional
arguments so parsing remains unambiguous. A final argument may be marked
`variadic` to accept any number of remaining values.

Registered flags support short and long forms, bundled boolean flags such as
`-al`, value forms such as `-o file`, `-ofile`, `--output file` and
`--output=file`, and `--` to stop flag parsing.

Unquoted, unescaped `*` characters expand against the CLI-local working
directory before argument parsing:

```text
ls *.txt
cat logs/*.txt | grep error
cp assets/*.bin backup
rm build/*.tmp
```

`*` matches within one path component and does not cross `/`. Results are
sorted, hidden names only match patterns whose component starts with `.`, and
an unmatched pattern is passed to the command unchanged. Use `"*.txt"` or
`\*.txt` when a literal asterisk is required. Expansion is capped at 256
matches to keep embedded memory use bounded.

Pipelines and output redirection are supported, including operators adjacent
to words:

```text
cat messages.txt | grep -in warning
echo "first line" > notes.txt
echo "another line">>notes.txt
```

`>` truncates and `>>` appends. The destination is resolved against the
CLI-local working directory. Operators inside quotes, or escaped with a
backslash, remain ordinary argument text. Redirection must follow the final
pipeline command. Intermediate pipeline output is bounded to 32 KiB so a
command cannot exhaust STM32 memory.

Register commands before `system_threads::start()` launches the CLI thread:

```cpp
#include "cli/Parser.hpp"

auto result = cli::registry().registerCommand({
  .name = "copy",
  .description = "Copy a source to an optional destination",
  .arguments =
    {
      { .name = "source", .description = "Source path" },
      { .name = "destination", .description = "Destination path", .optional = true },
    },
  .flags =
    {
      {
        .name = "force",
        .short_name = 'f',
        .description = "Replace an existing destination",
        .value_name = std::nullopt,
      },
      {
        .name = "mode",
        .short_name = 'm',
        .description = "Select the copy mode",
        .value_name = "name",
      },
    },
  .callback =
    [](const cli::Arguments& arguments, std::ostream& output) {
        output << "source: " << arguments.require("source") << '\n';
        if (const auto destination = arguments.get("destination")) {
            output << "destination: " << *destination << '\n';
        }
        output << "force: " << arguments.hasFlag("force") << '\n';
        if (const auto mode = arguments.flagValue("mode")) {
            output << "mode: " << *mode << '\n';
        }
        return 0;
    },
});

if (!result) {
    throw std::runtime_error(result.error());
}
```

Callbacks returning `int` remain supported, and that value is exposed as
`ExecutionResult::exit_code`. For an operational failure, return
`cli::callbackFailure("diagnostic", exit_code)`. The parser turns it into a
`CallbackFailed` result, prefixes it with the command name, and displays it in
both terminal frontends. Callback exceptions are still contained as a final
safety boundary and cannot terminate the CLI loop.

Commands that consume pipeline input use the stream-aware callback form:

```cpp
.callback =
  [](const cli::Arguments&, cli::CommandIO& io) {
      std::string line;
      while (std::getline(io.input, line)) {
          io.output << line << '\n';
      }
      return 0;
  },
```

The existing `(const cli::Arguments&, std::ostream&)` form is automatically
adapted and remains source-compatible.

The built-in filesystem command set contains `pwd`, `cd`, `ls`, `touch`,
`mkdir`, `rmdir`, `rm`, `cp`, `mv`, and `cat`. Paths are resolved against a
CLI-local working directory, so using `cd` does not alter the process working
directory or other application threads. Utility commands include variadic
`echo` (`-n`) and fixed-text `grep` (`-i`, `-n`, and `-v`). `grep` reads its
optional file argument or consumes standard input from the previous pipeline
stage. `stack` prints per-thread stack usage, and `heap` prints allocator
capacity, reservation, usage, available space, and fragmentation details with
a vertical usage bar spanning the metric rows.
Linux obtains heap statistics from glibc and reports its capacity as dynamic;
STM32 combines Newlib allocator statistics with the linker-defined AXI-SRAM
heap bounds.

## Linux terminal frontend

When both stdin and stdout are attached to a supported terminal, the Linux
simulation uses an ncursesw frontend. HAL and Pneumo log records remain in the
scrollable upper area while command input stays on the bottom line. Use the
arrow keys for editing and history, Page Up/Page Down for log history, and
Ctrl-C to close the CLI frontend.

Redirected input/output, `TERM=dumb`, and
`SIMPLCITY_CLI_PLAIN=1` select the plain stream frontend instead. This keeps
the CLI scriptable: commands and prompts use stdout, while HAL and logging
records use stderr. The STM32 build continues to use the original
`std::cin`/`std::cout` serial frontend.
