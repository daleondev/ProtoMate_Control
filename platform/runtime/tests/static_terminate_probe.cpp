#include <stdexcept>

namespace
{
    class ThrowDuringStaticInitialization
    {
      public:
        ThrowDuringStaticInitialization()
        {
            throw std::runtime_error{ "uncaught static initialization exception" };
        }
    };

    // NOLINTNEXTLINE(cert-err58-cpp,cppcoreguidelines-avoid-non-const-global-variables)
    ThrowDuringStaticInitialization throwing_static;
}

int main(int, char**) { return 1; }
