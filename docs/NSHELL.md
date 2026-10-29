# nShell

nShell is an interactive shell for the local console and NLK sessions. Each
session has its own current directory, command history, status, and I/O state.
The `help` command lists the built-in commands.

The parser supports single and double quotes, backslash escapes, command lists
(`;`, `&&`, `||`), pipelines (`|`), and file redirections (`<`, `>`, `>>`).
Input redirection is consumed by `cat`, `wc`, and `grep`; these also read standard input
when passed `-`. Output redirection and pipelines apply to built-in command
output.
Redirections can appear without a command or between leading assignments;
their expanded paths are treated as single operands, including spaces.

Pipeline stages currently execute sequentially, using a 4096-byte buffer per
stage. A producer that exceeds this limit fails with an output error and the
remaining pipeline stages are skipped; output is not silently truncated. This
is not a POSIX streaming pipe: commands that require concurrent producer and
consumer execution, or more than 4096 bytes of intermediate data, are not yet
supported. Pipeline buffers use heap memory and allocation can fail.

The status of the preceding command or pipeline is available as `$?` or
`${?}`. It expands outside single quotes, including in double quotes and
redirection paths; a backslash can protect the dollar sign. All stages of a
pipeline see the status from before that pipeline.

Session variables can be assigned with commands such as `NAME='hello world'`
and expanded as `$NAME` or `${NAME}`. Unquoted expansions
are split on spaces and tabs; double quotes preserve the value as one argument,
and single quotes suppress expansion. `export NAME[=value]` marks variables for
export, and `unset [-v] NAME...` removes them. Sessions do not share variables;
remote session reset clears them. Expansion and the resulting command remain
limited to 256 bytes. Leading `NAME=value` assignments temporarily apply to
ordinary built-ins, and persist for assignment-only commands, `export`, and
`unset`. Command arguments are expanded before the assignments are applied;
assignment values are expanded in order, so later assignments see earlier ones.
The shell initializes `HOME` and `PWD` to `/`; `cd` uses `HOME`, updates `PWD`
and `OLDPWD`, and supports `cd -`. Paths are physical because the VFS does not
currently support symbolic links. Arbitrary `IFS`, advanced parameter operators, and passing an
environment to external processes are not implemented. Pipeline stages still
run in the current session, so assignment or `unset` in a pipeline persists.

The shell is not yet POSIX conformant. Full parameter and pathname expansion,
subshells, external processes, job control, and a complete standard error
stream remain to be implemented. Some NOS-specific commands and VFS behavior
also differ from Unix. Avoid using destructive operations until VFS object
lifetime and concurrent access are fully addressed.

VFS descriptor operations retain an in-flight file reference across `close`
and serialize operations on the same open file's position. RAMFS appends choose
the end offset under its inode lock; atomic append across different FatFs
descriptors is not yet supported. If an operation remains in
flight, `close` returns before the final file
release; a later release error cannot be reported by that `close` call.

`grep` supports fixed-string matching (`-F`), ASCII case folding (`-i`),
inverted selection (`-v`), line numbers (`-n`), counts (`-c`) and suppression of
filename prefixes (`-h`). Regular expressions are not implemented; patterns
containing regex metacharacters require `-F`. Lines longer than 65536 bytes or
larger than available heap memory fail with an error rather than being truncated.

`test` and `[` support nonempty and empty strings, string equality, signed
64-bit integer comparisons, `-e`, `-f`, `-d`, `-s`, negation, parentheses, and
the `-a` / `-o` operators. Other Unix file predicates are not implemented.
Malformed expressions and out-of-range integers return status 2.

`printf` supports `%s`, `%c`, `%b`, `%d`, `%i`, `%u`, `%o`, `%x`, `%X`,
`%%`, format reuse, and common backslash escapes. Numeric and string field
widths and precisions are limited to 256; `%b` field widths and precisions,
floating-point conversions, and dynamic `*` widths are not implemented. Invalid
formats, numbers, and output failures return a nonzero status.
