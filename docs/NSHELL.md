# nShell

nShell is an interactive shell for the local console and NLK sessions. Each
session has its own current directory, command history, status, and I/O state.
The `help` command lists the built-in commands.

The parser supports single and double quotes, backslash escapes, command lists
(`;`, `&&`, `||`), pipelines (`|`), and file redirections (`<`, `>`, `>>`).
Input redirection is consumed by `cat` and `wc`; both also read standard input
when passed `-`. Output redirection and pipelines apply to built-in command
output.

Pipeline stages currently execute sequentially, using a 4096-byte buffer per
stage. A producer that exceeds this limit fails with an output error and the
remaining pipeline stages are skipped; output is not silently truncated. This
is not a POSIX streaming pipe: commands that require concurrent producer and
consumer execution, or more than 4096 bytes of intermediate data, are not yet
supported. Pipeline buffers use heap memory and allocation can fail.

The shell is not yet POSIX conformant. Variable and pathname expansion,
subshells, external processes, job control, and a complete standard error
stream remain to be implemented. Some NOS-specific commands and VFS behavior
also differ from Unix. Avoid using destructive operations until VFS object
lifetime and concurrent access are fully addressed.
