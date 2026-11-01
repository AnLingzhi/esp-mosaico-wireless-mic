# Button gesture tests

Run `cmake -S tests/controls -B .codex-runs/tests-controls`, build that directory,
then run `ctest --test-dir .codex-runs/tests-controls --output-on-failure`.

Checks long hold down/up, double-click issuing exactly one Enter, single clicks,
expired click windows, a held second press, and unsigned clock wrap. These are
logic tests, not a simulator and not proof of Mac key delivery.
