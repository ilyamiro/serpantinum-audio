#include <cassert>
#include <cstdio>

#include "../src/control.h"

int main() {
    sa::Params p;
    assert(sa::applyCommand(p, "set 5 7 5 2 1 0 0 0 1 2 -0.35 0 1 night 0"));
    assert(p.dynamics == sa::Dynamics::Night && p.swap && !p.mono && p.balance < -0.34f && p.gains[1] == 7);

    sa::Params q = p;
    assert(!sa::applyCommand(q, "set 1 2 3"));
    assert(!sa::applyCommand(q, "dynamics loud"));
    assert(!sa::applyCommand(q, "band 11 3"));
    assert(!sa::applyCommand(q, "unknown"));
    assert(q.gains == p.gains);

    assert(sa::applyCommand(q, "balance 7"));
    assert(q.balance == 1.0f);
    assert(sa::applyCommand(q, "band 3 -4.5"));
    assert(q.gains[2] == -4.5f);

    sa::Params r;
    assert(sa::applyCommand(r, sa::serialize(p)));
    assert(r.gains == p.gains && r.dynamics == p.dynamics && r.swap == p.swap && r.balance == p.balance);

    assert(sa::applyCommand(q, "dyneq 1"));
    assert(q.dynamicEq);
    assert(!sa::applyCommand(q, "dyneq maybe"));
    sa::Params legacy;
    assert(sa::applyCommand(legacy, "set 0 0 0 0 0 0 0 0 0 0 0 0 0 off 0"));
    assert(!legacy.dynamicEq);
    sa::Params withDyn;
    assert(sa::applyCommand(withDyn, "set 0 0 0 0 0 0 0 0 0 0 0 0 0 off 0 1"));
    assert(withDyn.dynamicEq);
    sa::Params round;
    assert(sa::applyCommand(round, sa::serialize(withDyn)));
    assert(round.dynamicEq);

    sa::Params safe;
    assert(!sa::applyCommand(safe, "band 1 nan"));
    assert(!sa::applyCommand(safe, "band 1 inf"));
    assert(!sa::applyCommand(safe, "balance nan"));
    assert(safe.gains[0] == 0.0f && safe.balance == 0.0f);
    assert(sa::applyCommand(safe, "band 1 1000"));
    assert(safe.gains[0] == sa::kMaxGainDb);
    assert(sa::applyCommand(safe, "band 2 -1000"));
    assert(safe.gains[1] == -sa::kMaxGainDb);

    std::puts("control ok");
    return 0;
}
