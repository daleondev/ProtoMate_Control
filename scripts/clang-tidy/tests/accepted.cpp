#include <meta>
#include <vector>
struct Pair
{
    int first{};
    int second{};
};
struct Settings
{
    int value{};
    Settings()
      : value{ 3 }
    {
    }
};
void exercise(int parameter = 0)
{
    double time{ 0 };
    auto inferred{ 1 };
    const auto& reference{ inferred };
    int array[]{ 1, 2 };
    auto [first, second]{ Pair{ 1, 2 } };
    constexpr auto reflected{ ^^Pair };
    for (auto value : array) {
        time = value;
    }
    auto capture{ [copy = inferred] { return copy; } };
    // NOLINTNEXTLINE(protomate-brace-initialization): select count/value constructor
    std::vector<int> count_and_value(10, 0);
}

void expansion_variables()
{
    template for (constexpr auto value : { 1, 2 }) { const auto copy{ value }; }
    template for (constexpr auto member : std::define_static_array(
                    std::meta::members_of(^^Pair, std::meta::access_context::current())))
    {
        constexpr auto reflected{ member };
    }
}
