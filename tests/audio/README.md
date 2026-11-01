# app_audio host tests

These tests exercise the SDK-independent bounded FIFO: wraparound ordering,
newest-data overflow policy, underflow, and stale-generation rejection.

```sh
cmake -S tests/audio -B /tmp/app_audio-tests
cmake --build /tmp/app_audio-tests
ctest --test-dir /tmp/app_audio-tests --output-on-failure
```
