#include "framework.hpp"
#include <meta>
#define APPLICATION_LOCAL auto generated = 1
#define MEMBER_INITIALIZER member(4)
int read_value() { return 1; }
struct Pair
{
    int first;
    int second;
};
struct Settings
{
    int field = 1;
    int braced{ 2 };
    Settings()
      : braced(3)
    {
    }
};
struct MacroSettings
{
    int member{};
    MacroSettings()
      : MEMBER_INITIALIZER
    {
    }
};
template<class T>
constexpr int VALUE = 1;
template<>
constexpr int VALUE<int> = 2;
void exercise(int parameter = 1)
{
    double time = 0;
    auto forbidden = read_value();
    int typed = 2;
    int parentheses(3);
    auto allowed{ read_value() };
    const auto& reference{ allowed };
    auto [first, second] = Pair{ 1, 2 };
    auto [third, fourth]{ Pair{ 3, 4 } };
    /* µ */ auto unicode_line = 1;
    auto comment /* keep */ = 2;
    int array[] = { 1, 2 };
    auto decimal = 0.125;
    auto raw = R"tag(raw string)tag";
    constexpr auto reflected = ^^Pair;
    int braced_array[]{ 1, 2 };
    for (auto value : braced_array) {
        allowed = value;
    }
    auto generic{ [](auto argument = 1) { return argument; } };
    auto capture{ [copy = allowed] { return copy; } };
    const auto* function{ __PRETTY_FUNCTION__ };
    APPLICATION_LOCAL;
    EXTERNAL_LOCAL;
    EXTERNAL_NAMED(external_argument);
}

void expansion_body()
{
    template for (constexpr auto value : { 1, 2 }) { const auto forbidden_local = value; }
}
