"""Check link images: the embedded ELF is data, not a mixed C++ ABI."""
import subprocess


def test_native_and_controller_link_isolation(simulator, controller, request):
    def symbols(path):
        return subprocess.check_output(
            [request.config.getoption("nm"), "--defined-only", "--demangle", path], text=True)

    host = symbols(simulator)
    firmware = symbols(controller)
    for required in ("simulation::Machine::execute", "simulation::Session::start", "simulation::assets::controller_image()"):
        assert required in host, f"native simulation is missing {required}"
    for forbidden in ("_tx_thread_schedule", "simulation::client::call", "touchgfx::", "runtime::thread::"):
        assert forbidden not in host, f"controller runtime leaked into native application: {forbidden}"
    for required in ("simulation::client::call", "ui::Application::", "io::Application::"):
        assert required in firmware, f"controller is missing {required}"
    for forbidden in ("simulation::Machine::", "simulation::Session::", "simulation::viewer::", "simulation::assets::",
                      "SimulatedStepHardware::", "Tmc2209Model::", "simulation::CollisionMesh::", "simulation::ContactSensors::", "simulation::Kinematics::"):
        assert forbidden not in firmware, f"simulated hardware leaked into controller: {forbidden}"

    assert "simulation::viewer::ModelAsset::read(" in host, "native viewer must use the model importer"
    for name in ("simulation::viewer::ModelAsset", "simulation::viewer::ModelRenderer", "Assimp::"):
        assert name not in firmware, f"native CAD importer leaked into controller: {name}"

    for required in ("ui::motion::RotaryAxisConversion::toAxisPosition", "ui::motion::LinearAxisConversion::toAxisPosition"):
        assert required in host and required in firmware, f"pure shared conversion missing: {required}"

    native_tests = request.config.getoption("native_test")
    assert native_tests, "Supply --native-test executables (CTest does this automatically)"
    for executable in native_tests:
        image = symbols(executable)
        assert "testing::InitGoogleTest" in image, executable
        for forbidden in ("_tx_thread_schedule", "runtime_libstdcxx_initialize", "simulation::client::call"):
            assert forbidden not in image, f"ThreadX runtime leaked into {executable}: {forbidden}"

