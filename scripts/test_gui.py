"""Test runner window for Mutant Video Producer.

Builds (optionally), then runs the CTest suite and shows each test's state, its PASS/FAIL checks as they
happen, the overall progress (weighted by how long each test took last time) and the failures.

    python scripts/test_gui.py
    python scripts/test_gui.py --run [--no-build] [--targets mvp_selftest ...] [--only REGEX] [--exclude REGEX] [--noisy]

With --run it starts at once, closes an earlier runner window, echoes all output to stdout and ends it with a
"RESULT: ..." line (the window stays open to look at).
"""
import argparse
import sys
import tempfile
import os
import queue
import re
import shutil
import subprocess
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import ttk

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = Path.home() / "AppData/Roaming/Python/Python312/Scripts"  # ctest / cmake / ninja from pip
NOISY = "editor|nodes|devices|monkey"  # GUI tests: they play audio and beep
DEFAULT_EXPECTED = 5.0  # seconds, for a test with no timing from an earlier run

START = re.compile(r"^\s*Start\s+(\d+):\s+(\S+)")
RESULT = re.compile(r"Test\s+#(\d+):\s+(\S+)\s+\.*\**(.*?)\s+([\d.]+)\s+sec\s*$")
OUTPUT = re.compile(r"^(\d+): ?(.*)$")
LISTED = re.compile(r"Test\s+#(\d+):\s+(\S+)\s*$")
NINJA = re.compile(r"^\[(\d+)/(\d+)\]\s*(.*)")
PIDFILE = Path(tempfile.gettempdir()) / "mvp-test-gui.pid"
CTEST_NOISE = ("Test command:", "Working Directory:", "Environment variables:", "Test timeout computed")


def fmt(seconds):
    seconds = int(max(0, seconds))
    return f"{seconds // 3600}:{seconds // 60 % 60:02d}:{seconds % 60:02d}" if seconds >= 3600 else f"{seconds // 60}:{seconds % 60:02d}"


def echo(text):
    if sys.stdout:  # (none under pythonw)
        try:
            print(text, flush=True)
        except (OSError, ValueError):
            pass


class App:
    def __init__(self, root, opts):
        self.root = root
        root.title("MVP tests")
        root.geometry("1100x760-0+0")  # top right of the screen (out of the way)
        self.env = os.environ.copy()
        self.env["PATH"] = str(SCRIPTS) + os.pathsep + self.env.get("PATH", "")
        self.env.setdefault("ProgramFiles(x86)", r"C:\Program Files (x86)")  # (dropped when started from Git Bash; build.bat needs it)
        self.events = queue.Queue()
        self.proc = None
        self.stopping = False
        self.running = False
        self.tests = {}  # name -> state
        self.by_number = {}
        self.dirty = set()
        self.phase = ""
        self.started = 0.0
        self.build_progress = (0, 0)
        self.view = None  # test whose output the log shows (None: everything)
        self.editor_seen = 0.0

        bar = ttk.Frame(root, padding=6)
        bar.pack(fill="x")
        ttk.Label(bar, text="Config").pack(side="left")
        self.config = ttk.Combobox(bar, values=["Release", "Debug", "RelWithDebInfo"], width=14, state="readonly")
        self.config.set(opts.config)
        self.config.pack(side="left", padx=(4, 12))
        self.build = tk.BooleanVar(value=not opts.no_build)
        self.targets = opts.targets
        self.exclude = opts.exclude
        ttk.Checkbutton(bar, text="Build first", variable=self.build).pack(side="left")
        self.noisy = tk.BooleanVar(value=opts.noisy)
        ttk.Checkbutton(bar, text="GUI tests (editor, nodes, devices, monkey: they make sound)", variable=self.noisy).pack(side="left", padx=12)
        ttk.Label(bar, text="Jobs").pack(side="left")
        self.jobs = tk.Spinbox(bar, from_=1, to=32, width=4)
        self.jobs.delete(0, "end")
        self.jobs.insert(0, str(opts.jobs or min(8, os.cpu_count() or 4)))
        self.jobs.pack(side="left", padx=(4, 12))
        ttk.Label(bar, text="Only (regex)").pack(side="left")
        self.only = ttk.Entry(bar, width=16)
        self.only.insert(0, opts.only)
        self.only.pack(side="left", padx=4)

        buttons = ttk.Frame(root, padding=(6, 0))
        buttons.pack(fill="x")
        self.run_button = ttk.Button(buttons, text="Run", command=lambda: self.start(False))
        self.run_button.pack(side="left")
        self.rerun_button = ttk.Button(buttons, text="Rerun failed", command=lambda: self.start(True))
        self.rerun_button.pack(side="left", padx=6)
        self.stop_button = ttk.Button(buttons, text="Stop", command=self.stop, state="disabled")
        self.stop_button.pack(side="left")

        status = ttk.Frame(root, padding=6)
        status.pack(fill="x")
        self.status = ttk.Label(status, text="Ready", font=("Segoe UI", 11, "bold"))
        self.status.pack(anchor="w")
        self.progress = ttk.Progressbar(status, maximum=1000)
        self.progress.pack(fill="x", pady=4)
        self.stats = ttk.Label(status, text="")
        self.stats.pack(anchor="w")

        panes = ttk.PanedWindow(root, orient="vertical")
        panes.pack(fill="both", expand=True, padx=6, pady=6)
        table = ttk.Frame(panes)
        columns = ("status", "checks", "time", "expected", "activity")
        self.tree = ttk.Treeview(table, columns=columns, height=14)
        for col, title, width, stretch in (("#0", "Test", 150, False), ("status", "Status", 90, False), ("checks", "Checks", 110, False),
                                           ("time", "Time", 70, False), ("expected", "Last run", 70, False), ("activity", "Doing", 500, True)):
            self.tree.heading(col, text=title)
            self.tree.column(col, width=width, stretch=stretch)
        self.tree.tag_configure("passed", foreground="#1b7f2a")
        self.tree.tag_configure("failed", foreground="#c62828")
        self.tree.tag_configure("running", foreground="#1f5fbf")
        self.tree.bind("<<TreeviewSelect>>", self.select)
        scroll = ttk.Scrollbar(table, command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")
        panes.add(table, weight=3)

        failures = ttk.LabelFrame(panes, text="Failures")
        self.failures = tk.Text(failures, height=6, wrap="none", foreground="#c62828")
        self.failures.pack(fill="both", expand=True)
        panes.add(failures, weight=1)

        log = ttk.LabelFrame(panes, text="Output (select a test to see only its output)")
        self.log = tk.Text(log, height=10, wrap="none", font=("Consolas", 9))
        log_scroll = ttk.Scrollbar(log, command=self.log.yview)
        self.log.configure(yscrollcommand=log_scroll.set)
        self.log.pack(side="left", fill="both", expand=True)
        log_scroll.pack(side="right", fill="y")
        panes.add(log, weight=2)

        root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.after(100, self.poll)
        if opts.run:
            self.root.after(300, lambda: self.start(opts.rerun_failed))

    # ---- running ---------------------------------------------------------------------------------------------
    def build_dir(self):
        return ROOT / "build" / self.config.get()

    def ctest_args(self, rerun):
        args = [shutil.which("ctest", path=self.env["PATH"]) or "ctest"]
        if rerun:
            args.append("--rerun-failed")
        if self.only.get().strip():
            args += ["-R", self.only.get().strip()]
        exclude = [e for e in (self.exclude, None if self.noisy.get() else NOISY) if e]
        if exclude:
            args += ["-E", "|".join(f"({e})" for e in exclude)]
        return args

    def start(self, rerun):
        if self.running:
            return
        self.running, self.stopping = True, False
        self.tests.clear()
        self.by_number.clear()
        self.tree.delete(*self.tree.get_children())
        self.failures.delete("1.0", "end")
        self.log.delete("1.0", "end")
        self.view = None
        self.started = time.time()
        self.build_progress = (0, 0)
        self.editor_seen = 0.0
        for b in (self.run_button, self.rerun_button):
            b.configure(state="disabled")
        self.stop_button.configure(state="normal")
        args, jobs, build, config = self.ctest_args(rerun), self.jobs.get(), self.build.get(), self.config.get()
        threading.Thread(target=self.work, args=(args, jobs, build, config), daemon=True).start()

    def work(self, args, jobs, build, config):
        code = 0
        try:
            if build:
                self.events.put(("phase", "build"))
                code = self.stream(["cmd", "/c", str(ROOT / "build.bat"), config] + list(self.targets), ROOT, "build")
                if code != 0 or self.stopping:
                    self.events.put(("done", code if code else -1, "Build failed" if code else "Stopped"))
                    return
            cwd = ROOT / "build" / config
            listed = subprocess.run(args + ["-N"], cwd=cwd, env=self.env, capture_output=True, text=True, errors="replace",
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            names = [(int(m.group(1)), m.group(2)) for m in map(LISTED.search, listed.stdout.splitlines()) if m]
            self.events.put(("list", names))
            if not names:
                self.events.put(("done", 1, "No tests matched"))
                return
            self.events.put(("phase", "test"))
            code = self.stream(args + ["-j", str(jobs), "-V"], cwd, "test")
            self.events.put(("done", code, "Stopped" if self.stopping else None))
        except Exception as e:  # (a missing ctest, a bad folder...)
            self.events.put(("line", "build", f"ERROR {e}"))
            self.events.put(("done", 1, str(e)))

    def stream(self, args, cwd, phase):
        self.proc = subprocess.Popen(args, cwd=cwd, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     errors="replace", bufsize=1, creationflags=subprocess.CREATE_NO_WINDOW)
        for line in self.proc.stdout:
            echo(line.rstrip())
            self.events.put(("line", phase, line.rstrip("\r\n")))
        return self.proc.wait()

    def stop(self):
        self.stopping = True
        if self.proc and self.proc.poll() is None:  # the whole tree: ctest's tests, or the compiler under build.bat
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(self.proc.pid)], capture_output=True,
                           creationflags=subprocess.CREATE_NO_WINDOW)

    def close(self):
        self.stop()
        self.root.destroy()

    # ---- events ----------------------------------------------------------------------------------------------
    def expected_times(self):
        costs = {}
        try:
            for line in (self.build_dir() / "Testing/Temporary/CTestCostData.txt").read_text(errors="replace").splitlines():
                parts = line.split()
                if len(parts) == 3:
                    costs[parts[0]] = float(parts[2])
        except (OSError, ValueError):
            pass
        return costs

    def handle(self, event):
        kind = event[0]
        if kind == "phase":
            self.phase = event[1]
        elif kind == "list":
            costs = self.expected_times()
            for number, name in event[1]:
                expected = costs.get(name)
                self.tests[name] = dict(number=number, status="queued", passes=0, fails=0, start=0.0, time=None,
                                        expected=expected if expected else DEFAULT_EXPECTED, known=expected is not None,
                                        activity="", lines=[])
                self.by_number[number] = name
                self.tree.insert("", "end", iid=name, text=name)
                self.dirty.add(name)
        elif kind == "line":
            self.line(event[1], event[2])
        elif kind == "done":
            self.finish(event[1], event[2])

    def line(self, phase, text):
        name = None
        if phase == "build":
            m = NINJA.match(text)
            if m:
                self.build_progress = (int(m.group(1)), int(m.group(2)))
                self.status.configure(text=f"Building: {m.group(3)[:140]}")
        else:
            out = OUTPUT.match(text)
            if out and int(out.group(1)) in self.by_number:
                name = self.by_number[int(out.group(1))]
                body = out.group(2)
                t = self.tests[name]
                t["lines"].append(body)
                if len(t["lines"]) > 4000:
                    del t["lines"][:1000]
                if body.startswith("PASS"):
                    t["passes"] += 1
                elif body.startswith("FAIL"):
                    t["fails"] += 1
                    self.failures.insert("end", f"{name}: {body}\n")
                    self.failures.see("end")
                if body.strip() and not body.startswith(CTEST_NOISE):
                    t["activity"] = body.strip()[:200]
                self.dirty.add(name)
            elif m := START.match(text):
                name = m.group(2)
                if name in self.tests:
                    self.tests[name].update(status="running", start=time.time())
                    self.dirty.add(name)
            elif m := RESULT.search(text):
                name = m.group(2)
                if name in self.tests:
                    t = self.tests[name]
                    result = m.group(3).strip().strip("*").strip() or "?"
                    t["status"] = "passed" if result.startswith("Passed") else result.lower()
                    t["time"] = float(m.group(4))
                    if t["status"] != "passed":
                        self.failures.insert("end", f"{name}: {result} after {t['time']:.1f} s\n")
                        self.failures.see("end")
                    self.dirty.add(name)
        if self.view is None or self.view == name:
            self.log.insert("end", text + "\n")
            if int(self.log.index("end-1c").split(".")[0]) > 5000:
                self.log.delete("1.0", "1001.0")
            self.log.see("end")

    def finish(self, code, why):
        self.running = False
        self.proc = None
        for b in (self.run_button, self.rerun_button):
            b.configure(state="normal")
        self.stop_button.configure(state="disabled")
        failed = [n for n, t in self.tests.items() if t["status"] not in ("passed", "queued")]
        for name, t in self.tests.items():
            if t["status"] == "running":  # (stopped)
                t["status"] = "stopped"
                self.dirty.add(name)
        verdict = why or (f"{len(failed)} failed: {', '.join(failed)}" if failed else "All passed" if code == 0 else f"ctest exit code {code}")
        self.status.configure(text=f"Done in {fmt(time.time() - self.started)}: {verdict}")
        self.root.title(f"MVP tests: {'FAILED' if failed or code else 'passed'}")
        self.root.bell()
        echo(f"RESULT: {verdict} (exit {code}, {fmt(time.time() - self.started)})")

    def select(self, _):
        chosen = self.tree.selection()
        self.view = chosen[0] if chosen else None
        self.log.delete("1.0", "end")
        if self.view in self.tests:
            self.log.insert("end", "\n".join(self.tests[self.view]["lines"][-2000:]) + "\n")
            self.log.see("end")

    def editor_progress(self):
        # The editor test writes its checks to a file, not to ctest's output.
        t = self.tests.get("editor")
        if not t or t["status"] != "running":
            return
        f = self.build_dir() / "test-output/editor-test/progress.txt"
        try:
            stamp = f.stat().st_mtime
            if stamp <= max(self.editor_seen, t["start"]):
                return
            self.editor_seen = stamp
            lines = [l for l in f.read_text(errors="replace").splitlines() if l.strip()]
        except OSError:
            return
        t["passes"] = sum(l.startswith("PASS") for l in lines)
        t["fails"] = sum(l.startswith("FAIL") for l in lines)
        if lines:
            t["activity"] = lines[-1][:200]
        self.dirty.add("editor")

    def poll(self):
        for _ in range(3000):
            try:
                self.handle(self.events.get_nowait())
            except queue.Empty:
                break
        if self.running:
            self.editor_progress()
        now = time.time()
        for name in self.dirty:
            t = self.tests.get(name)
            if not t or not self.tree.exists(name):
                continue
            checks = f"{t['passes']} ok" + (f", {t['fails']} FAIL" if t["fails"] else "") if t["passes"] or t["fails"] else ""
            shown = f"{t['time']:.1f} s" if t["time"] is not None else ""
            tag = "passed" if t["status"] == "passed" else "running" if t["status"] == "running" else "" if t["status"] == "queued" else "failed"
            self.tree.item(name, values=(t["status"], checks, shown, f"{t['expected']:.{0 if t['expected'] >= 10 else 1}f} s" if t["known"] else "", t["activity"]),
                           tags=(tag,) if tag else ())
        self.dirty.clear()
        running = [n for n, t in self.tests.items() if t["status"] == "running"]
        for name in running:  # the running time ticks
            t = self.tests[name]
            self.tree.set(name, "time", f"{now - t['start']:.0f} s")
        if self.running:
            self.update_progress(now, running)
        self.root.after(200, self.poll)

    def update_progress(self, now, running):
        elapsed = now - self.started
        if self.phase == "build" or not self.tests:
            done, total = self.build_progress
            self.progress.configure(value=1000 * done / total if total else 0)
            self.stats.configure(text=f"Build step {done}/{total}   Elapsed {fmt(elapsed)}")
            self.root.title(f"MVP tests: building {100 * done // total if total else 0}%")
            return
        total = sum(t["expected"] for t in self.tests.values())
        done = sum(t["expected"] if t["status"] not in ("queued", "running") else min(now - t["start"], t["expected"] * 0.95)
                   if t["status"] == "running" else 0 for t in self.tests.values())
        left = max(0.0, total - done)
        jobs = max(1, min(int(self.jobs.get() or 1), max(1, len(running))))
        finished = [t for t in self.tests.values() if t["status"] not in ("queued", "running")]
        passed = sum(t["status"] == "passed" for t in finished)
        checks = sum(t["passes"] for t in self.tests.values()), sum(t["fails"] for t in self.tests.values())
        self.progress.configure(value=1000 * done / total if total else 0)
        self.status.configure(text=f"Running: {', '.join(running)}" if running else "Starting tests...")
        self.stats.configure(text=f"Tests {len(finished)}/{len(self.tests)}   Passed {passed}   Failed {len(finished) - passed}   "
                                  f"Running {len(running)}   Checks {checks[0]} ok / {checks[1]} FAIL   "
                                  f"Elapsed {fmt(elapsed)}   Left ~{fmt(left / jobs)}")
        self.root.title(f"MVP tests: {100 * done / total if total else 0:.0f}%")


def replace_previous():
    # One runner window at a time: a new run closes the last one (and anything it still runs).
    try:
        old = int(PIDFILE.read_text())
        if old != os.getpid():
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(old)], capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
    except (OSError, ValueError):
        pass
    try:
        PIDFILE.write_text(str(os.getpid()))
    except OSError:
        pass


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MVP test runner window")
    parser.add_argument("--run", action="store_true", help="start at once")
    parser.add_argument("--rerun-failed", action="store_true")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--targets", nargs="*", default=[], help="build only these CMake targets")
    parser.add_argument("--only", default="", help="ctest -R regex")
    parser.add_argument("--exclude", default="", help="ctest -E regex (on top of the GUI tests unless --noisy)")
    parser.add_argument("--noisy", action="store_true", help="include the GUI tests (sound)")
    parser.add_argument("--jobs", type=int, default=0)
    opts = parser.parse_args()
    if opts.run:
        replace_previous()
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista")
    except tk.TclError:
        pass
    App(root, opts)
    root.mainloop()
