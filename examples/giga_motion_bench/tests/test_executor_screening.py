import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from capture_stream import executor_screening


@pytest.mark.parametrize('changes,expected', [
    ({}, True),
    ({'cpu_10ms_ticks': 93}, False),
    ({'refill_cycles': 13000}, False),
    ({'auxiliary': 940}, False),
    ({'minimum_stack_permill': 249}, False),
])
def test_electrically_correct_output_does_not_override_capacity(changes, expected):
    report = dict(core=4, cpu_10ms_ticks=85, wall_us=1000000,
                  elapsed_us=1000000, auxiliary=1000, minimum_stack_permill=300,
                  refill_budget_ticks=1000, refill_cycles=10000)
    report.update(changes)
    result = executor_screening([report], 20)
    assert result['passed'] is expected
    assert bool(result['failures']) is not expected
