"""Local credential transfer boundaries, using deliberately fake test values."""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import importlib.util
from unittest.mock import patch,Mock
from pathlib import Path

SCRIPT=Path(__file__).resolve().parents[3]/'research/cross_venue/secure_kalshi_environment.py'
spec=importlib.util.spec_from_file_location('secure_environment',SCRIPT)
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)


class SecureTransferTests(unittest.TestCase):
    def test_expired_receiver_reports_actionable_nonsecret_code(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(helper.TransferError,'RECEIVER_NOT_RUNNING_OR_EXPIRED'):
                helper.send(Path(temporary)/'absent.sock')

    def test_direct_launch_keeps_credentials_out_of_argv_and_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'build').mkdir();key=root/'fake-key';key.write_text('NOT A PRIVATE KEY')
            for name in ['cross_venue_key_check','run_cross_venue_live_shadow','cross_venue_feed_audit']:(root/'build'/name).touch()
            fake='FAKE_TEST_VALUE_NOT_A_CREDENTIAL'
            with patch.object(helper,'ROOT',root),patch.dict(os.environ,{'KALSHI_API_KEY_ID':fake,'KALSHI_PRIVATE_KEY_PATH':str(key)}),patch.object(helper.subprocess,'Popen',return_value=Mock(pid=123)) as child,patch('builtins.print'):
                helper.launch()
            command=child.call_args.args[0];self.assertIn('--automatic',command);self.assertNotIn(fake,repr(command))
            status=(root/'runs/cross_venue/authenticated_ws_launch.latest.json').read_text()
            self.assertNotIn(fake,status);self.assertFalse(json.loads(status)['credentials_persisted'])
            self.assertFalse((root/'secrets').exists())

    def test_missing_export_fails_safely_without_interactive_tty(self):
        with patch.dict(os.environ,{},clear=True),patch.object(helper.sys.stdin,'isatty',return_value=False):
            with self.assertRaisesRegex(helper.TransferError,'KEY_ID_NOT_EXPORTED'):helper.launch()

    def test_same_uid_transfer_without_credential_output_or_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);key=root/'fake-key';key.write_text('NOT A PRIVATE KEY')
            sock=root/'env.sock';value='FAKE_TEST_VALUE_NEVER_A_REAL_CREDENTIAL'
            env=dict(os.environ);env.update(KALSHI_API_KEY_ID=value,KALSHI_PRIVATE_KEY_PATH=str(key))
            child='import os; assert os.environ["KALSHI_API_KEY_ID"] == "'+value+'"'
            server=subprocess.Popen([sys.executable,str(SCRIPT),'--socket',str(sock),'--timeout-sec','5','receive','--',sys.executable,'-c',child],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                for _ in range(50):
                    if sock.exists():break
                    time.sleep(.02)
                sender=subprocess.run([sys.executable,str(SCRIPT),'--socket',str(sock),'send'],env=env,capture_output=True,text=True,timeout=3)
                self.assertEqual(sender.returncode,0,sender.stderr)
                stdout,stderr=server.communicate(timeout=3)
                self.assertEqual(server.returncode,0,stderr)
                self.assertNotIn(value,sender.stdout+sender.stderr+stdout+stderr)
                self.assertFalse(sock.exists())
                self.assertEqual(set(p.name for p in root.iterdir()),{'fake-key'})
            finally:
                if server.poll() is None:server.terminate();server.wait(timeout=3)

    def test_sender_refuses_insecure_socket_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'insecure.sock'
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as listener:
                listener.bind(str(path));path.chmod(0o666)
                r=subprocess.run([sys.executable,str(SCRIPT),'--socket',str(path),'send'],capture_output=True,text=True,timeout=3)
                self.assertNotEqual(r.returncode,0)

    def test_missing_key_id_never_starts_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'env.sock'
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as listener:
                listener.bind(str(path));path.chmod(0o600)
                env=dict(os.environ);env.pop('KALSHI_API_KEY_ID',None)
                r=subprocess.run([sys.executable,str(SCRIPT),'--socket',str(path),'send'],env=env,capture_output=True,text=True,timeout=3)
                self.assertNotEqual(r.returncode,0)


if __name__=='__main__':unittest.main()
