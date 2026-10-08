"""Prevent TIM6 from regressing to ThreadX PendSV's idle priority."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--cmake", required=True)
    args = parser.parse_args()
    ioc = "external/CubeMX/CubeMX.ioc"
    header = "external/CubeMX/Inc/stm32h7xx_hal_conf.h"
    mutations = (
        (ioc, br"NVIC.TIM6_DAC_IRQn=true\:14\:0", br"NVIC.TIM6_DAC_IRQn=true\:15\:0"),
        (header, b"TICK_INT_PRIORITY            (14UL)", b"TICK_INT_PRIORITY            (15UL)"),
    )
    files = (ioc, header, "external/CubeMX/STM32H753XX_FLASH.ld",
             "external/CubeMX/Src/main.c", "external/CubeMX/Src/stm32h7xx_it.c",
             "external/CubeMX/Src/gpio.c", "external/CubeMX/Src/usart.c",
             "external/CubeMX/Src/tim.c")
    with tempfile.TemporaryDirectory(prefix="protomate-timebase-guard-") as directory:
        fixture = Path(directory)
        for name in files:
            target = fixture / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.source / name, target)
        runner = fixture / "verify.cmake"
        runner.write_text(
            f'include("{(args.source.resolve() / "cmake/verify_cubemx.cmake").as_posix()}")\n'
            "verify_cubemx_generation()\n")

        def run():
            return subprocess.run(
                [args.cmake, f"-DPROJECT_SOURCE_DIR={fixture}", "-P", str(runner)],
                capture_output=True, text=True, timeout=10)

        baseline = run()
        assert baseline.returncode == 0, baseline.stdout + baseline.stderr
        for selected in ((mutations[0],), (mutations[1],), mutations):
            originals = {}
            for name, before, after in selected:
                target = fixture / name
                original = target.read_bytes()
                assert original.count(before) == 1, (name, before)
                originals[name] = original
                target.write_bytes(original.replace(before, after))
            result = run()
            assert result.returncode != 0 and "CubeMX generation guard failed" in result.stderr, (
                result.stdout, result.stderr)
            for name, original in originals.items():
                (fixture / name).write_bytes(original)
            print("Rejected priority-15 regression: " + ", ".join(originals))
    print("Valid TIM6 priority accepted; three idle-starvation regressions rejected.")


if __name__ == "__main__":
    main()
