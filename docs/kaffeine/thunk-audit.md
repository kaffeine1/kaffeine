# Generated thunk interface audit

The [CSV snapshot](thunk-audit.csv) describes 789 generated library entries
across 17 libraries. It classifies interface risks in the generated layer;
manual overrides determine the effective behavior of a supported call.
This snapshot is not a compatibility guarantee for every listed API.

## Categories

| Category | Meaning |
| --- | --- |
| S | Scalar arguments and result |
| B | Guest buffer or string requiring bounds validation |
| T | Structure or tag list requiring layout, pointer-width or byte-order conversion |
| R | Native pointer or handle result requiring guest representation |
| O | Output or in/out argument requiring conversion |
| C | Callback, function pointer or control-flow boundary |
| P | BPTR or BSTR representation boundary |
| W | LVO or register difference between interface descriptions |
| X | Generated entry replaced by a manual thunk |
| ? | Private or polymorphic entry not decidable from a prototype alone |

Several categories can apply to one entry. A manual override can provide
conversions or containment that the generated fallback cannot provide.
See [INTERNALS.md](../../rom/kaffeine/INTERNALS.md) for the supported behavior.

## Reproducing the table

The generator accepts AROS FD, NDK clib and NDK FD directories. Supply the
SDK inputs separately; they are not bundled in this repository.

```sh
python3 rom/kaffeine/kaffeine_thunkgen.py --audit \
    /path/to/aros/fd /path/to/ndk/clib \
    --ndk-fd /path/to/ndk/fd > /path/to/thunk-audit.csv
```

The registration check validates the current manual tables against the
AROS configuration files and can additionally use external FD files:

```sh
python3 developer/debug/test/kaffeine_host/test_thunk_registrations.py
python3 developer/debug/test/kaffeine_host/test_thunk_registrations.py \
    --ndk-fd-dir /path/to/ndk/fd --aros-fd-dir /path/to/aros/fd
```

The static audit identifies interfaces needing review. The host and target
tests establish the behavior of implemented adapters, including bounds,
ownership, register preservation and native-reference comparisons.
