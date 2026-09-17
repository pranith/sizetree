#!/usr/bin/env python3
"""Behavioral CLI and real terminal interaction tests; no third-party packages."""

import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import resource
import select
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
from unittest import mock


EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
DEFAULT_CONFIG = "cache_enabled=true\nrefresh_interval_seconds=86400\n"


class TerminalSession:
    def __init__(self, directory, rows=24, columns=100, arguments=(), command=None):
        self.master, self.slave = pty.openpty()
        self.original_settings = termios.tcgetattr(self.slave)
        self.resize(rows, columns)
        self.output = ""

        def controlling_terminal():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        self.process = subprocess.Popen(
            command or [EXECUTABLE, *arguments], cwd=directory,
            stdin=self.slave, stdout=self.slave, stderr=self.slave,
            env={**os.environ, "TERM": "xterm-256color", "PS1": "sizetree-test$ ",
                 "PROMPT_COMMAND": ""},
            preexec_fn=controlling_terminal,
        )

    def resize(self, rows, columns):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))

    def send(self, keys):
        os.write(self.master, keys)

    def screen(self):
        return ANSI.sub("", self.output.rsplit("\x1b[H", 1)[-1]).replace("\r", "")

    def first_screen(self):
        frames = self.output.split("\x1b[H")
        for frame in frames[1:]:
            screen = ANSI.sub("", frame).replace("\r", "")
            if "Total:" in screen:
                return screen
        return ""

    def wait_for(self, predicate, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate(self.screen()):
                return self.screen()
            readable, _, _ = select.select([self.master], [], [], 0.05)
            if readable:
                try:
                    data = os.read(self.master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                self.output += data.decode("utf-8", errors="replace")
        raise AssertionError(f"Expected terminal state not reached. Last screen:\n{self.screen()}")

    def finish(self, keys=b"q", expected_status=0):
        self.send(keys)
        status = self.process.wait(timeout=5)
        if status != expected_status:
            raise AssertionError(f"Exit status {status}, expected {expected_status}")
        if termios.tcgetattr(self.slave) != self.original_settings:
            raise AssertionError("Terminal settings were not restored")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        os.close(self.master)
        os.close(self.slave)

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()


class JobControlSession(TerminalSession):
    """A real job-control shell keeps the app's process group non-orphaned."""

    def __init__(self, directory):
        self.job_group = None
        super().__init__(directory, command=[shutil.which("bash"), "--noprofile", "--norc",
                                             "--noediting", "-i"])

    def start(self):
        self.wait_for(lambda screen: screen.endswith("sizetree-test$ "))
        self.output = ""
        self.send((shlex.quote(EXECUTABLE) + " --no-cache\n").encode())
        # Record the job before checking the screen so failed starts can be cleaned up.
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            group = os.tcgetpgrp(self.master)
            if group > 0 and group != self.process.pid:
                self.job_group = group
                break
            time.sleep(0.01)
        self.wait_for(lambda screen: "Scanned " in screen and "PgUp/PgDn:" in screen)

    def suspend(self):
        self.output = ""
        self.send(b"\x1a")
        self.wait_for(lambda screen: "Stopped" in screen and screen.endswith("sizetree-test$ "))

    def foreground(self):
        self.output = ""
        self.send(b"fg\n")
        self.wait_for(lambda screen: "Scanned " in screen and "PgUp/PgDn:" in screen)

    def finish(self, keys=b"q", expected_status=0):
        self.output = ""
        self.send(keys)
        self.wait_for(lambda screen: screen.endswith("sizetree-test$ "))
        super().finish(b'exit "$?"\n', expected_status)
        self.job_group = None

    def close(self):
        if self.job_group is not None:
            try:
                os.killpg(self.job_group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        super().close()


class SizetreeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="sizetree-test-")
        self.addCleanup(self.temporary.cleanup)
        self.cache_temporary = tempfile.TemporaryDirectory(prefix="sizetree-cache-test-")
        self.addCleanup(self.cache_temporary.cleanup)
        self.cache_home = Path(self.cache_temporary.name)
        self.config_temporary = tempfile.TemporaryDirectory(prefix="sizetree-config-test-")
        self.addCleanup(self.config_temporary.cleanup)
        self.config_home = Path(self.config_temporary.name)
        self.config_file = self.config_home / "sizetree" / "config"
        self.config_file.parent.mkdir()
        self.config_file.write_text(DEFAULT_CONFIG)
        environment = mock.patch.dict(os.environ, {
            "XDG_CACHE_HOME": str(self.cache_home), "XDG_CONFIG_HOME": str(self.config_home),
        })
        environment.start()
        self.addCleanup(environment.stop)
        self.root = Path(self.temporary.name)
        (self.root / "alpha" / "nested").mkdir(parents=True)
        (self.root / "alpha" / "payload.bin").write_bytes(b"a" * 2048)
        (self.root / "alpha" / "nested" / "secret.bin").write_bytes(b"b" * 1024)
        (self.root / "beta").mkdir()
        (self.root / "beta" / "tiny.txt").write_bytes(b"x")
        (self.root / "loose.txt").write_bytes(b"1234567")
        (self.root / ".hidden").write_bytes(b"123")
        (self.root / "link").symlink_to("alpha", target_is_directory=True)
        (self.root / "dangling").symlink_to("missing")
        (self.root / "alpha" / "back").symlink_to("..", target_is_directory=True)
        os.mkfifo(self.root / "pipe")

    def cli(self, *arguments, cwd=None):
        return subprocess.run(
            [EXECUTABLE, *map(str, arguments)], cwd=cwd or self.root,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True, timeout=5,
        )

    def test_recursive_sizes_and_plain_output(self):
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Total: 3.0 KiB", result.stdout)
        self.assertRegex(result.stdout, r"3\.0 KiB\s+\[\+\] alpha")
        self.assertRegex(result.stdout, r"1 B\s+\[\+\] beta")
        self.assertRegex(result.stdout, r"7 B\s+loose\.txt")
        self.assertRegex(result.stdout, r"3 B\s+\.hidden")
        self.assertNotIn("payload.bin", result.stdout)
        self.assertNotIn("\x1b", result.stdout)
        self.assertLess(result.stdout.index("[+] alpha"), result.stdout.index("[+] beta"))
        self.assertLess(result.stdout.index("[+] beta"), result.stdout.index("loose.txt"))
        self.assertEqual(self.cli("--list").stdout, result.stdout)
        self.assertEqual(result.stderr, "")

    def test_list_progress_uses_terminal_stderr_and_keeps_redirected_stdout_plain(self):
        expected = self.cli("--no-cache", "--list").stdout
        for arguments in (("--list",), ()):
            with self.subTest(arguments=arguments):
                master, slave = pty.openpty()
                try:
                    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
                    original = termios.tcgetattr(slave)
                    result = subprocess.run(
                        [EXECUTABLE, "--no-cache", *arguments], cwd=self.root,
                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=slave,
                        universal_newlines=True, timeout=5,
                    )
                    self.assertEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, expected)
                    self.assertEqual(termios.tcgetattr(slave), original)
                    output = b""
                    while select.select([master], [], [], 0)[0]:
                        output += os.read(master, 65536)
                    progress = output.decode()
                    self.assertIn("Scanning [|] | 0 entries | ~0 B | 0s", progress)
                    self.assertRegex(progress, r"Finishing .*12 entries \| 3\.0 KiB \| \d+s")
                    self.assertRegex(progress, r"\r +\r$")
                    self.assertNotIn("\x1b", progress)
                finally:
                    os.close(master)
                    os.close(slave)

    def test_symlinks_and_special_files_are_excluded(self):
        result = self.cli("--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ("link", "dangling"):
            self.assertRegex(result.stdout, rf"-\s+\[@\] {name}")
        self.assertRegex(result.stdout, r"-\s+\[\*\] pipe")
        self.assertNotIn("!", result.stdout)

    def test_large_sparse_files_and_hard_links_count_logical_bytes(self):
        directory = self.root / "large-files"
        directory.mkdir()
        sparse = directory / "sparse.bin"
        with sparse.open("wb") as output:
            output.truncate(5 * 1024 ** 3)
        os.link(str(sparse), str(directory / "hard-link.bin"))
        result = self.cli("--list", directory)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Total: 10.0 GiB", result.stdout)
        self.assertRegex(result.stdout, r"5\.0 GiB\s+sparse\.bin")
        self.assertRegex(result.stdout, r"5\.0 GiB\s+hard-link\.bin")

    def test_empty_directory_and_explicit_path(self):
        empty = self.root / "empty"
        empty.mkdir()
        result = self.cli("--list", empty)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Total: 0 B", result.stdout)
        self.assertNotIn("[+]", result.stdout)

    def test_paths_with_spaces_and_leading_dash(self):
        directory = self.root / "-a directory"
        directory.mkdir()
        (directory / "one").write_bytes(b"1")
        result = self.cli("--", "-a directory")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Total: 1 B", result.stdout)

    def test_bad_arguments_and_help(self):
        for arguments in (("missing",), ("loose.txt",), ("--bogus",), ("alpha", "beta"),
                          ("-j",), ("-j", "0"), ("-j", "65"), ("-j", "-1"),
                          ("--threads", "3.5"), ("--threads", "abc"), ("-j", "9999999999999999")):
            with self.subTest(arguments=arguments):
                result = self.cli(*arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("sizetree:", result.stderr)
        help_result = self.cli("--help")
        self.assertEqual(help_result.returncode, 0)
        self.assertIn("Up / Down", help_result.stdout)
        self.assertRegex(help_result.stdout, r"Tab +Expand or collapse")

    def test_parallel_scan_matches_single_worker_across_many_branches(self):
        tree = self.root / "many-branches"
        tree.mkdir()
        # More than one publication batch: descendants may finish while their
        # parent is still discovering siblings. Completion must include them all.
        for index in range(160):
            for leaf in range(3):
                directory = tree / ("branch-%03d" % index) / ("leaf-%d" % leaf)
                directory.mkdir(parents=True)
                (directory / "payload").write_bytes(b"x")
                (directory / "back").symlink_to("..", target_is_directory=True)
        reference = self.cli("--list", "-j", "1", tree)
        self.assertEqual(reference.returncode, 0, reference.stderr)
        self.assertIn("Total: 480 B", reference.stdout)
        self.assertEqual(len(re.findall(r"3 B\s+\[\+\] branch-", reference.stdout)), 160)
        for threads in (2, 8, 64):
            with self.subTest(threads=threads):
                result = self.cli("--list", "--threads", threads, tree)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, reference.stdout)

    def test_large_flat_directory_crosses_entry_buffers(self):
        tree = self.root / "flat"
        tree.mkdir()
        total = 0
        for index in range(2048):
            name = "file-%04d-" % index + "x" * 180 + "-π"
            payload = b"x" * (index % 13)
            (tree / name).write_bytes(payload)
            total += len(payload)
        reference = self.cli("--list", "--no-cache", "-j", "1", tree)
        self.assertEqual(reference.returncode, 0, reference.stderr)
        self.assertEqual(len(re.findall(r"file-\d{4}-", reference.stdout)), 2048)
        self.assertIn("Total: %.1f KiB" % (total / 1024), reference.stdout)
        parallel = self.cli("--list", "--no-cache", "-j", "8", tree)
        self.assertEqual(parallel.returncode, 0, parallel.stderr)
        self.assertEqual(parallel.stdout, reference.stdout)

    def test_parent_directory_handles_respect_low_descriptor_limit(self):
        tree = self.root / "limited-handles"
        for parent in range(40):
            for child in range(5):
                directory = tree / str(parent) / str(child)
                directory.mkdir(parents=True)
                (directory / "payload").write_bytes(b"x")

        def restrict_descriptors():
            _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            resource.setrlimit(resource.RLIMIT_NOFILE, (32, hard))

        result = subprocess.run(
            [EXECUTABLE, "--list", "--no-cache", "-j", "8", str(tree)], cwd=self.root,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True,
            preexec_fn=restrict_descriptors, timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Total: 200 B", result.stdout)
        self.assertEqual(len(re.findall(r"5 B\s+\[\+\]", result.stdout)), 40)

    @unittest.skipIf(os.geteuid() == 0, "root can read directories regardless of their permissions")
    def test_parallel_scan_propagates_nested_errors_and_keeps_other_totals(self):
        locked = self.root / "alpha" / "nested" / "locked"
        locked.mkdir()
        (locked / "secret").write_bytes(b"x")
        locked.chmod(0)
        try:
            reference = self.cli("--list", "-j", "1")
            parallel = self.cli("--list", "-j", "8")
            self.assertEqual(reference.returncode, 1)
            self.assertEqual(parallel.returncode, 1)
            self.assertEqual(parallel.stdout, reference.stdout)
            self.assertEqual(parallel.stderr, reference.stderr)
            self.assertRegex(parallel.stdout, r"3\.0 KiB!\s+\[\+\] alpha")
            self.assertRegex(parallel.stdout, r"1 B\s+\[\+\] beta")
        finally:
            locked.chmod(0o700)

    def test_control_characters_in_filenames_are_escaped(self):
        (self.root / "odd\n\x1b[31m.txt").write_bytes(b"x")
        result = self.cli("--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(r"odd\n\x1b[31m.txt", result.stdout)
        self.assertNotIn("\x1b", result.stdout)
        self.assertEqual(self.cli("--list").stdout, result.stdout)

    def cache_file(self):
        files = list((self.cache_home / "sizetree").glob("*.cache"))
        self.assertEqual(len(files), 1)
        return files[0]

    def test_first_run_saves_defaults_and_enables_cache_without_prompting(self):
        shutil.rmtree(self.config_file.parent)
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Total: 3.0 KiB | Scanned " in screen)
            self.assertRegex(terminal.screen(), r"Scanned 12 entries in \d+ min \d+ sec")
            self.assertNotIn("Enable cache?", terminal.output)
            terminal.finish()
        saved = self.config_file.read_text()
        self.assertIn("cache_enabled=true\n", saved)
        self.assertIn("refresh_interval_seconds=86400\n", saved)
        self.assertEqual(self.config_file.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.config_file.parent.stat().st_mode & 0o777, 0o700)
        self.cache_file()
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Scanned " in screen)
            self.assertNotIn("Enable cache?", terminal.output)
            self.assertIn("Cached; checking", terminal.output)
            terminal.finish()
        self.assertEqual(self.config_file.read_text(), saved)

    def test_disabled_preference_bypasses_existing_cache(self):
        self.assertEqual(self.cli("--list").returncode, 0)
        cached = self.cache_file()
        previous = cached.read_bytes()
        self.config_file.write_text("# User preference\n cache_enabled = false # disabled\n")
        (self.root / "alpha" / "payload.bin").write_bytes(b"x" * 8192)
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Total: 9.0 KiB | Scanned " in screen)
            self.assertRegex(terminal.output, r"Scanning\.\.\. \d+ entries \| elapsed \d+ min \d+ sec")
            self.assertNotIn("Cached; checking", terminal.output)
            self.assertNotIn("Enable cache?", terminal.output)
            terminal.finish()
        self.assertEqual(cached.read_bytes(), previous)

    def test_cache_flags_override_without_saving_preferences(self):
        saved = self.config_file.read_bytes()
        self.assertEqual(self.cli("--no-cache", "--list").returncode, 0)
        self.assertEqual(list(self.cache_home.iterdir()), [])
        self.assertEqual(self.config_file.read_bytes(), saved)
        disabled = "cache_enabled=false\nrefresh_interval_seconds=3600\n"
        self.config_file.write_text(disabled)
        self.assertEqual(self.cli("--cache", "--list").returncode, 0)
        self.cache_file()
        self.assertEqual(self.config_file.read_text(), disabled)
        for flag in ("--cache", "--no-cache"):
            self.config_file.unlink()
            with TerminalSession(self.root, arguments=(flag,)) as terminal:
                terminal.wait_for(lambda screen: "Scanned " in screen)
                self.assertNotIn("Enable cache?", terminal.output)
                self.assertEqual("Cached; checking" in terminal.output, flag == "--cache")
                terminal.finish()
            self.assertIn("cache_enabled=true\n", self.config_file.read_text())
            self.assertIn("refresh_interval_seconds=86400\n", self.config_file.read_text())

    def test_unconfigured_plain_runs_save_defaults_and_enable_cache(self):
        for arguments in ((), ("--list",)):
            self.config_file.unlink()
            result = self.cli(*arguments)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("Enable cache?", result.stdout + result.stderr)
            self.assertIn("cache_enabled=true\n", self.config_file.read_text())
            self.assertIn("refresh_interval_seconds=86400\n", self.config_file.read_text())
            self.cache_file()
        with TerminalSession(self.root, arguments=("--list",)) as terminal:
            terminal.wait_for(lambda screen: "Total: 3.0 KiB" in screen)
            self.assertNotIn("Enable cache?", terminal.output)
            self.assertEqual(terminal.process.wait(timeout=5), 0)

    def test_config_uses_home_when_xdg_config_home_is_empty_or_relative(self):
        for value in ("", "relative-config"):
            with self.subTest(value=value):
                fake_home = self.config_home / ("home-" + (value or "empty"))
                with mock.patch.dict(os.environ, {"HOME": str(fake_home), "XDG_CONFIG_HOME": value}):
                    with TerminalSession(self.root) as terminal:
                        terminal.wait_for(lambda screen: "Scanned " in screen)
                        self.assertNotIn("Enable cache?", terminal.output)
                        terminal.finish()
                    fallback = fake_home / ".config" / "sizetree" / "config"
                    self.assertIn("cache_enabled=true\n", fallback.read_text())
                    self.assertIn("refresh_interval_seconds=86400\n", fallback.read_text())
                    with TerminalSession(self.root) as terminal:
                        terminal.wait_for(lambda screen: "Scanned " in screen)
                        self.assertNotIn("Enable cache?", terminal.output)
                        terminal.finish()
        self.assertFalse((self.root / "relative-config").exists())
        self.assertEqual(self.config_file.read_text(), DEFAULT_CONFIG)

    def test_help_and_invalid_arguments_do_not_create_config(self):
        self.config_file.unlink()
        self.assertEqual(self.cli("--help").returncode, 0)
        self.assertNotEqual(self.cli("--bad-option").returncode, 0)
        self.assertNotEqual(self.cli("missing-folder").returncode, 0)
        self.assertFalse(self.config_file.exists())
        self.assertEqual(list(self.cache_home.iterdir()), [])

    def test_missing_settings_are_added_without_replacing_preferences(self):
        for original, enabled, interval in (
            ("# Keep my preference\ncache_enabled=false", False, 86400),
            ("refresh_interval_seconds=300\n# Keep this comment\n", True, 300),
            ("cache_enabled=false\nrefresh_interval_seconds=3600\nother=value\n", False, 3600),
        ):
            with self.subTest(original=original):
                shutil.rmtree(self.cache_home / "sizetree", ignore_errors=True)
                self.config_file.write_text(original)
                modified = self.config_file.stat().st_mtime_ns
                result = self.cli("--list")
                self.assertEqual(result.returncode, 0, result.stderr)
                saved = self.config_file.read_text()
                self.assertTrue(saved.startswith(original))
                self.assertIn("cache_enabled=" + str(enabled).lower(), saved)
                self.assertIn("refresh_interval_seconds=" + str(interval), saved)
                self.assertEqual(bool(list(self.cache_home.rglob("*.cache"))), enabled)
                if "cache_enabled=" in original and "refresh_interval_seconds=" in original:
                    self.assertEqual(saved, original)
                    self.assertEqual(self.config_file.stat().st_mtime_ns, modified)

    def test_configured_refresh_interval_applies_with_cache_disabled_or_overridden(self):
        for enabled, arguments in ((False, ()), (True, ("--no-cache",))):
            with self.subTest(enabled=enabled):
                saved = "cache_enabled=" + str(enabled).lower() + "\nrefresh_interval_seconds=1\n"
                self.config_file.write_text(saved)
                (self.root / "alpha" / "payload.bin").write_bytes(b"x" * 2048)
                with TerminalSession(self.root, arguments=arguments) as terminal:
                    terminal.wait_for(lambda screen: "Total: 3.0 KiB | Scanned " in screen)
                    (self.root / "alpha" / "payload.bin").write_bytes(b"x" * 8192)
                    terminal.wait_for(lambda screen: "Total: 9.0 KiB | Scanned " in screen)
                    terminal.finish()
                self.assertEqual(self.config_file.read_text(), saved)
                self.assertEqual(list(self.cache_home.iterdir()), [])

    def test_invalid_refresh_intervals_preserve_valid_cache_preference(self):
        for value in ("", "-1", "1.5", "24h", "4294967296", "9" * 100):
            with self.subTest(value=value):
                contents = "refresh_interval_seconds=" + value + "\ncache_enabled=false\n"
                self.config_file.write_text(contents)
                result = self.cli("--list")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("refresh_interval_seconds must be a whole number", result.stderr)
                self.assertEqual(self.config_file.read_text(), contents)
                self.assertEqual(list(self.cache_home.iterdir()), [])

    def test_invalid_and_unwritable_config_do_not_prevent_scanning(self):
        self.config_file.write_text("cache_enabled=maybe\n")
        result = self.cli("--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("cache_enabled must be true or false", result.stderr)
        self.cache_file()
        self.assertEqual(self.config_file.read_text(), "cache_enabled=maybe\n")
        shutil.rmtree(self.config_file.parent)
        self.config_file.parent.write_text("not a directory")
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Total: 3.0 KiB | Scanned " in screen)
            self.assertNotIn("Enable cache?", terminal.output)
            self.assertIn("sizetree: config:", terminal.output)
            terminal.finish()
        self.assertEqual(self.config_file.parent.read_text(), "not a directory")

    def test_persistent_cache_shows_old_results_then_checks_real_changes(self):
        reference = self.cli("--list")
        self.assertEqual(reference.returncode, 0, reference.stderr)
        cached = self.cache_file()
        self.assertEqual(cached.stat().st_mode & 0o077, 0)
        self.assertEqual(cached.parent.stat().st_mode & 0o077, 0)
        (self.root / "alpha" / "payload.bin").write_bytes(b"x" * 8192)
        (self.root / "beta" / "tiny.txt").unlink()
        (self.root / "beta" / "new.txt").write_bytes(b"x" * 2048)
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Cached; checking" in terminal.output)
            first = terminal.first_screen()
            self.assertIn("Total: ~3.0 KiB", first)
            self.assertRegex(first, r"~3\.0 KiB +\[\+\] alpha")
            terminal.wait_for(lambda screen: "Total: 11.0 KiB | Scanned 12 entries" in screen)
            terminal.finish()
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Cached; checking" in terminal.output)
            self.assertIn("Total: ~11.0 KiB", terminal.first_screen())
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.finish()

    def test_cache_is_updated_while_running_and_shared_readers_do_not_overwrite_it(self):
        self.assertEqual(self.cli("--list").returncode, 0)
        cached = self.cache_file()
        with TerminalSession(self.root) as writer:
            writer.wait_for(lambda screen: "Scanned " in screen)
            before = cached.read_bytes()
            (self.root / "beta" / "live-added.txt").write_bytes(b"x" * 4096)
            # A second process can validate the filesystem even while this one
            # owns the writer lock. Its results must not corrupt the shared cache.
            reader = self.cli("--list")
            self.assertEqual(reader.returncode, 0, reader.stderr)
            self.assertIn("Total: 7.0 KiB", reader.stdout)
            self.assertEqual(cached.read_bytes(), before)
            writer.send(b"\x1b[H\x1b[Br")
            writer.wait_for(lambda screen: "Total: 7.0 KiB | Scanned 13 entries" in screen)
            writer.wait_for(lambda screen: b"live-added.txt" in cached.read_bytes())
            with TerminalSession(self.root) as reader:
                reader.wait_for(lambda screen: "Cached; checking" in reader.output)
                self.assertIn("Total: ~7.0 KiB", reader.first_screen())
                reader.wait_for(lambda screen: "Scanned " in screen)
                reader.finish()
            writer.finish()

    def test_unchanged_scans_do_not_rewrite_cache_and_no_cache_bypasses_it(self):
        self.assertEqual(self.cli("--list").returncode, 0)
        cached = self.cache_file()
        original = cached.read_bytes()
        modified = cached.stat().st_mtime_ns
        self.assertEqual(self.cli("--list").returncode, 0)
        self.assertEqual(cached.read_bytes(), original)
        self.assertEqual(cached.stat().st_mtime_ns, modified)
        (self.root / "alpha" / "payload.bin").write_bytes(b"x" * 8192)
        result = self.cli("--no-cache", "--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Total: 9.0 KiB", result.stdout)
        self.assertEqual(cached.read_bytes(), original)
        with TerminalSession(self.root, arguments=("--no-cache",)) as terminal:
            terminal.wait_for(lambda screen: "Scanned " in screen)
            self.assertNotIn("Cached; checking", terminal.output)
            terminal.finish()

    def test_truncated_and_corrupt_caches_are_recovered(self):
        reference = self.cli("--list")
        cached = self.cache_file()
        original = cached.read_bytes()
        with cached.open("ab") as output:
            output.write(b"incomplete-tail")
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Cached; checking" in terminal.output)
            self.assertIn("Total: ~3.0 KiB", terminal.first_screen())
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.finish()
        self.assertEqual(cached.read_bytes(), original)
        for damaged in (b"bad header", original[:-1] + bytes([original[-1] ^ 255])):
            cached.write_bytes(damaged)
            result = self.cli("--list")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, reference.stdout)
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Cached; checking" in terminal.output)
            self.assertIn("Total: ~3.0 KiB", terminal.first_screen())
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.finish()

    def test_cache_paths_use_canonical_roots_and_home_fallback(self):
        self.assertEqual(self.cli("--list").returncode, 0)
        alias = self.cache_home / "alias"
        alias.symlink_to(self.root, target_is_directory=True)
        self.assertEqual(self.cli("--list", alias).returncode, 0)
        self.cache_file()  # The symlink and its canonical target share one cache.
        self.assertEqual(self.cli("--list", self.root / "alpha").returncode, 0)
        self.assertEqual(len(list((self.cache_home / "sizetree").glob("*.cache"))), 2)
        with mock.patch.dict(os.environ, {"HOME": str(self.cache_home), "XDG_CACHE_HOME": "relative-cache"}):
            self.assertEqual(self.cli("--list").returncode, 0)
        self.assertEqual(len(list((self.cache_home / ".cache" / "sizetree").glob("*.cache"))), 1)
        self.assertFalse((self.root / "relative-cache").exists())

    def test_unavailable_cache_does_not_prevent_a_scan(self):
        (self.cache_home / "sizetree").write_text("not a directory")
        result = self.cli("--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Total: 3.0 KiB", result.stdout)
        self.assertIn("cache unavailable", result.stderr)
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Scanned " in screen and "Cache unavailable" in screen)
            terminal.finish()

    def test_refresh_preserves_expanded_cached_directories_and_persists_deletions(self):
        self.assertEqual(self.cli("--list").returncode, 0)
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.send(b"\x1b[H\t\x1b[B\t\x1b[H")
            terminal.wait_for(lambda screen: "secret.bin" in screen and re.search(r"> .*alpha", screen))
            (self.root / "alpha" / "nested" / "secret.bin").write_bytes(b"x" * 2048)
            terminal.send(b"r")
            screen = terminal.wait_for(lambda screen: "Total: 4.0 KiB | Scanned " in screen
                                       and "secret.bin" in screen)
            self.assertIn("[-] nested", screen)
            shutil.rmtree(self.root / "alpha" / "nested")
            terminal.send(b"r")
            terminal.wait_for(lambda screen: "Total: 2.0 KiB | Scanned 10 entries" in screen
                              and "nested" not in screen)
            terminal.finish()
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Cached; checking" in terminal.output)
            self.assertIn("Total: ~2.0 KiB", terminal.first_screen())
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.send(b"\x1b[H\r")
            screen = terminal.wait_for(lambda screen: "payload.bin" in screen)
            self.assertNotIn("nested", screen)
            terminal.finish()

    def test_cache_compaction_keeps_latest_results(self):
        directory = self.root / "compact"
        directory.mkdir()
        for index in range(3000):
            (directory / ("item-%04d-" % index + "x" * 100)).write_bytes(b"x")
        self.assertEqual(self.cli("--list", directory).returncode, 0)
        cached = self.cache_file()
        original_size = cached.stat().st_size
        changing = directory / ("item-0000-" + "x" * 100)
        for size in range(2, 10):
            changing.write_bytes(b"x" * size)
            result = self.cli("--list", directory)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(cached.stat().st_size, original_size * 3)
        with TerminalSession(directory) as terminal:
            terminal.wait_for(lambda screen: "Cached; checking" in terminal.output)
            self.assertRegex(terminal.first_screen(), r"~9 B +item-0000-")
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.finish()

    def enlarge_cache_journal(self):
        self.assertEqual(self.cli("--list").returncode, 0)
        cached = self.cache_file()
        data = cached.read_bytes()
        # Repeated valid records are a realistic uncompacted journal. This
        # exercises slow initialization without creating millions of files.
        header_size = 13 + len(os.fsencode(self.root.resolve()))
        records = data[header_size:]
        block = records * max(1, (1024 * 1024) // len(records))
        with cached.open("ab") as output:
            for _ in range(64):
                output.write(block)
        return cached

    def test_overview_is_interactive_while_loading_and_quit_preserves_journal(self):
        cached = self.enlarge_cache_journal()
        overview = Path(str(cached) + ".overview")
        self.assertTrue(overview.is_file())
        self.assertLess(overview.stat().st_size, 4096)
        self.assertEqual(overview.stat().st_mode & 0o077, 0)
        size, modified = cached.stat().st_size, cached.stat().st_mtime_ns
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Total: ~3.0 KiB" in screen and "loading details" in screen)
            terminal.send(b"\t")
            terminal.wait_for(lambda screen: re.search(r"> .*\[-\] alpha", screen)
                              and "loading details" in screen)
            terminal.finish()
        self.assertEqual(cached.stat().st_size, size)
        self.assertEqual(cached.stat().st_mtime_ns, modified)

    def test_list_cache_loading_progress_clears_on_interrupt(self):
        self.enlarge_cache_journal()
        with TerminalSession(self.root, arguments=("--list",)) as terminal:
            terminal.wait_for(lambda screen: "Loading cache [" in screen)
            terminal.finish(b"\x03", 128 + signal.SIGINT)
            terminal.wait_for(lambda screen: re.search(r"\r +\r$", terminal.output))
            self.assertNotIn("Directory:", terminal.output)
            self.assertNotIn("\x1b[?1049h", terminal.output)

    def test_overview_selection_expansion_and_refresh_survive_full_cache_load(self):
        self.enlarge_cache_journal()
        (self.root / "alpha" / "payload.bin").write_bytes(b"x" * 8192)
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "loading details" in screen)
            # Move to beta and back while selecting/expanding from the overview,
            # then queue a refresh before detailed nodes have been restored.
            terminal.send(b"\x1b[B\x1b[A\tr")
            terminal.wait_for(lambda screen: re.search(r"> .*\[-\] alpha", screen)
                              and "refresh queued" in screen)
            screen = terminal.wait_for(lambda screen: "Total: 9.0 KiB | Scanned " in screen
                                       and re.search(r"> .*\[-\] alpha", screen)
                                       and "payload.bin" in screen, timeout=15)
            self.assertNotIn("refresh queued", screen)
            terminal.finish()
        result = self.cli("--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Total: 9.0 KiB", result.stdout)

    def test_missing_or_corrupt_overview_allows_quit_during_journal_load(self):
        cached = self.enlarge_cache_journal()
        overview = Path(str(cached) + ".overview")
        for damaged in (None, b"corrupt overview"):
            with self.subTest(damaged=damaged):
                if damaged is None:
                    overview.unlink()
                else:
                    overview.write_bytes(damaged)
                with TerminalSession(self.root) as terminal:
                    terminal.wait_for(lambda screen: "Loading saved sizes" in screen)
                    terminal.finish()
        # A broken overview does not prevent the authoritative cache from loading.
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Cached; checking" in terminal.output, timeout=15)
            self.assertIn("Total: ~3.0 KiB", terminal.first_screen())
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.finish()

    @unittest.skipIf(os.geteuid() == 0, "root can read directories regardless of their permissions")
    def test_unreadable_directory_is_reported_as_partial(self):
        locked = self.root / "locked"
        locked.mkdir()
        (locked / "secret").write_bytes(b"x" * 64)
        locked.chmod(0)
        try:
            result = self.cli("--list")
            self.assertEqual(result.returncode, 1)
            self.assertRegex(result.stdout, r"0 B!\s+\[\+\] locked")
            self.assertIn("unreadable", result.stderr)
        finally:
            locked.chmod(0o700)

    def test_arrows_navigate_and_tab_toggles_nested_directories(self):
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.send(b"\x1b[H")
            screen = terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] alpha", screen))
            self.assertNotIn("payload.bin", screen)
            terminal.send(b"\t")
            terminal.wait_for(lambda screen: re.search(r"> .*\[-\] alpha", screen)
                              and "payload.bin" in screen)
            terminal.send(b"\x1b[B")
            terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] nested", screen))
            terminal.send(b"\t")
            terminal.wait_for(lambda screen: re.search(r"> .*\[-\] nested", screen)
                              and "secret.bin" in screen)
            terminal.send(b"\x1b[B")
            terminal.wait_for(lambda screen: re.search(r"> .*secret\.bin", screen))
            # Tab on a file does nothing; Up then returns to its open parent.
            terminal.send(b"\t\x1b[A")
            terminal.wait_for(lambda screen: re.search(r"> .*\[-\] nested", screen)
                              and "secret.bin" in screen)
            terminal.send(b"\t")
            terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] nested", screen)
                              and "secret.bin" not in screen)
            terminal.send(b"\x1b[A\t")
            terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] alpha", screen)
                              and "payload.bin" not in screen)
            terminal.send(b"\x1b[B")
            terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] beta", screen))
            terminal.send(b"\x1b[A")
            terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] alpha", screen))
            terminal.finish()

    def test_scrolling_resize_and_parent_navigation(self):
        for index in range(30):
            (self.root / "alpha" / f"item-{index:02}.txt").write_bytes(b"x")
        with TerminalSession(self.root, rows=12, columns=80) as terminal:
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.send(b"\x1b[H\t\x1b[B\x1b[D")
            terminal.wait_for(lambda screen: re.search(r"> .*\[-\] alpha", screen))
            terminal.send(b"\x1b[B")
            terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] nested", screen))
            terminal.send(b"\x1b[6~")
            terminal.wait_for(lambda screen: re.search(r"> .*item-\d+\.txt", screen))
            terminal.send(b"\x1b[F")
            terminal.wait_for(lambda screen: re.search(r"> .*\[\*\] pipe", screen))
            terminal.resize(6, 30)
            terminal.wait_for(lambda screen: "Enlarge terminal" in screen)
            terminal.resize(24, 100)
            terminal.send(b"\x1b[H")
            terminal.wait_for(lambda screen: "sizetree |" in screen and re.search(r"> .*alpha", screen))
            terminal.finish()

    def test_fuzzy_search_reveals_collapsed_files_and_cycles_matches(self):
        (self.root / "alpha" / "nested" / "report-a.txt").write_text("a")
        (self.root / "beta" / "report-b.txt").write_text("b")
        with TerminalSession(self.root, arguments=("--no-cache",)) as terminal:
            terminal.wait_for(lambda s: "Scanned " in s)
            terminal.send(b"/SCB")
            terminal.wait_for(lambda s: "/SCB | 1/1 matches" in s and re.search(r"> .*secret\.bin", s))
            self.assertIn("[-] nested", terminal.screen())
            terminal.send(b"\x15rptxt")
            terminal.wait_for(lambda s: "1/2 matches" in s and re.search(r"> .*report-b\.txt", s))
            self.assertIn("[+] alpha", terminal.screen())
            terminal.send(b"\t")
            terminal.wait_for(lambda s: "2/2 matches" in s and re.search(r"> .*report-a\.txt", s))
            self.assertIn("[+] beta", terminal.screen())
            terminal.send(b"\t")
            terminal.wait_for(lambda s: "1/2 matches" in s and re.search(r"> .*report-b\.txt", s))
            terminal.send(b"\x1b[Z")
            terminal.wait_for(lambda s: "2/2 matches" in s and re.search(r"> .*report-a\.txt", s))
            terminal.send(b"\x1b")
            terminal.wait_for(lambda s: "Esc: exit search" not in s and re.search(r"> .*report-a\.txt", s))
            terminal.finish()

    def test_fuzzy_search_matches_all_terms_in_any_order_and_case(self):
        for name in ("EBM/LoadStore.cpp", "LoadStore/ebm_notes.txt", "EBM/compute.cpp", "misc/loadstore.cpp"):
            path = self.root / name
            path.parent.mkdir(exist_ok=True)
            path.write_text("x")
        with TerminalSession(self.root, arguments=("--no-cache",)) as terminal:
            terminal.wait_for(lambda s: "Scanned " in s)
            terminal.send(b"/")
            original_order = None
            for query in ("ebm loadstore", "loadstore ebm", "EBM LOADSTORE", "LoAdStOrE EbM",
                          "  ebm   loadstore  ", "Bm LDst"):
                with self.subTest(query=query):
                    terminal.send(b"\x15" + query.encode())
                    order = []
                    for index in (1, 2):
                        status = "/{} | {}/2 matches".format(query, index)
                        screen = terminal.wait_for(lambda s: status in s and "(searching...)" not in s
                                                   and "Esc: exit search" in s)
                        selected = re.search(r"> [^\n]*(LoadStore\.cpp|ebm_notes\.txt)", screen)
                        self.assertIsNotNone(selected, screen)
                        order.append(selected.group(1))
                        if index == 1:
                            terminal.send(b"\t")
                    self.assertEqual(set(order), {"LoadStore.cpp", "ebm_notes.txt"})
                    if original_order is None:
                        original_order = order
                    elif query != "Bm LDst":
                        self.assertEqual(order, original_order)
            terminal.send(b"\x15ebm missingterm")
            terminal.wait_for(lambda s: "/ebm missingterm | No matches" in s)
            terminal.send(b"\x15   ")
            terminal.wait_for(lambda s: "/    | Type a file name or path" in s)
            terminal.send(b"\x1b")
            terminal.wait_for(lambda s: "Esc: exit search" not in s)
            terminal.finish()

    def test_fuzzy_search_edits_unicode_and_treats_shortcuts_as_query_text(self):
        (self.root / "alpha" / "qjr.txt").write_text("a")
        (self.root / "alpha" / "caf\u00e9.txt").write_text("b")
        with TerminalSession(self.root, arguments=("--no-cache",)) as terminal:
            terminal.wait_for(lambda s: "Scanned " in s)
            terminal.send(b"/qjr")
            terminal.wait_for(lambda s: "/qjr | 1/1 matches" in s and re.search(r"> .*qjr\.txt", s))
            terminal.send("\x15caf\u00e9".encode())
            terminal.wait_for(lambda s: "1/1 matches" in s and re.search(r"> .*caf", s))
            terminal.send(b"\x7f")
            terminal.wait_for(lambda s: "/caf | 1/1 matches" in s)
            terminal.send(b"\x15zzzznotfound")
            terminal.wait_for(lambda s: "/zzzznotfound | No matches" in s)
            # Enter with no match must keep search active and open nothing.
            terminal.send(b"\r\x15scb")
            terminal.wait_for(lambda s: "/scb | 1/1 matches" in s and re.search(r"> .*secret\.bin", s))
            terminal.send(b"\x1b")
            terminal.wait_for(lambda s: "Esc: exit search" not in s)
            terminal.finish()

    def fake_opener(self):
        directory = self.cache_home / "bin"
        directory.mkdir()
        executable = directory / "xdg-open"
        executable.write_text("#!" + sys.executable + "\n" +
            "import json, os, sys, time\n"
            "with open(os.environ['SIZETREE_OPEN_LOG'], 'w') as output:\n"
            "    json.dump({'args': sys.argv[1:], 'tty': [os.isatty(i) for i in range(3)], "
            "'pid': os.getpid(), 'group': os.getpgrp()}, output)\n"
            "print('UNEXPECTED OPENER STDOUT')\n"
            "print('UNEXPECTED OPENER STDERR', file=sys.stderr)\n"
            "deadline = time.monotonic() + 5\n"
            "while os.environ.get('SIZETREE_OPEN_WAIT') and not os.path.exists(os.environ['SIZETREE_OPEN_WAIT']):\n"
            "    if time.monotonic() >= deadline: break\n"
            "    time.sleep(0.01)\n"
            "sys.exit(int(os.environ.get('SIZETREE_OPEN_STATUS', '0')))\n")
        executable.chmod(0o755)
        return directory, self.cache_home / "opened.json"

    def test_enter_opens_exact_filename_without_shell_and_keeps_browser_responsive(self):
        directory, log = self.fake_opener()
        release = self.cache_home / "release-opener"
        name = "-draft $(touch OWNED) 'quoted'.txt"
        target = self.root / "alpha" / "nested" / name
        target.write_text("document")
        with mock.patch.dict(os.environ, {"PATH": str(directory), "SIZETREE_OPEN_LOG": str(log),
                                        "SIZETREE_OPEN_WAIT": str(release)}):
            try:
                with TerminalSession(self.root, arguments=("--no-cache",)) as terminal:
                    terminal.wait_for(lambda s: "Scanned " in s)
                    terminal.send(b"/quoted")
                    terminal.wait_for(lambda s: "1/1 matches" in s and re.search(r"> .*quoted", s))
                    terminal.send(b"\r")
                    terminal.wait_for(lambda s: "Opening " in s and log.exists())
                    terminal.send(b"/scb")
                    terminal.wait_for(lambda s: "/scb | 1/1 matches" in s and re.search(r"> .*secret\.bin", s), timeout=2)
                    opened = json.loads(log.read_text())
                    self.assertEqual(opened["args"], [str(target)])
                    self.assertEqual(opened["tty"], [False, False, False])
                    self.assertEqual(opened["pid"], opened["group"])
                    self.assertFalse((self.root / "OWNED").exists())
                    release.touch()
                    terminal.wait_for(lambda s: "Opened " in s)
                    self.assertNotIn("UNEXPECTED OPENER", terminal.output)
                    terminal.send(b"\x1b")
                    terminal.wait_for(lambda s: "Esc: exit search" not in s)
                    terminal.finish()
            finally:
                release.touch()

    def test_open_failures_are_reported_without_exiting_the_browser(self):
        directory, log = self.fake_opener()
        with mock.patch.dict(os.environ, {"PATH": str(directory), "SIZETREE_OPEN_LOG": str(log),
                                        "SIZETREE_OPEN_STATUS": "4"}):
            with TerminalSession(self.root, arguments=("--no-cache",)) as terminal:
                terminal.wait_for(lambda s: "Scanned " in s)
                terminal.send(b"/loose")
                terminal.wait_for(lambda s: "1/1 matches" in s and re.search(r"> .*loose\.txt", s))
                terminal.send(b"\r")
                terminal.wait_for(lambda s: "xdg-open failed (exit 4)" in s)
                terminal.finish()
        (directory / "xdg-open").unlink()
        with mock.patch.dict(os.environ, {"PATH": str(directory)}):
            with TerminalSession(self.root, arguments=("--no-cache",)) as terminal:
                terminal.wait_for(lambda s: "Scanned " in s)
                terminal.send(b"/loose")
                terminal.wait_for(lambda s: "1/1 matches" in s and re.search(r"> .*loose\.txt", s))
                # Leaving search and pressing Enter also opens the selected file.
                terminal.send(b"\x1b\r")
                terminal.wait_for(lambda s: "xdg-open was not found" in s)
                terminal.finish()

    def test_refresh_replaces_contents_and_preserves_selection_and_other_sizes(self):
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.send(b"\x1b[H\x1b[B")
            terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] beta", screen))
            # Changes outside beta must remain cached until that folder is refreshed.
            (self.root / "alpha" / "payload.bin").write_bytes(b"x" * 65536)
            (self.root / "beta" / "tiny.txt").unlink()
            added = self.root / "beta" / "added"
            added.mkdir()
            (added / "new.bin").write_bytes(b"x" * 4096)
            terminal.send(b"r")
            screen = terminal.wait_for(lambda screen: "Total: 7.0 KiB | Scanned 13 entries" in screen
                                       and re.search(r"> +4\.0 KiB +\[\+\] beta", screen))
            self.assertRegex(screen, r"3\.0 KiB +\[\+\] alpha")
            terminal.send(b"\t\x1b[B\t")
            terminal.wait_for(lambda screen: "new.bin" in screen)
            terminal.send(b"\x1b[D\x1b[D")
            terminal.wait_for(lambda screen: re.search(r"> .*\[-\] beta", screen))
            shutil.rmtree(added)
            (self.root / "beta" / "replacement.bin").write_bytes(b"123456789")
            terminal.send(b"r")
            screen = terminal.wait_for(lambda screen: "Scanned 12 entries" in screen
                                       and re.search(r"> +9 B +\[-\] beta", screen)
                                       and "replacement.bin" in screen)
            self.assertIn("Total: 3.0 KiB", screen)
            self.assertNotIn("added", screen)
            self.assertNotIn("new.bin", screen)
            self.assertNotIn("tiny.txt", screen)
            terminal.finish()

    @unittest.skipIf(os.geteuid() == 0, "root can read directories regardless of their permissions")
    def test_refresh_corrects_ancestor_totals_and_clears_only_repaired_errors(self):
        first = self.root / "alpha" / "nested" / "locked"
        second = self.root / "beta" / "locked"
        for directory, size in ((first, 2048), (second, 512)):
            directory.mkdir()
            (directory / "secret").write_bytes(b"x" * size)
            directory.chmod(0)
        try:
            with TerminalSession(self.root) as terminal:
                terminal.wait_for(lambda screen: "Scanned " in screen and "2 errors" in screen)
                terminal.send(b"\x1b[H\t\x1b[B")
                terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] nested", screen))
                first.chmod(0o700)
                terminal.send(b"r")
                screen = terminal.wait_for(lambda screen: "Scanned " in screen and "1 errors" in screen
                                           and re.search(r"> +3\.0 KiB +\[\+\] nested", screen))
                self.assertRegex(screen, r"5\.0 KiB +\[-\] alpha")
                self.assertRegex(screen, r"1 B! +\[\+\] beta")
                (self.root / "alpha" / "nested" / "extra").write_bytes(b"x" * 1024)
                terminal.send(b"r")
                terminal.wait_for(lambda screen: "Scanned " in screen and "1 errors" in screen
                                  and re.search(r"> +4\.0 KiB +\[\+\] nested", screen))
                second.chmod(0o700)
                terminal.send(b"\x1b[H\t\x1b[Br")
                screen = terminal.wait_for(lambda screen: "Total: 6.5 KiB | Scanned " in screen
                                           and re.search(r"> +513 B +\[\+\] beta", screen))
                self.assertNotIn("errors", screen)
                terminal.finish()
        finally:
            first.chmod(0o700)
            second.chmod(0o700)

    def test_refresh_does_not_follow_replacement_symlink_and_can_recover(self):
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.send(b"\x1b[H\x1b[B")
            terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] beta", screen))
            beta = self.root / "beta"
            shutil.rmtree(beta)
            beta.symlink_to("alpha", target_is_directory=True)
            terminal.send(b"r")
            terminal.wait_for(lambda screen: "Scanned " in screen and "1 errors" in screen
                              and re.search(r"> +0 B! +\[\+\] beta", screen))
            beta.unlink()
            beta.mkdir()
            (beta / "recovered").write_bytes(b"x" * 17)
            terminal.send(b"r")
            screen = terminal.wait_for(lambda screen: "Scanned " in screen
                                       and re.search(r"> +17 B +\[\+\] beta", screen))
            self.assertNotIn("errors", screen)
            terminal.finish()

    def test_repeated_refresh_requests_coalesce_without_losing_counts(self):
        bulk = self.root / "alpha" / "bulk"
        for index in range(64):
            directory = bulk / ("branch-%02d" % index)
            directory.mkdir(parents=True)
            for entry in range(32):
                (directory / ("file-%02d" % entry)).write_bytes(b"x")
        with TerminalSession(self.root, arguments=("-j", "1")) as terminal:
            terminal.wait_for(lambda screen: "Scanned 2125 entries" in screen)
            terminal.send(b"\x1b[H")
            terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] alpha", screen))
            (self.root / "alpha" / "payload.bin").write_bytes(b"x" * 4096)
            # Submit refreshes while the subtree is being rebuilt, then navigate
            # into it while queued work may retire and replace its descendants.
            terminal.send(b"r" * 32 + b"\t\x1b[B\t")
            terminal.wait_for(lambda screen: "Total: 7.0 KiB | Scanned 2125 entries" in screen)
            terminal.send(b"\x1b[H")
            terminal.wait_for(lambda screen: re.search(r"> .*alpha", screen))
            (self.root / "alpha" / "payload.bin").write_bytes(b"x" * 2048)
            terminal.send(b"r")
            terminal.wait_for(lambda screen: "Total: 5.0 KiB | Scanned 2125 entries" in screen)
            terminal.finish()

    @unittest.skipUnless(shutil.which("bash"), "job-control tests require bash")
    def test_ctrl_z_and_fg_restore_terminal_and_browser_state(self):
        with JobControlSession(self.root) as terminal:
            terminal.start()
            terminal.send(b"\t\x1b[B\t")
            terminal.wait_for(lambda screen: re.search(r"> .*\[-\] nested", screen)
                              and "secret.bin" in screen)
            for cycle in range(2):
                terminal.suspend()
                self.assertIn("\x1b[?25h\x1b[?1049l", terminal.output)
                self.assertEqual(termios.tcgetattr(terminal.slave), terminal.original_settings)
                # First resume has an unchanged frame; the next also tests resizing.
                if cycle:
                    terminal.resize(13, 80)
                terminal.foreground()
                self.assertIn("\x1b[?1049h\x1b[?25l", terminal.output)
                self.assertRegex(terminal.screen(), r"> .*\[-\] nested")
                self.assertIn("secret.bin", terminal.screen())
                settings = termios.tcgetattr(terminal.slave)
                self.assertFalse(settings[3] & (termios.ICANON | termios.ECHO))
                # Navigation, toggling, and refresh must work after each fg.
                terminal.send(b"\x1b[B\x1b[A\t")
                terminal.wait_for(lambda screen: re.search(r"> .*\[\+\] nested", screen)
                                  and "secret.bin" not in screen)
                (self.root / "alpha" / "nested" / "secret.bin").write_bytes(b"b" * (2048 + cycle * 1024))
                terminal.send(b"r\t")
                terminal.wait_for(lambda screen: re.search(r"> +%d\.0 KiB +\[-\] nested" % (2 + cycle), screen)
                                  and "Scanned " in screen and "secret.bin" in screen)
            terminal.finish()

    @unittest.skipUnless(shutil.which("bash"), "job-control tests require bash")
    def test_background_resume_waits_for_fg_without_changing_shell_terminal(self):
        with JobControlSession(self.root) as terminal:
            terminal.start()
            terminal.suspend()
            terminal.output = ""
            terminal.send(b"bg\n")
            terminal.wait_for(lambda screen: screen.endswith("sizetree-test$ "))
            # Bash can defer the stopped-job notice until the next command.
            terminal.send(b"sleep 0.2\njobs\n")
            terminal.wait_for(lambda screen: "Stopped" in screen and screen.endswith("sizetree-test$ "))
            self.assertNotIn("\x1b[?1049h", terminal.output)
            self.assertNotIn("sizetree |", terminal.output)
            self.assertEqual(termios.tcgetattr(terminal.slave), terminal.original_settings)
            terminal.foreground()
            terminal.finish(b"\x03", 128 + signal.SIGINT)

    def test_ctrl_c_restores_terminal(self):
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.finish(b"\x03", 128 + signal.SIGINT)

    def test_sigterm_restores_terminal(self):
        with TerminalSession(self.root) as terminal:
            terminal.wait_for(lambda screen: "Scanned " in screen)
            terminal.process.send_signal(signal.SIGTERM)
            status = terminal.process.wait(timeout=5)
            self.assertEqual(status, 128 + signal.SIGTERM)
            self.assertEqual(termios.tcgetattr(terminal.slave), terminal.original_settings)

    def test_empty_directory_is_interactive(self):
        empty = self.root / "empty"
        empty.mkdir()
        with TerminalSession(empty) as terminal:
            terminal.wait_for(lambda screen: "(empty directory)" in screen)
            terminal.send(b"\t\r\x1b[A\x1b[B")
            terminal.finish()


if __name__ == "__main__":
    unittest.main(verbosity=2)
