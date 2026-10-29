# nShell

nShell is an interactive shell for the local console and NLK sessions. Each
session has its own current directory, command history, status, and I/O state.
The `help` command lists the built-in commands.

The parser supports single and double quotes, backslash escapes, command lists
(`;`, `&&`, `||`), pipelines (`|`), and file redirections (`<`, `>`, `>>`).
Input redirection is consumed by `cat`, `wc`, and `grep`; these also read standard input
when passed `-`. Output redirection and pipelines apply to built-in command
output.

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

The shell is not yet POSIX conformant. General variable and pathname expansion,
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
