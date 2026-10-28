"""Host shell forwards bytes and closes its remote session on exit."""
import io
import os
import unittest
from unittest import mock
from scripts import nlk_tool


class Client:
    def __init__(self):
        self.calls = []
        self.sent = []
    def find_cap_id(self, addr, cap):
        return 3
    def rpc_call(self, addr, cap, op, data=b"", **kwargs):
        self.calls.append(op)
        if op == nlk_tool.NLK_SHELL_OP_INPUT:
            self.sent.append(data[12:])
    def poll_async_packet(self, timeout_ms):
        return None
    def stream_write(self, addr, cap, op, data):
        self.sent.append(data)


def nllk_payload(token, data):
    return nlk_tool.struct.pack("<Q", token) + data


class TerminalTest(unittest.TestCase):
    def test_reliable_chunks_retry_same_offset_after_backpressure(self):
        class BackpressureClient(Client):
            def __init__(self):
                super().__init__()
                self.offsets = []
                self.failed = False
            def rpc_call(self, addr, cap, op, data=b"", **kwargs):
                if op == nlk_tool.NLK_SHELL_OP_INPUT:
                    self.offsets.append(nlk_tool.struct.unpack_from("<I", data, 8)[0])
                    if not self.failed:
                        self.failed = True
                        raise nlk_tool.NLKRPCError(-28)
                return super().rpc_call(addr, cap, op, data, **kwargs)
        client = BackpressureClient()
        session = nlk_tool.RemoteShellSession(client, 1, 3)
        session.open()
        with mock.patch.object(nlk_tool.time, "sleep"):
            session.write(b"x" * 150)
        session.close()
        self.assertEqual(client.offsets, [0, 0, 64, 128])
        self.assertEqual(b"".join(client.sent), b"x" * 150)

    def test_falls_back_to_legacy_v1_shell_when_reliable_open_rejected(self):
        class LegacyClient(Client):
            def rpc_call(self, addr, cap, op, data=b"", **kwargs):
                if op == nlk_tool.NLK_SHELL_OP_OPEN and len(data) > nlk_tool.SHELL_OPEN_STRUCT.size:
                    raise nlk_tool.NLKRPCError(-22)
                return super().rpc_call(addr, cap, op, data, **kwargs)
        client = LegacyClient()
        session = nlk_tool.RemoteShellSession(client, 1, 3)
        session.open()
        self.assertFalse(session.reliable)
        session.write(b"pwd\r")
        session.close()
        self.assertEqual(client.sent, [b"pwd\r"])

    def test_fragmented_stdout_is_reassembled_before_dispatch(self):
        token = 42
        payload = nllk_payload(token, b"hello")
        base = {"src_addr": 1, "dst_addr": 0, "dst_cap": 3,
                "mode": nlk_tool.NLK_MODE_STREAM, "cmd_op": 0x12, "seq_id": 9}
        first = dict(base, flags=nlk_tool.NLK_FLAG_FRAG | nlk_tool.NLK_FLAG_MORE_FRAG,
                     payload=nlk_tool.struct.pack("<HH", len(payload), 0) + payload[:6])
        last = dict(base, flags=nlk_tool.NLK_FLAG_FRAG,
                    payload=nlk_tool.struct.pack("<HH", len(payload), 6) + payload[6:])
        transport = mock.Mock()
        transport.recv_packet.side_effect = [first, last]
        client = nlk_tool.NLKHostClient(transport)
        session = nlk_tool.RemoteShellSession(client, 1, 3)
        session.token = token
        self.assertEqual(session.poll(timeout_ms=100)["payload"], b"hello")

    def test_matching_stdout_preserves_other_async_packets(self):
        transport = mock.Mock()
        transport.recv_packet.side_effect = TimeoutError()
        client = nlk_tool.NLKHostClient(transport)
        other = {"src_addr": 2, "dst_cap": 3, "mode": nlk_tool.NLK_MODE_STREAM,
                 "cmd_op": nlk_tool.NLK_SHELL_STREAM_STDOUT, "payload": b"other"}
        wanted = dict(other, src_addr=1, payload=b"wanted")
        client.async_queue.extend((other, wanted))
        self.assertIs(client.poll_matching(1, 3, nlk_tool.NLK_SHELL_STREAM_STDOUT), wanted)
        self.assertEqual(list(client.async_queue), [other])

    def test_stdout_is_bound_to_session_token(self):
        transport = mock.Mock()
        transport.recv_packet.side_effect = TimeoutError()
        client = nlk_tool.NLKHostClient(transport)
        session = nlk_tool.RemoteShellSession(client, 1, 3)
        stale = {"src_addr": 1, "dst_cap": 3, "mode": nlk_tool.NLK_MODE_STREAM,
                 "cmd_op": 0x12, "payload": nlk_tool.struct.pack("<Q", session.token + 1) + b"old"}
        wanted = dict(stale, payload=nllk_payload(session.token, b"new"))
        client.async_queue.extend((stale, wanted))
        self.assertEqual(session.poll(timeout_ms=1)["payload"], b"new")
        self.assertEqual(list(client.async_queue), [stale])

    def test_split_utf8_is_decoded_across_packets(self):
        client = Client()
        packets = iter([{"payload": b"\xe4\xbd"}, {"payload": b"\xa0"}, None])
        client.poll_async_packet = lambda **kwargs: next(packets)
        stdin = mock.Mock()
        stdin.fileno.return_value = 5
        stdin.isatty.return_value = False
        output = io.StringIO()
        with mock.patch.object(nlk_tool.sys, "stdin", stdin), \
             mock.patch.object(nlk_tool.sys, "stdout", output), \
             mock.patch.object(nlk_tool.select, "select", side_effect=[([], [], []), ([], [], []), ([5], [], [])]), \
             mock.patch.object(nlk_tool.os, "read", return_value=b"\x04"):
            nlk_tool.cmd_shell_interactive(client, 1)
        self.assertIn("你", output.getvalue())
        self.assertNotIn("�", output.getvalue())

    @unittest.skipIf(os.name == "nt", "POSIX terminal test")
    def test_restores_terminal_after_transport_error(self):
        import termios
        import tty
        client = Client()
        stdin = mock.Mock()
        stdin.fileno.return_value = 5
        stdin.isatty.return_value = True
        saved = [1, 2, 3]
        with mock.patch.object(nlk_tool.sys, "stdin", stdin), \
             mock.patch.object(nlk_tool.sys, "stdout", io.StringIO()), \
             mock.patch.object(termios, "tcgetattr", return_value=saved), \
             mock.patch.object(tty, "setraw"), \
             mock.patch.object(termios, "tcsetattr") as restore, \
             mock.patch.object(nlk_tool.select, "select", side_effect=OSError("broken")):
            with self.assertRaises(OSError):
                nlk_tool.cmd_shell_interactive(client, 1)
        restore.assert_called_once_with(5, termios.TCSADRAIN, saved)
        self.assertEqual(client.calls[-1], nlk_tool.NLK_SHELL_OP_CLOSE)

    def test_character_input_and_close(self):
        client = Client()
        stdin = mock.Mock()
        stdin.fileno.return_value = 5
        stdin.isatty.return_value = False
        stdin.readline.side_effect = ["a\n", "exit\n"]
        with mock.patch.object(nlk_tool.sys, "stdin", stdin), \
             mock.patch.object(nlk_tool.sys, "stdout", io.StringIO()), \
             mock.patch.object(nlk_tool.select, "select", return_value=([5], [], [])), \
             mock.patch.object(nlk_tool.os, "read", side_effect=[b"a\x03\x04"]):
            nlk_tool.cmd_shell_interactive(client, 1)
        self.assertEqual(client.sent, [b"a\x03"])
        self.assertEqual(client.calls, [nlk_tool.NLK_SHELL_OP_OPEN,
                                        nlk_tool.NLK_SHELL_OP_INPUT,
                                        nlk_tool.NLK_SHELL_OP_CLOSE])

if __name__ == "__main__":
    unittest.main()
