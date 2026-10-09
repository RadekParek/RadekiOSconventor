import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class TranslatedRuntimeFilesystemTests(unittest.TestCase):
    def test_app_private_guest_root_tmp_optional_obb_and_bundle_mounts(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if not compiler:
            self.skipTest("a host C compiler is required for the portable runtime filesystem test")
        repository = Path(__file__).resolve().parent.parent
        runtime = repository / "radek" / "game" / "rt"
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "rt-fs-test"
            compile_result = subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(runtime),
                    str(runtime / "rt_fs.c"),
                    str(runtime / "rt_fs_test.c"),
                    "-o",
                    str(executable),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            result = subprocess.run(
                [str(executable)],
                check=False,
                capture_output=True,
                text=True,
            )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("app-private, bundle, OBB, and traversal path tests passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
