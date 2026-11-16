"""get.sh's clone and update, offline: a new clone takes one branch with --filter=blob:none, an
older git that refuses the filter gets a plain one-branch clone, and an update fetches the
branch it wants by name (a one-branch clone fetches nothing else). git is a stub on PATH that
logs its arguments, and HOME is a temp folder; the script is only sourced, so main never runs
and nothing reaches the network."""
import pathlib
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "get.sh"

# Logs each call; a clone makes its folder (the last argument) unless FAKE_CLONE says otherwise:
# "refuse-filter" fails as an older git does on --filter (before making the folder), "half"
# makes the folder and fails, as a clone cut short might leave it.
STUB = r"""#!/bin/bash
printf '%s\n' "$*" >>"$FAKE_LOG"
if [ "$1" = clone ]; then
  dir=${@: -1}
  case "$FAKE_CLONE:$*" in
    refuse-filter:*--filter*) echo "error: unknown option \`filter=blob:none'" >&2; exit 129 ;;
    half:*) mkdir -p "$dir"; exit 128 ;;
    fail:*) exit 128 ;;
  esac
  mkdir -p "$dir"
fi
exit 0
"""


class Get(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        self.home, self.bin, self.log = root / "home", root / "bin", root / "git.log"
        self.home.mkdir()
        self.bin.mkdir()
        (self.bin / "git").write_text(STUB)
        (self.bin / "git").chmod(0o755)
        self.dir = self.home / "frametop"

    def tearDown(self):
        self.tmp.cleanup()

    def bash(self, script, clone=""):
        env = {"HOME": str(self.home), "PATH": f"{self.bin}:/usr/bin:/bin", "FAKE_LOG": str(self.log),
               "FAKE_CLONE": clone}
        return subprocess.run(["bash", "-c", f'source "$1" && {script}', "bash", str(SCRIPT)], env=env,
                              capture_output=True, text=True, timeout=30, stdin=subprocess.DEVNULL)

    def calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_sourcing_runs_nothing(self):
        r = self.bash("echo sourced")
        self.assertEqual((r.returncode, r.stdout, r.stderr), (0, "sourced\n", ""))
        self.assertEqual(self.calls(), [])

    def test_a_new_clone_is_one_branch_without_blobs(self):
        r = self.bash(f'clone_repo https://example.invalid/f.git experimental "{self.dir}"')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.calls(), [
            f"clone --single-branch --filter=blob:none --branch experimental https://example.invalid/f.git {self.dir}"])

    def test_a_git_that_refuses_the_filter_gets_a_plain_clone(self):
        r = self.bash(f'clone_repo https://example.invalid/f.git main "{self.dir}"', clone="refuse-filter")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.calls(), [
            f"clone --single-branch --filter=blob:none --branch main https://example.invalid/f.git {self.dir}",
            f"clone --single-branch --branch main https://example.invalid/f.git {self.dir}"])
        self.assertIn("without --filter", r.stderr)
        self.assertTrue(self.dir.is_dir())

    def test_a_folder_left_behind_isnt_cloned_over(self):
        r = self.bash(f'clone_repo https://example.invalid/f.git main "{self.dir}"', clone="half")
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(len(self.calls()), 1)

    def test_a_failing_clone_fails(self):
        r = self.bash(f'clone_repo https://example.invalid/f.git main "{self.dir}"', clone="fail")
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(len(self.calls()), 2)

    def test_an_update_fetches_its_branch_by_name(self):
        r = self.bash(f'fetch_branch "{self.dir}" experimental')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.calls(), [
            f"-C {self.dir} fetch --quiet origin +refs/heads/experimental:refs/remotes/origin/experimental"])

    # main with the Steam Frame check passed (grep says yes for /etc/os-release) and install_release
    # only printing what it's given: install.sh's options pass through, others are refused.
    MAIN = ('grep() { case "$*" in *os-release*) return 0 ;; esac; command grep "$@"; }; '
            'install_release() { printf "[%s]" "$@"; }; main --release --yes ')

    def test_no_keyboard_passes_through_to_the_install(self):
        r = self.bash(self.MAIN + "--no-hands --no-keyboard")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("unknown option", r.stderr)
        # channel, version, zip, dir, clone-only, any SteamOS, then install.sh's options
        self.assertEqual(r.stdout, "[stable][][][][0][0][--][--yes][--no-hands][--no-keyboard]")

    def test_an_unknown_option_is_refused(self):
        r = self.bash(self.MAIN + "--no-such-thing")
        self.assertEqual(r.returncode, 2)
        self.assertIn("unknown option: --no-such-thing", r.stderr)
        self.assertEqual(r.stdout, "")

    def test_usage_lists_no_keyboard(self):
        r = self.bash("usage")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.count("[--no-keyboard]"), 2)  # the clone line and the --release line

    def test_main_uses_them(self):
        text = SCRIPT.read_text()
        self.assertIn('clone_repo "$repo" "$branch" "$dir"', text)
        self.assertIn('fetch_branch "$dir" "$branch"', text)
        self.assertNotIn("git clone --branch", text)


if __name__ == "__main__":
    unittest.main()
