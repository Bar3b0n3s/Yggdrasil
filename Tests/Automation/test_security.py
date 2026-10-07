"""Transport and path security (Docs/Architecture.md §13.2, §13.4, §15.7, Roadmap M4): token authentication, HTTP probes,
frame limits, and project paths that try to escape the project."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client


class SecurityTests(AutomationTestCase):
    def test_bad_token_rejected_and_closed_after_three(self) -> None:
        editor = self.start_editor()
        assert editor.session is not None
        connection = engine_client.connect_raw(editor.session.port)
        client = engine_client.EngineClient(connection)
        hello = {"token": "0" * 64, "protocolVersion": engine_client.PROTOCOL_VERSION,
                 "client": {"name": "intruder", "version": "1"}}
        for _ in range(3):
            response = client.request("session.hello", hello)
            self.assertEqual(response["error"]["code"], engine_client.UNAUTHORIZED)
        with self.assertRaises(engine_client.ConnectionClosed):
            client.receive_message(timeout=10.0)
        # The editor keeps serving clients with the right token.
        self.connect(editor).call("session.info")

    def test_http_probe_closes_socket(self) -> None:
        editor = self.start_editor()
        assert editor.session is not None
        connection = engine_client.connect_raw(editor.session.port)
        connection.sendall(b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://example.invalid\r\n\r\n")
        connection.settimeout(10.0)
        try:
            data = connection.recv(4096)
        except ConnectionResetError:
            data = b""
        self.assertEqual(data, b"")
        connection.close()

    def test_oversized_frame_closes(self) -> None:
        editor = self.start_editor()
        assert editor.session is not None
        client = self.connect(editor)
        client.send_raw(f"Content-Length: {engine_client.MAX_FRAME_PAYLOAD_BYTES + 1}\r\n\r\n".encode("ascii"))
        # Only a close passes: a server that ignored the header would leave the socket open and raise TimeoutError.
        with self.assertRaises(engine_client.ConnectionClosed):
            client.receive_message(timeout=10.0)
        self.connect(editor).call("session.info")

    def test_path_escapes_are_rejected_and_write_nothing_outside_the_project(self) -> None:
        client, _ = self.open_editor_with_scene()
        escapes = ("../Escape.scene", "Assets/../../Escape.scene", "/Escape.scene", "C:/Escape.scene", "user://Escape.scene",
                   "project://../Escape.scene", "Assets\\..\\..\\Escape.scene")
        for path in escapes:
            for method, params in (("scene.new", {"path": path, "discardChanges": True}), ("scene.open", {"path": path}),
                                   ("scene.save", {"path": path})):
                with self.subTest(method=method, path=path):
                    with self.assertRaises(engine_client.EngineError) as raised:
                        client.call(method, params)
                    self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)
                    self.assertEqual(raised.exception.issues[0]["pointer"], "/path")
        # Nothing was written: not next to the project, not in the user-data folder, not in the project.
        self.assertEqual(list(self.directory.rglob("Escape.scene")), [])


if __name__ == "__main__":
    unittest.main()
