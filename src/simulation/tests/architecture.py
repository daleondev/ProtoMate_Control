"""Check the real link images: the embedded ELF is data, not a mixed C++ ABI."""
import subprocess
import sys

nm, native, controller = sys.argv[1:]
def symbols(path):
    return subprocess.check_output([nm, "--defined-only", "--demangle", path], text=True)

host = symbols(native)
firmware = symbols(controller)
for required in ("simulation::Machine::execute", "simulation::Session::start", "protomate_controller_image_start"):
    assert required in host, f"native simulation is missing {required}"
for forbidden in ("_tx_thread_schedule", "simulation::client::call", "touchgfx::", "runtime::thread::"):
    assert forbidden not in host, f"controller runtime leaked into native application: {forbidden}"
for required in ("simulation::client::call", "ui::Application::", "io::Application::"):
    assert required in firmware, f"controller is missing {required}"
for forbidden in ("simulation::Machine::", "simulation::Session::", "simulation::viewer::",
                  "SimulatedStepHardware::", "Tmc2209Model::"):
    assert forbidden not in firmware, f"simulated hardware leaked into controller: {forbidden}"
print("PASS: native and ThreadX link closures remain isolated; production controller contains proxies only")
