#include "presence.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>

using baby_edge::Presence;
using baby_edge::PresenceFilter;

void expect(Presence actual, Presence expected)
{
    if (actual != expected) {
        std::cerr << baby_edge::name(actual) << " != " << baby_edge::name(expected) << '\n';
        std::exit(1);
    }
}

int main()
{
    PresenceFilter filter;
    expect(filter.current(0), Presence::Unknown);
    expect(filter.update(0, 0, true, 0.5f), Presence::Unknown);
    expect(filter.update(1999, 1999, true, 0.9f), Presence::Unknown);
    expect(filter.update(2000, 2000, true, 0.9f), Presence::Present);
    expect(filter.update(3000, 3000, true, 0), Presence::Present);
    expect(filter.update(7999, 7999, true, 0), Presence::Present);
    expect(filter.update(8000, 8000, true, 0), Presence::Absent);
    expect(filter.current(18001), Presence::Unknown);
    expect(filter.update(20000, 20000, true, 0.9f), Presence::Unknown);
    expect(filter.update(22000, 22000, true, 0.9f), Presence::Present);
    expect(filter.update(22000, 22000, true, 0.9f), Presence::Unknown);
    expect(filter.update(23000, 23000, true, 0.9f), Presence::Unknown);
    expect(filter.update(25000, 25000, false, 0.9f), Presence::Unknown);
    expect(filter.update(26000, 26000, true, std::numeric_limits<float>::quiet_NaN()), Presence::Unknown);
    expect(filter.update(27000, 27000, true, 2), Presence::Unknown);
    expect(filter.update(28000, 27999, true, 0.9f), Presence::Unknown);
    expect(filter.update(0, 10001, true, 0.9f), Presence::Unknown);
    expect(filter.update(30000, 30000, true, 0.9f), Presence::Unknown);
    expect(filter.update(50000, 50000, true, 0.9f), Presence::Unknown);
    expect(filter.update(51000, 51000, true, 0), Presence::Unknown);
    expect(filter.update(52000, 52000, true, 0.9f), Presence::Unknown);
    expect(filter.update(54000, 54000, true, 0.9f), Presence::Present);
    expect(filter.current(53000), Presence::Unknown);
    PresenceFilter bad({0, 0, 0, 100});
    expect(bad.update(0, 0, true, 0), Presence::Unknown);
    std::cout << "presence tests passed\n";
}
