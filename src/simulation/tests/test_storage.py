"""Verify actual FileX/LevelX persistence across controller restarts."""
import subprocess


def test_storage_persists_across_restart(launcher, storage_probe, tmp_path):
    for action in ("write", "read"):
        result = subprocess.run([launcher, storage_probe, str(tmp_path), action],
                                capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, (action, result.stdout, result.stderr)
    assert (tmp_path / "flash.img").stat().st_size == 16 * 1024 * 1024
    assert (tmp_path / "sd.img").stat().st_size == 64 * 1024 * 1024
