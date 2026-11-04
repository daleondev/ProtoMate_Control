#include "../Session.hpp"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char** argv)
try {
    if (argc != 4)
        return 2;
    setenv("PROTOMATE_MEDIA_DIR", argv[2], 1);
    const int image{ open(argv[1], O_RDONLY | O_CLOEXEC) };
    if (image < 0)
        return 3;
    simulation::Session session{};
    try {
        session.launch(image, { argv + 3, 1 });
    } catch (...) {
        close(image);
        throw;
    }
    close(image);
    session.start();
    while (session.running())
        std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
    return session.childStatus();
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
